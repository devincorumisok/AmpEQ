/* AmpEQ SpringBoard part (AmpEQSB.dylib, SpringBoard only):
 *  1. Headphone detection: publishes the audio route so Gain is only applied on wired headphones.
 *  2. Volume steps: 16 (stock) .. 100 button steps.
 *  3. Volume number: the current step, shown in the status bar while the volume changes.
 *
 * Volume steps: stock iOS handles the buttons and the volume bar; every volume call it makes for a press
 * is rewritten to exactly one step of the chosen count:
 *  A. AVSystemController: direction-only calls ("one step up/down") become "change by" one exact step;
 *     "change by" / "set to" calls of up to 1/16 right after a press become one exact step.
 *  B. SBVolumeControl: any step-size getter / "change volume by" method found at runtime, same rule.
 *  C. Only if nothing above could be hooked: correct after the fact when a 1/16 change is seen.
 * Every layer only touches a move of exactly the stock 1/16, so layers can't stack and the Control
 * Center slider / Music app volume are never changed. */
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/QuartzCore.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <notify.h>
#include <math.h>
#include <syslog.h>
#include "AmpEQShared.h"

extern void MSHookMessageEx(Class _class, SEL message, IMP hook, IMP *old);

static int g_steps = 16;      /* button steps from silent to max */
static int g_numPos = 0;      /* 0 off, otherwise shown in the status bar */
static BOOL g_hpDetect = YES, g_master = YES;
static unsigned g_hooks = 0;  /* which layers are installed (status) */
static unsigned g_adjusted = 0;
static float g_lastVol = -1;
static BOOL g_selfSet = NO;
static CFTimeInterval g_btnTime = 0;
static __weak id g_vc;             /* SpringBoard's SBVolumeControl, captured when first seen */

enum { HK_SB_STEP = 1, HK_SB_BY = 2, HK_AV_ACT_BY = 4, HK_AV_CAT_BY = 8, HK_AV_ACT_TO = 16, HK_AV_CAT_TO = 32, HK_SB_BTN = 64, HK_FALLBACK = 128,
       HK_BUTTONS = 256, HK_AV_DIR = 512, HK_SB_SET = 1024 };

static void publish_hookstate(void) {
    int t = -1;
    if (notify_register_check(AEQ_NS "st.sb.vol", &t) == NOTIFY_STATUS_OK) {
        notify_set_state(t, (uint64_t)(g_hooks & 0xFFFF) | ((uint64_t)(g_adjusted & 0xFFFFFF) << 16) | ((uint64_t)(g_steps & 0xFF) << 40));
        notify_cancel(t);
    }
}

/* ------------------------------------------------------------------ prefs */
static void load_prefs(void) {
    NSDictionary *d = [NSDictionary dictionaryWithContentsOfFile:@AEQ_PREF_PATH];
    int s = d[@"volSteps"] ? [d[@"volSteps"] intValue] : 16;
    g_steps = s < 16 ? 16 : (s > 100 ? 100 : s);
    g_numPos = d[@"volNumber"] ? [d[@"volNumber"] intValue] : 0;
    if (g_numPos < 0 || g_numPos > 3) g_numPos = 0;
    g_hpDetect = d[@"hpDetect"] ? [d[@"hpDetect"] boolValue] : YES;
    g_master = d[@"ampeqEnabled"] ? [d[@"ampeqEnabled"] boolValue] : YES;
    if (!g_master) { g_steps = 16; g_numPos = 0; }
}

/* ------------------------------------------------------------------ route */
static int current_route(void) {
    AVAudioSessionRouteDescription *r = [[AVAudioSession sharedInstance] currentRoute];
    if (!r.outputs.count) return 0;
    for (AVAudioSessionPortDescription *p in r.outputs)
        if ([p.portType isEqualToString:AVAudioSessionPortHeadphones] || [p.portType isEqualToString:AVAudioSessionPortLineOut]) return 1;
    return 2;
}
static void publish_route(void) {
    int v = (g_hpDetect && g_master) ? current_route() : 0;
    int t = -1;
    if (notify_register_check(AEQ_ROUTE, &t) == NOTIFY_STATUS_OK) { notify_set_state(t, (uint64_t)v); notify_cancel(t); }
    notify_post(AEQ_NOTE_ROUTE);
}

/* ------------------------------------------------------------------ step maths */
static inline BOOL is_stock_step(float d) { return fabsf(fabsf(d) - 0.0625f) < 0.0015f; }
static inline BOOL active(void) { return g_steps > 16 && g_master; }
/* next volume one button step up/down from `cur`, on the grid of g_steps */
static float next_volume(float cur, BOOL up) {
    float q = roundf(cur * g_steps) + (up ? 1 : -1);
    return fmaxf(0.0f, fminf(1.0f, q / (float)g_steps));
}
static void note_adjust(void) { g_adjusted++; if ((g_adjusted & 7) == 1) dispatch_async(dispatch_get_main_queue(), ^{ publish_hookstate(); }); }
/* the volume AmpEQ just sent: the number shows it at once instead of waiting for the audio service */
static float g_target = -1; static CFTimeInterval g_targetTime;
static void show_number_soon(void);
static void set_target(float t) { g_target = t; g_targetTime = CACurrentMediaTime(); show_number_soon(); }

/* ------------------------------------------------------------------ AVSystemController */
static id avsc(void) {
    Class c = objc_getClass("AVSystemController");
    return (c && [c respondsToSelector:@selector(sharedAVSystemController)]) ? ((id (*)(id, SEL))objc_msgSend)(c, @selector(sharedAVSystemController)) : nil;
}
static BOOL (*o_getVol)(id, SEL, float *, NSString *);
static float volume_for(NSString *cat) {
    id s = avsc(); float x = -1;
    SEL g = NSSelectorFromString(@"getVolume:forCategory:");
    if (s && [s respondsToSelector:g]) {
        float v = 0;
        BOOL ok = o_getVol ? o_getVol(s, g, &v, cat) : ((BOOL (*)(id, SEL, float *, NSString *))objc_msgSend)(s, g, &v, cat);
        if (ok) x = v;
    }
    return x;
}
static float media_volume(void) {
    float v = volume_for(@"Audio/Video");
    if (v < 0) v = [[AVAudioSession sharedInstance] outputVolume];
    return v;
}
static NSString *active_category(void) {
    id s = avsc(); SEL g = NSSelectorFromString(@"getActiveCategoryVolume:andName:");
    if (s && [s respondsToSelector:g]) {
        float v = 0; NSString *name = nil;
        if (((BOOL (*)(id, SEL, float *, NSString **))objc_msgSend)(s, g, &v, &name) && name.length) return name;
    }
    return @"Audio/Video";
}
/* A volume-button move: a stock 1/16 step, or (within 0.6 s of a press / while held) any move of up to 1/16
 * - iOS snaps to its own 1/16 grid, so from an AmpEQ step the stock move can be shorter than 1/16.
 * It becomes exactly one step on AmpEQ's grid. Re-applying it gives the same target (idempotent). */
static BOOL button_recent(void) { return CACurrentMediaTime() - g_btnTime < 0.6; }
static BOOL is_button_move(float d) { return fabsf(d) > 1e-6f && (is_stock_step(d) || (button_recent() && fabsf(d) <= 0.0630f)); }
static float rewrite_delta(float d, float cur) {
    if (!active() || cur < 0 || !is_button_move(d)) return d;
    note_adjust(); float t = next_volume(cur, d > 0); set_target(t); return t - cur;
}
static float rewrite_set(float target, NSString *cat) {
    if (!active() || g_selfSet) return target;
    float cur = volume_for(cat ?: active_category());
    if (cur < 0) return target;
    return cur + rewrite_delta(target - cur, cur);
}
static BOOL (*o_avActBy)(id, SEL, float);
static BOOL h_avActBy(id self, SEL _cmd, float d) { return o_avActBy(self, _cmd, rewrite_delta(d, volume_for(active_category()))); }
static BOOL (*o_avCatBy)(id, SEL, float, id);
static BOOL h_avCatBy(id self, SEL _cmd, float d, id cat) { return o_avCatBy(self, _cmd, rewrite_delta(d, volume_for(cat)), cat); }

/* --- direction-only calls ("one step up/down", the audio service picks 1/16): replaced by the
 *     matching "change by" call with AmpEQ's exact step. Every pair below exists on iOS 15.8.6. --- */
static float step_from(float cur, BOOL up) { note_adjust(); float t = next_volume(cur, up); set_target(t); return t - cur; }
static BOOL (*o_dirAct)(id, SEL, BOOL);
static BOOL h_dirAct(id self, SEL _cmd, BOOL up) {
    float cur = volume_for(active_category()); SEL by = NSSelectorFromString(@"changeActiveCategoryVolumeBy:");
    if (!active() || cur < 0 || ![self respondsToSelector:by]) return o_dirAct(self, _cmd, up);
    return ((BOOL (*)(id, SEL, float))objc_msgSend)(self, by, step_from(cur, up));
}
static BOOL (*o_dirCat)(id, SEL, BOOL, id);
static BOOL h_dirCat(id self, SEL _cmd, BOOL up, id cat) {
    float cur = volume_for(cat); SEL by = NSSelectorFromString(@"changeVolumeBy:forCategory:");
    if (!active() || cur < 0 || ![self respondsToSelector:by]) return o_dirCat(self, _cmd, up, cat);
    return ((BOOL (*)(id, SEL, float, id))objc_msgSend)(self, by, step_from(cur, up), cat);
}
static BOOL (*o_dirFb)(id, SEL, BOOL, id, float *, id *);
static BOOL h_dirFb(id self, SEL _cmd, BOOL up, id fb, float *res, id *aff) {
    SEL g = NSSelectorFromString(@"getActiveCategoryVolume:andName:fallbackCategory:"), by = NSSelectorFromString(@"changeActiveCategoryVolumeBy:fallbackCategory:resultVolume:affectedCategory:");
    float cur = -1; NSString *name = nil;
    if (active() && [self respondsToSelector:g] && [self respondsToSelector:by] &&
        ((BOOL (*)(id, SEL, float *, NSString **, id))objc_msgSend)(self, g, &cur, &name, fb) && cur >= 0)
        return ((BOOL (*)(id, SEL, float, id, float *, id *))objc_msgSend)(self, by, step_from(cur, up), fb, res, aff);
    return o_dirFb(self, _cmd, up, fb, res, aff);
}
static BOOL (*o_dirRt)(id, SEL, BOOL, id, id);
static BOOL h_dirRt(id self, SEL _cmd, BOOL up, id route, id dev) {
    SEL g = NSSelectorFromString(@"getActiveCategoryVolume:andName:forRoute:andDeviceIdentifier:"), by = NSSelectorFromString(@"changeActiveCategoryVolumeBy:forRoute:andDeviceIdentifier:");
    float cur = -1; NSString *name = nil;
    if (active() && [self respondsToSelector:g] && [self respondsToSelector:by] &&
        ((BOOL (*)(id, SEL, float *, NSString **, id, id))objc_msgSend)(self, g, &cur, &name, route, dev) && cur >= 0)
        return ((BOOL (*)(id, SEL, float, id, id))objc_msgSend)(self, by, step_from(cur, up), route, dev);
    return o_dirRt(self, _cmd, up, route, dev);
}
static BOOL (*o_dirMode)(id, SEL, BOOL, id, id, id, id, id);
static BOOL h_dirMode(id self, SEL _cmd, BOOL up, id cat, id mode, id route, id dev, id sub) {
    SEL g = NSSelectorFromString(@"getVolumeForRoute:forCategory:mode:route:deviceIdentifier:andRouteSubtype:"),
        by = NSSelectorFromString(@"changeVolumeForRouteBy:forCategory:mode:route:deviceIdentifier:andRouteSubtype:");
    float cur = -1;
    if (active() && [self respondsToSelector:g] && [self respondsToSelector:by] &&
        ((BOOL (*)(id, SEL, float *, id, id, id, id, id))objc_msgSend)(self, g, &cur, cat, mode, route, dev, sub) && cur >= 0)
        return ((BOOL (*)(id, SEL, float, id, id, id, id, id))objc_msgSend)(self, by, step_from(cur, up), cat, mode, route, dev, sub);
    return o_dirMode(self, _cmd, up, cat, mode, route, dev, sub);
}
static BOOL (*o_avActTo)(id, SEL, float);
static BOOL h_avActTo(id self, SEL _cmd, float v) { return o_avActTo(self, _cmd, rewrite_set(v, nil)); }
static BOOL (*o_avCatTo)(id, SEL, float, id);
static BOOL h_avCatTo(id self, SEL _cmd, float v, id cat) { return o_avCatTo(self, _cmd, rewrite_set(v, cat), cat); }

/* exact-shape check so a hook's C signature always matches the method */
static BOOL shape(Method m, char ret, const char *args) {
    if (!m) return NO;
    unsigned n = method_getNumberOfArguments(m);
    if (n != 2 + strlen(args)) return NO;
    char t[16]; method_getReturnType(m, t, sizeof t);
    if (t[0] != ret && !(ret == 'B' && t[0] == 'c')) return NO;
    for (unsigned i = 0; i < strlen(args); i++) { method_getArgumentType(m, 2 + i, t, sizeof t); if (t[0] != args[i]) return NO; }
    return YES;
}
static BOOL hook_shape(Class c, NSString *name, char ret, const char *args, IMP repl, IMP *orig) {
    SEL s = NSSelectorFromString(name); Method m = c ? class_getInstanceMethod(c, s) : NULL;
    if (!shape(m, ret, args)) return NO;
    MSHookMessageEx(c, s, repl, orig);
    syslog(LOG_NOTICE, "AmpEQ[sb]: hooked -[%s %s]", class_getName(c), name.UTF8String);
    return YES;
}

/* ------------------------------------------------------------------ SBVolumeControl (generic) */
#define MAXG 6
static float (*o_getter[MAXG])(id, SEL); static SEL g_getterSel[MAXG]; static int g_ngetter;
static float getter_call(int i, id self, SEL _cmd) { g_vc = self; float v = o_getter[i](self, _cmd); if (active() && is_stock_step(v)) { note_adjust(); return copysignf(1.0f / g_steps, v); } return v; }
static float g0(id s, SEL c) { return getter_call(0, s, c); } static float g1(id s, SEL c) { return getter_call(1, s, c); }
static float g2(id s, SEL c) { return getter_call(2, s, c); } static float g3(id s, SEL c) { return getter_call(3, s, c); }
static float g4(id s, SEL c) { return getter_call(4, s, c); } static float g5(id s, SEL c) { return getter_call(5, s, c); }
static float (*const g_getterImp[MAXG])(id, SEL) = { g0, g1, g2, g3, g4, g5 };
static void (*o_sbBy)(id, SEL, float);
static void h_sbBy(id self, SEL _cmd, float d) {
    g_vc = self; o_sbBy(self, _cmd, rewrite_delta(d, volume_for(active_category()))); show_number_soon();
}
static void (*o_inc)(id, SEL), (*o_dec)(id, SEL);
static void h_inc(id self, SEL _cmd) { g_vc = self; g_btnTime = CACurrentMediaTime(); o_inc(self, _cmd); show_number_soon(); }
static void h_dec(id self, SEL _cmd) { g_vc = self; g_btnTime = CACurrentMediaTime(); o_dec(self, _cmd); show_number_soon(); }
static void (*o_hvb)(id, SEL, long long, BOOL);
static void h_hvb(id self, SEL _cmd, long long type, BOOL down) { g_vc = self; if (down) g_btnTime = CACurrentMediaTime(); o_hvb(self, _cmd, type, down); }
static void (*o_sbSetAct)(id, SEL, float);
static void h_sbSetAct(id self, SEL _cmd, float v) { g_vc = self; o_sbSetAct(self, _cmd, rewrite_set(v, nil)); }
static void (*o_sbSetCat)(id, SEL, float, id);
static void h_sbSetCat(id self, SEL _cmd, float v, id cat) { g_vc = self; o_sbSetCat(self, _cmd, rewrite_set(v, cat), cat); }

static void install_sb_hooks(void) {
    Class vc = objc_getClass("SBVolumeControl");
    if (!vc) { syslog(LOG_NOTICE, "AmpEQ[sb]: SBVolumeControl missing"); return; }
    unsigned n = 0; Method *ms = class_copyMethodList(vc, &n);
    for (unsigned i = 0; i < n; i++) {
        SEL s = method_getName(ms[i]); NSString *name = NSStringFromSelector(s); NSString *low = name.lowercaseString;
        if (g_ngetter < MAXG && [low containsString:@"step"] && shape(ms[i], 'f', "")) {
            int k = g_ngetter++; g_getterSel[k] = s;
            MSHookMessageEx(vc, s, (IMP)g_getterImp[k], (IMP *)&o_getter[k]); g_hooks |= HK_SB_STEP;
            syslog(LOG_NOTICE, "AmpEQ[sb]: hooked step getter -[SBVolumeControl %s]", name.UTF8String);
        } else if (!o_sbBy && [low containsString:@"volumeby"] && shape(ms[i], 'v', "f")) {
            MSHookMessageEx(vc, s, (IMP)h_sbBy, (IMP *)&o_sbBy); g_hooks |= HK_SB_BY;
            syslog(LOG_NOTICE, "AmpEQ[sb]: hooked -[SBVolumeControl %s]", name.UTF8String);
        }
    }
    free(ms);
    BOOL i1 = hook_shape(vc, @"increaseVolume", 'v', "", (IMP)h_inc, (IMP *)&o_inc);
    BOOL i2 = hook_shape(vc, @"decreaseVolume", 'v', "", (IMP)h_dec, (IMP *)&o_dec);
    if (i1 || i2) g_hooks |= HK_SB_BTN;
    hook_shape(vc, @"handleVolumeButtonWithType:down:", 'v', "qB", (IMP)h_hvb, (IMP *)&o_hvb);
    BOOL s1 = hook_shape(vc, @"setActiveCategoryVolume:", 'v', "f", (IMP)h_sbSetAct, (IMP *)&o_sbSetAct);
    BOOL s2 = hook_shape(vc, @"setVolume:forCategory:", 'v', "f@", (IMP)h_sbSetCat, (IMP *)&o_sbSetCat);
    if (s1 || s2) g_hooks |= HK_SB_SET;
}
static void install_av_hooks(void) {
    Class c = objc_getClass("AVSystemController"); if (!c) { syslog(LOG_NOTICE, "AmpEQ[sb]: AVSystemController missing"); return; }
    Method gm = class_getInstanceMethod(c, NSSelectorFromString(@"getVolume:forCategory:"));
    if (gm) o_getVol = (BOOL (*)(id, SEL, float *, NSString *))method_getImplementation(gm);
    if (hook_shape(c, @"changeActiveCategoryVolumeBy:", 'B', "f", (IMP)h_avActBy, (IMP *)&o_avActBy)) g_hooks |= HK_AV_ACT_BY;
    if (hook_shape(c, @"changeVolumeBy:forCategory:", 'B', "f@", (IMP)h_avCatBy, (IMP *)&o_avCatBy)) g_hooks |= HK_AV_CAT_BY;
    if (hook_shape(c, @"setActiveCategoryVolumeTo:", 'B', "f", (IMP)h_avActTo, (IMP *)&o_avActTo)) g_hooks |= HK_AV_ACT_TO;
    if (hook_shape(c, @"setVolumeTo:forCategory:", 'B', "f@", (IMP)h_avCatTo, (IMP *)&o_avCatTo)) g_hooks |= HK_AV_CAT_TO;
    BOOL d1 = hook_shape(c, @"changeActiveCategoryVolume:", 'B', "B", (IMP)h_dirAct, (IMP *)&o_dirAct);
    BOOL d2 = hook_shape(c, @"changeVolume:forCategory:", 'B', "B@", (IMP)h_dirCat, (IMP *)&o_dirCat);
    BOOL d3 = hook_shape(c, @"changeActiveCategoryVolume:fallbackCategory:resultVolume:affectedCategory:", 'B', "B@^^", (IMP)h_dirFb, (IMP *)&o_dirFb);
    BOOL d4 = hook_shape(c, @"changeActiveCategoryVolume:forRoute:andDeviceIdentifier:", 'B', "B@@", (IMP)h_dirRt, (IMP *)&o_dirRt);
    BOOL d5 = hook_shape(c, @"changeVolumeForRoute:forCategory:mode:route:deviceIdentifier:andRouteSubtype:", 'B', "B@@@@@", (IMP)h_dirMode, (IMP *)&o_dirMode);
    if (d1 || d2 || d3 || d4 || d5) g_hooks |= HK_AV_DIR;
}
static void set_media_volume(float v) {
    id s = avsc(); SEL st = NSSelectorFromString(@"setVolumeTo:forCategory:");
    if (!s || ![s respondsToSelector:st]) return;
    g_selfSet = YES;
    ((BOOL (*)(id, SEL, float, NSString *))objc_msgSend)(s, st, v, @"Audio/Video");
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.15 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ g_selfSet = NO; });
}

/* ------------------------------------------------------------------ number in the status bar
 * A small capsule (speaker glyph + step) placed in the status bar, centred in the gap between the
 * left items (device name + Wi-Fi) and the clock. Fixed size (room for "100"), digits cross-fade,
 * it pops in on a volume press and fades out with the volume bar. */
static UIWindow *g_win; static UIView *g_pill; static UILabel *g_lbl; static UIImageView *g_icon;
static NSUInteger g_hideGen; static BOOL g_visible;
static const CGFloat kPillH = 16;

static void ensure_window(void) {
    if (g_win && g_win.windowScene && g_win.windowScene.activationState != UISceneActivationStateUnattached) return;
    if (g_win) { g_win.hidden = YES; g_win = nil; g_visible = NO; }
    UIWindowScene *scene = nil;
    for (UIScene *sc in UIApplication.sharedApplication.connectedScenes)
        if ([sc isKindOfClass:[UIWindowScene class]]) { scene = (UIWindowScene *)sc; break; }
    g_win = scene ? [[UIWindow alloc] initWithWindowScene:scene] : [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    g_win.frame = UIScreen.mainScreen.bounds;
    g_win.windowLevel = 10000;
    g_win.userInteractionEnabled = NO;
    g_win.backgroundColor = UIColor.clearColor;
    g_win.rootViewController = [UIViewController new];
    g_win.rootViewController.view.backgroundColor = UIColor.clearColor;
    g_win.rootViewController.view.userInteractionEnabled = NO;

    g_pill = [UIView new];
    UIVisualEffectView *blur = [[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemThinMaterial]];
    blur.clipsToBounds = YES; blur.layer.cornerRadius = kPillH / 2; blur.layer.cornerCurve = kCACornerCurveContinuous;
    blur.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [g_pill addSubview:blur];
    UIImageSymbolConfiguration *cfg = [UIImageSymbolConfiguration configurationWithPointSize:9 weight:UIImageSymbolWeightSemibold];
    g_icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"speaker.wave.2.fill" withConfiguration:cfg]];
    g_icon.tintColor = UIColor.labelColor; g_icon.contentMode = UIViewContentModeCenter;
    [blur.contentView addSubview:g_icon];
    g_lbl = [UILabel new];
    UIFont *base = [UIFont monospacedDigitSystemFontOfSize:11 weight:UIFontWeightSemibold];
    UIFontDescriptor *fd = [base.fontDescriptor fontDescriptorWithDesign:UIFontDescriptorSystemDesignRounded];
    g_lbl.font = fd ? [UIFont fontWithDescriptor:[fd fontDescriptorByAddingAttributes:@{UIFontDescriptorFeatureSettingsAttribute:
        @[@{UIFontFeatureTypeIdentifierKey: @(6), UIFontFeatureSelectorIdentifierKey: @(0)}]}] size:11] : base;   /* monospaced digits */
    g_lbl.textColor = UIColor.labelColor;
    g_lbl.textAlignment = NSTextAlignmentLeft;
    [blur.contentView addSubview:g_lbl];
    [g_win.rootViewController.view addSubview:g_pill];
    g_win.hidden = YES;
}
/* where the gap is: left items end ~ device name + Wi-Fi glyph, clock is centred */
static CGRect pill_frame(void) {
    CGRect sb = UIScreen.mainScreen.bounds;
    UIFont *sf = [UIFont systemFontOfSize:12 weight:UIFontWeightSemibold];
    NSString *name = [[UIDevice.currentDevice.model ?: @"iPod" componentsSeparatedByString:@" "] firstObject];   /* status bar shows "iPod" */
    CGFloat leftEnd = 6 + ceil([name sizeWithAttributes:@{NSFontAttributeName: sf}].width) + 5 + 15;
    NSString *clock = [NSDateFormatter localizedStringFromDate:[NSDate date] dateStyle:NSDateFormatterNoStyle timeStyle:NSDateFormatterShortStyle];
    CGFloat clockW = ceil([clock sizeWithAttributes:@{NSFontAttributeName: sf}].width);
    CGFloat clockStart = sb.size.width / 2 - clockW / 2;
    CGFloat textW = ceil([@"100" sizeWithAttributes:@{NSFontAttributeName: g_lbl.font}].width);
    CGFloat w = 6 + 12 + 2 + textW + 7, h = kPillH;
    CGFloat statusH = 20;   /* home-button devices */
    if (@available(iOS 13.0, *)) {
        for (UIScene *sc in UIApplication.sharedApplication.connectedScenes)
            if ([sc isKindOfClass:[UIWindowScene class]]) { CGFloat sh = ((UIWindowScene *)sc).statusBarManager.statusBarFrame.size.height; if (sh > 0) statusH = sh; break; }
    }
    CGFloat x = (leftEnd + clockStart) / 2 - w / 2;
    if (x + w > clockStart - 4) x = clockStart - 4 - w;        /* never touch the clock */
    if (x < leftEnd + 2) x = leftEnd + 2;
    return CGRectMake(round(x), round((statusH - h) / 2), w, h);
}
static void layout_pill(void) {
    CGRect b = g_pill.bounds;
    g_icon.frame = CGRectMake(5, 0, 13, b.size.height);
    g_lbl.frame = CGRectMake(19, 0, b.size.width - 21, b.size.height);
}
static float shown_volume(void) {
    if (g_target >= 0 && CACurrentMediaTime() - g_targetTime < 0.35) return g_target;   /* just sent, not yet confirmed */
    return media_volume();
}
static void set_pill_content(float v, BOOL animated) {
    int step = (int)lrintf(v * g_steps);
    NSString *text = [NSString stringWithFormat:@"%d", step];
    NSString *sym = step == 0 ? @"speaker.slash.fill" : (v < 0.34f ? @"speaker.wave.1.fill" : (v < 0.67f ? @"speaker.wave.2.fill" : @"speaker.wave.3.fill"));
    if ([g_lbl.text isEqualToString:text] && g_icon.image) return;
    UIImageSymbolConfiguration *cfg = [UIImageSymbolConfiguration configurationWithPointSize:9 weight:UIImageSymbolWeightSemibold];
    void (^set)(void) = ^{ g_lbl.text = text; g_icon.image = [UIImage systemImageNamed:sym withConfiguration:cfg]; };
    if (animated) [UIView transitionWithView:g_pill duration:0.12 options:UIViewAnimationOptionTransitionCrossDissolve | UIViewAnimationOptionBeginFromCurrentState animations:set completion:nil];
    else set();
}
static void hide_number_now(void) {
    g_hideGen++;
    if (!g_win) return;
    [g_pill.layer removeAllAnimations];
    g_pill.alpha = 0; g_pill.transform = CGAffineTransformIdentity; g_win.hidden = YES; g_visible = NO;
}
/* Show / refresh the number. Safe to call any number of times, from any state:
 *  - hidden            -> pops in
 *  - fading out        -> the fade is cancelled and it comes straight back (this was the "disappears" bug)
 *  - visible           -> digits cross-fade, hide timer restarts                                   */
static void show_number(void) {
    if (!g_numPos || !g_master) { if (g_visible) hide_number_now(); return; }
    float v = shown_volume(); if (v < 0) return;
    ensure_window();
    if (!g_visible || g_win.hidden) {
        [g_pill.layer removeAllAnimations];
        g_pill.frame = pill_frame(); layout_pill();
        set_pill_content(v, NO);
        g_pill.alpha = 0; g_pill.transform = CGAffineTransformMakeScale(0.8, 0.8);
        g_win.hidden = NO; g_visible = YES;
    } else {
        set_pill_content(v, YES);
    }
    [UIView animateWithDuration:0.22 delay:0 usingSpringWithDamping:0.8 initialSpringVelocity:0
        options:UIViewAnimationOptionBeginFromCurrentState | UIViewAnimationOptionAllowUserInteraction
        animations:^{ g_pill.alpha = 1; g_pill.transform = CGAffineTransformIdentity; } completion:nil];
    NSUInteger gen = ++g_hideGen;
    /* settle: show the volume the audio service actually confirmed */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.4 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (gen == g_hideGen && g_visible) { float a = media_volume(); if (a >= 0) set_pill_content(a, YES); }
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (gen != g_hideGen) return;
        [UIView animateWithDuration:0.25 delay:0 options:UIViewAnimationOptionBeginFromCurrentState
            animations:^{ g_pill.alpha = 0; g_pill.transform = CGAffineTransformMakeScale(0.9, 0.9); }
            completion:^(BOOL f) { if (gen == g_hideGen) { g_win.hidden = YES; g_visible = NO; g_pill.transform = CGAffineTransformIdentity; } }];
    });
}
static void show_number_soon(void) {
    /* coalesce bursts (holding a button fires many changes): at most one refresh per 30 ms */
    static BOOL queued;
    if (queued) return; queued = YES;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.03 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ queued = NO; show_number(); });
}

/* ------------------------------------------------------------------ layer C + number trigger */
static void volume_changed(NSNotification *n) {
    float v = media_volume();
    NSString *reason = n.userInfo[@"AVSystemController_AudioVolumeChangeReasonNotificationParameter"];
    BOOL explicitChange = !reason || [reason isEqualToString:@"ExplicitVolumeChange"];
    float old = g_lastVol;
    BOOL hooked = (g_hooks & (HK_BUTTONS | HK_AV_DIR | HK_SB_SET | HK_SB_STEP | HK_SB_BY | HK_AV_ACT_BY | HK_AV_CAT_BY | HK_AV_ACT_TO | HK_AV_CAT_TO)) != 0;
    if (!hooked && !g_selfSet && active() && explicitChange && old >= 0 && v >= 0) {
        float d = v - old;
        BOOL up = is_stock_step(d) && d > 0, down = is_stock_step(d) && d < 0;
        if (!up && !down && v >= 0.999f && old > 0.93f && d > 0) up = YES;
        if (!up && !down && v <= 0.001f && old < 0.07f && d < 0) down = YES;
        if (up || down) {
            float t = next_volume(old, up); g_lastVol = t; set_media_volume(t); note_adjust();
            show_number_soon(); return;
        }
    }
    if (v >= 0) g_lastVol = v;
    if (explicitChange && !g_selfSet) show_number_soon();
}

/* Settings changed. Step count changed -> the current volume is moved onto the nearest step of the
 * new grid (at most half a step), so the number is exact and the next press is exactly one step.
 * Number switched on -> shown once; switched off -> hidden at once. */
static void prefs_changed(void) {
    int oldSteps = g_steps, oldNum = g_numPos;
    load_prefs();
    if (!g_numPos || !g_master) hide_number_now();
    if (g_steps != oldSteps) {
        g_target = -1;
        float cur = media_volume();
        if (active() && cur >= 0) {
            float t = roundf(cur * g_steps) / (float)g_steps;
            if (fabsf(t - cur) > 1e-4f) set_media_volume(t);
            g_target = t; g_targetTime = CACurrentMediaTime(); g_lastVol = t;
        }
        if (g_numPos) show_number();
    } else if (g_numPos && !oldNum) show_number();
}

/* ------------------------------------------------------------------ load */
__attribute__((constructor)) static void ampeq_sb_init(void) {
    @autoreleasepool {
        if (![NSBundle.mainBundle.bundleIdentifier isEqualToString:@"com.apple.springboard"]) return;
        load_prefs();
        install_av_hooks();
        install_sb_hooks();
        if (!(g_hooks & (HK_BUTTONS | HK_AV_DIR | HK_SB_SET | HK_SB_STEP | HK_SB_BY | HK_AV_ACT_BY | HK_AV_CAT_BY | HK_AV_ACT_TO | HK_AV_CAT_TO))) g_hooks |= HK_FALLBACK;
        syslog(LOG_NOTICE, "AmpEQ[sb]: volume hooks 0x%x", g_hooks);

        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        for (NSString *name in @[@"AVSystemController_SystemVolumeDidChangeNotification", @"SystemVolumeDidChange"])
            [nc addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { volume_changed(n); }];
        id s = avsc(); SEL attr = NSSelectorFromString(@"setAttribute:forKey:error:");
        if (s && [s respondsToSelector:attr])
            ((BOOL (*)(id, SEL, id, NSString *, NSError **))objc_msgSend)(s, attr, @[@"AVSystemController_SystemVolumeDidChangeNotification", @"SystemVolumeDidChange"],
                @"AVSystemController_SubscribeToNotificationsAttribute", NULL);

        [nc addObserverForName:AVAudioSessionRouteChangeNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { publish_route(); }];
        int tok;
        notify_register_dispatch(AEQ_NOTE_CHANGED, &tok, dispatch_get_main_queue(), ^(int t) { prefs_changed(); publish_route(); publish_hookstate(); });
        notify_register_dispatch(AEQ_NOTE_HELLO, &tok, dispatch_get_main_queue(), ^(int t) { publish_route(); });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            publish_route(); publish_hookstate(); g_lastVol = media_volume();
        });
    }
}
