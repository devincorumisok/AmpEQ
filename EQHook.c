/* AmpEQ - audio hooks for mediaserverd (EQ, plugins, gain, clip meter).
 *
 * Design rules
 *  - No file I/O, no Objective-C, no CFPreferences inside mediaserverd. Settings
 *    arrive as Darwin notify state (see AmpEQShared.h). This is the v1.4.2 fix.
 *  - Every stage reports itself: hooks installed, render calls seen, which audio
 *    units exist (type/subtype/format), which one is processed, frames processed,
 *    and why buffers were skipped.
 *  - The render path never allocates, never locks, never logs.
 *  - Hooks are installed exactly once, at load. "Reconnect" never re-hooks; it
 *    clears caches, releases the processing lock and re-pulls settings.
 */
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <notify.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <syslog.h>
#include <unistd.h>
#include <mach/mach.h>
#include <os/lock.h>
#include <substrate.h>
#include "AmpEQShared.h"
#include "EQDSP.h"
#include "AEQPlugins.h"
#include <errno.h>

int  AmpEQ_AmpObserverInstall(void *substrateHook); /* AmpObserver.c */
void AmpEQ_AmpObserverSetEnabled(int on);
void AmpEQ_AmpBoostSet(int steps);
void AmpEQ_AmpObserverPublish(void);
void AmpEQ_AmpRouteSet(int route);
int  AmpEQ_AmpRouteGet(void);
uint32_t AmpEQ_AmpCodecNow(void);
void AmpEQ_RelayInstall(void);                      /* Relay.c */

typedef OSStatus (*render_t)(AudioUnit,AudioUnitRenderActionFlags*,const AudioTimeStamp*,UInt32,UInt32,AudioBufferList*);
typedef OSStatus (*init_t)(AudioUnit);
typedef OSStatus (*dispose_t)(AudioComponentInstance);
typedef OSStatus (*convfill_t)(AudioConverterRef,AudioConverterComplexInputDataProc,void*,UInt32*,AudioBufferList*,AudioStreamPacketDescription*);
typedef OSStatus (*auproc_t)(AudioUnit,AudioUnitRenderActionFlags*,const AudioTimeStamp*,UInt32,AudioBufferList*);
static render_t   o_render;
static init_t     o_init;
static dispose_t  o_dispose;
static convfill_t o_convfill;
static auproc_t   o_auproc;

static bool g_processing_process=false; /* true in mediaserverd, false in Music (observe-only) */
static const char *g_pfx="ms";

/* ---------- live settings (written by notify handler, read by render thread) ---------- */
static _Atomic float g_tone_amp=AEQ_TONE_LEVEL;
/* clip meter + Peace-style protection */
static _Atomic int g_prot_on=0, g_prot_cdb=0, g_prot_reset=-1;
static _Atomic uint32_t g_peak_bits=0;   /* largest |sample| (positive float bits; ordered like the floats) */
static inline void peak_note(float a){
 if(!(a>0)||!isfinite(a))return; uint32_t nb; memcpy(&nb,&a,4);
 uint32_t cur=atomic_load_explicit(&g_peak_bits,memory_order_relaxed);
 while(nb>cur&&!atomic_compare_exchange_weak_explicit(&g_peak_bits,&cur,nb,memory_order_relaxed,memory_order_relaxed)){}
}
static _Atomic int g_enabled=0, g_target=AEQ_TARGET_AUTO, g_have_settings=0, g_tone=0;
static _Atomic int g_gain_c[EQ_BANDS];        /* centi-dB */
static _Atomic int g_total_c=0;               /* centi-dB: preamp - auto headroom */
static _Atomic unsigned g_gen=1;              /* bumped on every settings change */
static _Atomic unsigned g_reset_gen=1;        /* bumped on reconnect -> clear filter history */
static _Atomic uint32_t g_serial=0;
/* plugins: double-buffered params (written on the control queue, read on the render thread) */
static AEQPluginParams g_pp[2]; static _Atomic int g_pp_idx=0; static _Atomic uint32_t g_plg_flags=0;
static AEQPluginState *g_ps; static _Atomic(AEQIR*) g_ir44, g_ir48; static uint32_t g_conv_loaded=0xFFFFFFFF;
static _Atomic int g_ir_err=0; static int g_plg_slot=-1;

/* ---------- counters ---------- */
static _Atomic uint64_t g_render_calls, g_frames, g_conv_calls, g_proc_calls;
static _Atomic uint32_t g_skip[AEQ_SKIP_COUNT];
static uint32_t g_skip_prev[AEQ_SKIP_COUNT];
static _Atomic uint32_t g_flags;
static uint32_t g_secs;
static dispatch_source_t g_timer; /* lives for the process lifetime */

/* ---------- unit table ---------- */
enum { K_NONE=0, K_F32, K_S16, K_S32 };
typedef struct {
 _Atomic int state;           /* 0 free, 1 claiming, 2 ready */
 AudioUnit unit; UInt32 bus;
 uint32_t type, subtype;
 _Atomic uint64_t calls;
 _Atomic int fmt_state;       /* 0 query needed, 1 supported, 2 unsupported */
 AudioStreamBasicDescription fmt;
 int kind; unsigned frac;
 unsigned gen, reset_gen; float rate_cfg;
 EQEngine eng;
 uint64_t seen_calls;         /* timer-only */
 double tone_phase;
} Slot;
#define NSLOTS 24
static Slot g_slots[NSLOTS];
static _Atomic int g_lock=-1;            /* AUTO mode: index of the one slot we process */
static _Atomic int g_any_mixer_eligible=0;

static inline void skip(int why){atomic_fetch_add_explicit(&g_skip[why],1,memory_order_relaxed);}

static Slot *slot_for(AudioUnit u,UInt32 bus){
 for(int i=0;i<NSLOTS;i++){
  Slot *s=&g_slots[i];
  if(atomic_load_explicit(&s->state,memory_order_acquire)==2&&s->unit==u&&s->bus==bus)return s;
 }
 for(int i=0;i<NSLOTS;i++){
  Slot *s=&g_slots[i]; int expect=0;
  if(atomic_compare_exchange_strong(&s->state,&expect,1)){
   s->unit=u; s->bus=bus; s->type=s->subtype=0; s->kind=K_NONE; s->frac=0;
   s->gen=0; s->reset_gen=0; s->rate_cfg=0; s->seen_calls=0;
   atomic_store(&s->calls,0); atomic_store(&s->fmt_state,0);
   AudioComponent c=AudioComponentInstanceGetComponent(u);
   AudioComponentDescription d; memset(&d,0,sizeof d);
   if(c&&AudioComponentGetDescription(c,&d)==noErr){s->type=d.componentType;s->subtype=d.componentSubType;}
   atomic_store_explicit(&s->state,2,memory_order_release);
   return s;
  }
 }
 return NULL;
}
static void query_format(Slot *s){
 AudioStreamBasicDescription f; memset(&f,0,sizeof f); UInt32 sz=sizeof f;
 int st=2;
 if(AudioUnitGetProperty(s->unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,s->bus,&f,&sz)==noErr){
  s->fmt=f;
  UInt32 fl=f.mFormatFlags; int k=K_NONE; unsigned frac=0;
  if(f.mFormatID==kAudioFormatLinearPCM&&!(fl&kAudioFormatFlagIsBigEndian)&&
     f.mChannelsPerFrame>=1&&f.mChannelsPerFrame<=EQ_CHANNELS&&f.mSampleRate>=8000&&f.mSampleRate<=192000){
   if((fl&kAudioFormatFlagIsFloat)&&f.mBitsPerChannel==32)k=K_F32;
   else if((fl&kAudioFormatFlagIsSignedInteger)&&f.mBitsPerChannel==16)k=K_S16;
   else if((fl&kAudioFormatFlagIsSignedInteger)&&f.mBitsPerChannel==32){
    k=K_S32; frac=(fl&kLinearPCMFormatFlagsSampleFractionMask)>>kLinearPCMFormatFlagsSampleFractionShift;
    if(frac==0)frac=31;
   }
  }
  s->kind=k; s->frac=frac; st=k?1:2;
 }else s->kind=K_NONE;
 atomic_store(&s->fmt_state,st);
}
static inline bool eligible(const Slot *s){
 if(s->type==kAudioUnitType_Mixer)return s->bus==0;
 if(s->type==kAudioUnitType_Output)return s->bus==0;   /* bus 1 on an I/O unit = microphone */
 return false;
}
static bool is_target(Slot *s,int idx,int target){
 if(!eligible(s))return false;
 switch(target){
 case AEQ_TARGET_MIXERS: return s->type==kAudioUnitType_Mixer;
 case AEQ_TARGET_OUTPUT: return s->type==kAudioUnitType_Output;
 case AEQ_TARGET_AUTO: default: {
  if(s->type==kAudioUnitType_Mixer)atomic_store_explicit(&g_any_mixer_eligible,1,memory_order_relaxed);
  int lock=atomic_load_explicit(&g_lock,memory_order_acquire);
  if(lock==idx)return true;
  if(lock>=0)return false;
  /* prefer the first mixer; fall back to an output unit only if no mixer ever qualifies */
  if(s->type==kAudioUnitType_Output&&
     (atomic_load_explicit(&g_any_mixer_eligible,memory_order_relaxed)||
      atomic_load_explicit(&g_render_calls,memory_order_relaxed)<4000))return false;
  int expect=-1;
  return atomic_compare_exchange_strong(&g_lock,&expect,idx)||expect==idx;
 }}
}
static void configure(Slot *s){
 unsigned gen=atomic_load(&g_gen), rg=atomic_load(&g_reset_gen);
 float rate=(float)s->fmt.mSampleRate;
 if(s->gen==gen&&s->reset_gen==rg&&s->rate_cfg==rate)return;
 float gains[EQ_BANDS];
 for(int i=0;i<EQ_BANDS;i++)gains[i]=(float)atomic_load_explicit(&g_gain_c[i],memory_order_relaxed)/100.0f;
 if(s->reset_gen!=rg||s->rate_cfg!=rate)eq_reset_history(&s->eng);
 eq_set(&s->eng,rate,gains,(float)atomic_load(&g_total_c)/100.0f,1);
 s->gen=gen; s->reset_gen=rg; s->rate_cfg=rate;
}
/* Measurement tone: replaces the target mix with a 1 kHz sine on every channel (-0.5 dBFS unless set.tone says otherwise). */
static void tone(Slot *s,UInt32 n,AudioBufferList *d){
 const AudioStreamBasicDescription *f=&s->fmt;
 int ni=(f->mFormatFlags&kAudioFormatFlagIsNonInterleaved)!=0;
 UInt32 ch=f->mChannelsPerFrame, bps=f->mBitsPerChannel/8;
 if(ni?(d->mNumberBuffers!=ch):(d->mNumberBuffers!=1)){skip(AEQ_SKIP_SHAPE);return;}
 double w=2.0*3.14159265358979323846*1000.0/f->mSampleRate, ph=s->tone_phase;
 float amp=atomic_load_explicit(&g_tone_amp,memory_order_relaxed);
 for(UInt32 b=0;b<d->mNumberBuffers;b++){
  AudioBuffer *ab=&d->mBuffers[b]; UInt32 bc=ni?1:ch;
  if(!ab->mData||ab->mDataByteSize<(UInt64)n*bc*bps){skip(AEQ_SKIP_SHAPE);return;}
 }
 for(UInt32 i=0;i<n;i++){
  float x=amp*(float)sin(ph+w*i);
  for(UInt32 b=0;b<d->mNumberBuffers;b++){
   AudioBuffer *ab=&d->mBuffers[b]; UInt32 bc=ni?1:ch;
   for(UInt32 c=0;c<bc;c++){
    size_t k=(size_t)i*bc+c;
    switch(s->kind){
     case K_F32: ((float*)ab->mData)[k]=x; break;
     case K_S16: ((int16_t*)ab->mData)[k]=(int16_t)lrintf(x*32767.0f); break;
     case K_S32: ((int32_t*)ab->mData)[k]=(int32_t)llrint((double)x*ldexp(1.0,(int)s->frac)); break;
    }
   }
  }
 }
 s->tone_phase=fmod(ph+w*n,2.0*3.14159265358979323846);
 atomic_fetch_add_explicit(&g_frames,n,memory_order_relaxed);
}
static float buf_peak(Slot *s,UInt32 n,AudioBufferList *d){
 const AudioStreamBasicDescription *f=&s->fmt; int ni=(f->mFormatFlags&kAudioFormatFlagIsNonInterleaved)!=0;
 UInt32 ch=f->mChannelsPerFrame; float pk=0;
 for(UInt32 b=0;b<d->mNumberBuffers;b++){ UInt32 cnt=n*(ni?1:ch); void *p=d->mBuffers[b].mData; if(!p)continue;
  switch(s->kind){
   case K_F32: for(UInt32 i=0;i<cnt;i++){float a=fabsf(((float*)p)[i]); if(a>pk)pk=a;} break;
   case K_S16: for(UInt32 i=0;i<cnt;i++){float a=fabsf((float)((int16_t*)p)[i])*(1.0f/32768.0f); if(a>pk)pk=a;} break;
   case K_S32: {float sc=(float)ldexp(1.0,-(int)s->frac); for(UInt32 i=0;i<cnt;i++){float a=fabsf((float)((int32_t*)p)[i])*sc; if(a>pk)pk=a;}} break;
  }}
 return pk;
}
static void scale_buf(Slot *s,UInt32 n,AudioBufferList *d,float g){
 const AudioStreamBasicDescription *f=&s->fmt; int ni=(f->mFormatFlags&kAudioFormatFlagIsNonInterleaved)!=0;
 UInt32 ch=f->mChannelsPerFrame;
 for(UInt32 b=0;b<d->mNumberBuffers;b++){ UInt32 cnt=n*(ni?1:ch); void *p=d->mBuffers[b].mData; if(!p)continue;
  switch(s->kind){
   case K_F32: for(UInt32 i=0;i<cnt;i++)((float*)p)[i]*=g; break;
   case K_S16: for(UInt32 i=0;i<cnt;i++)((int16_t*)p)[i]=(int16_t)lrintf((float)((int16_t*)p)[i]*g); break;
   case K_S32: for(UInt32 i=0;i<cnt;i++)((int32_t*)p)[i]=(int32_t)llrint((double)((int32_t*)p)[i]*g); break;
  }}
}
static void process(Slot *s,UInt32 n,AudioBufferList *d){
 configure(s);
 int eq=atomic_load_explicit(&g_enabled,memory_order_relaxed)&&s->eng.active;
 uint32_t pf=atomic_load_explicit(&g_plg_flags,memory_order_relaxed);
 const AudioStreamBasicDescription *f=&s->fmt;
 int ni=(f->mFormatFlags&kAudioFormatFlagIsNonInterleaved)!=0;
 UInt32 ch=f->mChannelsPerFrame, bps=f->mBitsPerChannel/8;
 int plg=pf&&g_ps&&s->kind==K_F32&&ch==2;
 int prot=atomic_load_explicit(&g_prot_on,memory_order_relaxed);
 if(!eq&&!plg&&!prot){skip(AEQ_SKIP_INACTIVE);return;}
 float pg=powf(10.0f,-(float)atomic_load_explicit(&g_prot_cdb,memory_order_relaxed)/2000.0f);
 if(ni?(d->mNumberBuffers!=ch):(d->mNumberBuffers!=1)){skip(AEQ_SKIP_SHAPE);return;}
 for(UInt32 b=0;b<d->mNumberBuffers;b++){
  AudioBuffer *ab=&d->mBuffers[b];
  UInt32 bc=ni?1:ch;
  if(!ab->mData||(ab->mNumberChannels&&ab->mNumberChannels!=bc)||ab->mDataByteSize<(UInt64)n*bc*bps){skip(AEQ_SKIP_SHAPE);return;}
 }
 if(eq){s->eng.xgain=pg; s->eng.peak=0;}
 else if(pg<0.9999f)scale_buf(s,n,d,pg);   /* protection without EQ: lower the level directly */
 if(eq)for(UInt32 b=0;b<d->mNumberBuffers;b++){
  AudioBuffer *ab=&d->mBuffers[b]; UInt32 bc=ni?1:ch;
  for(UInt32 c=0;c<bc;c++){
   unsigned chan=ni?b:c;
   switch(s->kind){
    case K_F32: eq_process_float(&s->eng,((float*)ab->mData)+c,n,bc,chan); break;
    case K_S16: eq_process_s16(&s->eng,((int16_t*)ab->mData)+c,n,bc,chan); break;
    case K_S32: eq_process_s32(&s->eng,((int32_t*)ab->mData)+c,n,bc,chan,s->frac); break;
   }
  }
 }
 if(eq)peak_note(s->eng.peak);           /* level before the EQ's soft limiter */
 if(plg){
  int idx=(int)(s-g_slots); if(idx!=g_plg_slot){aeq_plg_reset(g_ps);g_plg_slot=idx;}
  float rate=(float)f->mSampleRate;
  AEQIR *ir=(uint32_t)rate==44100?atomic_load(&g_ir44):((uint32_t)rate==48000?atomic_load(&g_ir48):NULL);
  const AEQPluginParams *pp=&g_pp[atomic_load_explicit(&g_pp_idx,memory_order_acquire)];
  float *L=(float*)d->mBuffers[0].mData, *R=ni?(float*)d->mBuffers[1].mData:L+1;
  aeq_plg_process(g_ps,pp,ir,rate,L,R,ni?1:2,n);
 }
 peak_note(buf_peak(s,n,d));                /* what actually goes to the codec */
 atomic_fetch_add_explicit(&g_frames,n,memory_order_relaxed);
}

/* ---------------- hooks ---------------- */
static OSStatus h_render(AudioUnit u,AudioUnitRenderActionFlags *fl,const AudioTimeStamp *ts,UInt32 bus,UInt32 n,AudioBufferList *d){
 OSStatus r=o_render(u,fl,ts,bus,n,d);
 atomic_fetch_add_explicit(&g_render_calls,1,memory_order_relaxed);
 if(r!=noErr){skip(AEQ_SKIP_RENDER_ERR);return r;}
 if(!u||!d||!d->mNumberBuffers||!n||n>16384){skip(AEQ_SKIP_EMPTY);return r;}
 Slot *s=slot_for(u,bus);
 if(!s){skip(AEQ_SKIP_TABLE_FULL);return r;}
 atomic_fetch_add_explicit(&s->calls,1,memory_order_relaxed);
 if(atomic_load_explicit(&s->fmt_state,memory_order_relaxed)==0)query_format(s);
 if(!g_processing_process){skip(AEQ_SKIP_NOT_TARGET);return r;}
 int target=atomic_load_explicit(&g_target,memory_order_relaxed);
 if(target==AEQ_TARGET_OBSERVE){skip(AEQ_SKIP_NOT_TARGET);return r;}
 int tn=atomic_load_explicit(&g_tone,memory_order_relaxed);
 if(!tn&&!atomic_load_explicit(&g_enabled,memory_order_relaxed)&&!atomic_load_explicit(&g_plg_flags,memory_order_relaxed)&&
    !atomic_load_explicit(&g_prot_on,memory_order_relaxed)){skip(AEQ_SKIP_DISABLED);return r;}
 if(atomic_load_explicit(&s->fmt_state,memory_order_relaxed)!=1){skip(AEQ_SKIP_FORMAT);return r;}
 if(!is_target(s,(int)(s-g_slots),target)){skip(AEQ_SKIP_NOT_TARGET);return r;}
 if(tn)tone(s,n,d); else process(s,n,d);
 return r;
}
static OSStatus h_init(AudioUnit u){
 OSStatus r=o_init(u);
 for(int i=0;i<NSLOTS;i++){Slot *s=&g_slots[i];
  if(atomic_load(&s->state)==2&&s->unit==u)atomic_store(&s->fmt_state,0);} /* format may have changed */
 return r;
}
static OSStatus h_dispose(AudioComponentInstance u){
 for(int i=0;i<NSLOTS;i++){Slot *s=&g_slots[i];
  if(atomic_load(&s->state)==2&&s->unit==(AudioUnit)u){
   int expect=i; atomic_compare_exchange_strong(&g_lock,&expect,-1);
   atomic_store(&s->state,0);
  }}
 return o_dispose(u);
}
static OSStatus h_convfill(AudioConverterRef c,AudioConverterComplexInputDataProc p,void *ud,UInt32 *np,AudioBufferList *o,AudioStreamPacketDescription *pd){
 atomic_fetch_add_explicit(&g_conv_calls,1,memory_order_relaxed);
 return o_convfill(c,p,ud,np,o,pd);
}
static OSStatus h_auproc(AudioUnit u,AudioUnitRenderActionFlags *fl,const AudioTimeStamp *ts,UInt32 n,AudioBufferList *d){
 atomic_fetch_add_explicit(&g_proc_calls,1,memory_order_relaxed);
 return o_auproc(u,fl,ts,n,d);
}

/* ---------------- settings ---------------- */
static int t_tone=-1;
static int t_hdr=-1,t_a=-1,t_b=-1,t_p[4]={-1,-1,-1,-1};
/* Convolution IR: raw stereo float32 files prepared by Settings (one per sample rate). */
static AEQIR *load_ir(uint32_t rate,int *err){
 char path[160]; snprintf(path,sizeof path,AEQ_IR_FILE_FMT,rate);
 FILE *f=fopen(path,"rb"); if(!f){*err=(errno==ENOENT)?1:2;return NULL;}
 uint32_t h[4]={0}; AEQIR *ir=NULL;
 if(fread(h,4,4,f)==4&&h[0]==0x49514541u&&h[1]==rate&&h[3]==2&&h[2]>0){
  uint32_t fr=h[2]>AEQ_IR_MAX_FRAMES?AEQ_IR_MAX_FRAMES:h[2];
  float *buf=malloc((size_t)fr*2*sizeof(float));
  if(buf&&fread(buf,sizeof(float)*2,fr,f)==fr)ir=aeq_ir_create(buf,fr,rate);
  free(buf);
 }
 fclose(f); if(!ir&&!*err)*err=3; return ir;
}
static void reload_ir(void){
 int e44=0,e48=0; AEQIR *n44=load_ir(44100,&e44), *n48=load_ir(48000,&e48);
 AEQIR *o44=atomic_exchange(&g_ir44,n44), *o48=atomic_exchange(&g_ir48,n48);
 atomic_store(&g_ir_err,(n44||n48)?0:(e48?e48:e44));
 syslog(LOG_NOTICE,"AmpEQ[%s]: convolution IR 44.1k %s, 48k %s",g_pfx,n44?"loaded":"missing",n48?"loaded":"missing");
 /* free the old ones once no render cycle can still be using them */
 if(o44||o48)dispatch_after(dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC),dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{aeq_ir_free(o44);aeq_ir_free(o48);});
}
static void pull_settings(void){
 uint64_t h=0,a=0,b=0;
 if(t_hdr<0||notify_get_state(t_hdr,&h)!=NOTIFY_STATUS_OK)return;
 notify_get_state(t_a,&a); notify_get_state(t_b,&b);
 AEQSettings s;
 if(!aeq_unpack(h,a,b,&s))return;
 for(int i=0;i<EQ_BANDS;i++)atomic_store(&g_gain_c[i],(int)lrintf(s.gains[i]*100));
 float total=eq_auto_gain_db(s.gains,s.preamp,48000.0f);
 atomic_store(&g_total_c,(int)lrintf(total*100));
 if(atomic_load(&g_target)!=s.target)atomic_store(&g_lock,-1);
 atomic_store(&g_target,s.target);
 atomic_store(&g_enabled,s.enabled);
 { uint64_t tl=0; if(t_tone>=0)notify_get_state(t_tone,&tl); if(tl>12000)tl=12000;
   atomic_store(&g_tone_amp,tl?powf(10.0f,-(float)tl/2000.0f):AEQ_TONE_LEVEL); }
 atomic_store(&g_tone,s.tone);
 atomic_store(&g_prot_on,s.clipProtect);
 if(atomic_exchange(&g_prot_reset,s.protReset)!=s.protReset||!s.clipProtect)atomic_store(&g_prot_cdb,0);
 { uint64_t w[4]={0,0,0,0}; for(int i=0;i<4;i++)if(t_p[i]>=0)notify_get_state(t_p[i],&w[i]);
   int ni=1-atomic_load(&g_pp_idx); aeq_plg_unpack(w,&g_pp[ni]); atomic_store_explicit(&g_pp_idx,ni,memory_order_release);
   uint32_t fl=g_processing_process?g_pp[ni].flags:0;
   if((fl&PLG_CONV)&&g_pp[ni].conv_serial!=g_conv_loaded){g_conv_loaded=g_pp[ni].conv_serial;reload_ir();}
   atomic_store(&g_plg_flags,fl&(PLG_DRX|PLG_GXFEED|PLG_XFEED|PLG_FIELD|PLG_PITCH|PLG_CONV|PLG_BALANCE)?fl:0); }
 atomic_store(&g_serial,s.serial);
 atomic_store(&g_have_settings,1);
 atomic_fetch_or(&g_flags,AEQ_F_SETTINGS);
 atomic_fetch_add(&g_gen,1);
 if(g_processing_process){AmpEQ_AmpObserverSetEnabled(s.ampObserve);AmpEQ_AmpBoostSet(s.ampBoost);}
}
/* ---------------- clip meter (10 per second, mediaserverd only) ---------------- */
static int t_route=-1, t_meter=-1;
static dispatch_source_t g_mtimer;
static uint64_t g_m_frames=0; static unsigned g_m_seq=0;
static void read_route(void){uint64_t r=0; if(t_route>=0&&notify_get_state(t_route,&r)==NOTIFY_STATUS_OK)AmpEQ_AmpRouteSet((int)(r&3));}
/* level where the output starts to clip, in dBFS: 0 dBFS digital, or lower when Gain pushes the
 * codec past +6.5 dB (measured: codec_dB + peak_dBFS > +6.5 -> analog clipping) */
static float clip_point_db(int *route){
 int rt=AmpEQ_AmpRouteGet(); if(route)*route=rt;
 uint32_t cv=AmpEQ_AmpCodecNow();
 if(rt==2||cv<100)return 0.0f;
 float codec=-162.0f+0.5f*(float)cv;
 return fminf(0.0f,AEQ_ANALOG_CLIP_CODEC_DB-codec);
}
static void meter_tick(void){
 uint32_t bits=atomic_exchange(&g_peak_bits,0); float pk; memcpy(&pk,&bits,4);
 uint64_t fr=atomic_load(&g_frames); int running=fr!=g_m_frames; g_m_frames=fr;
 int route=0; float ceil_db=clip_point_db(&route);
 float rel=pk>1e-6f?20.0f*log10f(pk)-ceil_db:-100.0f;
 int clipped=running&&rel>0.0f;
 int on=atomic_load(&g_prot_on);
 /* Peace-style "prevent clipping": while it clips, lower the level 0.5 dB at a time (max 12 dB) */
 if(on&&clipped){int c=atomic_load(&g_prot_cdb); if(c<1200)atomic_store(&g_prot_cdb,c+50);}
 if(t_meter>=0)notify_set_state(t_meter,aeq_pack_meter(rel,clipped,route,atomic_load(&g_prot_cdb),ceil_db,on,running,++g_m_seq));
}
static void reconnect(void){
 atomic_store(&g_lock,-1);
 atomic_store(&g_any_mixer_eligible,0);
 for(int i=0;i<NSLOTS;i++)if(atomic_load(&g_slots[i].state)==2)atomic_store(&g_slots[i].fmt_state,0);
 atomic_fetch_add(&g_reset_gen,1);
 pull_settings();
 syslog(LOG_NOTICE,"AmpEQ[%s]: reconnect: caches cleared, lock released, settings serial %u",g_pfx,atomic_load(&g_serial));
}

/* ---------------- status publishing ---------------- */
typedef struct {int heart,render,frames,conv,proc,applied,fmt,skip,plg,uk[AEQ_UNIT_SLOTS_PUBLISHED],uv[AEQ_UNIT_SLOTS_PUBLISHED];} Tokens;
static Tokens T;
static int reg(const char *name){int t=-1; char full[160]; snprintf(full,sizeof full,AEQ_NS "st.%s.%s",g_pfx,name);
 return notify_register_check(full,&t)==NOTIFY_STATUS_OK?t:-1;}
static void set(int tok,uint64_t v){if(tok>=0)notify_set_state(tok,v);}
static uint64_t slot_v(Slot *s,bool tgt){
 int ok=atomic_load(&s->fmt_state)==1;
 const AudioStreamBasicDescription *f=&s->fmt;
 return aeq_pack_unitv(atomic_load(&s->calls),s->bus,f->mChannelsPerFrame,(f->mFormatFlags&kAudioFormatFlagIsFloat)!=0,
   (f->mFormatFlags&kAudioFormatFlagIsNonInterleaved)!=0,ok,tgt,f->mSampleRate);
}
static void tick(void){
 g_secs++;
 set(T.heart,aeq_pack_heart((uint32_t)getpid(),atomic_load(&g_flags),g_secs));
 set(T.render,atomic_load(&g_render_calls));
 set(T.frames,atomic_load(&g_frames));
 set(T.conv,atomic_load(&g_conv_calls));
 set(T.proc,atomic_load(&g_proc_calls));
 uint64_t applied=(uint64_t)atomic_load(&g_serial)|((uint64_t)(atomic_load(&g_enabled)?1:0)<<32)|
   ((uint64_t)(atomic_load(&g_target)&0xF)<<33)|((uint64_t)(uint8_t)aeq_q((float)atomic_load(&g_total_c)/100.0f)<<40)|
   ((uint64_t)(atomic_load(&g_have_settings)?1:0)<<48);
 set(T.applied,applied);
 set(T.plg,(uint64_t)(atomic_load(&g_plg_flags)&0xFFFF)|((uint64_t)(atomic_load(&g_ir44)?1:0)<<16)|((uint64_t)(atomic_load(&g_ir48)?1:0)<<17)|((uint64_t)(atomic_load(&g_ir_err)&0xF)<<20));
 uint64_t sk=0;
 for(int i=0;i<AEQ_SKIP_COUNT;i++){uint32_t now=atomic_load(&g_skip[i]),d=now-g_skip_prev[i];g_skip_prev[i]=now;
  sk|=((uint64_t)(d>255?255:d))<<(8*i);}
 set(T.skip,sk);
 /* AUTO lock maintenance: release a lock on a unit that has gone silent */
 int lock=atomic_load(&g_lock);
 if(lock>=0){Slot *s=&g_slots[lock];
  uint64_t c=atomic_load(&s->calls);
  if(atomic_load(&s->state)!=2)atomic_store(&g_lock,-1);
  else if(c==s->seen_calls){
   for(int i=0;i<NSLOTS;i++){Slot *o=&g_slots[i];
    if(i!=lock&&atomic_load(&o->state)==2&&eligible(o)&&atomic_load(&o->calls)!=o->seen_calls){
     int e=lock; atomic_compare_exchange_strong(&g_lock,&e,-1);
     syslog(LOG_NOTICE,"AmpEQ[%s]: auto-relock: slot %d silent, slot %d active",g_pfx,lock,i);break;}}
  }
 }
 lock=atomic_load(&g_lock);
 uint64_t fmt=0; int out=0;
 for(int i=0;i<NSLOTS&&out<AEQ_UNIT_SLOTS_PUBLISHED;i++){Slot *s=&g_slots[i];
  uint64_t calls=atomic_load(&s->calls);
  if(atomic_load(&s->state)!=2){continue;}
  bool tgt=(i==lock)||(atomic_load(&g_target)==AEQ_TARGET_MIXERS&&s->type==kAudioUnitType_Mixer&&eligible(s))||
           (atomic_load(&g_target)==AEQ_TARGET_OUTPUT&&s->type==kAudioUnitType_Output&&eligible(s));
  if(tgt&&!fmt)fmt=slot_v(s,true);
  if(calls){set(T.uk[out],((uint64_t)s->type<<32)|s->subtype);set(T.uv[out],slot_v(s,tgt));out++;}
 }
 for(int i=0;i<NSLOTS;i++)g_slots[i].seen_calls=atomic_load(&g_slots[i].calls);
 for(;out<AEQ_UNIT_SLOTS_PUBLISHED;out++){set(T.uk[out],0);set(T.uv[out],0);}
 set(T.fmt,fmt);
 if(g_processing_process)AmpEQ_AmpObserverPublish();
}

static void *hook_sym(void *lib,const char *name,void *repl,void **orig,uint32_t flag){
 void *p=dlsym(lib,name); if(!p)p=dlsym(RTLD_DEFAULT,name);
 if(!p){syslog(LOG_NOTICE,"AmpEQ[%s]: symbol %s NOT FOUND",g_pfx,name);return NULL;}
 *orig=NULL; MSHookFunction(p,repl,orig);
 if(*orig){atomic_fetch_or(&g_flags,flag);syslog(LOG_NOTICE,"AmpEQ[%s]: hooked %s",g_pfx,name);}
 else syslog(LOG_NOTICE,"AmpEQ[%s]: hook FAILED for %s",g_pfx,name);
 return *orig;
}

__attribute__((constructor)) static void ampeq_init(void){
 const char *p=getprogname(); if(!p)return;
 if(!strcmp(p,"SpringBoard")){AmpEQ_RelayInstall();return;}
 bool ms=!strcmp(p,"mediaserverd"), mu=!strcmp(p,"Music");
 if(!ms&&!mu)return;
 g_processing_process=ms; g_pfx=ms?"ms":"mu";
 openlog("AmpEQ",LOG_PID,LOG_DAEMON);

 /* register status tokens FIRST so even a partial failure is visible */
 T.heart=reg("heart");T.render=reg("render");T.frames=reg("frames");T.conv=reg("conv");T.proc=reg("proc");
 T.applied=reg("applied");T.fmt=reg("fmt");T.skip=reg("skip");T.plg=reg("plg");
 for(int i=0;i<AEQ_UNIT_SLOTS_PUBLISHED;i++){char k[8];snprintf(k,sizeof k,"u%dk",i);T.uk[i]=reg(k);snprintf(k,sizeof k,"u%dv",i);T.uv[i]=reg(k);}
 atomic_fetch_or(&g_flags,AEQ_F_SUBSTRATE); /* MSHookFunction is linked; load would have failed otherwise */
 set(T.heart,aeq_pack_heart((uint32_t)getpid(),atomic_load(&g_flags),0));

 notify_register_check(AEQ_SET_TONE,&t_tone);
 notify_register_check(AEQ_SET_HDR,&t_hdr); notify_register_check(AEQ_SET_A,&t_a); notify_register_check(AEQ_SET_B,&t_b);
 for(int i=0;i<4;i++){char k[64];snprintf(k,sizeof k,AEQ_SET_P "%d",i);notify_register_check(k,&t_p[i]);}
 if(ms)g_ps=aeq_plg_state_create();
 { /* master switch OFF -> install nothing at all (fully unloaded behaviour after a restart) */
  uint64_t h=0; if(t_hdr>=0&&notify_get_state(t_hdr,&h)==NOTIFY_STATUS_OK&&(h&AEQ_HDR_VALID)&&(h&16ULL)){
   set(T.heart,0); syslog(LOG_NOTICE,"AmpEQ[%s]: turned OFF in Settings - no hooks installed",g_pfx); return; }
 }
 pull_settings();
 dispatch_queue_t q=dispatch_queue_create("com.ampeq.ctl",DISPATCH_QUEUE_SERIAL);
 int tok;
 notify_register_dispatch(AEQ_NOTE_CHANGED,&tok,q,^(__unused int t){pull_settings();});
 notify_register_dispatch(AEQ_NOTE_RECONNECT,&tok,q,^(__unused int t){reconnect();});
 if(ms){ notify_register_check(AEQ_ROUTE,&t_route); notify_register_check(AEQ_METER,&t_meter);
   notify_register_dispatch(AEQ_NOTE_ROUTE,&tok,q,^(__unused int t){read_route();}); }

 void *at=dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox",RTLD_NOW|RTLD_GLOBAL);
 hook_sym(at,"AudioUnitRender",(void*)h_render,(void**)&o_render,AEQ_F_RENDER_HOOK);
 hook_sym(at,"AudioUnitInitialize",(void*)h_init,(void**)&o_init,AEQ_F_INIT_HOOK);
 hook_sym(at,"AudioComponentInstanceDispose",(void*)h_dispose,(void**)&o_dispose,AEQ_F_DISPOSE_HOOK);
 hook_sym(at,"AudioConverterFillComplexBuffer",(void*)h_convfill,(void**)&o_convfill,AEQ_F_CONV_HOOK);
 hook_sym(at,"AudioUnitProcess",(void*)h_auproc,(void**)&o_auproc,AEQ_F_PROC_HOOK);
 if(ms&&AmpEQ_AmpObserverInstall((void*)MSHookFunction))atomic_fetch_or(&g_flags,AEQ_F_IOKIT_HOOK);

 dispatch_source_t timer=g_timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,q);
 dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,0),NSEC_PER_SEC,NSEC_PER_SEC/10);
 dispatch_source_set_event_handler(timer,^{tick();});
 dispatch_resume(timer);
 if(ms){ read_route();
  g_mtimer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,q);
  dispatch_source_set_timer(g_mtimer,dispatch_time(DISPATCH_TIME_NOW,0),NSEC_PER_SEC/10,NSEC_PER_SEC/50);
  dispatch_source_set_event_handler(g_mtimer,^{meter_tick();});
  dispatch_resume(g_mtimer); }

 /* ask SpringBoard (or Settings) to publish saved settings now */
 notify_post(AEQ_NOTE_HELLO);
 syslog(LOG_NOTICE,"AmpEQ[%s] v%d.%d loaded pid %d flags 0x%x",g_pfx,AEQ_VERSION_MAJOR,AEQ_VERSION_MINOR,getpid(),atomic_load(&g_flags));
}
