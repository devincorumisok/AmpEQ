/* One page per plugin (layout from Plg*.plist), plus the Convolution IR file picker.
 * The IR is converted here (this process can read any file the user picks) into raw
 * stereo float32 at 44.1 and 48 kHz, which the audio service loads. */
#import <UIKit/UIKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import "PreferencesCompat.h"
#import "AEQStore.h"
#include <sys/stat.h>
#include "../AEQPlugins.h"

#import "AEQBase.h"

@interface AEQPluginController : AEQBaseListController
@end
@implementation AEQPluginController { BOOL _pickingIR; }
- (NSString *)plistName { return [self.specifier propertyForKey:@"pluginPlist"]?:@"Plugins"; }
- (NSString *)convFileName:(PSSpecifier *)s { NSString *n=[AEQStore load][@"convFile"]; return n.length?n:@"None"; }
- (void)chooseIR {
 _pickingIR=YES;
 UIDocumentPickerViewController *p=[[UIDocumentPickerViewController alloc] initWithDocumentTypes:@[@"public.audio",@"com.microsoft.waveform-audio",@"public.data"] inMode:UIDocumentPickerModeImport];
 p.delegate=self; [self presentViewController:p animated:YES completion:nil];
}
static BOOL writeIR(NSURL *url,double rate,NSString **why){
 ExtAudioFileRef f=NULL;
 if(ExtAudioFileOpenURL((__bridge CFURLRef)url,&f)!=noErr||!f){*why=@"Not an audio file iOS can read.";return NO;}
 AudioStreamBasicDescription src={0}; UInt32 sz=sizeof src;
 ExtAudioFileGetProperty(f,kExtAudioFileProperty_FileDataFormat,&sz,&src);
 UInt32 ch=src.mChannelsPerFrame>=2?2:1;
 AudioStreamBasicDescription cli={0}; cli.mSampleRate=rate; cli.mFormatID=kAudioFormatLinearPCM;
 cli.mFormatFlags=kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked; cli.mChannelsPerFrame=ch; cli.mBitsPerChannel=32;
 cli.mBytesPerFrame=4*ch; cli.mFramesPerPacket=1; cli.mBytesPerPacket=4*ch;
 if(ExtAudioFileSetProperty(f,kExtAudioFileProperty_ClientDataFormat,sizeof cli,&cli)!=noErr){ExtAudioFileDispose(f);*why=@"Could not convert this file.";return NO;}
 UInt32 maxF=AEQ_IR_MAX_FRAMES; float *in=calloc((size_t)maxF*ch,sizeof(float)), *st=calloc((size_t)maxF*2,sizeof(float)); UInt32 got=0;
 while(got<maxF){
  AudioBufferList bl; bl.mNumberBuffers=1; bl.mBuffers[0].mNumberChannels=ch;
  bl.mBuffers[0].mDataByteSize=(maxF-got)*4*ch; bl.mBuffers[0].mData=in+(size_t)got*ch;
  UInt32 n=maxF-got; if(ExtAudioFileRead(f,&n,&bl)!=noErr||n==0)break; got+=n;
 }
 ExtAudioFileDispose(f);
 if(!got){free(in);free(st);*why=@"The file is empty.";return NO;}
 for(UInt32 i=0;i<got;i++){st[2*i]=in[(size_t)i*ch]; st[2*i+1]=in[(size_t)i*ch+(ch-1)];}
 mkdir("/var/jb/var/mobile/Library/AmpEQ",0755);
 char path[160]; snprintf(path,sizeof path,AEQ_IR_FILE_FMT,(unsigned)rate);
 FILE *o=fopen(path,"wb"); BOOL ok=NO;
 if(o){uint32_t h[4]={0x49514541u,(uint32_t)rate,got,2}; ok=fwrite(h,4,4,o)==4&&fwrite(st,sizeof(float)*2,got,o)==got; fclose(o); chmod(path,0644);}
 free(in); free(st);
 if(!ok)*why=@"Could not save the converted file.";
 return ok;
}
- (void)documentPicker:(UIDocumentPickerViewController *)c didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
 if(!_pickingIR){[super documentPicker:c didPickDocumentsAtURLs:urls];return;}
 _pickingIR=NO;
 NSURL *url=urls.firstObject; if(!url)return;
 BOOL sec=[url startAccessingSecurityScopedResource];
 NSString *why=nil; BOOL ok=writeIR(url,44100,&why)&&writeIR(url,48000,&why);
 if(sec)[url stopAccessingSecurityScopedResource];
 if(!ok){[self alert:@"Can't use this file" message:why];return;}
 NSMutableDictionary *d=[AEQStore load];
 d[@"convFile"]=url.lastPathComponent; d[@"convSerial"]=@(arc4random_uniform(2000000000)+1); d[@"plgConv"]=@YES;
 [AEQStore save:d]; [self reloadSpecifiers];
 [self alert:@"Loaded" message:[NSString stringWithFormat:@"%@ (first 2 s used).",url.lastPathComponent]];
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)c { _pickingIR=NO; }
@end
