/* AmpEQ shared contract. Pure C: included by the audio-service hook,
 * the SpringBoard part and the Settings bundle.
 *
 * WHY THIS EXISTS (the v1.4.2 failure):
 *   v1.4.2 read its settings plist from inside mediaserverd. mediaserverd is
 *   sandboxed and cannot read /var/mobile/Library/Preferences/<3rd-party>.plist,
 *   so eqEnabled/ampSteps always loaded as 0 and nothing was ever processed.
 *   AmpEQ never touches files inside mediaserverd. Settings travel as Darwin
 *   notify *state* (uint64 values held by notifyd), written by Settings /
 *   SpringBoard and read by mediaserverd. Status travels back the
 *   same way.
 */
#ifndef AMPEQ_SHARED_H
#define AMPEQ_SHARED_H
#include <stdint.h>
#include <string.h>
#include <math.h>

#define AEQ_VERSION_MAJOR 1
#define AEQ_VERSION_MINOR 14
#define AEQ_BANDS 12
#define AEQ_PREF_PATH "/var/mobile/Library/Preferences/com.ampeq.ipod7.plist"

#define AEQ_NS "com.ampeq.ipod7.v15."
/* settings: written by Settings/SpringBoard, read by mediaserverd */
#define AEQ_SET_HDR   AEQ_NS "set.hdr"
#define AEQ_SET_A     AEQ_NS "set.a"     /* bands 0..7 */
#define AEQ_SET_B     AEQ_NS "set.b"     /* bands 8..11 + serial */
#define AEQ_SET_TONE  AEQ_NS "set.tone"  /* test tone level: centi-dB below full scale (0 = default -0.5 dBFS) */
#define AEQ_SET_P     AEQ_NS "set.p"
#define AEQ_ROUTE     AEQ_NS "route"     /* SpringBoard: 0 unknown, 1 wired headphones, 2 speaker / Bluetooth / other */
#define AEQ_NOTE_ROUTE AEQ_NS "note.route"
#define AEQ_METER     AEQ_NS "st.ms.meter" /* see aeq_pack_meter */     /* + digit 0..3: plugin parameter words (AEQPlugins.h) */
#define AEQ_NOTE_CHANGED   AEQ_NS "note.changed"
#define AEQ_NOTE_RECONNECT AEQ_NS "note.reconnect" /* reset caches, relock, re-pull */
#define AEQ_NOTE_HELLO     AEQ_NS "note.hello"     /* mediaserverd started; please publish */
/* status: written by the hooked process. <p> = "ms" (mediaserverd) or "mu" (Music) */
#define AEQ_ST(p, k) AEQ_NS "st." p "." k
/* amp observer (mediaserverd only, read-only) */
#define AEQ_AMP_COUNT AEQ_NS "amp.count"
#define AEQ_AMP_OBS   AEQ_NS "amp.o"   /* + digit 0..7 */
#define AEQ_AMP_CLS   AEQ_NS "amp.c"   /* + digit 0..3 */
#define AEQ_AMP_B0    AEQ_NS "amp.b0"  /* learned volume-call template */
#define AEQ_AMP_B1    AEQ_NS "amp.b1"  /* last iOS value / last sent / boost / kr */
#define AEQ_BOOST_MAX_STEPS 57         /* 315 (-4.5 dB stock max) + 57 = 372 = +24 dB (driver range top) */
#define AEQ_BOOST_CLEAN_STEPS 9        /* 315 + 9 = 324 = 0.0 dB codec full scale (default limit until measured) */
#define AEQ_BOOST_HW_MAX 23           /* gain list top: +11.5 dB (clips peaks above -0.5 dBFS) */
/* Measured on the user's iPod (1.13 sweep, 1 kHz, loopback): every step is 0.50 dB, THD 0.001-0.003 %
 * until  gain_over_stock + signal_peak_dBFS > +11.0 dB  (0.05 % at +11.1, 2 % at +12, 15 % at +15).
 * In codec terms: clipping when codec_dB + peak_dBFS > +6.5 dB (codec_dB = -162 + 0.5 * value). */
#define AEQ_ANALOG_CLIP_CODEC_DB 6.5f
#define AEQ_GAIN_LOW 0
#define AEQ_GAIN_MID 11                /* +5.5 dB */
#define AEQ_GAIN_HIGH 21               /* +10.5 dB: clean with 0.5 dB to spare for loud masters */
#define AEQ_GAIN_CLEAN_MAX 22          /* +11.0 dB: clean up to 0 dBFS sample peaks */
#define AEQ_PREAMP_MAX 12.0f
#define AEQ_TONE_LEVEL 0.944f          /* -0.5 dBFS */
#define AEQ_UNIT_SLOTS_PUBLISHED 8
#define AEQ_AMP_OBS_SLOTS 8
#define AEQ_AMP_CLASS_SLOTS 4

/* processing target */
enum { AEQ_TARGET_AUTO = 0, AEQ_TARGET_MIXERS = 1, AEQ_TARGET_OUTPUT = 2, AEQ_TARGET_OBSERVE = 3 };

/* ---- settings header ----
 * bit 0      valid
 * bit 1      EQ enabled
 * bit 2      amp observer enabled (read-only IOKit logging)
 * bit 3      measurement tone (replaces the mix with a -0.5 dBFS 1 kHz sine)
 * bit 4      AmpEQ master OFF (mediaserverd installs no hooks at load; live: everything off)
 * bit 5      clipping protection ON (Peace-style: level drops 0.5 dB at a time while clipping)
 * bits 28-31 protection reset counter (any change -> reduction back to 0)
 * bits 20-27 headphone boost steps (0.5 dB each, 0..57)
 * bits 8-15  user preamp, int8 quarter-dB
 * bits 16-19 target mode
 * bits 32-63 serial (must equal serial in set.b)                         */
#define AEQ_HDR_VALID 1ULL
static inline int8_t aeq_q(float db){
 float q=db*4.0f; if(!(q==q))q=0; if(q>127)q=127; if(q<-127)q=-127;
 return (int8_t)(q>=0?q+0.5f:q-0.5f);
}
static inline float aeq_dq(int8_t q){return (float)q*0.25f;}
static inline uint64_t aeq_pack_hdr_m(int enabled,int ampObserve,float preamp,int target,uint32_t serial,int boost,int tone,int masterOff){
 uint64_t v=AEQ_HDR_VALID;
 if(enabled)v|=2ULL; if(ampObserve)v|=4ULL; if(tone)v|=8ULL; if(masterOff)v|=16ULL;
 v|=((uint64_t)(uint8_t)aeq_q(preamp))<<8;
 v|=((uint64_t)(target&0xF))<<16;
 if(boost<0)boost=0; if(boost>AEQ_BOOST_MAX_STEPS)boost=AEQ_BOOST_MAX_STEPS;
 v|=((uint64_t)(boost&0xFF))<<20;
 v|=((uint64_t)serial)<<32;
 return v;
}
/* clipping-protection bits on top of a packed header */
static inline uint64_t aeq_hdr_protect(uint64_t v,int on,int resetCounter){
 v&=~((1ULL<<5)|(0xFULL<<28)); if(on)v|=1ULL<<5; v|=((uint64_t)(resetCounter&0xF))<<28; return v;
}
static inline uint64_t aeq_pack_hdr_t(int e,int o,float p,int t,uint32_t s,int b,int tone){return aeq_pack_hdr_m(e,o,p,t,s,b,tone,0);}
static inline uint64_t aeq_pack_hdr(int e,int o,float p,int t,uint32_t s,int b){return aeq_pack_hdr_m(e,o,p,t,s,b,0,0);}
static inline uint64_t aeq_pack_a(const float g[AEQ_BANDS]){
 uint64_t v=0; for(int i=0;i<8;i++)v|=((uint64_t)(uint8_t)aeq_q(g[i]))<<(8*i); return v;
}
static inline uint64_t aeq_pack_b(const float g[AEQ_BANDS],uint32_t serial){
 uint64_t v=0; for(int i=0;i<4;i++)v|=((uint64_t)(uint8_t)aeq_q(g[8+i]))<<(8*i);
 return v|((uint64_t)serial<<32);
}
typedef struct { int valid,enabled,ampObserve,target,ampBoost,tone,masterOff,clipProtect,protReset; float preamp; float gains[AEQ_BANDS]; uint32_t serial; uint64_t plg[4]; } AEQSettings;
/* Returns 1 only if header is valid and both halves carry the same serial. */
static inline int aeq_unpack(uint64_t hdr,uint64_t a,uint64_t b,AEQSettings *s){
 memset(s,0,sizeof(*s));
 if(!(hdr&AEQ_HDR_VALID))return 0;
 if((uint32_t)(hdr>>32)!=(uint32_t)(b>>32))return 0;
 s->valid=1; s->enabled=(hdr&2)!=0; s->ampObserve=(hdr&4)!=0; s->tone=(hdr&8)!=0; s->masterOff=(hdr&16)!=0;
 s->clipProtect=(hdr&32)!=0; s->protReset=(int)((hdr>>28)&0xF);
 s->preamp=aeq_dq((int8_t)((hdr>>8)&0xFF)); s->target=(int)((hdr>>16)&0xF);
 if(s->target>3)s->target=0;
 s->ampBoost=(int)((hdr>>20)&0xFF); if(s->ampBoost>AEQ_BOOST_MAX_STEPS)s->ampBoost=AEQ_BOOST_MAX_STEPS;
 for(int i=0;i<8;i++)s->gains[i]=aeq_dq((int8_t)((a>>(8*i))&0xFF));
 for(int i=0;i<4;i++)s->gains[8+i]=aeq_dq((int8_t)((b>>(8*i))&0xFF));
 s->serial=(uint32_t)(hdr>>32);
 return 1;
}

/* ---- heartbeat ----
 * bits 0-15  magic 0xA15E
 * bits 16-39 pid
 * bits 40-47 flags (AEQ_F_*)
 * bits 48-63 seconds alive (wraps) -> proves the process is running NOW   */
#define AEQ_MAGIC 0xA15EULL
enum {
 AEQ_F_SUBSTRATE   = 1<<0, /* MSHookFunction resolved */
 AEQ_F_RENDER_HOOK = 1<<1, /* AudioUnitRender hooked */
 AEQ_F_INIT_HOOK   = 1<<2, /* AudioUnitInitialize hooked */
 AEQ_F_DISPOSE_HOOK= 1<<3, /* AudioComponentInstanceDispose hooked */
 AEQ_F_CONV_HOOK   = 1<<4, /* AudioConverterFillComplexBuffer counted */
 AEQ_F_PROC_HOOK   = 1<<5, /* AudioUnitProcess counted */
 AEQ_F_SETTINGS    = 1<<6, /* a valid settings set has been received */
 AEQ_F_IOKIT_HOOK  = 1<<7, /* IOConnectCall* observer installed */
};
static inline uint64_t aeq_pack_heart(uint32_t pid,uint32_t flags,uint32_t secs){
 return AEQ_MAGIC|((uint64_t)(pid&0xFFFFFF)<<16)|((uint64_t)(flags&0xFF)<<40)|((uint64_t)(secs&0xFFFF)<<48);
}

/* ---- per-unit census ----
 * key k: componentType<<32 | componentSubType
 * key v: bits 0-39 render calls (saturating), 40-47 bus, 48-51 channels,
 *        52 float, 53 non-interleaved, 54 format-supported, 55 is-processing-target,
 *        56-63 sample rate / 1000 (44 -> 44.1k, 48 -> 48k)                   */
static inline uint64_t aeq_pack_unitv(uint64_t calls,uint32_t bus,uint32_t ch,int isFloat,int ni,int ok,int target,double rate){
 if(calls>0xFFFFFFFFFFULL)calls=0xFFFFFFFFFFULL;
 uint64_t r=(uint64_t)(rate/1000.0); if(r>255)r=255;
 return calls|((uint64_t)(bus&0xFF)<<40)|((uint64_t)(ch&0xF)<<48)|((uint64_t)(isFloat?1:0)<<52)|
  ((uint64_t)(ni?1:0)<<53)|((uint64_t)(ok?1:0)<<54)|((uint64_t)(target?1:0)<<55)|(r<<56);
}

/* ---- skip/bypass reasons, per-second deltas, 8 x 8-bit saturating ---- */
enum { AEQ_SKIP_DISABLED=0, AEQ_SKIP_NOT_TARGET, AEQ_SKIP_FORMAT, AEQ_SKIP_SHAPE,
       AEQ_SKIP_RENDER_ERR, AEQ_SKIP_EMPTY, AEQ_SKIP_TABLE_FULL, AEQ_SKIP_INACTIVE, AEQ_SKIP_COUNT };

/* ---- amp observer entry ----
 * bits 0-7 selector, 8-11 scalar-input count, 12-31 input0 (20 bits),
 * 32-55 input1 (24 bits), 56-59 class slot, 60-63 repeat count (saturating) */
static inline uint64_t aeq_pack_obs(uint32_t sel,uint32_t n,uint64_t in0,uint64_t in1,uint32_t cls,uint32_t rep){
 if(rep>15)rep=15;
 return (uint64_t)(sel&0xFF)|((uint64_t)(n&0xF)<<8)|((in0&0xFFFFFULL)<<12)|((in1&0xFFFFFFULL)<<32)|
  ((uint64_t)(cls&0xF)<<56)|((uint64_t)rep<<60);
}
static inline uint64_t aeq_pack_name8(const char *s){
 uint64_t v=0; for(int i=0;i<8&&s&&s[i];i++)v|=((uint64_t)(uint8_t)s[i])<<(8*i); return v;
}
static inline void aeq_unpack_name8(uint64_t v,char out[9]){
 for(int i=0;i<8;i++){char c=(char)((v>>(8*i))&0xFF); out[i]=(c>=32&&c<127)?c:0;} out[8]=0;
}
static inline void aeq_fourcc(uint32_t v,char out[5]){
 for(int i=0;i<4;i++){char c=(char)((v>>(24-8*i))&0xFF); out[i]=(c>=32&&c<127)?c:'?';} out[4]=0;
}
/* clip meter (mediaserverd, 10 per second):
 * bits 0-15  peak relative to the clip point, centi-dB + 10000 (10000 = exactly at the clip point)
 * bit 16     clipped during the last 0.1 s
 * bits 17-18 route the ceiling was computed for (0 unknown, 1 headphones, 2 other)
 * bits 20-31 protection reduction, centi-dB (0..1200)
 * bits 32-47 clip point in dBFS, centi-dB + 10000 (0 dBFS digital; lower with high Gain)
 * bit 48     protection on, bit 49 audio running
 * bits 56-63 sequence                                                                   */
static inline uint64_t aeq_pack_meter(float rel_db,int clipped,int route,int prot_cdb,float ceil_db,int prot_on,int running,unsigned seq){
 int r=(int)lrintf(rel_db*100)+10000; if(r<0)r=0; if(r>65535)r=65535;
 int c=(int)lrintf(ceil_db*100)+10000; if(c<0)c=0; if(c>65535)c=65535;
 if(prot_cdb<0)prot_cdb=0; if(prot_cdb>4095)prot_cdb=4095;
 return (uint64_t)r|((uint64_t)(clipped?1:0)<<16)|((uint64_t)(route&3)<<17)|((uint64_t)prot_cdb<<20)|((uint64_t)c<<32)|
  ((uint64_t)(prot_on?1:0)<<48)|((uint64_t)(running?1:0)<<49)|((uint64_t)(seq&0xFF)<<56);
}
/* boost template: bits 0-1 kind, 2-3 api, 4 learned, 8-15 selector, 16-23 value index,
 * 24-39 scalar count or struct size, 40-63 matches */
static inline uint64_t aeq_pack_boost0(int kind,int api,int learned,uint32_t sel,uint32_t idx,uint32_t size,uint32_t matches){
 if(matches>0xFFFFFF)matches=0xFFFFFF;
 return (uint64_t)(kind&3)|((uint64_t)(api&3)<<2)|((uint64_t)(learned?1:0)<<4)|((uint64_t)(sel&0xFF)<<8)|
  ((uint64_t)(idx&0xFF)<<16)|((uint64_t)(size&0xFFFF)<<24)|((uint64_t)matches<<40);
}
/* bits 0-15 last value iOS sent, 16-31 last value actually sent, 32-39 boost steps, 40-63 kr (low 24 bits) */
static inline uint64_t aeq_pack_boost1(uint32_t ios,uint32_t sent,uint32_t boost,uint32_t kr){
 return (uint64_t)(ios&0xFFFF)|((uint64_t)(sent&0xFFFF)<<16)|((uint64_t)(boost&0xFF)<<32)|((uint64_t)(kr&0xFFFFFF)<<40);
}
#endif
