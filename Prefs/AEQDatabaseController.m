/* Headphone EQ database: 8,853 AutoEq corrections (MIT, github.com/jaakkopasanen/AutoEq),
 * pre-fitted to AmpEQ's 12 bands. Search by name or source, tap to apply. */
#import <UIKit/UIKit.h>
#import "PreferencesCompat.h"
#import <Preferences/PSViewController.h>
#import "AEQStore.h"
#include <math.h>

@interface AEQDatabaseController : PSViewController <UITableViewDataSource,UITableViewDelegate,UISearchBarDelegate>
@end
@implementation AEQDatabaseController {
 UISearchBar *_search; UITableView *_table; UILabel *_credit;
 NSArray<NSArray *> *_all; NSArray<NSArray *> *_shown;   /* row = @[name, source, kind, gainsString] */
}
+ (NSArray *)database {
 static NSArray *db; static dispatch_once_t once;
 dispatch_once(&once,^{
  NSString *p=[[NSBundle bundleForClass:self] pathForResource:@"headphones" ofType:@"tsv"];
  NSString *s=p?[NSString stringWithContentsOfFile:p encoding:NSUTF8StringEncoding error:nil]:nil;
  NSMutableArray *rows=[NSMutableArray arrayWithCapacity:9000];
  [s enumerateLinesUsingBlock:^(NSString *line,__unused BOOL *stop){
   NSArray *f=[line componentsSeparatedByString:@"\t"]; if(f.count==4)[rows addObject:f];
  }];
  db=rows;
 });
 return db;
}
- (void)viewDidLoad {
 [super viewDidLoad];
 self.title=@"Headphones";
 self.view.backgroundColor=[UIColor systemGroupedBackgroundColor];
 _search=[UISearchBar new]; _search.placeholder=@"Search headphones (e.g. K702)"; _search.delegate=self;
 _search.searchBarStyle=UISearchBarStyleMinimal; _search.autocorrectionType=UITextAutocorrectionTypeNo; _search.autocapitalizationType=UITextAutocapitalizationTypeNone;
 _table=[[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStyleInsetGrouped];
 _table.dataSource=self; _table.delegate=self; _table.keyboardDismissMode=UIScrollViewKeyboardDismissModeOnDrag;
 _credit=[UILabel new]; _credit.font=[UIFont systemFontOfSize:11]; _credit.textColor=[UIColor tertiaryLabelColor]; _credit.textAlignment=NSTextAlignmentCenter;
 for(UIView *v in @[_search,_table,_credit])[self.view addSubview:v];
 _all=[[self class] database]; _shown=_all;
 _credit.text=[NSString stringWithFormat:@"%lu measurements · AutoEq (MIT) · fitted to 12 bands",(unsigned long)_all.count];
}
- (void)viewDidLayoutSubviews {
 [super viewDidLayoutSubviews];
 UIEdgeInsets in=self.view.safeAreaInsets; CGFloat W=self.view.bounds.size.width, H=self.view.bounds.size.height;
 _search.frame=CGRectMake(4,in.top+4,W-8,48);
 _credit.frame=CGRectMake(12,H-in.bottom-22,W-24,18);
 _table.frame=CGRectMake(0,CGRectGetMaxY(_search.frame),W,CGRectGetMinY(_credit.frame)-CGRectGetMaxY(_search.frame)-2);
}
- (void)searchBar:(UISearchBar *)b textDidChange:(NSString *)text {
 NSArray *words=[[text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet] componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
 words=[words filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"length > 0"]];
 if(!words.count){_shown=_all;[_table reloadData];return;}
 NSStringCompareOptions o=NSCaseInsensitiveSearch|NSDiacriticInsensitiveSearch;
 NSMutableArray *r=[NSMutableArray array];
 for(NSArray *row in _all){
  NSString *hay=[NSString stringWithFormat:@"%@ %@ %@",row[0],row[1],row[2]]; BOOL ok=YES;
  for(NSString *w in words){NSString *w2=[w stringByReplacingOccurrencesOfString:@"-" withString:@""];
   if([hay rangeOfString:w options:o].location==NSNotFound&&
      [[hay stringByReplacingOccurrencesOfString:@" " withString:@""] rangeOfString:w2 options:o].location==NSNotFound){ok=NO;break;}}
  if(ok)[r addObject:row];
 }
 _shown=r; [_table reloadData];
}
- (void)searchBarSearchButtonClicked:(UISearchBar *)b { [b resignFirstResponder]; }
- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { return (NSInteger)_shown.count; }
- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)ip {
 UITableViewCell *c=[t dequeueReusableCellWithIdentifier:@"hp"]?:[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"hp"];
 NSArray *row=_shown[(NSUInteger)ip.row];
 c.textLabel.text=row[0]; c.textLabel.font=[UIFont systemFontOfSize:16 weight:UIFontWeightMedium];
 c.detailTextLabel.text=[NSString stringWithFormat:@"%@ · %@",row[1],row[2]]; c.detailTextLabel.textColor=[UIColor secondaryLabelColor];
 c.textLabel.adjustsFontSizeToFitWidth=YES; c.textLabel.minimumScaleFactor=0.75;
 return c;
}
- (void)tableView:(UITableView *)t didSelectRowAtIndexPath:(NSIndexPath *)ip {
 [t deselectRowAtIndexPath:ip animated:YES];
 NSArray *row=_shown[(NSUInteger)ip.row];
 NSString *title=[NSString stringWithFormat:@"%@",row[0]];
 UIAlertController *a=[UIAlertController alertControllerWithTitle:title
  message:[NSString stringWithFormat:@"%@ measurement. Sets the 12-band EQ, turns the EQ on and saves it as a ★ preset.",row[1]] preferredStyle:UIAlertControllerStyleAlert];
 [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
 [a addAction:[UIAlertAction actionWithTitle:@"Apply" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *x){[self apply:row];}]];
 [self presentViewController:a animated:YES completion:nil];
}
- (void)apply:(NSArray *)row {
 NSMutableArray *g=[NSMutableArray array];
 for(NSString *q in [row[3] componentsSeparatedByString:@","])[g addObject:@([q floatValue]/4.0f)];
 if(g.count!=12)return;
 NSMutableDictionary *d=[AEQStore load];
 NSString *src=[[row[1] componentsSeparatedByString:@" · "] firstObject]?:row[1];
 [AEQStore storeUserPreset:[NSString stringWithFormat:@"%@ (%@)",row[0],src] gains:g into:d];
 [AEQStore applyGains:g into:d]; d[@"preamp"]=@0; d[@"eqEnabled"]=@YES;
 [AEQStore save:d];
 UIAlertController *ok=[UIAlertController alertControllerWithTitle:@"Applied" message:row[0] preferredStyle:UIAlertControllerStyleAlert];
 [self presentViewController:ok animated:YES completion:^{
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(0.9*NSEC_PER_SEC)),dispatch_get_main_queue(),^{[ok dismissViewControllerAnimated:YES completion:nil];});}];
}
@end
