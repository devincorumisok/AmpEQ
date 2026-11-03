#import <UIKit/UIKit.h>
#import "PreferencesCompat.h"
@interface AEQBaseListController : PSListController <UIDocumentPickerDelegate>
- (NSString *)plistName;
- (void)alert:(NSString *)t message:(NSString *)m;
- (void)aeqSliderMoved:(UISlider *)sl specifier:(PSSpecifier *)spec;
@end
