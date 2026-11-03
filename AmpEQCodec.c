/* Read-only access to the audio codec driver state in the IORegistry (Status page).
 * Prints every IOService entry whose class or name looks audio/codec related, a
 * one-line-per-control summary of any "controls" style arrays, and the full
 * property list. Run it at minimum and maximum volume and diff the two files:
 * any control that moves with the volume buttons is a candidate for the real
 * headphone gain path. Nothing is written to any driver. */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include "AmpEQCodec.h"

typedef mach_port_t io_object_t; typedef io_object_t io_registry_entry_t, io_iterator_t;
extern const mach_port_t kIOMasterPortDefault;
kern_return_t IORegistryCreateIterator(mach_port_t,const char*,uint32_t,io_iterator_t*);
io_object_t IOIteratorNext(io_iterator_t);
kern_return_t IOObjectRelease(io_object_t);
kern_return_t IOObjectGetClass(io_object_t,char[128]);
kern_return_t IORegistryEntryGetName(io_registry_entry_t,char[128]);
kern_return_t IORegistryEntryGetRegistryEntryID(io_registry_entry_t,uint64_t*);
kern_return_t IORegistryEntryCreateCFProperties(io_registry_entry_t,CFMutableDictionaryRef*,CFAllocatorRef,uint32_t);
#define kIORegistryIterateRecursively 1

static int wanted(const char *cls,const char *name,int all){
 const char *keys[]={"CS4","Cirrus","Codec","codec","EmbeddedAudio","Headphone","headphone","hpout","Amp",NULL};
 for(int i=0;keys[i];i++)if(strstr(cls,keys[i])||strstr(name,keys[i]))return 1;
 return all&&(strstr(cls,"Audio")||strstr(name,"audio")||strstr(name,"Audio"));
}
static void scalar(CFTypeRef v,char *b,size_t n){
 if(!v){snprintf(b,n,"-");return;}
 CFTypeID t=CFGetTypeID(v);
 if(t==CFNumberGetTypeID()){long long x=0;double d=0;
  if(CFNumberIsFloatType((CFNumberRef)v)){CFNumberGetValue((CFNumberRef)v,kCFNumberDoubleType,&d);snprintf(b,n,"%g",d);}
  else{CFNumberGetValue((CFNumberRef)v,kCFNumberLongLongType,&x);snprintf(b,n,"%lld",x);}}
 else if(t==CFStringGetTypeID()){if(!CFStringGetCString((CFStringRef)v,b,(CFIndex)n,kCFStringEncodingUTF8))snprintf(b,n,"?");}
 else if(t==CFBooleanGetTypeID())snprintf(b,n,"%s",CFBooleanGetValue((CFBooleanRef)v)?"true":"false");
 else if(t==CFDataGetTypeID()){size_t L=(size_t)CFDataGetLength((CFDataRef)v);const UInt8 *p=CFDataGetBytePtr((CFDataRef)v);
  size_t o=0;o+=(size_t)snprintf(b,n,"<%zu bytes:",L);for(size_t i=0;i<L&&i<24&&o+4<n;i++)o+=(size_t)snprintf(b+o,n-o," %02x",p[i]);if(o+2<n)snprintf(b+o,n-o,">");}
 else if(t==CFArrayGetTypeID()){CFIndex c=CFArrayGetCount((CFArrayRef)v);size_t o=(size_t)snprintf(b,n,"[");
  for(CFIndex i=0;i<c&&o+16<n;i++){char s[64];CFTypeRef e=CFArrayGetValueAtIndex((CFArrayRef)v,i);
   if(CFGetTypeID(e)==CFDictionaryGetTypeID()||CFGetTypeID(e)==CFArrayGetTypeID())snprintf(s,sizeof s,"{..}");else scalar(e,s,sizeof s);
   o+=(size_t)snprintf(b+o,n-o,"%s%s",i?",":"",s);}
  if(o+2<n)snprintf(b+o,n-o,"]");}
 else if(t==CFDictionaryGetTypeID())snprintf(b,n,"{dict %ld}",(long)CFDictionaryGetCount((CFDictionaryRef)v));
 else snprintf(b,n,"<type %lu>",(unsigned long)t);
}
typedef struct {FILE *f;} Ctx;
static void dict_line(const void *k,const void *v,void *ctx){
 char ks[96],vs[512]; scalar(k,ks,sizeof ks); scalar(v,vs,sizeof vs);
 fprintf(((Ctx*)ctx)->f," %s=%s",ks,vs);
}
/* Any array of dictionaries is printed one element per line, sorted keys not needed for diffing
 * because CF dictionary iteration order is stable for identical content. */
static void controls(const void *k,const void *v,void *ctx){
 if(CFGetTypeID(v)!=CFArrayGetTypeID())return;
 CFArrayRef a=(CFArrayRef)v; CFIndex c=CFArrayGetCount(a);
 if(!c||CFGetTypeID(CFArrayGetValueAtIndex(a,0))!=CFDictionaryGetTypeID())return;
 char ks[96]; scalar(k,ks,sizeof ks);
 FILE *f=((Ctx*)ctx)->f;
 fprintf(f,"  [%s] %ld entries\n",ks,(long)c);
 for(CFIndex i=0;i<c;i++){CFTypeRef e=CFArrayGetValueAtIndex(a,i);
  if(CFGetTypeID(e)!=CFDictionaryGetTypeID())continue;
  fprintf(f,"   #%ld",(long)i); CFDictionaryApplyFunction((CFDictionaryRef)e,dict_line,ctx); fprintf(f,"\n");
  /* nested dicts (e.g. "range map") get one more level */
 }
}
static void nested(const void *k,const void *v,void *ctx){
 (void)k; if(CFGetTypeID(v)!=CFArrayGetTypeID())return;
 CFArrayRef a=(CFArrayRef)v;
 for(CFIndex i=0;i<CFArrayGetCount(a);i++){CFTypeRef e=CFArrayGetValueAtIndex(a,i);
  if(CFGetTypeID(e)!=CFDictionaryGetTypeID())continue;
  CFDictionaryRef d=(CFDictionaryRef)e; CFIndex n=CFDictionaryGetCount(d);
  const void *ks[64],*vs[64]; if(n>64)n=64; CFDictionaryGetKeysAndValues(d,ks,vs);
  for(CFIndex j=0;j<n;j++){
   if(CFGetTypeID(vs[j])==CFDictionaryGetTypeID()||CFGetTypeID(vs[j])==CFArrayGetTypeID()){
    char kn[96]; scalar(ks[j],kn,sizeof kn);
    FILE *f=((Ctx*)ctx)->f; fprintf(f,"     #%ld.%s:",(long)i,kn);
    if(CFGetTypeID(vs[j])==CFDictionaryGetTypeID())CFDictionaryApplyFunction((CFDictionaryRef)vs[j],dict_line,ctx);
    else{CFArrayRef in=(CFArrayRef)vs[j];for(CFIndex q=0;q<CFArrayGetCount(in);q++){CFTypeRef x=CFArrayGetValueAtIndex(in,q);
      if(CFGetTypeID(x)==CFDictionaryGetTypeID()){fprintf(f," {");CFDictionaryApplyFunction((CFDictionaryRef)x,dict_line,ctx);fprintf(f," }");}
      else{char s[96];scalar(x,s,sizeof s);fprintf(f," %s",s);}}}
    fprintf(f,"\n");
   }
  }
 }
}
int aeq_codec_dump(FILE *f,int all,int full){
 io_iterator_t it=0; int found=0;
 if(IORegistryCreateIterator(kIOMasterPortDefault,"IOService",kIORegistryIterateRecursively,&it)!=KERN_SUCCESS){fprintf(f,"cannot iterate IORegistry\n");return 0;}
 io_object_t o; Ctx ctx={f};
 while((o=IOIteratorNext(it))){
  char cls[128]={0},name[128]={0}; IOObjectGetClass(o,cls); IORegistryEntryGetName(o,name);
  if(wanted(cls,name,all)){
   uint64_t id=0; IORegistryEntryGetRegistryEntryID(o,&id);
   fprintf(f,"\n== %s  \"%s\"  id 0x%llx\n",cls,name,(unsigned long long)id); found++;
   CFMutableDictionaryRef p=NULL;
   if(IORegistryEntryCreateCFProperties(o,&p,kCFAllocatorDefault,0)==KERN_SUCCESS&&p){
    fprintf(f," properties:");
    /* scalar properties on one line each */
    CFIndex n=CFDictionaryGetCount(p); const void **ks=calloc((size_t)n,sizeof(void*)),**vs=calloc((size_t)n,sizeof(void*));
    if(ks&&vs){CFDictionaryGetKeysAndValues(p,ks,vs);
     for(CFIndex i=0;i<n;i++){char k[128],v[700];scalar(ks[i],k,sizeof k);scalar(vs[i],v,sizeof v);fprintf(f,"\n  %s = %s",k,v);}}
    free(ks);free(vs); fprintf(f,"\n");
    CFDictionaryApplyFunction(p,controls,&ctx);
    CFDictionaryApplyFunction(p,nested,&ctx);
    if(full){CFDataRef x=CFPropertyListCreateData(NULL,p,kCFPropertyListXMLFormat_v1_0,0,NULL);
     if(x){fwrite(CFDataGetBytePtr(x),1,(size_t)CFDataGetLength(x),f);CFRelease(x);}}
    CFRelease(p);
   } else fprintf(f," (properties not readable)\n");
  }
  IOObjectRelease(o);
 }
 IOObjectRelease(it);
 fprintf(f,"\n%d matching registry entries\n",found);
 return found;
}

/* Read one control's live value from the AppleEmbeddedAudioDevice whose
 * "device UID" (registry name) is `dev` ("Codec" = CS42L81 headphones).
 * dB uses the driver's own range map (32.32 fixed-point dB). Returns 1 on success. */
static int num64(CFDictionaryRef d,const char *k,long long *out){
 CFStringRef key=CFStringCreateWithCString(NULL,k,kCFStringEncodingUTF8);
 CFTypeRef v=key?CFDictionaryGetValue(d,key):NULL; if(key)CFRelease(key);
 if(!v||CFGetTypeID(v)!=CFNumberGetTypeID())return 0;
 return CFNumberGetValue((CFNumberRef)v,kCFNumberLongLongType,out);
}
int aeq_codec_control(const char *dev,unsigned id,int *value,double *db,double *dbmin,double *dbmax){
 io_iterator_t it=0; int ok=0;
 if(IORegistryCreateIterator(kIOMasterPortDefault,"IOService",kIORegistryIterateRecursively,&it)!=KERN_SUCCESS)return 0;
 io_object_t o;
 while(!ok&&(o=IOIteratorNext(it))){
  char cls[128]={0},name[128]={0}; IOObjectGetClass(o,cls); IORegistryEntryGetName(o,name);
  if(!strcmp(cls,"AppleEmbeddedAudioDevice")&&!strcmp(name,dev)){
   CFMutableDictionaryRef p=NULL;
   if(IORegistryEntryCreateCFProperties(o,&p,kCFAllocatorDefault,0)==KERN_SUCCESS&&p){
    CFArrayRef ctl=CFDictionaryGetValue(p,CFSTR("controls"));
    if(ctl&&CFGetTypeID(ctl)==CFArrayGetTypeID())
     for(CFIndex i=0;i<CFArrayGetCount(ctl)&&!ok;i++){
      CFDictionaryRef c=CFArrayGetValueAtIndex(ctl,i); long long cid=0,v=0;
      if(CFGetTypeID(c)!=CFDictionaryGetTypeID()||!num64(c,"control ID",&cid)||cid!=(long long)id||!num64(c,"value",&v))continue;
      *value=(int)v; ok=1; *db=*dbmin=*dbmax=0;
      CFArrayRef rm=CFDictionaryGetValue(c,CFSTR("range map"));
      if(rm&&CFGetTypeID(rm)==CFArrayGetTypeID()&&CFArrayGetCount(rm)>0){
       CFDictionaryRef r=CFArrayGetValueAtIndex(rm,0); long long sdb=0,sint=0,step=0,steps=0;
       if(CFGetTypeID(r)==CFDictionaryGetTypeID()&&num64(r,"start db value",&sdb)&&num64(r,"db per step",&step)){
        num64(r,"start int value",&sint); num64(r,"integer steps",&steps);
        double a=(double)sdb/4294967296.0, s=(double)step/4294967296.0;
        *db=a+(double)(v-sint)*s; *dbmin=a; *dbmax=a+(double)steps*s; ok=2;
       }
      }
     }
    CFRelease(p);
   }
  }
  IOObjectRelease(o);
 }
 IOObjectRelease(it);
 return ok;
}
