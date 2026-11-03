/* SpringBoard relay.
 * 1. Publishes saved settings at boot/respring and whenever mediaserverd says hello.
 * 2. Master switch: if "AmpEQ" was switched on/off in Settings since the last respring,
 *    restart mediaserverd once so the hooks really load (on) or are not installed (off).
 *    The state that was applied is remembered in the plist ("masterApplied") so this
 *    happens exactly once per change, never on every boot. */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <notify.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <syslog.h>
#include "AmpEQSettings.h"

extern char **environ;

static CFMutableDictionaryRef load_plist(void){
 FILE *f=fopen(AEQ_PREF_PATH,"rb"); if(!f)return NULL;
 unsigned char *buf=malloc(262144); if(!buf){fclose(f);return NULL;}
 size_t n=fread(buf,1,262144,f); fclose(f);
 CFDataRef d=n?CFDataCreate(NULL,buf,(CFIndex)n):NULL; free(buf); if(!d)return NULL;
 CFPropertyListRef p=CFPropertyListCreateWithData(NULL,d,kCFPropertyListMutableContainersAndLeaves,NULL,NULL); CFRelease(d);
 if(p&&CFGetTypeID(p)!=CFDictionaryGetTypeID()){CFRelease(p);return NULL;}
 return (CFMutableDictionaryRef)p;
}
static int boolval(CFDictionaryRef d,CFStringRef k,int def){
 CFTypeRef v=CFDictionaryGetValue(d,k); if(!v)return def;
 if(CFGetTypeID(v)==CFBooleanGetTypeID())return CFBooleanGetValue((CFBooleanRef)v);
 if(CFGetTypeID(v)==CFNumberGetTypeID()){int i=0;CFNumberGetValue((CFNumberRef)v,kCFNumberIntType,&i);return i!=0;}
 return def;
}
static void save_plist(CFDictionaryRef d){
 CFDataRef x=CFPropertyListCreateData(NULL,d,kCFPropertyListBinaryFormat_v1_0,0,NULL); if(!x)return;
 char tmp[]=AEQ_PREF_PATH ".tmp";
 FILE *f=fopen(tmp,"wb"); if(f){fwrite(CFDataGetBytePtr(x),1,(size_t)CFDataGetLength(x),f);fclose(f);rename(tmp,AEQ_PREF_PATH);}
 CFRelease(x);
}
static int restart_mediaserverd(void){
 const char *tools[]={"/var/jb/usr/bin/killall","/var/jb/bin/killall",NULL};
 for(int i=0;tools[i];i++){
  pid_t pid; char *argv[]={(char*)tools[i],"-9","mediaserverd",NULL};
  if(posix_spawn(&pid,tools[i],NULL,NULL,argv,environ)==0){int st=0;waitpid(pid,&st,0);if(WIFEXITED(st)&&WEXITSTATUS(st)==0)return 1;}
 }
 /* fallback: pid from AmpEQ's own heartbeat (only exists while hooks are loaded) */
 uint64_t h=aeq_state(AEQ_NS "st.ms.heart");
 if((h&0xFFFF)==AEQ_MAGIC){pid_t p=(pid_t)((h>>16)&0xFFFFFF); if(p>1&&kill(p,SIGKILL)==0)return 1;}
 return 0;
}
static void apply_master(void){
 CFMutableDictionaryRef d=load_plist(); if(!d)return;
 int want=boolval(d,CFSTR("ampeqEnabled"),1), applied=boolval(d,CFSTR("masterApplied"),1);
 if(want!=applied){
  int ok=restart_mediaserverd();
  syslog(LOG_NOTICE,"AmpEQ[relay]: AmpEQ switched %s - audio service restart %s",want?"ON":"OFF",ok?"done":"FAILED");
  if(ok){CFDictionarySetValue(d,CFSTR("masterApplied"),want?kCFBooleanTrue:kCFBooleanFalse);save_plist(d);}
 }
 CFRelease(d);
}
void AmpEQ_RelayInstall(void){
 uint32_t s=aeq_settings_publish_saved();
 syslog(LOG_NOTICE,"AmpEQ[relay]: published saved settings serial %u",s);
 /* give the system a moment to settle after a respring, then apply the master switch */
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(3*NSEC_PER_SEC)),dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{apply_master();});
 int tok;
 notify_register_dispatch(AEQ_NOTE_HELLO,&tok,dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^(__unused int t){
  uint32_t x=aeq_settings_publish_saved();
  syslog(LOG_NOTICE,"AmpEQ[relay]: audio process said hello; republished serial %u",x);
 });
}
