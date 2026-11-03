/* AmpEQ plugins - see AEQPlugins.h. Everything here is AmpEQ's own DSP. */
#include "AEQPlugins.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static float clampf(float x,float a,float b){return !(x==x)?a:(x<a?a:(x>b?b:x));}

/* ------------------------------------------------------------------ wire format */
void aeq_plg_pack(const AEQPluginParams *p,uint64_t w[4]){
 w[0]=(uint64_t)(p->flags&0xFFFF)|((uint64_t)(p->xfeed_level&3)<<16)|
  ((uint64_t)(uint8_t)(int8_t)lrintf(clampf(p->drx_bass,-1,2)*50)<<24)|((uint64_t)(uint8_t)(int8_t)lrintf(clampf(p->drx_mids,-1,2)*50)<<32)|
  ((uint64_t)(uint8_t)(int8_t)lrintf(clampf(p->drx_treble,-1,2)*50)<<40)|((uint64_t)(uint8_t)(int8_t)lrintf(clampf(p->drx_gain_db,-10,5)*10)<<48)|
  ((uint64_t)(uint8_t)lrintf(clampf(p->field_width,0,2)*100)<<56);
 w[1]=(uint64_t)(uint16_t)(int16_t)lrintf(clampf(p->gx_bass_db,-20,0)*10)|((uint64_t)(uint16_t)(int16_t)lrintf(clampf(p->gx_mids_db,-20,0)*10)<<16)|
  ((uint64_t)(uint8_t)lrintf(clampf(p->gx_delay_ms,0,1)*100)<<32)|((uint64_t)(uint8_t)(int8_t)lrintf(clampf(p->gx_gain_db,-10,5)*10)<<40)|
  ((uint64_t)(uint8_t)(int8_t)lrintf(clampf(p->pitch_gain_db,-10,0)*10)<<48)|((uint64_t)(uint8_t)(int8_t)lrintf(clampf(p->balance,-100,100))<<56);
 w[2]=(uint64_t)(uint16_t)lrintf(clampf(p->gx_bass_hz,50,2000))|((uint64_t)(uint16_t)lrintf(clampf(p->gx_mids_hz,500,12000))<<16)|
  ((uint64_t)(uint16_t)(int16_t)lrintf(clampf(p->conv_level,-20,20)*10)<<40);
 w[3]=p->conv_serial;
}
void aeq_plg_unpack(const uint64_t w[4],AEQPluginParams *p){
 memset(p,0,sizeof *p);
 p->flags=(uint32_t)(w[0]&0xFFFF); p->xfeed_level=(int)((w[0]>>16)&3); if(p->xfeed_level>2)p->xfeed_level=2;
 p->drx_bass=(int8_t)((w[0]>>24)&0xFF)/50.0f; p->drx_mids=(int8_t)((w[0]>>32)&0xFF)/50.0f; p->drx_treble=(int8_t)((w[0]>>40)&0xFF)/50.0f;
 p->drx_gain_db=(int8_t)((w[0]>>48)&0xFF)/10.0f; p->field_width=((w[0]>>56)&0xFF)/100.0f;
 p->gx_bass_db=(int16_t)(w[1]&0xFFFF)/10.0f; p->gx_mids_db=(int16_t)((w[1]>>16)&0xFFFF)/10.0f;
 p->gx_delay_ms=((w[1]>>32)&0xFF)/100.0f; p->gx_gain_db=(int8_t)((w[1]>>40)&0xFF)/10.0f;
 p->pitch_gain_db=(int8_t)((w[1]>>48)&0xFF)/10.0f; p->balance=(int8_t)((w[1]>>56)&0xFF);
 p->gx_bass_hz=(float)(w[2]&0xFFFF); p->gx_mids_hz=(float)((w[2]>>16)&0xFFFF); p->conv_level=(int16_t)((w[2]>>40)&0xFFFF)/10.0f;
 p->conv_serial=(uint32_t)w[3];
 /* defaults for an all-zero (never published) block */
 if(p->gx_bass_hz<50)p->gx_bass_hz=400; if(p->gx_mids_hz<500)p->gx_mids_hz=4200;
}

/* ------------------------------------------------------------------ helpers */
typedef struct {float b0,b1,b2,a1,a2,z1,z2;} BQ;
static void bq_set(BQ *q,int type,float f,float Q,float sr){ /* 0 LP, 1 HP, 2 BP(0dB peak) */
 float w=2*(float)M_PI*clampf(f,10,sr*0.45f)/sr,c=cosf(w),s=sinf(w),al=s/(2*Q),a0=1+al;
 if(type==0){q->b0=(1-c)/2/a0;q->b1=(1-c)/a0;q->b2=(1-c)/2/a0;}
 else if(type==1){q->b0=(1+c)/2/a0;q->b1=-(1+c)/a0;q->b2=(1+c)/2/a0;}
 else{q->b0=al/a0;q->b1=0;q->b2=-al/a0;}
 q->a1=-2*c/a0;q->a2=(1-al)/a0;
}
static inline float bq(BQ *q,float x){float y=q->b0*x+q->z1;q->z1=q->b1*x-q->a1*y+q->z2;q->z2=q->b2*x-q->a2*y;return y;}
static inline float soft(float x){float a=fabsf(x);return a<=0.89f?x:copysignf(0.89f+0.11f*tanhf((a-0.89f)/0.11f),x);}

/* ------------------------------------------------------------------ convolution IR */
#define FFTN (2*AEQ_CONV_BLOCK)
#define NB (AEQ_CONV_BLOCK+1)
typedef struct {float re,im;} cpx;
struct AEQIR { uint32_t rate, parts; cpx *H; /* parts x 2ch x NB */ };
static void fft(cpx *a,int n,int inv){
 for(int i=1,j=0;i<n;i++){int b=n>>1;for(;j&b;b>>=1)j^=b;j^=b;if(i<j){cpx t=a[i];a[i]=a[j];a[j]=t;}}
 for(int len=2;len<=n;len<<=1){double ang=2*M_PI/len*(inv?1:-1);cpx wl={(float)cos(ang),(float)sin(ang)};
  for(int i=0;i<n;i+=len){cpx w={1,0};for(int k=0;k<len/2;k++){cpx u=a[i+k],v=a[i+k+len/2];
   cpx t={v.re*w.re-v.im*w.im,v.re*w.im+v.im*w.re};a[i+k]=(cpx){u.re+t.re,u.im+t.im};a[i+k+len/2]=(cpx){u.re-t.re,u.im-t.im};
   w=(cpx){w.re*wl.re-w.im*wl.im,w.re*wl.im+w.im*wl.re};}}}
 if(inv)for(int i=0;i<n;i++){a[i].re/=n;a[i].im/=n;}
}
AEQIR *aeq_ir_create(const float *st,uint32_t frames,uint32_t rate){
 if(!st||!frames||rate<8000)return NULL;
 if(frames>AEQ_IR_MAX_FRAMES)frames=AEQ_IR_MAX_FRAMES;
 double e[2]={0,0}; for(uint32_t i=0;i<frames;i++){e[0]+=st[2*i]*st[2*i];e[1]+=st[2*i+1]*st[2*i+1];}
 double em=e[0]>e[1]?e[0]:e[1]; if(em<=1e-12)return NULL;
 float g=(float)(1.0/sqrt(em));
 AEQIR *ir=calloc(1,sizeof *ir); if(!ir)return NULL;
 ir->rate=rate; ir->parts=(frames+AEQ_CONV_BLOCK-1)/AEQ_CONV_BLOCK;
 ir->H=calloc((size_t)ir->parts*2*NB,sizeof(cpx)); if(!ir->H){free(ir);return NULL;}
 cpx buf[FFTN];
 for(uint32_t p=0;p<ir->parts;p++)for(int ch=0;ch<2;ch++){
  memset(buf,0,sizeof buf);
  for(int k=0;k<AEQ_CONV_BLOCK;k++){uint32_t i=p*AEQ_CONV_BLOCK+(uint32_t)k; if(i<frames)buf[k].re=st[2*i+ch]*g;}
  fft(buf,FFTN,0); memcpy(&ir->H[((size_t)p*2+ch)*NB],buf,NB*sizeof(cpx));
 }
 return ir;
}
void aeq_ir_free(AEQIR *ir){if(ir){free(ir->H);free(ir);}}
uint32_t aeq_ir_rate(const AEQIR *ir){return ir?ir->rate:0;}

/* ------------------------------------------------------------------ state */
#define DLY 256          /* crossfeed delay line (>= 1 ms at 192 kHz) */
#define PB 8192          /* pitch shifter buffer */
struct AEQPluginState {
 float sr; uint32_t cfg_key;
 /* DRX: 3 bands per channel, linked envelopes */
 BQ lo[2][2], hi[2][2]; float envF[3], envS[3];
 /* crossfeed (general) and gxFeed */
 BQ xlp[2]; BQ gxlp[2][2], gxbp[2]; float dl[2][DLY]; int dpos;
 /* pitch */
 float pbuf[2][PB]; int pw; double pr;
 /* convolution */
 const AEQIR *ir_seen; float in[2][AEQ_CONV_BLOCK], outq[2][AEQ_CONV_BLOCK]; int fill;
 float prev[2][AEQ_CONV_BLOCK]; cpx *fdl; uint32_t fdl_parts; uint32_t fdl_head; cpx work[FFTN];
};
AEQPluginState *aeq_plg_state_create(void){
 AEQPluginState *s=calloc(1,sizeof *s); if(!s)return NULL;
 s->fdl_parts=AEQ_IR_MAX_FRAMES/AEQ_CONV_BLOCK+1;
 s->fdl=calloc((size_t)s->fdl_parts*2*NB,sizeof(cpx));
 if(!s->fdl){free(s);return NULL;}
 return s;
}
void aeq_plg_reset(AEQPluginState *s){
 if(!s)return; cpx *f=s->fdl; uint32_t fp=s->fdl_parts;
 memset(f,0,(size_t)fp*2*NB*sizeof(cpx));
 memset(s,0,sizeof *s); s->fdl=f; s->fdl_parts=fp;
}
static void configure(AEQPluginState *s,const AEQPluginParams *p,float sr){
 uint32_t key=(uint32_t)sr^(uint32_t)lrintf(p->gx_bass_hz*7)^((uint32_t)lrintf(p->gx_mids_hz)<<11)^((uint32_t)p->xfeed_level<<28);
 if(s->sr==sr&&s->cfg_key==key)return;
 if(s->sr!=sr){const AEQIR *keep=NULL;(void)keep;aeq_plg_reset(s);}
 s->sr=sr; s->cfg_key=key;
 for(int c=0;c<2;c++){
  for(int k=0;k<2;k++){bq_set(&s->lo[c][k],0,200,0.7071f,sr);bq_set(&s->hi[c][k],1,2500,0.7071f,sr);}
  static const float xf[3]={700,700,650};
  bq_set(&s->xlp[c],0,xf[p->xfeed_level],0.5f,sr);
  bq_set(&s->gxlp[c][0],0,p->gx_bass_hz,0.7071f,sr); bq_set(&s->gxlp[c][1],0,p->gx_bass_hz,0.7071f,sr);
  float lo=p->gx_bass_hz, hi=p->gx_mids_hz>lo*1.5f?p->gx_mids_hz:lo*1.5f, fc=sqrtf(lo*hi);
  bq_set(&s->gxbp[c],2,fc,fc/(hi-lo),sr);
 }
}

/* ------------------------------------------------------------------ processing */
static void conv_block(AEQPluginState *s,const AEQIR *ir,float wetgain){
 uint32_t P=ir->parts<s->fdl_parts?ir->parts:s->fdl_parts;
 s->fdl_head=(s->fdl_head+s->fdl_parts-1)%s->fdl_parts;
 for(int ch=0;ch<2;ch++){
  cpx *w=s->work;
  for(int k=0;k<AEQ_CONV_BLOCK;k++){w[k]=(cpx){s->prev[ch][k],0};w[k+AEQ_CONV_BLOCK]=(cpx){s->in[ch][k],0};}
  memcpy(s->prev[ch],s->in[ch],sizeof s->prev[ch]);
  fft(w,FFTN,0);
  memcpy(&s->fdl[((size_t)s->fdl_head*2+ch)*NB],w,NB*sizeof(cpx));
  cpx acc[NB]; memset(acc,0,sizeof acc);
  for(uint32_t p=0;p<P;p++){
   const cpx *X=&s->fdl[((size_t)((s->fdl_head+p)%s->fdl_parts)*2+ch)*NB], *H=&ir->H[((size_t)p*2+ch)*NB];
   for(int b=0;b<NB;b++){acc[b].re+=X[b].re*H[b].re-X[b].im*H[b].im; acc[b].im+=X[b].re*H[b].im+X[b].im*H[b].re;}
  }
  for(int b=0;b<NB;b++)w[b]=acc[b];
  for(int b=1;b<AEQ_CONV_BLOCK;b++)w[FFTN-b]=(cpx){acc[b].re,-acc[b].im};
  fft(w,FFTN,1);
  for(int k=0;k<AEQ_CONV_BLOCK;k++){s->outq[ch][k]=w[AEQ_CONV_BLOCK+k].re*wetgain;}
 }
}
void aeq_plg_process(AEQPluginState *s,const AEQPluginParams *p,const AEQIR *ir,float sr,float *L,float *R,uint32_t st,uint32_t n){
 if(!s||!p||!p->flags||!L||!R||!st)return;
 configure(s,p,sr);
 uint32_t f=p->flags;
 int conv=(f&PLG_CONV)&&ir&&(aeq_ir_rate(ir)==(uint32_t)lrintf(sr));
 if(conv&&s->ir_seen!=ir){s->ir_seen=ir;s->fill=0;memset(s->outq,0,sizeof s->outq);memset(s->prev,0,sizeof s->prev);
  memset(s->fdl,0,(size_t)s->fdl_parts*2*NB*sizeof(cpx));}
 /* per-sample coefficients */
 float drxA[3]={p->drx_bass,p->drx_mids,p->drx_treble}, drxG=powf(10,p->drx_gain_db/20);
 /* transient detector: fast vs slow mean power per band (steady signal -> ratio 1).
  * fast time constant scales with the band so bass ripple isn't read as an attack */
 static const float tauF[3]={0.012f,0.004f,0.0015f};
 float kF[3]; for(int b=0;b<3;b++)kF[b]=1-expf(-1/(tauF[b]*sr)); float kS=1-expf(-1/(0.080f*sr));
 static const float xfeed_db[3]={-9.5f,-6.0f,-4.5f};           /* Low / Medium / High crossfeed amount */
 float xa=powf(10,xfeed_db[p->xfeed_level]/20), xn=1/(1+xa); int xd=(int)lrintf(0.00030f*sr); if(xd>DLY-1)xd=DLY-1;
 float gb=powf(10,p->gx_bass_db/20), gm=powf(10,p->gx_mids_db/20), gg=powf(10,p->gx_gain_db/20)/(1+gb);
 int gd=(int)lrintf(p->gx_delay_ms*0.001f*sr); if(gd>DLY-1)gd=DLY-1;
 float wdt=p->field_width, wnorm=wdt>1?2/(1+wdt):1;
 double ratio=432.0/440.0; int W=(int)(0.050f*sr); if(W>PB/2-4)W=PB/2-4;
 float pg=powf(10,p->pitch_gain_db/20);
 float bl=p->balance, gL=bl>0?1-bl/100:1, gR=bl<0?1+bl/100:1;
 for(uint32_t i=0;i<n;i++){
  float l=L[(size_t)i*st], r=R[(size_t)i*st];
  if(f&PLG_DRX){
   float lb[2]={bq(&s->lo[0][1],bq(&s->lo[0][0],l)),bq(&s->lo[1][1],bq(&s->lo[1][0],r))};
   float hb[2]={bq(&s->hi[0][1],bq(&s->hi[0][0],l)),bq(&s->hi[1][1],bq(&s->hi[1][0],r))};
   float mb[2]={l-lb[0]-hb[0],r-lb[1]-hb[1]};
   float *bands[3]={lb,mb,hb}; float ol=l,orr=r;
   for(int b=0;b<3;b++){
    float x=fmaxf(bands[b][0]*bands[b][0],bands[b][1]*bands[b][1]);
    s->envF[b]+=kF[b]*(x-s->envF[b]); s->envS[b]+=kS*(x-s->envS[b]);
    float t=s->envS[b]>1e-10f?sqrtf(s->envF[b]/s->envS[b])-1.10f:0; t=clampf(t,0,3);
    float g=clampf(drxA[b]*t*0.5f,-0.9f,3);                  /* extra (or, below 0, less) gain on transients only */
    ol+=bands[b][0]*g; orr+=bands[b][1]*g;
   }
   l=ol*drxG; r=orr*drxG;
  }
  if(f&(PLG_XFEED|PLG_GXFEED)){
   int wp=s->dpos; s->dl[0][wp]=l; s->dl[1][wp]=r; s->dpos=(wp+1)%DLY;
   if(f&PLG_GXFEED){
    float dLr=s->dl[0][(wp-gd+DLY)%DLY], dRr=s->dl[1][(wp-gd+DLY)%DLY];
    float xL=bq(&s->gxlp[1][1],bq(&s->gxlp[1][0],dRr))*gb+bq(&s->gxbp[1],dRr)*gm;
    float xR=bq(&s->gxlp[0][1],bq(&s->gxlp[0][0],dLr))*gb+bq(&s->gxbp[0],dLr)*gm;
    l=(l+xL)*gg; r=(r+xR)*gg;
   } else {
    float dLr=s->dl[0][(wp-xd+DLY)%DLY], dRr=s->dl[1][(wp-xd+DLY)%DLY];
    float xL=bq(&s->xlp[1],dRr)*xa, xR=bq(&s->xlp[0],dLr)*xa;
    l=(l+xL)*xn; r=(r+xR)*xn;
   }
  }
  if(f&PLG_FIELD){float m=(l+r)*0.5f, sd=(l-r)*0.5f*wdt; l=(m+sd)*wnorm; r=(m-sd)*wnorm;}
  if(f&PLG_PITCH){
   /* two read taps sweeping a delay line, 180 deg apart, raised-cosine crossfade */
   s->pbuf[0][s->pw]=l; s->pbuf[1][s->pw]=r;
   s->pr+= (1.0-ratio); if(s->pr>=W)s->pr-=W;
   float out[2]={0,0};
   for(int t=0;t<2;t++){
    double d=s->pr+(t?W*0.5:0); if(d>=W)d-=W;
    double rp=(double)s->pw-d-2; while(rp<0)rp+=PB;
    int i0=(int)rp; float fr=(float)(rp-i0); int i1=(i0+1)%PB;
    float wgt=0.5f-0.5f*cosf(2*(float)M_PI*(float)(d/W));
    for(int c=0;c<2;c++)out[c]+=wgt*(s->pbuf[c][i0]*(1-fr)+s->pbuf[c][i1]*fr);
   }
   s->pw=(s->pw+1)%PB;
   l=out[0]*pg; r=out[1]*pg;
   if(f&PLG_PITCH_LIMIT){l=soft(l);r=soft(r);}
  }
  if(conv){
   s->in[0][s->fill]=l; s->in[1][s->fill]=r;
   l=s->outq[0][s->fill]; r=s->outq[1][s->fill];
   if(++s->fill==AEQ_CONV_BLOCK){s->fill=0; conv_block(s,ir,powf(10.0f,p->conv_level/20.0f));}
  }
  if(f&PLG_BALANCE){l*=gL;r*=gR;}
  if(!isfinite(l)||!isfinite(r)){l=r=0;}
  L[(size_t)i*st]=l; R[(size_t)i*st]=r;
 }
}
