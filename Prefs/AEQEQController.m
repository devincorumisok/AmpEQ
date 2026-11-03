/* One-screen graphic EQ: live response curve (the exact filters + headroom
 * mediaserverd runs), 12 vertical sliders, preamp, Flat / Import / Save. */
#import <UIKit/UIKit.h>
#import "PreferencesCompat.h"
#import <Preferences/PSViewController.h>
#import "AEQStore.h"
#include <math.h>
#include "../EQDSP.h"
#include "../AmpEQShared.h"
#include "../AmpEQSettings.h"

static NSString *const kFreqLabels[12]={@"31",@"62",@"125",@"250",@"500",@"1k",@"2k",@"4k",@"6k",@"8k",@"12k",@"16k"};

@interface AEQCurveView : UIView
@property(nonatomic) float preamp;
- (void)setGains:(const float *)g;
@end
@implementation AEQCurveView { float _g[12]; }
- (void)setGains:(const float *)g { memcpy(_g,g,sizeof _g); [self setNeedsDisplay]; }
- (void)drawRect:(CGRect)rect {
 CGContextRef c=UIGraphicsGetCurrentContext(); CGRect b=CGRectInset(self.bounds,1,1);
 [[UIColor secondarySystemGroupedBackgroundColor] setFill];
 [[UIBezierPath bezierPathWithRoundedRect:b cornerRadius:12] fill];
 CGFloat x0=b.origin.x+8, w=b.size.width-16, y0=b.origin.y+8, h=b.size.height-28;
 float (^X)(float)=^float(float f){return x0+w*log10f(f/20.0f)/3.0f;};
 float (^Y)(float)=^float(float db){return y0+h*(0.5f-db/24.0f);};
 CGContextSetLineWidth(c,0.5);
 [[UIColor separatorColor] setStroke];
 for(int db=-12;db<=12;db+=6){CGContextMoveToPoint(c,x0,Y(db));CGContextAddLineToPoint(c,x0+w,Y(db));}
 for(float f=100;f<=10000;f*=10){CGContextMoveToPoint(c,X(f),y0);CGContextAddLineToPoint(c,X(f),y0+h);}
 CGContextStrokePath(c);
 /* curve: EQ response only (headroom shown as text, so the shape is readable) */
 UIBezierPath *p=[UIBezierPath bezierPath]; float peak=-99;
 for(int k=0;k<=160;k++){float f=20*powf(1000,k/160.0f), r=eq_response_db(_g,48000,f); if(r>peak)peak=r;
  CGPoint pt=CGPointMake(X(f),Y(fmaxf(-12,fminf(12,r)))); if(k)[p addLineToPoint:pt]; else [p moveToPoint:pt];}
 p.lineWidth=2.5; p.lineJoinStyle=kCGLineJoinRound; [self.tintColor setStroke]; [p stroke];
 float total=eq_auto_gain_db(_g,_preamp,48000);
 NSString *s=[NSString stringWithFormat:@"curve peak %+.1f dB · output gain %+.1f dB",fmaxf(0,peak),total];
 NSDictionary *a=@{NSFontAttributeName:[UIFont monospacedDigitSystemFontOfSize:11 weight:UIFontWeightMedium],NSForegroundColorAttributeName:[UIColor secondaryLabelColor]};
 CGSize sz=[s sizeWithAttributes:a];
 [s drawAtPoint:CGPointMake(b.origin.x+(b.size.width-sz.width)/2,y0+h+4) withAttributes:a];
}
@end

/* Peace-style peak meter: 100 % = the point where the output clips (0 dBFS, or lower when Gain is high) */
@interface AEQClipBar : UIView
- (void)setPercent:(float)pct clipped:(BOOL)clip running:(BOOL)run;
@end
@implementation AEQClipBar { float _pct, _hold; BOOL _clip, _run; CFTimeInterval _clipUntil; }
- (void)setPercent:(float)pct clipped:(BOOL)clip running:(BOOL)run {
 CFTimeInterval now=CACurrentMediaTime();
 _run=run; _pct=run?pct:0; _hold=fmaxf(_pct,_hold*0.92f);       /* smooth fall like a meter */
 if(clip)_clipUntil=now+1.5; _clip=now<_clipUntil;
 [self setNeedsDisplay];
}
- (void)drawRect:(CGRect)rect {
 CGRect b=self.bounds; CGFloat h=b.size.height;
 UIBezierPath *track=[UIBezierPath bezierPathWithRoundedRect:b cornerRadius:h/2];
 [[UIColor tertiarySystemFillColor] setFill]; [track fill];
 [track addClip];
 CGFloat frac=fminf(1.0f,_hold/110.0f);            /* scale ends at 110 % so 100 % has a visible mark */
 UIColor *c=(_clip||_hold>=100)?[UIColor systemRedColor]:(_hold>=90?[UIColor systemYellowColor]:[UIColor systemGreenColor]);
 [c setFill]; UIRectFill(CGRectMake(0,0,b.size.width*frac,h));
 CGFloat x100=b.size.width*100.0/110.0;                /* 100 % line */
 [[UIColor labelColor] setFill]; UIRectFill(CGRectMake(x100-0.5,0,1,h));
}
@end

@interface AEQEQController : PSViewController <UIDocumentPickerDelegate>
@end
@implementation AEQEQController {
 NSMutableDictionary *_d; AEQCurveView *_curve;
 UISlider *_s[12]; UILabel *_val[12], *_freq[12];
 UISlider *_pre; UILabel *_preLbl, *_preVal; UIStackView *_buttons;
 AEQClipBar *_bar; UILabel *_peakLbl, *_peakVal, *_protLbl, *_protVal; UISwitch *_protSw; UIButton *_protReset; NSTimer *_meterTimer;
}
- (void)viewDidLoad {
 [super viewDidLoad];
 self.title=@"Equalizer";
 self.view.backgroundColor=[UIColor systemGroupedBackgroundColor];
 _curve=[AEQCurveView new]; _curve.backgroundColor=[UIColor clearColor]; _curve.contentMode=UIViewContentModeRedraw;
 [self.view addSubview:_curve];
 UIImage *th=[AEQStore thumbImage:self.view.tintColor diameter:18];
 for(int i=0;i<12;i++){
  UISlider *s=[UISlider new]; s.minimumValue=-12; s.maximumValue=12; s.tag=i;
  [s setThumbImage:th forState:UIControlStateNormal];
  s.minimumTrackTintColor=self.view.tintColor; s.maximumTrackTintColor=[UIColor tertiarySystemFillColor];
  s.transform=CGAffineTransformMakeRotation(-M_PI_2);
  [s addTarget:self action:@selector(bandChanged:) forControlEvents:UIControlEventValueChanged];
  [self.view addSubview:s]; _s[i]=s;
  UILabel *v=[UILabel new]; v.font=[UIFont monospacedDigitSystemFontOfSize:10 weight:UIFontWeightSemibold]; v.textAlignment=NSTextAlignmentCenter;
  v.textColor=[UIColor labelColor]; v.adjustsFontSizeToFitWidth=YES; [self.view addSubview:v]; _val[i]=v;
  UILabel *f=[UILabel new]; f.font=[UIFont systemFontOfSize:10 weight:UIFontWeightMedium]; f.textAlignment=NSTextAlignmentCenter;
  f.textColor=[UIColor secondaryLabelColor]; f.text=kFreqLabels[i]; [self.view addSubview:f]; _freq[i]=f;
 }
 _preLbl=[UILabel new]; _preLbl.text=@"Preamp"; _preLbl.font=[UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
 _preVal=[UILabel new]; _preVal.font=[UIFont monospacedDigitSystemFontOfSize:15 weight:UIFontWeightRegular]; _preVal.textAlignment=NSTextAlignmentRight; _preVal.textColor=[UIColor secondaryLabelColor];
 _pre=[UISlider new]; _pre.minimumValue=-12; _pre.maximumValue=12; [_pre setThumbImage:th forState:UIControlStateNormal];
 [_pre addTarget:self action:@selector(preChanged:) forControlEvents:UIControlEventValueChanged];
 for(UIView *v in @[_preLbl,_preVal,_pre])[self.view addSubview:v];
 NSMutableArray *bs=[NSMutableArray array];
 for(NSArray *b in @[@[@"Flat",@"flat"],@[@"Import…",@"importEQ"],@[@"Save…",@"savePreset"]]){
  UIButton *x=[UIButton buttonWithType:UIButtonTypeSystem]; [x setTitle:b[0] forState:UIControlStateNormal];
  x.titleLabel.font=[UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
  x.backgroundColor=[UIColor secondarySystemGroupedBackgroundColor]; x.layer.cornerRadius=10;
  [x addTarget:self action:NSSelectorFromString(b[1]) forControlEvents:UIControlEventTouchUpInside]; [bs addObject:x];
 }
 _peakLbl=[UILabel new]; _peakLbl.text=@"Peak"; _peakLbl.font=[UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
 _bar=[AEQClipBar new]; _bar.backgroundColor=[UIColor clearColor]; _bar.contentMode=UIViewContentModeRedraw;
 _peakVal=[UILabel new]; _peakVal.font=[UIFont monospacedDigitSystemFontOfSize:15 weight:UIFontWeightRegular]; _peakVal.textAlignment=NSTextAlignmentRight; _peakVal.textColor=[UIColor secondaryLabelColor]; _peakVal.text=@"–";
 _protLbl=[UILabel new]; _protLbl.text=@"Prevent clipping"; _protLbl.font=[UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
 _protVal=[UILabel new]; _protVal.font=[UIFont monospacedDigitSystemFontOfSize:13 weight:UIFontWeightRegular]; _protVal.textColor=[UIColor secondaryLabelColor]; _protVal.textAlignment=NSTextAlignmentRight;
 _protReset=[UIButton buttonWithType:UIButtonTypeSystem]; [_protReset setTitle:@"Reset" forState:UIControlStateNormal];
 _protReset.titleLabel.font=[UIFont systemFontOfSize:15 weight:UIFontWeightSemibold]; [_protReset addTarget:self action:@selector(resetProtection) forControlEvents:UIControlEventTouchUpInside];
 _protSw=[UISwitch new]; [_protSw addTarget:self action:@selector(protChanged:) forControlEvents:UIControlEventValueChanged];
 for(UIView *v in @[_peakLbl,_bar,_peakVal,_protLbl,_protVal,_protReset,_protSw])[self.view addSubview:v];
 _buttons=[[UIStackView alloc] initWithArrangedSubviews:bs]; _buttons.distribution=UIStackViewDistributionFillEqually; _buttons.spacing=8;
 [self.view addSubview:_buttons];
}
- (void)viewWillAppear:(BOOL)a { [super viewWillAppear:a]; _d=[AEQStore load]; [self refresh];
 _meterTimer=[NSTimer scheduledTimerWithTimeInterval:0.1 target:self selector:@selector(meterTick) userInfo:nil repeats:YES]; }
- (void)viewWillDisappear:(BOOL)a { [super viewWillDisappear:a]; [_meterTimer invalidate]; _meterTimer=nil; }
- (void)meterTick {
 uint64_t m=aeq_state(AEQ_METER);
 BOOL run=(m>>49)&1, clip=(m>>16)&1; float rel=((int)(m&0xFFFF)-10000)/100.0f;
 float pct=run?100.0f*powf(10.0f,rel/20.0f):0;
 [_bar setPercent:pct clipped:clip running:run];
 _peakVal.text=run?[NSString stringWithFormat:@"%.0f %%",fminf(pct,999)]:@"–";
 _peakVal.textColor=(clip||pct>=100)?[UIColor systemRedColor]:[UIColor secondaryLabelColor];
 int red=(int)((m>>20)&0xFFF);
 _protVal.text=red?[NSString stringWithFormat:@"−%.1f dB",red/100.0f]:@"";
 _protReset.hidden=red==0;
}
- (void)protChanged:(UISwitch *)sw { _d[@"clipProtect"]=@(sw.on); [AEQStore save:_d]; }
- (void)resetProtection { _d[@"protReset"]=@(([_d[@"protReset"] intValue]+1)&15); [AEQStore save:_d]; }
- (void)refresh {
 float g[12];
 for(int i=0;i<12;i++){g[i]=[_d[[NSString stringWithFormat:@"g%d",i]] floatValue]; _s[i].value=g[i];
  _val[i].text=fabsf(g[i])<0.01f?@"0":[NSString stringWithFormat:@"%+g",g[i]];}
 _protSw.on=_d[@"clipProtect"]?[_d[@"clipProtect"] boolValue]:YES;
 float p=[_d[@"preamp"] floatValue]; _pre.value=p; _preVal.text=[NSString stringWithFormat:@"%+.1f dB",p];
 _curve.preamp=p; [_curve setGains:g];
}
- (void)viewDidLayoutSubviews {
 [super viewDidLayoutSubviews];
 UIEdgeInsets in=self.view.safeAreaInsets; CGFloat W=self.view.bounds.size.width, H=self.view.bounds.size.height;
 CGFloat m=12, top=in.top+10, bottom=H-in.bottom-10, w=W-2*m;
 CGFloat btnH=42, preH=34, curveH=MIN(132,MAX(96,(bottom-top)*0.24));
 _buttons.frame=CGRectMake(m,bottom-btnH,w,btnH);
 CGFloat protY=CGRectGetMinY(_buttons.frame)-8-preH, meterY=protY-preH;
 _protLbl.frame=CGRectMake(m+4,protY,140,preH);
 _protSw.frame=CGRectMake(W-m-51,protY+(preH-31)/2,51,31);
 _protReset.frame=CGRectMake(CGRectGetMinX(_protSw.frame)-58,protY,52,preH);
 _protVal.frame=CGRectMake(CGRectGetMinX(_protReset.frame)-64,protY,60,preH);
 _peakLbl.frame=CGRectMake(m+4,meterY,66,preH); _peakVal.frame=CGRectMake(W-m-74,meterY,72,preH);
 _bar.frame=CGRectMake(m+72,meterY+(preH-10)/2,w-72-80,10);
 CGFloat preY=meterY-preH;
 _preLbl.frame=CGRectMake(m+4,preY,66,preH); _preVal.frame=CGRectMake(W-m-74,preY,72,preH);
 _pre.frame=CGRectMake(m+70,preY,w-70-78,preH);
 _curve.frame=CGRectMake(m,top,w,curveH);
 CGFloat valY=CGRectGetMaxY(_curve.frame)+8, freqY=preY-8-14;
 CGFloat sTop=valY+16, sH=freqY-4-sTop, col=w/12.0;
 for(int i=0;i<12;i++){
  CGFloat cx=m+col*(i+0.5);
  _val[i].frame=CGRectMake(cx-col/2,valY,col,14);
  _freq[i].frame=CGRectMake(cx-col/2,freqY,col,14);
  _s[i].bounds=CGRectMake(0,0,MAX(60,sH),30); _s[i].center=CGPointMake(cx,sTop+sH/2);
 }
}
- (void)bandChanged:(UISlider *)s {
 float v=roundf(s.value*2)/2; NSString *k=[NSString stringWithFormat:@"g%ld",(long)s.tag];
 if(fabsf([_d[k] floatValue]-v)<0.01f&&_d[k])return;
 _d[k]=@(v); _d[@"preset"]=@(-1); [AEQStore save:_d]; [self refreshKeepingSlider:s];
}
- (void)refreshKeepingSlider:(UISlider *)keep {
 float g[12]; for(int i=0;i<12;i++){g[i]=[_d[[NSString stringWithFormat:@"g%d",i]] floatValue];
  _val[i].text=fabsf(g[i])<0.01f?@"0":[NSString stringWithFormat:@"%+g",g[i]];}
 _curve.preamp=[_d[@"preamp"] floatValue]; [_curve setGains:g];
}
- (void)preChanged:(UISlider *)s {
 float v=roundf(s.value*2)/2; if(fabsf([_d[@"preamp"] floatValue]-v)<0.01f&&_d[@"preamp"])return;
 _d[@"preamp"]=@(v); _d[@"protReset"]=@(([_d[@"protReset"] intValue]+1)&15); _preVal.text=[NSString stringWithFormat:@"%+.1f dB",v]; [AEQStore save:_d]; [self refreshKeepingSlider:s];
}
- (void)flat { [AEQStore applyGains:@[] into:_d]; _d[@"preamp"]=@0; _d[@"preset"]=@0; _d[@"protReset"]=@(([_d[@"protReset"] intValue]+1)&15); [AEQStore save:_d]; [self refresh]; }
- (void)alert:(NSString *)t message:(NSString *)m {
 UIAlertController *a=[UIAlertController alertControllerWithTitle:t message:m preferredStyle:UIAlertControllerStyleAlert];
 [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
 [self presentViewController:a animated:YES completion:nil];
}
- (void)savePreset {
 UIAlertController *a=[UIAlertController alertControllerWithTitle:@"Save preset" message:nil preferredStyle:UIAlertControllerStyleAlert];
 [a addTextFieldWithConfigurationHandler:^(UITextField *t){t.placeholder=@"Name";}];
 [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
 [a addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *x){
  NSString *n=[a.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
  [AEQStore storeUserPreset:n.length?n:@"Custom" gains:nil into:self->_d]; [AEQStore save:self->_d];}]];
 [self presentViewController:a animated:YES completion:nil];
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
 [AEQStore storeUserPreset:name gains:g into:_d]; [AEQStore applyGains:g into:_d]; [AEQStore save:_d]; [self refresh];
 [self alert:@"Imported" message:[NSString stringWithFormat:@"“%@” (%@), fitted to 12 bands.",name,kind]];
}
@end
