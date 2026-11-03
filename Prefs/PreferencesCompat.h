/* Compile with Theos' official Preferences headers when present. Minimal
 * fallback declarations avoid requiring a separately downloaded headers ZIP.
 * Preferences.framework linkage uses the local, link-only Vendor .tbd. */
#import <UIKit/UIKit.h>
#if __has_include(<Preferences/PSListController.h>) && __has_include(<Preferences/PSSpecifier.h>)
#import <Preferences/PSListController.h>
#import <Preferences/PSSpecifier.h>
#else
@interface PSSpecifier : NSObject
- (id)propertyForKey:(NSString *)key;
@end
@interface PSListController : UIViewController {
@protected
 NSArray *_specifiers;
}
- (NSArray *)loadSpecifiersFromPlistName:(NSString *)name target:(id)target;
- (void)reloadSpecifiers;
@end
#endif
