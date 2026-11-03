/* Shared storage + preset/import logic for every AmpEQ settings screen. */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

@interface AEQStore : NSObject
+ (NSMutableDictionary *)load;
+ (BOOL)save:(NSDictionary *)d;                 /* writes plist + publishes to mediaserverd */
+ (NSArray *)builtIns;
+ (NSArray *)userPresets:(NSDictionary *)d;
+ (NSArray *)allPresets:(NSDictionary *)d;
+ (void)applyGains:(NSArray *)g into:(NSMutableDictionary *)d;
+ (void)storeUserPreset:(NSString *)name gains:(NSArray *)gains into:(NSMutableDictionary *)d;
+ (NSArray *)gainsFrom:(NSDictionary *)d;
/* returns 12 gains or nil; sets *kind and may update *name */
+ (NSArray *)importData:(NSData *)bytes name:(NSString **)name kind:(NSString **)kind;
+ (int)hwMax:(NSDictionary *)d;                 /* measured hardware max in 0.5 dB steps (default 9 = chip 0 dB) */
+ (BOOL)hwMeasured:(NSDictionary *)d;
+ (UIImage *)thumbImage:(UIColor *)color diameter:(CGFloat)dia;
@end
