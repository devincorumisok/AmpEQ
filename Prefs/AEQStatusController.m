/* Live status: the staged report as scrollable text, refreshed every 2 s. */
#import <UIKit/UIKit.h>
#import "PreferencesCompat.h"
#import <Preferences/PSViewController.h>
#include <notify.h>
#include "../AmpEQShared.h"
#include "../AmpEQSettings.h"
#include "../AmpEQStatus.h"

@interface AEQStatusController : PSViewController
@end
@implementation AEQStatusController { UITextView *_tv; UIStackView *_bar; NSTimer *_t; }
- (void)viewDidLoad {
 [super viewDidLoad];
 self.title=@"Status";
 self.view.backgroundColor=[UIColor systemGroupedBackgroundColor];
 _tv=[UITextView new]; _tv.editable=NO; _tv.font=[UIFont monospacedSystemFontOfSize:10.5 weight:UIFontWeightRegular];
 _tv.backgroundColor=[UIColor secondarySystemGroupedBackgroundColor]; _tv.layer.cornerRadius=12; _tv.textContainerInset=UIEdgeInsetsMake(10,8,10,8);
 [self.view addSubview:_tv];
 NSMutableArray *bs=[NSMutableArray array];
 for(NSArray *b in @[@[@"Reconnect",@"reconnect"],@[@"Copy",@"copyReport"]]){
  UIButton *x=[UIButton buttonWithType:UIButtonTypeSystem]; [x setTitle:b[0] forState:UIControlStateNormal];
  x.titleLabel.font=[UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
  x.backgroundColor=[UIColor secondarySystemGroupedBackgroundColor]; x.layer.cornerRadius=10;
  [x addTarget:self action:NSSelectorFromString(b[1]) forControlEvents:UIControlEventTouchUpInside]; [bs addObject:x];
 }
 _bar=[[UIStackView alloc] initWithArrangedSubviews:bs]; _bar.distribution=UIStackViewDistributionFillEqually; _bar.spacing=8;
 [self.view addSubview:_bar];
}
- (void)viewDidLayoutSubviews {
 [super viewDidLayoutSubviews];
 UIEdgeInsets in=self.view.safeAreaInsets; CGFloat W=self.view.bounds.size.width, H=self.view.bounds.size.height;
 _bar.frame=CGRectMake(12,H-in.bottom-10-42,W-24,42);
 _tv.frame=CGRectMake(12,in.top+10,W-24,CGRectGetMinY(_bar.frame)-10-(in.top+10));
}
- (NSString *)report { static char b[16384]; aeq_status_report(b,sizeof b,0); return [NSString stringWithUTF8String:b]?:@""; }
- (void)refresh { CGPoint o=_tv.contentOffset; _tv.text=[self report]; [_tv setContentOffset:o animated:NO]; }
- (void)viewWillAppear:(BOOL)a { [super viewWillAppear:a]; [self refresh];
 _t=[NSTimer scheduledTimerWithTimeInterval:2 repeats:YES block:^(__unused NSTimer *t){[self refresh];}]; }
- (void)viewWillDisappear:(BOOL)a { [super viewWillDisappear:a]; [_t invalidate]; _t=nil; }
- (void)reconnect { aeq_settings_publish_saved(); notify_post(AEQ_NOTE_RECONNECT);
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(1.2*NSEC_PER_SEC)),dispatch_get_main_queue(),^{[self refresh];}); }
- (void)copyReport { [UIPasteboard generalPasteboard].string=[self report]; }
@end
