/* EQ file import -> 12 band gains. Pure C so it is host-testable.
 * Supported:
 *  1. Equalizer APO / AutoEQ "GraphicEQ: f g; f g; ..."
 *  2. Equalizer APO / AutoEQ ParametricEQ.txt
 *       Preamp: -6.4 dB
 *       Filter 1: ON PK Fc 105 Hz Gain -2.1 dB Q 0.70
 *       Filter 2: ON LSC Fc 105 Hz Gain 5.5 dB Q 0.71     (LS/LSC/HS/HSC/PK/PEQ)
 *  3. JSON {"gains":[12 numbers], "preamp": x}  (parsed by the ObjC side)
 * The target response at the 12 band centres is then FITTED (eq_fit) so the
 * overlapping peaking filters actually reproduce it. Imported preamps are
 * ignored on purpose: AmpEQ computes exact headroom from the final curve. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "EQDSP.h"
#include "AEQImport.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *ci_find(const char *h,const char *n){
 size_t L=strlen(n);
 for(;*h;h++){size_t i=0;while(i<L&&h[i]&&tolower((unsigned char)h[i])==tolower((unsigned char)n[i]))i++; if(i==L)return h;}
 return NULL;
}
static int graphic(const char *text,float target[EQ_BANDS]){
 const char *p=ci_find(text,"GraphicEQ:"); if(!p)return 0; p+=10;
 double f[1024],g[1024]; int n=0;
 while(*p&&n<1024){
  char *e; double a=strtod(p,&e); if(e==p){p++;continue;} p=e;
  double b=strtod(p,&e); if(e==p)break; p=e;
  if(isfinite(a)&&isfinite(b)&&a>=10&&a<=24000){
   /* keep sorted */
   int k=n; while(k>0&&f[k-1]>a){f[k]=f[k-1];g[k]=g[k-1];k--;} f[k]=a;g[k]=b;n++;
  }
  while(*p&&*p!=';')p++;
 }
 if(n<2)return 0;
 for(int i=0;i<EQ_BANDS;i++){
  double x=eq_freqs[i],v;
  if(x<=f[0])v=g[0]; else if(x>=f[n-1])v=g[n-1];
  else{v=g[n-1];for(int k=0;k<n-1;k++)if(x>=f[k]&&x<=f[k+1]){double t=f[k+1]>f[k]?log(x/f[k])/log(f[k+1]/f[k]):0;v=g[k]*(1-t)+g[k+1]*t;break;}}
  target[i]=(float)v;
 }
 return 1;
}
/* RBJ magnitude in dB for PK / LS / HS at frequency x */
static double rbj_db(int type,double fc,double gain,double q,double x,double rate){
 double A=pow(10,gain/40), w0=2*M_PI*fc/rate, cw=cos(w0), sw=sin(w0), alpha=sw/(2*q), b0,b1,b2,a0,a1,a2;
 if(type==0){b0=1+alpha*A;b1=-2*cw;b2=1-alpha*A;a0=1+alpha/A;a1=-2*cw;a2=1-alpha/A;}
 else{
  double s=2*sqrt(A)*alpha;
  if(type==1){b0=A*((A+1)-(A-1)*cw+s);b1=2*A*((A-1)-(A+1)*cw);b2=A*((A+1)-(A-1)*cw-s);
              a0=(A+1)+(A-1)*cw+s;a1=-2*((A-1)+(A+1)*cw);a2=(A+1)+(A-1)*cw-s;}
  else       {b0=A*((A+1)+(A-1)*cw+s);b1=-2*A*((A-1)+(A+1)*cw);b2=A*((A+1)+(A-1)*cw-s);
              a0=(A+1)-(A-1)*cw+s;a1=2*((A-1)-(A+1)*cw);a2=(A+1)-(A-1)*cw-s;}
 }
 double w=2*M_PI*x/rate,c1=cos(w),s1=sin(w),c2=cos(2*w),s2=sin(2*w);
 double nr=b0+b1*c1+b2*c2,ni=-(b1*s1+b2*s2),dr=a0+a1*c1+a2*c2,di=-(a1*s1+a2*s2);
 return 10*log10((nr*nr+ni*ni)/(dr*dr+di*di));
}
static int parametric(const char *text,float target[EQ_BANDS]){
 int count=0; double sum[EQ_BANDS]={0};
 const char *line=text;
 while(line&&*line){
  const char *end=strchr(line,'\n'); size_t L=end?(size_t)(end-line):strlen(line);
  char buf[512]; if(L>=sizeof buf)L=sizeof buf-1; memcpy(buf,line,L); buf[L]=0;
  line=end?end+1:NULL;
  if(!ci_find(buf,"Filter")||!ci_find(buf," ON "))continue;
  int type=-1;
  if(ci_find(buf," PK ")||ci_find(buf," PEQ "))type=0;
  else if(ci_find(buf," LSC ")||ci_find(buf," LS "))type=1;
  else if(ci_find(buf," HSC ")||ci_find(buf," HS "))type=2;
  const char *fc=ci_find(buf,"Fc"),*gn=ci_find(buf,"Gain"),*qq=ci_find(buf," Q ");
  if(type<0||!fc||!gn)continue;
  double F=strtod(fc+2,NULL),G=strtod(gn+4,NULL),Q=qq?strtod(qq+3,NULL):(type?0.707:1.0);
  if(!(F>=10&&F<=24000)||!isfinite(G)||!(Q>0.05&&Q<30))continue;
  for(int i=0;i<EQ_BANDS;i++)sum[i]+=rbj_db(type,F,G,Q,eq_freqs[i],48000.0);
  count++;
 }
 if(!count)return 0;
 for(int i=0;i<EQ_BANDS;i++)target[i]=(float)sum[i];
 return count;
}
int aeq_import_text(const char *text,float gains_out[EQ_BANDS],char *kind,size_t kindlen){
 float target[EQ_BANDS]; int n;
 if(!text)return 0;
 if(graphic(text,target)){snprintf(kind,kindlen,"GraphicEQ");}
 else if((n=parametric(text,target))>0){snprintf(kind,kindlen,"ParametricEQ (%d filters)",n);}
 else return 0;
 /* re-centre so the loudest band is at most +12 and nothing is lost to clamping;
  * absolute level does not matter because headroom is automatic */
 float mx=-1e9f; for(int i=0;i<EQ_BANDS;i++)if(target[i]>mx)mx=target[i];
 if(mx>12)for(int i=0;i<EQ_BANDS;i++)target[i]-=mx-12;
 eq_fit(target,gains_out);
 return 1;
}
