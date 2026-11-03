/* Load saved settings from the plist and publish them as notify state.
 * Used by SpringBoard (relay) and Settings - processes that CAN
 * read /var/mobile/Library/Preferences. Never called inside mediaserverd. */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "AmpEQShared.h"
#include "AmpEQSettings.h"
#include "AEQPlugins.h"

static float num(CFDictionaryRef d,const char *key,float def){
 CFStringRef k=CFStringCreateWithCString(NULL,key,kCFStringEncodingUTF8);
 CFTypeRef v=k?CFDictionaryGetValue(d,k):NULL; if(k)CFRelease(k);
 if(!v)return def;
 if(CFGetTypeID(v)==CFBooleanGetTypeID())return CFBooleanGetValue((CFBooleanRef)v)?1:0;
 if(CFGetTypeID(v)==CFNumberGetTypeID()){double x=0;CFNumberGetValue((CFNumberRef)v,kCFNumberDoubleType,&x);return (float)x;}
 if(CFGetTypeID(v)==CFStringGetTypeID())return (float)CFStringGetDoubleValue((CFStringRef)v);
 return def;
}
static float clampf(float x,float a,float b){return (x==x)?(x<a?a:(x>b?b:x)):0;}
int aeq_settings_from_dict(const void *dict,AEQSettings *s){
 memset(s,0,sizeof(*s));
 CFDictionaryRef d=(CFDictionaryRef)dict;
 if(!d||CFGetTypeID(d)!=CFDictionaryGetTypeID())return 0;
 s->valid=1;
 s->enabled=num(d,"eqEnabled",0)!=0;
 s->ampObserve=num(d,"ampObserve",0)!=0;
 s->preamp=clampf(num(d,"preamp",0),-12,AEQ_PREAMP_MAX);
 /* Boost limit: +11.5 dB (23 steps) - measured with a loopback recording on an
  * iPod: clean through +11.0 dB, a -0.5 dBFS tone clips at +11.5 dB. */
 s->tone=num(d,"testTone",0)!=0;
 int b=(int)num(d,"ampBoost",0), lim=s->tone?AEQ_BOOST_MAX_STEPS:AEQ_BOOST_HW_MAX;
 s->ampBoost=b<0?0:(b>lim?lim:b);
 /* master switch: key "ampeqEnabled", default ON */
 s->masterOff=num(d,"ampeqEnabled",1)==0;
 s->clipProtect=num(d,"clipProtect",1)!=0;
 s->protReset=((int)num(d,"protReset",0))&0xF;
 /* plugins */
 AEQPluginParams pp; memset(&pp,0,sizeof pp);
 struct {const char *k; uint32_t bit;} fl[]={{"plgDRX",PLG_DRX},{"plgGX",PLG_GXFEED},{"plgXF",PLG_XFEED},{"plgField",PLG_FIELD},
  {"plgPitch",PLG_PITCH},{"plgConv",PLG_CONV},{"plgBalance",PLG_BALANCE},{"pitchLimiter",PLG_PITCH_LIMIT}};
 for(unsigned i=0;i<sizeof fl/sizeof fl[0];i++)if(num(d,fl[i].k,fl[i].bit==PLG_PITCH_LIMIT)!=0)pp.flags|=fl[i].bit;
 /* DRX defaults: all 0.0 (no effect until the user raises them) */
 pp.drx_bass=num(d,"drxBass",0); pp.drx_mids=num(d,"drxMids",0); pp.drx_treble=num(d,"drxTreble",0); pp.drx_gain_db=num(d,"drxGain",0);
 pp.gx_bass_db=num(d,"gxBass",-4.5f); pp.gx_mids_db=num(d,"gxMids",-20); pp.gx_delay_ms=num(d,"gxDelay",0.3f);
 pp.gx_bass_hz=num(d,"gxBassHz",700); pp.gx_mids_hz=num(d,"gxMidsHz",4000); pp.gx_gain_db=num(d,"gxGain",0);
 pp.xfeed_level=(int)num(d,"xfLevel",1); pp.field_width=num(d,"fieldWidth",1.0f); pp.pitch_gain_db=num(d,"pitchGain",0);
 pp.conv_level=num(d,"convLevel",0); pp.balance=num(d,"balance",0); pp.conv_serial=(uint32_t)num(d,"convSerial",0);
 aeq_plg_pack(&pp,s->plg);
 if(s->masterOff){s->enabled=0;s->ampObserve=0;s->ampBoost=0;s->tone=0;s->clipProtect=0;memset(s->plg,0,sizeof s->plg);}
 int t=(int)num(d,"target",0); s->target=(t>=0&&t<=3)?t:0;
 for(int i=0;i<AEQ_BANDS;i++){char k[8];snprintf(k,sizeof k,"g%d",i);s->gains[i]=clampf(num(d,k,0),-12,12);}
 return 1;
}
int aeq_settings_load(AEQSettings *s){
 memset(s,0,sizeof(*s));
 FILE *f=fopen(AEQ_PREF_PATH,"rb"); if(!f)return 0;
 unsigned char *buf=malloc(262144); if(!buf){fclose(f);return 0;}
 size_t n=fread(buf,1,262144,f); fclose(f);
 if(!n){free(buf);return 0;}
 CFDataRef data=CFDataCreate(NULL,buf,(CFIndex)n); free(buf); if(!data)return 0;
 CFPropertyListRef pl=CFPropertyListCreateWithData(NULL,data,kCFPropertyListImmutable,NULL,NULL);
 CFRelease(data);
 int ok=aeq_settings_from_dict(pl,s);
 if(pl)CFRelease(pl);
 return ok;
}
static void put(const char *name,uint64_t v){
 int t=-1; if(notify_register_check(name,&t)==NOTIFY_STATUS_OK){notify_set_state(t,v);notify_cancel(t);}
}
uint32_t aeq_settings_publish(const AEQSettings *s){
 static uint32_t bump=0;
 uint32_t serial=(uint32_t)(mach_absolute_time()>>8)+(++bump);
 if(!serial)serial=1;
 /* order matters: halves first, header last (reader validates serials) */
 for(int i=0;i<4;i++){char k[64];snprintf(k,sizeof k,AEQ_SET_P "%d",i);put(k,s->plg[i]);}
 put(AEQ_SET_A,aeq_pack_a(s->gains));
 put(AEQ_SET_B,aeq_pack_b(s->gains,serial));
 put(AEQ_SET_HDR,aeq_hdr_protect(aeq_pack_hdr_m(s->enabled,s->ampObserve,s->preamp,s->target,serial,s->ampBoost,s->tone,s->masterOff),s->clipProtect,s->protReset));
 notify_post(AEQ_NOTE_CHANGED);
 return serial;
}
uint32_t aeq_settings_publish_saved(void){
 AEQSettings s;
 if(!aeq_settings_load(&s)){memset(&s,0,sizeof s);s.valid=1;} /* nothing saved: publish "off" explicitly */
 return aeq_settings_publish(&s);
}
uint64_t aeq_state(const char *name){
 int t=-1; uint64_t v=0;
 if(notify_register_check(name,&t)==NOTIFY_STATUS_OK){notify_get_state(t,&v);notify_cancel(t);}
 return v;
}
