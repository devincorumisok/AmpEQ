/* AmpEQ Settings: main page, "More" page and the logo header. */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import "PreferencesCompat.h"
#import <Preferences/PSHeaderFooterView.h>
#import "AEQStore.h"
#import "AEQBase.h"
#include <notify.h>
#include <spawn.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
extern char **environ;
#include <math.h>
#include "../AmpEQShared.h"
#include "../AmpEQSettings.h"
#include <sys/stat.h>

/* ---------------- logo header ---------------- */
@interface AEQHeaderView : UIView <PSHeaderFooterView>
@end
@implementation AEQHeaderView {
 UIImageView *_art; UILabel *_title, *_sub;
}
- (instancetype)initWithSpecifier:(PSSpecifier *)specifier {
 if((self=[super initWithFrame:CGRectMake(0,0,320,108)])){
  NSBundle *b=[NSBundle bundleForClass:[AEQHeaderView class]];
  _art=[[UIImageView alloc] initWithImage:[UIImage imageNamed:@"logo" inBundle:b compatibleWithTraitCollection:nil]];
  _art.contentMode=UIViewContentModeScaleAspectFit;
  _title=[UILabel new]; _title.text=@"AmpEQ";
  UIFontDescriptor *fd=[[UIFont systemFontOfSize:30 weight:UIFontWeightHeavy].fontDescriptor fontDescriptorWithDesign:UIFontDescriptorSystemDesignRounded];
  _title.font=fd?[UIFont fontWithDescriptor:fd size:30]:[UIFont systemFontOfSize:30 weight:UIFontWeightHeavy];
  _title.textColor=[UIColor labelColor]; _title.adjustsFontSizeToFitWidth=YES;
  _sub=[UILabel new]; _sub.numberOfLines=2; _sub.text=@"12-band EQ\nHeadphone gain";
  _sub.font=[UIFont systemFontOfSize:13 weight:UIFontWeightMedium]; _sub.textColor=[UIColor secondaryLabelColor];
  for(UIView *v in @[_art,_title,_sub])[self addSubview:v];
 }
 return self;
}
- (CGFloat)preferredHeightForWidth:(CGFloat)width { return 108; }
- (CGFloat)preferredHeightForWidth:(CGFloat)width inTableView:(UITableView *)tableView { return 108; }
- (void)layoutSubviews {
 [super layoutSubviews];
 CGFloat w=self.bounds.size.width, artW=MIN(176,w*0.52), artH=artW*150.0/280.0;
 CGFloat x=MAX(12,self.safeAreaInsets.left+8);
 _art.frame=CGRectMake(x,(108-artH)/2+2,artW,artH);
 CGFloat tx=CGRectGetMaxX(_art.frame)+8, tw=w-tx-x;
 _title.frame=CGRectMake(tx,26,tw,34);
 _sub.frame=CGRectMake(tx,60,tw,34);
}
@end

/* ---------------- live slider ---------------- */
#import <Preferences/PSSliderTableCell.h>
@interface AEQSliderCell : PSSliderTableCell
@end
@implementation AEQSliderCell
- (void)refreshCellContentsWithSpecifier:(PSSpecifier *)spec {
 [super refreshCellContentsWithSpecifier:spec];
 UISlider *sl=(UISlider *)self.control;
 if([sl isKindOfClass:[UISlider class]]){
  sl.continuous=YES;
  if(![[sl actionsForTarget:self forControlEvent:UIControlEventValueChanged] containsObject:@"aeqMoved:"])
   [sl addTarget:self action:@selector(aeqMoved:) forControlEvents:UIControlEventValueChanged];
 }
}
- (void)aeqMoved:(UISlider *)sl {
 id t=self.specifier.target;
 if([t respondsToSelector:@selector(aeqSliderMoved:specifier:)])[t aeqSliderMoved:sl specifier:self.specifier];
}
@end

/* ---------------- shared list behaviour ---------------- */
#import "AEQBase.h"
@implementation AEQBaseListController
- (NSString *)plistName { return @"Root"; }
- (NSArray *)specifiers {
 if(!_specifiers)_specifiers=[self loadSpecifiersFromPlistName:[self plistName] target:self];
 return _specifiers;
}
- (void)viewWillAppear:(BOOL)a { [super viewWillAppear:a]; [self reloadSpecifiers]; }
- (void)alert:(NSString *)t message:(NSString *)m {
 UIAlertController *a=[UIAlertController alertControllerWithTitle:t message:m preferredStyle:UIAlertControllerStyleAlert];
 [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
 [self presentViewController:a animated:YES completion:nil];
}
- (id)readPreferenceValue:(PSSpecifier *)spec {
 NSString *key=[spec propertyForKey:@"key"]; if(!key)return nil;
 id v=[AEQStore load][key];
 return v?:([spec propertyForKey:@"default"]?:@0);
}
- (void)setPreferenceValue:(id)value specifier:(PSSpecifier *)spec {
 NSString *key=[spec propertyForKey:@"key"]; if(!key)return;
 NSMutableDictionary *d=[AEQStore load];
 static NSSet *plgBool, *plgNum;
 if(!plgBool){plgBool=[NSSet setWithArray:@[@"plgDRX",@"plgGX",@"plgXF",@"plgField",@"plgPitch",@"plgConv",@"plgBalance",@"pitchLimiter",@"gainDetailed",@"hpDetect"]];
  plgNum=[NSSet setWithArray:@[@"drxBass",@"drxMids",@"drxTreble",@"drxGain",@"gxBass",@"gxMids",@"gxDelay",@"gxBassHz",@"gxMidsHz",@"gxGain",@"xfLevel",@"fieldWidth",@"pitchGain",@"convLevel",@"balance"]];}
 if([plgBool containsObject:key]){d[key]=@([value boolValue]);[AEQStore save:d];if([key isEqualToString:@"gainDetailed"])[self reloadSpecifiers];return;}
 if([key isEqualToString:@"volSteps"]){d[key]=@(MAX(16,MIN(100,[value intValue])));[AEQStore save:d];return;}
 if([key isEqualToString:@"volNumber"]){d[key]=@([value boolValue]?1:0);[AEQStore save:d];return;}
 if([plgNum containsObject:key]){
  float x=[value floatValue]; if(!isfinite(x))x=0;
  NSNumber *mn=[spec propertyForKey:@"min"], *mx=[spec propertyForKey:@"max"]; if(mn)x=fmaxf(x,mn.floatValue); if(mx)x=fminf(x,mx.floatValue);
  float stp=[[spec propertyForKey:@"step"] floatValue]; if(stp>0)x=roundf(x/stp)*stp;
  d[key]=@(x); [AEQStore save:d];
  PSSpecifier *l=[self specifierForID:[@"lbl_" stringByAppendingString:key]]; if(l)[self reloadSpecifier:l animated:NO];
  return;
 }
 if([key isEqualToString:@"preset"]){
  NSInteger i=[value integerValue]; NSArray *all=[AEQStore allPresets:d];
  if(i<0||i>=(NSInteger)all.count)return;
  [AEQStore applyGains:all[i][@"g"] into:d]; d[@"preset"]=@(i); d[@"protReset"]=@(([d[@"protReset"] intValue]+1)&15);
 }
 else if([key isEqualToString:@"eqEnabled"]||[key isEqualToString:@"ampObserve"])d[key]=@([value boolValue]);
 else if([key isEqualToString:@"target"])d[key]=@(MAX(0,MIN(3,[value integerValue])));
 else if([key isEqualToString:@"ampBoost"]){
  NSInteger nv=MAX(0,MIN(AEQ_BOOST_HW_MAX,[value integerValue]));
  if(nv>AEQ_GAIN_CLEAN_MAX&&[d[@"ampBoost"] integerValue]<=AEQ_GAIN_CLEAN_MAX){[self confirmClip:nv];return;}
  d[key]=@(nv);
 }
 else if([key isEqualToString:@"ampeqEnabled"]){
  d[key]=@([value boolValue]); [AEQStore save:d];
  UIAlertController *a=[UIAlertController alertControllerWithTitle:[value boolValue]?@"AmpEQ on":@"AmpEQ off"
   message:@"Respring to apply." preferredStyle:UIAlertControllerStyleAlert];
  [a addAction:[UIAlertAction actionWithTitle:@"Later" style:UIAlertActionStyleCancel handler:nil]];
  [a addAction:[UIAlertAction actionWithTitle:@"Respring" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *x){[self respring];}]];
  [self presentViewController:a animated:YES completion:nil];
  return;
 }
 [AEQStore save:d];
}
/* value shown on the right of a plugin row title (the slider itself shows none, so nothing is cut off) */
- (NSString *)valueText:(float)x spec:(PSSpecifier *)spec {
 NSString *fmt=[spec propertyForKey:@"fmt"]?:@"%.1f";
 if([fmt isEqualToString:@"%%"])return [NSString stringWithFormat:@"%.0f %%",x*100];
 if([fmt isEqualToString:@"bal"]){int b=(int)lrintf(x); return b==0?@"Center":[NSString stringWithFormat:@"%@ %d",b<0?@"L":@"R",abs(b)];}
 if(fabsf(x)<1e-4f)x=0;
 NSString *r=[NSString stringWithFormat:fmt,x];
 return [r stringByReplacingOccurrencesOfString:@"-" withString:@"\u2212"];
}
- (NSString *)valueLabel:(PSSpecifier *)spec {
 NSString *k=[spec propertyForKey:@"valueKey"];
 id v=k?[AEQStore load][k]:nil; float x=v?[v floatValue]:[[spec propertyForKey:@"default"] floatValue];
 return [self valueText:x spec:spec];
}
/* called by AEQSliderCell on every movement: updates the number while dragging (saving happens on release) */
- (void)aeqSliderMoved:(UISlider *)sl specifier:(PSSpecifier *)spec {
 NSString *k=[spec propertyForKey:@"key"]; if(!k)return;
 float x=sl.value, stp=[[spec propertyForKey:@"step"] floatValue]; if(stp>0)x=roundf(x/stp)*stp;
 PSSpecifier *l=[self specifierForID:[@"lbl_" stringByAppendingString:k]]; if(!l)return;
 PSTableCell *c=[self cachedCellForSpecifier:l];
 NSString *t=[self valueText:x spec:l];
 if(c){c.detailTextLabel.text=t; [c setNeedsLayout];}
}
/* Reset: every setting on this page except the Enable switch and the chosen IR file */
- (void)resetPlugin {
 UIAlertController *a=[UIAlertController alertControllerWithTitle:@"Reset to defaults?" message:@"All settings on this page go back to their defaults." preferredStyle:UIAlertControllerStyleAlert];
 [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
 [a addAction:[UIAlertAction actionWithTitle:@"Reset" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *x){
  NSMutableDictionary *d=[AEQStore load];
  for(PSSpecifier *sp in self.specifiers){
   NSString *k=[sp propertyForKey:@"key"]; if(!k||[k hasPrefix:@"plg"])continue;
   [d removeObjectForKey:k];
  }
  [AEQStore save:d]; [self reloadSpecifiers];
 }]];
 [self presentViewController:a animated:YES completion:nil];
}
/* list data sources */
- (NSArray *)presetTitles {
 NSMutableArray *t=[NSMutableArray array]; NSDictionary *d=[AEQStore load];
 NSUInteger nb=[AEQStore builtIns].count, i=0;
 for(NSDictionary *p in [AEQStore allPresets:d]){[t addObject:i<nb?p[@"name"]:[@"★ " stringByAppendingString:p[@"name"]?:@"Custom"]];i++;}
 [t addObject:@"Custom"]; return t;
}
- (NSArray *)presetValues {
 NSMutableArray *v=[NSMutableArray array]; NSUInteger n=[AEQStore allPresets:[AEQStore load]].count;
 for(NSUInteger i=0;i<n;i++)[v addObject:@(i)];
 [v addObject:@(-1)]; return v;
}
- (BOOL)gainDetailed { return [[AEQStore load][@"gainDetailed"] boolValue]; }
- (NSArray *)boostValues {
 if(![self gainDetailed])return @[@(AEQ_GAIN_LOW),@(AEQ_GAIN_MID),@(AEQ_GAIN_HIGH)];
 NSMutableArray *v=[NSMutableArray array];
 for(int i=0;i<=AEQ_BOOST_HW_MAX;i++)[v addObject:@(i)]; return v;
}
- (NSArray *)boostTitles {
 if(![self gainDetailed])return @[@"Low · stock",@"Middle · +5.5 dB",@"High · +10.5 dB"];
 NSMutableArray *t=[NSMutableArray array];
 for(int i=0;i<=AEQ_BOOST_HW_MAX;i++){
  if(i==0){[t addObject:@"Off · stock"];continue;}
  NSString *s=[NSString stringWithFormat:@"+%.1f dB",i*0.5];
  if(i==AEQ_GAIN_MID)s=[s stringByAppendingString:@" · Middle"];
  else if(i==AEQ_GAIN_HIGH)s=[s stringByAppendingString:@" · High"];
  else if(i==AEQ_GAIN_CLEAN_MAX)s=[s stringByAppendingString:@" · max clean"];
  else if(i>AEQ_GAIN_CLEAN_MAX)s=[s stringByAppendingString:@" · may clip"];
  [t addObject:s];
 }
 return t;
}
- (void)confirmClip:(NSInteger)steps {
 UIAlertController *a=[UIAlertController alertControllerWithTitle:@"May cause clipping"
  message:[NSString stringWithFormat:@"+%.1f dB clips song peaks above %.1f dBFS (measured: clean up to +11.0 dB). Keep Prevent clipping on in Equalizer.",steps*0.5,11.0-steps*0.5]
  preferredStyle:UIAlertControllerStyleAlert];
 [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(__unused UIAlertAction *x){[self reloadSpecifiers];}]];
 [a addAction:[UIAlertAction actionWithTitle:@"Use it" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *x){
  NSMutableDictionary *d=[AEQStore load]; d[@"ampBoost"]=@(steps); [AEQStore save:d]; [self reloadSpecifiers];}]];
 [self presentViewController:a animated:YES completion:nil];
}
- (void)respring {
 const char *tools[]={"/var/jb/usr/bin/sbreload","/var/jb/usr/bin/killall",NULL};
 for(int i=0;tools[i];i++){
  pid_t pid; char *argv[]={(char*)tools[i],i?"SpringBoard":NULL,NULL};
  if(posix_spawn(&pid,tools[i],NULL,NULL,argv,environ)==0){int st=0;waitpid(pid,&st,0);if(WIFEXITED(st)&&WEXITSTATUS(st)==0)return;}
 }
 [self alert:@"Couldn't respring" message:@"Respring manually (e.g. from your jailbreak app)."];
}
/* actions */
- (void)reconnect {
 aeq_settings_publish_saved(); notify_post(AEQ_NOTE_RECONNECT);
 [self alert:@"Reconnected" message:@"Settings re-sent to the audio service."];
}
- (void)importEQ {
 UIDocumentPickerViewController *p=[[UIDocumentPickerViewController alloc] initWithDocumentTypes:@[@"public.text",@"public.plain-text",@"public.json",@"public.data"] inMode:UIDocumentPickerModeImport];
 p.delegate=self; [self presentViewController:p animated:YES completion:nil];
}
- (void)documentPicker:(__unused UIDocumentPickerViewController *)c didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
 NSURL *url=urls.firstObject; if(!url)return;
 BOOL sec=[url startAccessingSecurityScopedResource]; NSData *bytes=[NSData dataWithContentsOfURL:url]; if(sec)[url stopAccessingSecurityScopedResource];
 NSString *name=[[url lastPathComponent] stringByDeletingPathExtension]?:@"Imported", *kind=nil;
 NSArray *g=[AEQStore importData:bytes name:&name kind:&kind];
 if(!g){[self alert:@"Can't import" message:@"Use an AutoEQ GraphicEQ or ParametricEQ file, or AmpEQ JSON."];return;}
 NSMutableDictionary *d=[AEQStore load]; [AEQStore storeUserPreset:name gains:g into:d]; [AEQStore applyGains:g into:d];
 [AEQStore save:d]; [self reloadSpecifiers];
 [self alert:@"Imported" message:[NSString stringWithFormat:@"“%@” (%@), fitted to 12 bands.",name,kind]];
}
- (void)saveAsPreset {
 UIAlertController *a=[UIAlertController alertControllerWithTitle:@"Save preset" message:nil preferredStyle:UIAlertControllerStyleAlert];
 [a addTextFieldWithConfigurationHandler:^(UITextField *t){t.placeholder=@"Name";}];
 [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
 [a addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *x){
  NSString *n=[a.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
  NSMutableDictionary *d=[AEQStore load]; [AEQStore storeUserPreset:n.length?n:@"Custom" gains:nil into:d]; [AEQStore save:d]; [self reloadSpecifiers];
 }]];
 [self presentViewController:a animated:YES completion:nil];
}
- (void)deleteUserPresets {
 UIAlertController *a=[UIAlertController alertControllerWithTitle:@"Delete all ★ presets?" message:nil preferredStyle:UIAlertControllerStyleAlert];
 [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
 [a addAction:[UIAlertAction actionWithTitle:@"Delete" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *x){
  NSMutableDictionary *d=[AEQStore load]; [d removeObjectForKey:@"userPresets"]; d[@"preset"]=@(-1); [AEQStore save:d]; [self reloadSpecifiers];}]];
 [self presentViewController:a animated:YES completion:nil];
}
@end

@interface AEQRootListController : AEQBaseListController
@end
@implementation AEQRootListController { NSTimer *_hpTimer; int _lastRoute; }
- (NSString *)plistName { return @"Root"; }
- (void)viewDidLoad { [super viewDidLoad]; aeq_settings_publish_saved(); _lastRoute=-1; }
- (void)viewDidAppear:(BOOL)a { [super viewDidAppear:a];
 _hpTimer=[NSTimer scheduledTimerWithTimeInterval:1 target:self selector:@selector(hpTick) userInfo:nil repeats:YES]; }
- (void)viewWillDisappear:(BOOL)a { [super viewWillDisappear:a]; [_hpTimer invalidate]; _hpTimer=nil; }
- (void)hpTick { int r=(int)(aeq_state(AEQ_ROUTE)&3); if(r!=_lastRoute){_lastRoute=r; PSSpecifier *s=[self specifierForID:@"hpState"]; if(s)[self reloadSpecifier:s animated:NO];} }
- (NSString *)headphoneState:(PSSpecifier *)spec {
 NSDictionary *d=[AEQStore load];
 if(d[@"hpDetect"]&&![d[@"hpDetect"] boolValue])return @"Detection off";
 switch((int)(aeq_state(AEQ_ROUTE)&3)){ case 1: return [d[@"ampBoost"] intValue]?@"Plugged in · Gain on":@"Plugged in";
  case 2: return @"Not plugged in · Gain off"; default: return @"Checking…"; }
}
@end

@interface AEQMoreController : AEQBaseListController
@end
@implementation AEQMoreController
- (NSString *)plistName { return @"More"; }
@end
