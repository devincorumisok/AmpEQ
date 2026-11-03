/* Presets: built-ins (fixed) + your ★ presets (swipe left to delete). Tap to apply. */
#import <UIKit/UIKit.h>
#import "PreferencesCompat.h"
#import <Preferences/PSViewController.h>
#import "AEQStore.h"

@interface AEQPresetsController : PSViewController <UITableViewDataSource,UITableViewDelegate>
@end
@implementation AEQPresetsController { UITableView *_table; NSMutableDictionary *_d; }
- (void)viewDidLoad {
 [super viewDidLoad];
 self.title=@"Presets";
 _table=[[UITableView alloc] initWithFrame:self.view.bounds style:UITableViewStyleInsetGrouped];
 _table.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
 _table.dataSource=self; _table.delegate=self;
 [self.view addSubview:_table];
}
- (void)viewWillAppear:(BOOL)a { [super viewWillAppear:a]; _d=[AEQStore load]; [_table reloadData]; }
- (NSArray *)builtIns { return [AEQStore builtIns]; }
- (NSArray *)mine { return [AEQStore userPresets:_d]; }
- (NSInteger)current { id p=_d[@"preset"]; return p?[p integerValue]:0; }
- (NSInteger)numberOfSectionsInTableView:(UITableView *)t { return [self mine].count?2:1; }
- (NSString *)tableView:(UITableView *)t titleForHeaderInSection:(NSInteger)s { return s?@"My presets":@"Built-in"; }
- (NSString *)tableView:(UITableView *)t titleForFooterInSection:(NSInteger)s { return s?@"Swipe left to delete.":nil; }
- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { return (NSInteger)(s?[self mine].count:[self builtIns].count); }
- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)ip {
 UITableViewCell *c=[t dequeueReusableCellWithIdentifier:@"p"]?:[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:@"p"];
 NSArray *list=ip.section?[self mine]:[self builtIns];
 c.textLabel.text=list[(NSUInteger)ip.row][@"name"]?:@"Custom";
 c.textLabel.adjustsFontSizeToFitWidth=YES; c.textLabel.minimumScaleFactor=0.75;
 NSInteger idx=ip.section?(NSInteger)[self builtIns].count+ip.row:ip.row;
 c.accessoryType=idx==[self current]?UITableViewCellAccessoryCheckmark:UITableViewCellAccessoryNone;
 return c;
}
- (void)tableView:(UITableView *)t didSelectRowAtIndexPath:(NSIndexPath *)ip {
 [t deselectRowAtIndexPath:ip animated:YES];
 NSArray *list=ip.section?[self mine]:[self builtIns];
 [AEQStore applyGains:list[(NSUInteger)ip.row][@"g"] into:_d];
 _d[@"preamp"]=@0; _d[@"preset"]=@(ip.section?(NSInteger)[self builtIns].count+ip.row:ip.row);
 [AEQStore save:_d]; [t reloadData];
}
- (BOOL)tableView:(UITableView *)t canEditRowAtIndexPath:(NSIndexPath *)ip { return ip.section==1; }
- (void)tableView:(UITableView *)t commitEditingStyle:(UITableViewCellEditingStyle)style forRowAtIndexPath:(NSIndexPath *)ip {
 if(style!=UITableViewCellEditingStyleDelete||ip.section!=1)return;
 NSMutableArray *u=[[self mine] mutableCopy]; if((NSUInteger)ip.row>=u.count)return;
 [u removeObjectAtIndex:(NSUInteger)ip.row];
 NSInteger del=(NSInteger)[self builtIns].count+ip.row, cur=[self current];
 if(cur==del)_d[@"preset"]=@(-1);            /* curve stays as it is, shown as Custom */
 else if(cur>del)_d[@"preset"]=@(cur-1);
 if(u.count)_d[@"userPresets"]=u; else [_d removeObjectForKey:@"userPresets"];
 [AEQStore save:_d];
 if(u.count)[t deleteRowsAtIndexPaths:@[ip] withRowAnimation:UITableViewRowAnimationAutomatic];
 else [t reloadData];
}
@end
