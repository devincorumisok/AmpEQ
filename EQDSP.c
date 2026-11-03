/* 12-band RBJ peaking EQ, transposed direct form II, no allocation, no locks. */
#include "EQDSP.h"
#include <math.h>
#include <string.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
const float eq_freqs[EQ_BANDS] = {31,62,125,250,500,1000,2000,4000,6000,8000,12000,16000};
static float bound(float v,float lo,float hi){return !isfinite(v)?0:(v<lo?lo:(v>hi?hi:v));}

static void design(EQCoeff *c,float rate,float f,float gain_db){
 float w=2.0f*(float)M_PI*f/rate, cw=cosf(w), sn=sinf(w);
 float A=powf(10.0f,gain_db/40.0f), alpha=sn/(2.0f*EQ_Q), a0=1.0f+alpha/A;
 c->b0=(1.0f+alpha*A)/a0; c->b1=(-2.0f*cw)/a0; c->b2=(1.0f-alpha*A)/a0;
 c->a1=(-2.0f*cw)/a0;     c->a2=(1.0f-alpha/A)/a0;
}
static int band_usable(int i,float rate,float g){return fabsf(g)>=0.01f && eq_freqs[i]<rate*0.45f;}

static double biquad_mag_db(const EQCoeff *c,double rate,double f){
 double w=2*M_PI*f/rate, c1=cos(w), s1=sin(w), c2=cos(2*w), s2=sin(2*w);
 double nr=c->b0+c->b1*c1+c->b2*c2, ni=-(c->b1*s1+c->b2*s2);
 double dr=1+c->a1*c1+c->a2*c2,     di=-(c->a1*s1+c->a2*s2);
 double num=nr*nr+ni*ni, den=dr*dr+di*di;
 if(den<=0||num<=0)return 0;
 return 10*log10(num/den);
}
float eq_response_db(const float gains[EQ_BANDS],float rate,float f){
 rate=bound(rate,8000,192000); double sum=0;
 for(int i=0;i<EQ_BANDS;i++){
  float g=bound(gains?gains[i]:0,-12,12);
  if(!band_usable(i,rate,g))continue;
  EQCoeff c; design(&c,rate,eq_freqs[i],g); sum+=biquad_mag_db(&c,rate,f);
 }
 return (float)sum;
}
float eq_peak_db(const float gains[EQ_BANDS],float rate){
 rate=bound(rate,8000,192000);
 float top=fminf(20000.0f,rate*0.49f), peak=-1e9f;
 for(int k=0;k<=240;k++){
  float f=20.0f*powf(top/20.0f,(float)k/240.0f);
  float r=eq_response_db(gains,rate,f); if(r>peak)peak=r;
 }
 for(int i=0;i<EQ_BANDS;i++){ /* make sure exact centres are included */
  if(eq_freqs[i]>=top)continue; float r=eq_response_db(gains,rate,eq_freqs[i]); if(r>peak)peak=r;
 }
 return peak;
}
float eq_auto_gain_db(const float gains[EQ_BANDS],float preamp_db,float rate){
 float peak=eq_peak_db(gains,rate);
 return bound(preamp_db,-12,12)-fmaxf(0.0f,peak);
}
void eq_fit(const float target[EQ_BANDS],float out[EQ_BANDS]){
 float t[EQ_BANDS];
 for(int i=0;i<EQ_BANDS;i++){t[i]=bound(target[i],-12,12);out[i]=t[i];}
 for(int it=0;it<40;it++){
  for(int i=0;i<EQ_BANDS;i++){
   float r=eq_response_db(out,48000.0f,eq_freqs[i]);
   out[i]=bound(out[i]+0.6f*(t[i]-r),-12,12);
  }
 }
 for(int i=0;i<EQ_BANDS;i++)out[i]=roundf(out[i]*4.0f)/4.0f; /* quarter-dB like the wire format */
}
void eq_reset_history(EQEngine *e){if(e)memset(e->hist,0,sizeof(e->hist));}
void eq_set(EQEngine *e,float rate,const float gains[EQ_BANDS],float total_gain_db,int enabled){
 if(!e)return;
 rate=bound(rate,8000,192000);
 e->sample_rate=rate; e->nbands=0;
 total_gain_db=bound(total_gain_db,-40,12);
 e->gain=powf(10.0f,total_gain_db/20.0f); if(!(e->xgain>0))e->xgain=1.0f;
 int any=0;
 for(int i=0;i<EQ_BANDS;i++){
  float g=bound(gains?gains[i]:0,-12,12);
  e->band_on[i]=0;
  if(!enabled||!band_usable(i,rate,g)){e->coeff[i]=(EQCoeff){1,0,0,0,0};continue;}
  design(&e->coeff[i],rate,eq_freqs[i],g); e->band_on[i]=1; any=1; e->nbands++;
 }
 e->active=enabled&&(any||fabsf(total_gain_db)>0.01f);
}
void eq_configure(EQEngine *e,float rate,const float gains[EQ_BANDS],float preamp_db,int enabled){
 if(!e)return; memset(e,0,sizeof(*e));
 eq_set(e,rate,gains,eq_auto_gain_db(gains,preamp_db,bound(rate,8000,192000)),enabled);
}
static inline float limit(float x){
 /* transparent below 0.89 FS (-1 dBFS); smooth knee to the rail above */
 float a=fabsf(x);
 if(!isfinite(x))return 0;
 if(a<=0.89f)return x;
 return copysignf(0.89f+0.11f*tanhf((a-0.89f)/0.11f),x);
}
float eq_sample(EQEngine *e,unsigned ch,float x){
 if(!e||!e->active||ch>=EQ_CHANNELS)return x;
 EQHistory *h=e->hist[ch];
 for(int i=0;i<EQ_BANDS;i++){
  if(!e->band_on[i])continue;
  const EQCoeff *c=&e->coeff[i]; EQHistory *s=&h[i];
  float y=c->b0*x+s->z1;
  s->z1=c->b1*x-c->a1*y+s->z2;
  s->z2=c->b2*x-c->a2*y;
  x=y;
 }
 /* denormal / blow-up guard */
 if(!isfinite(x)){memset(h,0,sizeof(EQHistory)*EQ_BANDS);return 0;}
 x*=e->gain*e->xgain;
 { float a=fabsf(x); if(a>e->peak)e->peak=a; }
 return limit(x);
}
void eq_process_float(EQEngine *e,float *p,uint32_t n,uint32_t stride,unsigned ch){
 if(!p||!stride||!e||!e->active)return;
 for(uint32_t i=0;i<n;i++)p[(size_t)i*stride]=eq_sample(e,ch,p[(size_t)i*stride]);
}
void eq_process_s16(EQEngine *e,int16_t *p,uint32_t n,uint32_t stride,unsigned ch){
 if(!p||!stride||!e||!e->active)return;
 for(uint32_t i=0;i<n;i++){
  float v=eq_sample(e,ch,(float)p[(size_t)i*stride]*(1.0f/32768.0f))*32768.0f;
  if(v>32767.0f)v=32767.0f; if(v<-32768.0f)v=-32768.0f;
  p[(size_t)i*stride]=(int16_t)lrintf(v);
 }
}
void eq_process_s32(EQEngine *e,int32_t *p,uint32_t n,uint32_t stride,unsigned ch,unsigned frac){
 if(!p||!stride||!e||!e->active||frac<1||frac>31)return;
 double scale=ldexp(1.0,(int)frac), inv=1.0/scale, maxv=2147483647.0, minv=-2147483648.0;
 for(uint32_t i=0;i<n;i++){
  double v=(double)eq_sample(e,ch,(float)((double)p[(size_t)i*stride]*inv))*scale;
  if(v>maxv)v=maxv; if(v<minv)v=minv;
  p[(size_t)i*stride]=(int32_t)llrint(v);
 }
}
