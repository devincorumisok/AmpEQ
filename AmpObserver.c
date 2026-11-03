/* AmpEQ v1.6 headphone boost + IOKit observer (mediaserverd only).
 *
 * EVIDENCE (user's iPod9,1 / iOS 15.8.6 registry, 2026-11-01):
 *   AppleEmbeddedAudioDevice "Codec" (device name CS42L81), control ID 306,
 *   class 'vlme', scope 'outp', element 1. Range map: start -162 dB, 0.5 dB/step,
 *   372 steps (-162 .. +24 dB). iOS sends 264 (-30 dB, + mute 307) at minimum
 *   volume and 315 (-4.5 dB) at maximum. The speaker device's max is exactly 0 dB.
 *   mediaserverd is the codec's exclusive access owner, so it issues these calls.
 *
 * BOOST (1.10): at EVERY volume step, when iOS sends control 306 = v (264..315),
 * send v+N instead (N = boost steps of 0.5 dB). Volume steps keep their normal
 * spacing, just shifted up by the boost; the loudest value is still 315+N, the level
 * the user measured as clean. Values are capped at 372 (top of the driver's range).
 *   315..324 (-4.5 .. 0.0 dB) is the clean region below codec full scale; applied instantly.
 *   325..372 (+0.5 .. +24 dB) is above full scale (most likely digital gain inside the
 *   codec: louder, but loud passages clip). Reached by a RAMP of 0.5 dB every 30 ms from
 *   0 dB, never a jump, and aborted the moment iOS sends any other volume.
 * Safety gates:
 *   - the call must carry 306 immediately followed by an in-range value (scalar),
 *     or 306 followed by an in-range uint32 within 4 words (struct);
 *   - that exact layout (API, selector, size, value position) must first be seen
 *     carrying at least TWO different values (i.e. the user moved the volume) - this
 *     proves the slot is the live volume before anything is modified;
 *   - only the value 315 is ever rewritten; everything else passes unchanged.
 * Re-apply: on slider change, the learned call is replayed on the same connection
 * with 315+N (or 315 to restore stock), only if iOS's last value was 315.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <dlfcn.h>
#include <notify.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef AMPEQ_TEST
#include <sys/sysctl.h>
#endif
#include <syslog.h>
#include <unistd.h>
#include <dispatch/dispatch.h>
#include "AmpEQShared.h"

#define CTL_HP_VOL   306u
#define HP_STOCK_MAX 315u
#define HP_ZERO_DB   324u   /* -162 + 324*0.5 = 0.0 dB */
#define HP_RANGE_MAX 372u

typedef mach_port_t io_object_t_;
typedef kern_return_t (*callmethod_t)(mach_port_t,uint32_t,const uint64_t*,uint32_t,const void*,size_t,uint64_t*,uint32_t*,void*,size_t*);
typedef kern_return_t (*callscalar_t)(mach_port_t,uint32_t,const uint64_t*,uint32_t,uint64_t*,uint32_t*);
typedef kern_return_t (*callasync_t)(mach_port_t,uint32_t,mach_port_t,uint64_t*,uint32_t,const uint64_t*,uint32_t,const void*,size_t,uint64_t*,uint32_t*,void*,size_t*);
typedef kern_return_t (*setprop_t)(mach_port_t,CFStringRef,CFTypeRef);
typedef kern_return_t (*setprops_t)(mach_port_t,CFTypeRef);
typedef kern_return_t (*getservice_t)(mach_port_t,io_object_t_*);
typedef kern_return_t (*getclass_t)(io_object_t_,char[128]);
typedef kern_return_t (*release_t)(io_object_t_);
typedef void (*mshook_t)(void*,void*,void**);

static callmethod_t o_method; static callscalar_t o_scalar; static callasync_t o_async;
static setprop_t o_setprop; static setprops_t o_setprops;
static getservice_t f_getservice; static getclass_t f_getclass; static release_t f_release;

static _Atomic int g_on=0;           /* observer */
static _Atomic int g_boost=0;        /* steps 0..9 */
static _Atomic uint64_t g_count=0;
static os_unfair_lock g_lk=OS_UNFAIR_LOCK_INIT;
static __thread int g_inside=0;

/* ---------- observer: inventory of distinct calls ---------- */
typedef struct {mach_port_t port; int cls;} Conn;
static Conn g_conns[32]; static int g_nconns=0;
static char g_cls[AEQ_AMP_CLASS_SLOTS][128]; static int g_ncls=0;
typedef struct {uint32_t sel,n,cls; uint64_t in0,in1,count;} Inv;
static Inv g_inv[AEQ_AMP_OBS_SLOTS]; static int g_ninv=0;
static int t_count=-1,t_obs[AEQ_AMP_OBS_SLOTS],t_cls[AEQ_AMP_CLASS_SLOTS],t_b0=-1,t_b1=-1;

static int interesting(const char *c){
 return strstr(c,"CS4")||strstr(c,"Audio")||strstr(c,"Codec")||strstr(c,"Amp")||strstr(c,"SPI")||strstr(c,"I2C")||strstr(c,"DSP")||strstr(c,"Speaker")||strstr(c,"Headphone");
}
static int class_slot(const char *name){
 for(int i=0;i<g_ncls;i++)if(!strcmp(g_cls[i],name))return i;
 if(g_ncls<AEQ_AMP_CLASS_SLOTS){strlcpy(g_cls[g_ncls],name,128);return g_ncls++;}
 return 14;
}
static int classify(mach_port_t port){
 for(int i=0;i<g_nconns;i++)if(g_conns[i].port==port)return g_conns[i].cls;
 int cls=15; io_object_t_ svc=0; char name[128]={0};
 if(f_getservice&&f_getclass&&f_getservice(port,&svc)==KERN_SUCCESS&&svc){
  if(f_getclass(svc,name)==KERN_SUCCESS)cls=interesting(name)?class_slot(name):-1;
  if(f_release)f_release(svc);
 }
 if(g_nconns<32)g_conns[g_nconns++]=(Conn){port,cls};
 return cls;
}
static void inventory(mach_port_t port,uint32_t sel,const uint64_t *in,uint32_t n){
 if(g_inside||!os_unfair_lock_trylock(&g_lk))return;
 g_inside=1;
 int cls=classify(port);
 if(cls>=0){
  int i; for(i=0;i<g_ninv;i++)if(g_inv[i].sel==sel&&g_inv[i].n==n&&g_inv[i].cls==(uint32_t)cls)break;
  if(i==g_ninv&&g_ninv<AEQ_AMP_OBS_SLOTS){g_ninv++;g_inv[i]=(Inv){sel,n,(uint32_t)cls,0,0,0};
   syslog(LOG_NOTICE,"AmpEQ[amp]: new call type cls=%d sel=%u n=%u",cls,sel,n);}
  if(i<g_ninv){g_inv[i].in0=n>0&&in?in[0]:0;g_inv[i].in1=n>1&&in?in[1]:0;g_inv[i].count++;}
  atomic_fetch_add(&g_count,1);
 }
 g_inside=0; os_unfair_lock_unlock(&g_lk);
}

/* ---------- boost: learned volume-call template ---------- */
enum { K_NONE=0, K_SCALAR=1, K_STRUCT=2 };
typedef struct {
 int kind; int api;              /* api: 1 CallMethod, 2 CallScalarMethod, 3 Async (observe only) */
 mach_port_t conn; uint32_t sel;
 uint32_t n; uint64_t in[16];
 size_t sn; uint8_t st[1024];
 uint32_t idx;                   /* scalar index, or struct uint32 word index of the value */
 uint32_t outn; size_t outsn;    /* output sizes the caller provided, for replay */
 uint32_t v_first; int learned;
 uint32_t last_ios, last_sent; kern_return_t last_kr; uint32_t matches;
} Tmpl;
static Tmpl T;                    /* guarded by g_lk */

static int scan_scalar(const uint64_t *in,uint32_t n,uint32_t *idx){
 if(!in||n<2||n>16)return 0;
 for(uint32_t i=0;i+1<n;i++)if(in[i]==CTL_HP_VOL&&in[i+1]>=100&&in[i+1]<=HP_RANGE_MAX){*idx=i+1;return 1;}
 return 0;
}
static int scan_struct(const void *si,size_t sn,uint32_t *idx){
 if(!si||sn<8||sn>sizeof T.st||((uintptr_t)si&3))return 0;
 const uint32_t *w=(const uint32_t*)si; size_t nw=sn/4;
 for(size_t j=0;j+1<nw;j++)if(w[j]==CTL_HP_VOL)
  for(size_t k=j+1;k<nw&&k<=j+4;k++)if(w[k]>=100&&w[k]<=HP_RANGE_MAX){*idx=(uint32_t)k;return 1;}
 return 0;
}
/* caller holds g_lk. Returns 1 if this call may be boosted. */
static int learn(int api,int kind,mach_port_t c,uint32_t sel,const uint64_t *in,uint32_t n,const void *si,size_t sn,uint32_t idx,uint32_t outn,size_t outsn){
 uint32_t v=kind==K_SCALAR?(uint32_t)in[idx]:((const uint32_t*)si)[idx];
 int same=T.kind==kind&&T.api==api&&T.sel==sel&&T.idx==idx&&(kind==K_SCALAR?T.n==n:T.sn==sn);
 if(!same&&T.learned)return 0;   /* a learned layout is never replaced by a look-alike call */
 if(!same){
  memset(&T,0,sizeof T); T.kind=kind; T.api=api; T.sel=sel; T.idx=idx; T.v_first=v;
  syslog(LOG_NOTICE,"AmpEQ[boost]: candidate volume call api=%d sel=%u kind=%d n=%u sn=%zu idx=%u value=%u",api,sel,kind,n,sn,idx,v);
 }else if(!T.learned&&v!=T.v_first){
  T.learned=1;
  syslog(LOG_NOTICE,"AmpEQ[boost]: volume call LEARNED (values %u and %u seen at the same slot)",T.v_first,v);
 }
 T.conn=c; T.n=n; if(kind==K_SCALAR)memcpy(T.in,in,n*sizeof(uint64_t));
 T.sn=sn; if(kind==K_STRUCT)memcpy(T.st,si,sn);
 T.outn=outn; T.outsn=outsn; T.last_ios=v; T.matches++;
 return T.learned&&api!=3&&v>=100&&v<=HP_STOCK_MAX;
}
static inline int in_range(uint32_t v){return v>=100&&v<=HP_STOCK_MAX;}
static _Atomic int g_route=0; /* 0 unknown, 1 wired headphones, 2 other (no boost) */
static uint32_t boost_steps(void){
 if(atomic_load(&g_route)==2)return 0;
 int b=atomic_load(&g_boost); if(b<0)b=0; if(b>(int)(HP_RANGE_MAX-HP_STOCK_MAX))b=(int)(HP_RANGE_MAX-HP_STOCK_MAX);
 return (uint32_t)b;
}
/* value to send for an iOS volume value v */
static uint32_t target_for(uint32_t v){ uint32_t t=v+boost_steps(); return t>HP_RANGE_MAX?HP_RANGE_MAX:t; }
/* Normal volume presses move ~1.5 dB, so they're sent directly. A big jump that would land
 * above 0 dB (e.g. headphones just plugged in at max) goes to 0 dB at once and ramps the rest. */
static inline uint32_t immediate_value(uint32_t tv,uint32_t prev){return (tv>HP_ZERO_DB&&tv>prev+4)?HP_ZERO_DB:tv;}
static void schedule_ramp(void);

static kern_return_t h_method(mach_port_t c,uint32_t sel,const uint64_t *in,uint32_t n,const void *si,size_t sn,uint64_t *out,uint32_t *on,void *so,size_t *son){
 if(!g_inside){
  uint32_t idx; int kind=scan_scalar(in,n,&idx)?K_SCALAR:(scan_struct(si,sn,&idx)?K_STRUCT:K_NONE);
  if(kind){
   os_unfair_lock_lock(&g_lk);
   uint32_t prev=T.last_sent;
   int may=learn(1,kind,c,sel,in,n,si,sn,idx,on?*on:0,son?*son:0);
   uint32_t v=T.last_ios, tv=target_for(v);
   os_unfair_lock_unlock(&g_lk);
   if(may&&tv!=v){
    kern_return_t r; uint32_t full=tv; tv=immediate_value(tv,prev);
    if(kind==K_SCALAR){uint64_t m[16];memcpy(m,in,n*sizeof *m);m[idx]=tv;r=o_method(c,sel,m,n,si,sn,out,on,so,son);}
    else{uint8_t m[sizeof T.st];memcpy(m,si,sn);((uint32_t*)(void*)m)[idx]=tv;r=o_method(c,sel,in,n,m,sn,out,on,so,son);}
    os_unfair_lock_lock(&g_lk); T.last_sent=tv; T.last_kr=r; os_unfair_lock_unlock(&g_lk);
    syslog(LOG_NOTICE,"AmpEQ[boost]: iOS sent 306=%u, sent %u (kr 0x%x)",v,tv,r);
    if(r==KERN_SUCCESS&&full>tv)schedule_ramp();
    return r;
   }
   kern_return_t r=o_method(c,sel,in,n,si,sn,out,on,so,son);
   os_unfair_lock_lock(&g_lk); T.last_sent=T.last_ios; T.last_kr=r; os_unfair_lock_unlock(&g_lk);
   return r;
  }
 }
 if(atomic_load_explicit(&g_on,memory_order_relaxed))inventory(c,sel,in,n);
 return o_method(c,sel,in,n,si,sn,out,on,so,son);
}
static kern_return_t h_scalar(mach_port_t c,uint32_t sel,const uint64_t *in,uint32_t n,uint64_t *out,uint32_t *on){
 if(!g_inside){
  uint32_t idx;
  if(scan_scalar(in,n,&idx)){
   os_unfair_lock_lock(&g_lk);
   uint32_t prev=T.last_sent;
   int may=learn(2,K_SCALAR,c,sel,in,n,NULL,0,idx,on?*on:0,0);
   uint32_t v=T.last_ios, tv=target_for(v);
   os_unfair_lock_unlock(&g_lk);
   uint64_t m[16]; memcpy(m,in,n*sizeof *m);
   if(may&&tv!=v)m[idx]=immediate_value(tv,prev);
   kern_return_t r=o_scalar(c,sel,m,n,out,on);
   os_unfair_lock_lock(&g_lk); T.last_sent=(uint32_t)m[idx]; T.last_kr=r; os_unfair_lock_unlock(&g_lk);
   if(m[idx]!=in[idx])syslog(LOG_NOTICE,"AmpEQ[boost]: iOS sent 306=%u, sent %u (kr 0x%x)",v,(uint32_t)m[idx],r);
   if(may&&r==KERN_SUCCESS&&tv>m[idx])schedule_ramp();
   return r;
  }
 }
 if(atomic_load_explicit(&g_on,memory_order_relaxed))inventory(c,sel,in,n);
 return o_scalar(c,sel,in,n,out,on);
}
static kern_return_t h_async(mach_port_t c,uint32_t sel,mach_port_t wp,uint64_t *ref,uint32_t rc,const uint64_t *in,uint32_t n,const void *si,size_t sn,uint64_t *out,uint32_t *on,void *so,size_t *son){
 if(!g_inside){
  uint32_t idx; int kind=scan_scalar(in,n,&idx)?K_SCALAR:(scan_struct(si,sn,&idx)?K_STRUCT:K_NONE);
  if(kind){os_unfair_lock_lock(&g_lk);learn(3,kind,c,sel,in,n,si,sn,idx,0,0);os_unfair_lock_unlock(&g_lk);} /* recorded, never modified */
 }
 if(atomic_load_explicit(&g_on,memory_order_relaxed))inventory(c,sel|0x80,in,n);
 return o_async(c,sel,wp,ref,rc,in,n,si,sn,out,on,so,son);
}
static void record_prop(mach_port_t entry,CFStringRef key){
 if(g_inside||!os_unfair_lock_trylock(&g_lk))return;
 g_inside=1; char cls[128]={0},k[64]="(dict)";
 if(key)CFStringGetCString(key,k,sizeof k,kCFStringEncodingUTF8);
 if(f_getclass&&f_getclass(entry,cls)==KERN_SUCCESS&&interesting(cls))
  syslog(LOG_NOTICE,"AmpEQ[amp]: SetCFProperty on %s key %s",cls,k);
 g_inside=0; os_unfair_lock_unlock(&g_lk);
}
static kern_return_t h_setprop(mach_port_t e,CFStringRef k,CFTypeRef v){
 if(atomic_load_explicit(&g_on,memory_order_relaxed))record_prop(e,k);
 return o_setprop(e,k,v);
}
static kern_return_t h_setprops(mach_port_t e,CFTypeRef v){
 if(atomic_load_explicit(&g_on,memory_order_relaxed))record_prop(e,NULL);
 return o_setprops(e,v);
}

/* Send the learned call again with value v (never below what iOS itself asked for). */
static kern_return_t replay(uint32_t v){
 Tmpl t; os_unfair_lock_lock(&g_lk); t=T; os_unfair_lock_unlock(&g_lk);
 if(!t.learned||t.api==3||!in_range(t.last_ios))return KERN_FAILURE;
 if(v>HP_RANGE_MAX)v=HP_RANGE_MAX; if(v<t.last_ios)v=t.last_ios;
 uint64_t outs[16]={0}; uint32_t outn=t.outn>16?16:t.outn; size_t outsn=t.outsn>4096?4096:t.outsn;
 uint8_t *so=outsn?calloc(1,outsn):NULL; kern_return_t r;
 g_inside=1;
 if(t.kind==K_SCALAR){t.in[t.idx]=v;
  r=t.api==2?o_scalar(t.conn,t.sel,t.in,t.n,outs,&outn):o_method(t.conn,t.sel,t.in,t.n,NULL,0,outs,&outn,so,so?&outsn:NULL);}
 else{((uint32_t*)(void*)t.st)[t.idx]=v; r=o_method(t.conn,t.sel,t.n?t.in:NULL,t.n,t.st,t.sn,outs,&outn,so,so?&outsn:NULL);}
 g_inside=0; free(so);
 os_unfair_lock_lock(&g_lk); if(r==KERN_SUCCESS)T.last_sent=v; T.last_kr=r; os_unfair_lock_unlock(&g_lk);
 return r;
}
static _Atomic unsigned g_ramp_gen=0;
/* Walk last_sent -> target. Upward: one 0.5 dB step per 30 ms. Downward: immediate.
 * Aborts if a newer ramp starts, iOS changes the volume, or the driver returns an error. */
static void ramp(unsigned gen){
 for(int guard=0;guard<200;guard++){
  if(atomic_load(&g_ramp_gen)!=gen)return;
  uint32_t tv,cur; int ok;
  os_unfair_lock_lock(&g_lk); cur=T.last_sent; ok=T.learned&&in_range(T.last_ios); tv=target_for(T.last_ios); os_unfair_lock_unlock(&g_lk);
  if(!ok||cur==tv)break;
  uint32_t next=cur<tv?(cur<HP_ZERO_DB?(tv<HP_ZERO_DB?tv:HP_ZERO_DB):cur+1):tv;
  kern_return_t r=replay(next);
  if(r!=KERN_SUCCESS){syslog(LOG_NOTICE,"AmpEQ[boost]: driver rejected 306=%u (kr 0x%x), ramp stopped",next,r);break;}
#ifndef AMPEQ_TEST
  if(next>HP_ZERO_DB&&next<tv)usleep(30000);
#endif
 }
 uint32_t fin; os_unfair_lock_lock(&g_lk); fin=T.last_sent; os_unfair_lock_unlock(&g_lk);
 syslog(LOG_NOTICE,"AmpEQ[boost]: 306 now %u (%+.1f dB)",fin,-162.0+fin*0.5);
}
#ifndef AMPEQ_TEST
static dispatch_queue_t g_rq;
#endif
static void schedule_ramp(void){
 unsigned gen=atomic_fetch_add(&g_ramp_gen,1)+1;
#ifdef AMPEQ_TEST
 ramp(gen);
#else
 if(!g_rq)g_rq=dispatch_queue_create("com.ampeq.boost",NULL);
 dispatch_async(g_rq,^{ramp(gen);});
#endif
}
static void reapply(void){schedule_ramp();}

/* Gain is only for the iPod touch 7: the values written belong to its audio chip's headphone
 * volume control; on any other device the same call could mean something else. */
static int is_ipod_touch_7(void){
#ifdef AMPEQ_TEST
 return 1;
#else
 char m[32]={0}; size_t n=sizeof m;
 return sysctlbyname("hw.machine",m,&n,NULL,0)==0&&!strcmp(m,"iPod9,1");
#endif
}
int AmpEQ_AmpObserverInstall(void *hookfn){
 mshook_t hook=(mshook_t)hookfn;
 if(!is_ipod_touch_7()){syslog(LOG_NOTICE,"AmpEQ[boost]: not an iPod touch 7 - Gain disabled");return 0;}
 void *io=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_GLOBAL);
 if(!io||!hook)return 0;
 f_getservice=(getservice_t)dlsym(io,"IOConnectGetService");
 f_getclass=(getclass_t)dlsym(io,"IOObjectGetClass");
 f_release=(release_t)dlsym(io,"IOObjectRelease");
 notify_register_check(AEQ_AMP_COUNT,&t_count);
 notify_register_check(AEQ_AMP_B0,&t_b0); notify_register_check(AEQ_AMP_B1,&t_b1);
 for(int i=0;i<AEQ_AMP_OBS_SLOTS;i++){char k[96];snprintf(k,sizeof k,AEQ_AMP_OBS "%d",i);t_obs[i]=-1;notify_register_check(k,&t_obs[i]);}
 for(int i=0;i<AEQ_AMP_CLASS_SLOTS;i++){char k[96];snprintf(k,sizeof k,AEQ_AMP_CLS "%d",i);t_cls[i]=-1;notify_register_check(k,&t_cls[i]);}
 void *m=dlsym(io,"IOConnectCallMethod"),*s=dlsym(io,"IOConnectCallScalarMethod"),*a=dlsym(io,"IOConnectCallAsyncMethod");
 void *sp=dlsym(io,"IORegistryEntrySetCFProperty"),*sps=dlsym(io,"IORegistryEntrySetCFProperties");
 if(m)hook(m,(void*)h_method,(void**)&o_method);
 if(s)hook(s,(void*)h_scalar,(void**)&o_scalar);
 if(a)hook(a,(void*)h_async,(void**)&o_async);
 if(sp)hook(sp,(void*)h_setprop,(void**)&o_setprop);
 if(sps)hook(sps,(void*)h_setprops,(void**)&o_setprops);
 return o_method&&o_scalar;
}
void AmpEQ_AmpObserverSetEnabled(int on){
 if(atomic_exchange(&g_on,on?1:0)!=(on?1:0))syslog(LOG_NOTICE,"AmpEQ[amp]: observer %s",on?"ON (read-only)":"off");
}
void AmpEQ_AmpRouteSet(int route){
 if(route<0||route>2)route=0;
 if(atomic_exchange(&g_route,route)!=route){syslog(LOG_NOTICE,"AmpEQ[boost]: route %s",route==1?"wired headphones":route==2?"not headphones - gain off":"unknown");reapply();}
}
int AmpEQ_AmpRouteGet(void){return atomic_load(&g_route);}
/* codec headphone-volume value right now (what AmpEQ last sent, or iOS's own value), 0 = unknown */
uint32_t AmpEQ_AmpCodecNow(void){
 uint32_t v; os_unfair_lock_lock(&g_lk); v=T.last_sent?T.last_sent:T.last_ios; os_unfair_lock_unlock(&g_lk); return v;
}
void AmpEQ_AmpBoostSet(int steps){
 if(steps<0)steps=0; if(steps>AEQ_BOOST_MAX_STEPS)steps=AEQ_BOOST_MAX_STEPS;
 if(atomic_exchange(&g_boost,steps)!=steps){syslog(LOG_NOTICE,"AmpEQ[boost]: boost set to %d steps (+%.1f dB over stock, codec %+.1f dB)",steps,steps*0.5,-4.5+steps*0.5);reapply();}
}
void AmpEQ_AmpObserverPublish(void){
 os_unfair_lock_lock(&g_lk);
 if(t_count>=0)notify_set_state(t_count,atomic_load(&g_count)|((uint64_t)atomic_load(&g_on)<<63));
 for(int i=0;i<AEQ_AMP_OBS_SLOTS;i++){uint64_t v=0;
  if(i<g_ninv)v=aeq_pack_obs(g_inv[i].sel,g_inv[i].n,g_inv[i].in0,g_inv[i].in1,g_inv[i].cls,(uint32_t)(g_inv[i].count>15?15:g_inv[i].count));
  if(t_obs[i]>=0)notify_set_state(t_obs[i],v);}
 for(int i=0;i<AEQ_AMP_CLASS_SLOTS;i++){
  const char *n=g_cls[i]; size_t L=strlen(n); const char *p=n; const char *cs=strstr(n,"CS42"); if(cs)p=cs; else if(L>8)p=n+(L-8);
  if(t_cls[i]>=0)notify_set_state(t_cls[i],aeq_pack_name8(i<g_ncls?p:""));}
 if(t_b0>=0)notify_set_state(t_b0,aeq_pack_boost0(T.kind,T.api,T.learned,T.sel,T.idx,T.kind==K_SCALAR?T.n:(uint32_t)T.sn,T.matches));
 if(t_b1>=0)notify_set_state(t_b1,aeq_pack_boost1(T.last_ios,T.last_sent,(uint32_t)atomic_load(&g_boost),(uint32_t)T.last_kr));
 os_unfair_lock_unlock(&g_lk);
}
#ifdef AMPEQ_TEST
void AmpEQ_AmpBoostResetForTest(void){memset(&T,0,sizeof T);}
#endif
