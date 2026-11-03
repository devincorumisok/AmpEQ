/* Human-readable status report for the Settings > AmpEQ > Status page. */
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/sysctl.h>
#include <CoreFoundation/CoreFoundation.h>
#include "AmpEQShared.h"
#include "AmpEQSettings.h"
#include "AmpEQStatus.h"
#include "AmpEQCodec.h"

typedef struct {char *p; size_t left;} Out;
static void say(Out *o,const char *fmt,...){
 if(o->left<2)return; va_list ap; va_start(ap,fmt);
 int n=vsnprintf(o->p,o->left,fmt,ap); va_end(ap);
 if(n<0)return; if((size_t)n>=o->left)n=(int)o->left-1; o->p+=n; o->left-=(size_t)n;
}
static const char *target_name(int t){
 switch(t){case AEQ_TARGET_AUTO:return "Auto (one mixer)";case AEQ_TARGET_MIXERS:return "All mixers";
  case AEQ_TARGET_OUTPUT:return "Output unit";case AEQ_TARGET_OBSERVE:return "Observe only";}
 return "?";
}
static uint64_t st(const char *p,const char *k){char n[160];snprintf(n,sizeof n,AEQ_NS "st.%s.%s",p,k);return aeq_state(n);}
static void fmtdesc(uint64_t v,char *b,size_t n){
 if(!v){snprintf(b,n,"none");return;}
 unsigned ch=(unsigned)((v>>48)&0xF), rate=(unsigned)((v>>56)&0xFF);
 char r[8]; if(rate==44)snprintf(r,sizeof r,"44.1"); else snprintf(r,sizeof r,"%u",rate);
 snprintf(b,n,"%s kHz %uch %s%s",r,ch,
  (v>>52)&1?"float32":"integer",(v>>53)&1?" non-interleaved":" interleaved");
}
typedef struct {int alive,loaded; uint32_t pid,flags,secs; uint64_t render,frames,conv,proc,applied,fmt,skip;} Proc;
static Proc read_proc(const char *p){
 Proc r; memset(&r,0,sizeof r);
 uint64_t h=st(p,"heart");
 if((h&0xFFFF)==AEQ_MAGIC){
  r.loaded=1; r.pid=(uint32_t)((h>>16)&0xFFFFFF); r.flags=(uint32_t)((h>>40)&0xFF); r.secs=(uint32_t)(h>>48);
  r.alive=(kill((pid_t)r.pid,0)==0||errno==EPERM);
 }
 r.render=st(p,"render");r.frames=st(p,"frames");r.conv=st(p,"conv");r.proc=st(p,"proc");
 r.applied=st(p,"applied");r.fmt=st(p,"fmt");r.skip=st(p,"skip");
 return r;
}
static void units(Out *o,const char *p){
 int any=0;
 for(int i=0;i<AEQ_UNIT_SLOTS_PUBLISHED;i++){
  char k[8]; snprintf(k,sizeof k,"u%dk",i); uint64_t kk=st(p,k); snprintf(k,sizeof k,"u%dv",i); uint64_t v=st(p,k);
  if(!kk&&!v)continue; any=1;
  char t[5],s[5],f[64]; aeq_fourcc((uint32_t)(kk>>32),t); aeq_fourcc((uint32_t)kk,s); fmtdesc(v,f,sizeof f);
  say(o,"   %s/%s bus %u: %llu calls, %s%s\n",t,s,(unsigned)((v>>40)&0xFF),(unsigned long long)(v&0xFFFFFFFFFFULL),
      (v>>54)&1?f:"format pending or unsupported",(v>>55)&1?"  <= EQ TARGET":"");
 }
 if(!any)say(o,"   (no AudioUnitRender calls recorded yet)\n");
}
static const char *skipname[AEQ_SKIP_COUNT]={"EQ off","not target","bad format","buffer shape","render error","empty","table full","flat/no-op"};

static const char *steps_title(int b){
 static char t[48];
 if(b==AEQ_GAIN_LOW)return "Low (stock)";
 if(b==AEQ_GAIN_MID)snprintf(t,sizeof t,"Middle, +%.1f dB",b*0.5);
 else if(b==AEQ_GAIN_HIGH)snprintf(t,sizeof t,"High, +%.1f dB",b*0.5);
 else snprintf(t,sizeof t,"+%.1f dB",b*0.5);
 return t;
}
size_t aeq_status_report(char *buf,size_t len,int verbose){
 Out o={buf,len}; if(len)buf[0]=0;
 uint64_t hdr=aeq_state(AEQ_SET_HDR),a=aeq_state(AEQ_SET_A),b=aeq_state(AEQ_SET_B);
 AEQSettings pub; int havepub=aeq_unpack(hdr,a,b,&pub);
 AEQSettings saved; int havesaved=aeq_settings_load(&saved);
 Proc ms=read_proc("ms"), mu=read_proc("mu");
 uint32_t appliedSerial=(uint32_t)ms.applied;
 float autoGain=aeq_dq((int8_t)((ms.applied>>40)&0xFF)); int haveSettings=(int)((ms.applied>>48)&1);
 int current=haveSettings&&havepub&&appliedSerial==pub.serial;
 uint64_t m=aeq_state(AEQ_METER); int running=(int)((m>>49)&1);
 int route=(int)(aeq_state(AEQ_ROUTE)&3);
 uint64_t b0=aeq_state(AEQ_AMP_B0),b1=aeq_state(AEQ_AMP_B1);
 int learned=(int)((b0>>4)&1), api=(int)((b0>>2)&3); unsigned ios=(unsigned)(b1&0xFFFF), sent=(unsigned)((b1>>16)&0xFFFF), kr=(unsigned)((b1>>40)&0xFFFFFF);
 int cval=0; double cdb=0,cmin=0,cmax=0; int rb=aeq_codec_control("Codec",306,&cval,&cdb,&cmin,&cmax);
 int want=havepub?pub.ampBoost:0; if(route==2)want=0;
 unsigned expect=ios+(unsigned)want; if(expect>372)expect=372;
 int gainOk=want&&learned&&rb>=1&&ios>=100&&(unsigned)cval==expect;

 /* ---------- verdict ---------- */
 const char *v;
 if(havepub&&pub.masterOff) v="AmpEQ is off. Respring to fully unload it, or turn it back on and respring.";
 else if(!ms.loaded||!ms.alive) v="AmpEQ isn't running in the audio service yet. Play a sound and check again; if it stays like this, respring.";
 else if(!(ms.flags&AEQ_F_RENDER_HOOK)) v="AmpEQ loaded but couldn't attach to the audio service. Reinstall, then respring.";
 else if(!current) v="AmpEQ is running but hasn't got your latest settings. Tap Reconnect (More).";
 else if(want&&!learned) v="Working. Gain is waiting for one volume press: turn the volume down a step and back up.";
 else if(want&&route!=2&&kr) v="Working, but the audio chip refused the Gain value. Set Gain to Low.";
 else v="Everything is working.";
 say(&o,"%s\n\n",v);

 /* ---------- equalizer ---------- */
 say(&o,"EQUALIZER\n");
 if(havepub){
  say(&o," %s, preamp %+.1f dB, output level %+.1f dB%s\n",pub.enabled?"On":"Off",pub.preamp,autoGain,
   pub.enabled&&autoGain<pub.preamp-0.05f?" (lowered so EQ boosts can't clip)":"");
  say(&o," %s\n",!pub.enabled?"Not processing (switch is off).":(running&&ms.frames?"Processing audio now.":"Ready (plays when audio does)."));
 } else say(&o," No settings sent yet. Tap Reconnect (More).\n");

 /* ---------- clipping ---------- */
 say(&o,"\nCLIPPING\n");
 { float rel=((int)(m&0xFFFF)-10000)/100.0f, ceil=((int)((m>>32)&0xFFFF)-10000)/100.0f; int red=(int)((m>>20)&0xFFF);
   say(&o," Prevent clipping: %s\n",havepub&&pub.clipProtect?"on":"off");
   if(red)say(&o," Level lowered by %.1f dB to stop clipping (Reset in Equalizer).\n",red/100.0f);
   if(ceil<-0.05f)say(&o," Clip point: %.1f dBFS (at this Gain the amp clips before digital full scale)\n",ceil);
   else say(&o," Clip point: 0 dBFS (digital full scale)\n");
   if(running)say(&o," Peak now: %.0f %% of the clip point%s\n",100.0f*powf(10.0f,rel/20.0f),(m>>16)&1?" - CLIPPING":"");
   else say(&o," Peak now: no audio playing\n"); }

 /* ---------- gain ---------- */
 say(&o,"\nGAIN (the chip's headphone amp, no digital boost)\n");
 { char mdl[32]={0}; size_t mn=sizeof mdl; sysctlbyname("hw.machine",mdl,&mn,NULL,0);
   if(strcmp(mdl,"iPod9,1")){say(&o," Not available on this device (%s) - Gain is made for the iPod touch 7 only.\n",mdl[0]?mdl:"unknown");goto after_gain;} }
 say(&o," Setting: %s at every volume step\n",steps_title(havepub?pub.ampBoost:0));
 say(&o," Headphones: %s\n",route==1?"plugged in":route==2?"not plugged in - Gain off":"not known yet (Gain stays on)");
 if(havepub&&pub.ampBoost&&route!=2){
  if(!learned)say(&o," Status: waiting for one volume press\n");
  else if(gainOk)say(&o," Status: ACTIVE - the chip confirms it\n");
  else if(rb>=1&&ios>=100&&(unsigned)cval>ios&&(unsigned)cval<expect)say(&o," Status: ramping up (0.5 dB per 30 ms)\n");
  else if(kr)say(&o," Status: refused by the chip (error 0x%x)\n",kr);
  else say(&o," Status: not confirmed - press volume down and up once\n");
 }
 if(rb==2)say(&o," Chip volume now: %+.1f dB (value %d; iOS max is -4.5 dB)\n",cdb,cval);
 say(&o," Measured on an iPod touch 7: 0.50 dB per step, clean until Gain + song peak > +11.0 dB.\n"
        "  Low = stock, Middle = +5.5 dB, High = +10.5 dB, +11.0 dB = max clean.\n");
 after_gain:;

 /* ---------- volume / plugins ---------- */
 { int vs=16,vn=0;  /* the two volume keys, straight from the saved file */
   CFDataRef dd=NULL; CFPropertyListRef pl=NULL; FILE *fp=fopen(AEQ_PREF_PATH,"rb");
   if(fp){unsigned char bb[65536]; size_t n=fread(bb,1,sizeof bb,fp); fclose(fp); dd=CFDataCreate(NULL,bb,(CFIndex)n);}
   if(dd){pl=CFPropertyListCreateWithData(NULL,dd,kCFPropertyListImmutable,NULL,NULL); CFRelease(dd);}
   if(pl&&CFGetTypeID(pl)==CFDictionaryGetTypeID()){
    CFNumberRef n1=CFDictionaryGetValue(pl,CFSTR("volSteps")), n2=CFDictionaryGetValue(pl,CFSTR("volNumber"));
    if(n1&&CFGetTypeID(n1)==CFNumberGetTypeID())CFNumberGetValue(n1,kCFNumberIntType,&vs);
    if(n2&&CFGetTypeID(n2)==CFNumberGetTypeID())CFNumberGetValue(n2,kCFNumberIntType,&vn);}
   if(pl)CFRelease(pl);
   say(&o,"\nVOLUME\n %d steps%s, number in status bar %s\n",vs,vs==16?" (stock)":"",vn?"on":"off");
   uint64_t sv=aeq_state(AEQ_NS "st.sb.vol"); unsigned hk=(unsigned)(sv&0xFFFF), adj=(unsigned)((sv>>16)&0xFFFFFF);
   if(vs>16){
    if(!sv)say(&o," Step control: not reported yet (respring once)\n");
    else if(hk&(256|512))say(&o," Step control: audio service, exact steps%s\n",adj?"":" (press a volume button to test)");
    else if(hk&(4|8|16|32))say(&o," Step control: system volume service%s\n",adj?"":" (press a volume button to test)");
    else if(hk&(1|2))say(&o," Step control: SpringBoard%s\n",adj?"":" (press a volume button to test)");
    else say(&o," Step control: fallback (each press is corrected after iOS moves it)\n");
   }
   if(verbose)say(&o," hooks 0x%x, presses adjusted %u\n",hk,adj); }
 { uint64_t pg=st("ms","plg"); unsigned fl=(unsigned)(pg&0xFFFF), err=(unsigned)((pg>>20)&0xF);
   static const char *nm[7]={"DRX Dynamics","gxFeed","Crossfeed","Sound field","Pitch 432","Convolution","Balance"};
   say(&o,"\nPLUGINS\n"); int any=0; for(int i=0;i<7;i++)if(fl&(1u<<i)){say(&o,"%s%s",any?", ":" ",nm[i]);any=1;} say(&o,any?"\n":" none on\n");
   if(fl&(1u<<5)){int ok=(int)((pg>>16)&1)||(int)((pg>>17)&1);
    say(&o," Convolution file: %s\n",ok?"loaded":err==2?"blocked by the audio service's sandbox":err==1?"none chosen yet":err==3?"damaged - choose it again":"loading");} }

 if(!verbose)return len-o.left;
 /* ---------- technical details (verbose) ---------- */
 say(&o,"\n---------- details ----------\n");
 say(&o,"saved file: %s\n",havesaved?"ok":"missing/unreadable");
 if(havepub)say(&o,"published: serial %u, target %s, boost %d steps, protect %d (reset #%d)\n",pub.serial,target_name(pub.target),pub.ampBoost,pub.clipProtect,pub.protReset);
 if(ms.loaded){
  say(&o,"mediaserverd pid %u, %s, up %us, settings serial %u%s\n",ms.pid,ms.alive?"running":"EXITED",ms.secs,appliedSerial,current?" (current)":" (STALE)");
  say(&o,"hooks: Render %s Initialize %s Dispose %s Converter %s AUProcess %s IOKit %s\n",
   ms.flags&AEQ_F_RENDER_HOOK?"OK":"FAIL",ms.flags&AEQ_F_INIT_HOOK?"OK":"FAIL",ms.flags&AEQ_F_DISPOSE_HOOK?"OK":"FAIL",
   ms.flags&AEQ_F_CONV_HOOK?"OK":"FAIL",ms.flags&AEQ_F_PROC_HOOK?"OK":"FAIL",ms.flags&AEQ_F_IOKIT_HOOK?"OK":"FAIL");
  say(&o,"AudioUnitRender %llu, converter %llu, AUProcess %llu, processed frames %llu\n",
   (unsigned long long)ms.render,(unsigned long long)ms.conv,(unsigned long long)ms.proc,(unsigned long long)ms.frames);
  char f[64]; fmtdesc(ms.fmt,f,sizeof f); say(&o,"target format: %s\n",f);
  say(&o,"skipped last second:"); int anyskip=0;
  for(int i=0;i<AEQ_SKIP_COUNT;i++){unsigned d=(unsigned)((ms.skip>>(8*i))&0xFF);if(d){say(&o," %s=%u%s",skipname[i],d,d==255?"+":"");anyskip=1;}}
  say(&o,anyskip?"\n":" none\n");
  say(&o,"audio units:\n"); units(&o,"ms");
 }
 if(mu.loaded){say(&o,"Music app (observe only): pid %u %s, render %llu\n",mu.pid,mu.alive?"running":"exited",(unsigned long long)mu.render);}
 if(learned||(b0&3))say(&o,"volume call: %s, api %d, selector %u, index %u, seen %llu; iOS %u -> sent %u, kr 0x%x\n",learned?"learned":"seen once",api,
   (unsigned)((b0>>8)&0xFF),(unsigned)((b0>>16)&0xFF),(unsigned long long)(b0>>40),ios,sent,kr);
 if(rb==2)say(&o,"codec 306: %d = %+.1f dB (range %+.0f..%+.0f)\n",cval,cdb,cmin,cmax);
 say(&o,"meter: rel %+.2f dB, clip point %+.2f dBFS, reduction %d cdB, route %d\n",((int)(m&0xFFFF)-10000)/100.0,((int)((m>>32)&0xFFFF)-10000)/100.0,(int)((m>>20)&0xFFF),route);
 return len-o.left;
}
