#import "AEQStore.h"
#include <math.h>
#include "../AmpEQShared.h"
#include "../AmpEQSettings.h"
#include "../AEQImport.h"

#define PREF @AEQ_PREF_PATH

@implementation AEQStore
+ (NSMutableDictionary *)load {
 NSDictionary *d=[NSDictionary dictionaryWithContentsOfFile:PREF];
 NSMutableDictionary *m=[d isKindOfClass:[NSDictionary class]]?[d mutableCopy]:[NSMutableDictionary dictionary];
 /* 1.12: plugin values go back to the new stock defaults once (keeps the Enable switches) */
 if(d.count&&[m[@"plgDefaults"] intValue]<2){
  for(NSString *k in @[@"drxBass",@"drxMids",@"drxTreble",@"drxGain",@"gxBass",@"gxMids",@"gxDelay",@"gxBassHz",@"gxMidsHz",@"gxGain",
   @"xfLevel",@"fieldWidth",@"pitchGain",@"pitchLimiter",@"convLevel",@"balance"])[m removeObjectForKey:k];
  m[@"plgDefaults"]=@2; [self save:m];
 }
 /* 1.12.1: DRX defaults changed to 0.0 */
 if(d.count&&[m[@"plgDefaults"] intValue]<3){
  for(NSString *k in @[@"drxBass",@"drxMids",@"drxTreble",@"drxGain"])[m removeObjectForKey:k];
  m[@"plgDefaults"]=@3; [self save:m];
 }
 /* 1.14: new plugin ranges (DRX -1..2 / -10..+5 dB, convolution level in dB): all plugin values back to defaults once */
 if(d.count&&[m[@"plgDefaults"] intValue]<4){
  for(NSString *k in @[@"drxBass",@"drxMids",@"drxTreble",@"drxGain",@"gxBass",@"gxMids",@"gxDelay",@"gxBassHz",@"gxMidsHz",@"gxGain",
   @"fieldWidth",@"pitchGain",@"convLevel",@"balance"])[m removeObjectForKey:k];
  m[@"plgDefaults"]=@4; [self save:m];
 }
 return m;
}
+ (int)hwMax:(NSDictionary *)d {
 id v=d[@"hwMax"]; int h=v?[v intValue]:AEQ_BOOST_CLEAN_STEPS;
 return h<0?0:(h>AEQ_BOOST_MAX_STEPS?AEQ_BOOST_MAX_STEPS:h);
}
+ (BOOL)hwMeasured:(NSDictionary *)d { return [d[@"hwMeasured"] boolValue]; }
+ (BOOL)save:(NSDictionary *)d {
 NSMutableDictionary *m=[d mutableCopy];
 int lim=[m[@"testTone"] boolValue]?AEQ_BOOST_MAX_STEPS:AEQ_BOOST_HW_MAX;
 if([m[@"ampBoost"] intValue]>lim)m[@"ampBoost"]=@(lim);
 [m removeObjectForKey:@"boostUnlock"]; [m removeObjectForKey:@"hwMax"]; [m removeObjectForKey:@"hwMeasured"];
 if(![m writeToFile:PREF atomically:YES])return NO;
 AEQSettings s; if(aeq_settings_from_dict((__bridge const void *)m,&s))aeq_settings_publish(&s);
 return YES;
}
+ (NSArray *)builtIns {
 return @[
  @{@"name":@"Flat",       @"g":@[@0,@0,@0,@0,@0,@0,@0,@0,@0,@0,@0,@0]},
  @{@"name":@"Bass boost", @"g":@[@6,@6,@4.5,@2.5,@1,@0,@0,@0,@0,@0,@0,@0]},
  @{@"name":@"Warm",       @"g":@[@3,@3,@2.5,@1.5,@0.5,@0,@0,@-0.5,@-1,@-1,@-1,@-1]},
  @{@"name":@"Vocal",      @"g":@[@-2,@-1.5,@-1,@0,@1,@2,@3,@2.5,@1.5,@0.5,@0,@-1]},
  @{@"name":@"Treble",     @"g":@[@0,@0,@0,@0,@0,@0,@0.5,@1.5,@2.5,@3,@3,@2.5]},
  @{@"name":@"Loudness",   @"g":@[@5,@4,@2.5,@1,@0,@-0.5,@0,@1,@2,@2.5,@3,@2.5]},
  @{@"name":@"De-ess",     @"g":@[@0,@0,@0,@0,@0,@0,@0,@-1,@-3,@-3.5,@-1.5,@0]},
 ];
}
+ (NSArray *)userPresets:(NSDictionary *)d { NSArray *u=d[@"userPresets"]; return [u isKindOfClass:[NSArray class]]?u:@[]; }
+ (NSArray *)allPresets:(NSDictionary *)d { return [[self builtIns] arrayByAddingObjectsFromArray:[self userPresets:d]]; }
+ (void)applyGains:(NSArray *)g into:(NSMutableDictionary *)d {
 for(int i=0;i<12;i++){float x=i<(int)g.count?[g[i] floatValue]:0; if(!isfinite(x))x=0;
  d[[NSString stringWithFormat:@"g%d",i]]=@(fmaxf(-12,fminf(12,roundf(x*4)/4)));}
}
+ (NSArray *)gainsFrom:(NSDictionary *)d {
 NSMutableArray *g=[NSMutableArray array];
 for(int i=0;i<12;i++)[g addObject:d[[NSString stringWithFormat:@"g%d",i]]?:@0];
 return g;
}
+ (void)storeUserPreset:(NSString *)name gains:(NSArray *)gains into:(NSMutableDictionary *)d {
 if(!gains)gains=[self gainsFrom:d];
 NSMutableArray *u=[[self userPresets:d] mutableCopy];
 NSIndexSet *same=[u indexesOfObjectsPassingTest:^BOOL(NSDictionary *p,__unused NSUInteger i,__unused BOOL *s){return [p[@"name"] isEqual:name];}];
 [u removeObjectsAtIndexes:same];
 [u addObject:@{@"name":name,@"g":gains}];
 d[@"userPresets"]=u; d[@"preset"]=@([self builtIns].count+u.count-1);
}
+ (NSArray *)importData:(NSData *)bytes name:(NSString **)name kind:(NSString **)kind {
 if(!bytes||bytes.length>1024*1024)return nil;
 id obj=[NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil];
 if([obj isKindOfClass:[NSDictionary class]]&&[obj[@"gains"] isKindOfClass:[NSArray class]]&&[obj[@"gains"] count]==12){
  NSMutableArray *g=[NSMutableArray array];
  for(id v in obj[@"gains"]){if(![v respondsToSelector:@selector(floatValue)]||!isfinite([v floatValue]))return nil;[g addObject:@(fmaxf(-12,fminf(12,[v floatValue])))];}
  if(kind)*kind=@"AmpEQ JSON"; if(name&&[obj[@"name"] isKindOfClass:[NSString class]])*name=obj[@"name"];
  return g;
 }
 NSString *text=[[NSString alloc] initWithData:bytes encoding:NSUTF8StringEncoding]?:[[NSString alloc] initWithData:bytes encoding:NSISOLatin1StringEncoding];
 float out[12]; char k[64]={0};
 if(!text||!aeq_import_text(text.UTF8String,out,k,sizeof k))return nil;
 NSMutableArray *g=[NSMutableArray array]; for(int i=0;i<12;i++)[g addObject:@(out[i])];
 if(kind)*kind=@(k);
 return g;
}
+ (UIImage *)thumbImage:(UIColor *)color diameter:(CGFloat)dia {
 UIGraphicsImageRenderer *r=[[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(dia+4,dia+4)];
 return [r imageWithActions:^(UIGraphicsImageRendererContext *c){
  CGContextRef g=c.CGContext;
  CGContextSetShadowWithColor(g,CGSizeMake(0,1),2,[UIColor colorWithWhite:0 alpha:0.3].CGColor);
  [[UIColor whiteColor] setFill]; CGContextFillEllipseInRect(g,CGRectMake(2,1,dia,dia));
  CGContextSetShadowWithColor(g,CGSizeZero,0,NULL);
  [color setFill]; CGContextFillEllipseInRect(g,CGRectMake(2+dia*0.28,1+dia*0.28,dia*0.44,dia*0.44));
 }];
}
@end
