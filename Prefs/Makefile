TARGET ?= iphone:clang:latest:15.0
ARCHS = arm64
include $(THEOS)/makefiles/common.mk
BUNDLE_NAME = AmpEQPrefs
AmpEQPrefs_FILES = AEQRootListController.m AEQStore.m AEQEQController.m AEQStatusController.m AEQDatabaseController.m AEQPresetsController.m AEQPluginController.m ../AEQPlugins.c ../AmpEQSettings.c ../AmpEQStatus.c ../AmpEQCodec.c ../EQDSP.c ../AEQImport.c
AmpEQPrefs_FRAMEWORKS = UIKit Foundation CoreFoundation IOKit AudioToolbox
# Xcode's public SDK lacks the private Preferences.framework stub; Vendor/ has a link-only .tbd.
AmpEQPrefs_LDFLAGS += -F$(CURDIR)/Vendor
AmpEQPrefs_PRIVATE_FRAMEWORKS = Preferences
AmpEQPrefs_INSTALL_PATH = /Library/PreferenceBundles
AmpEQPrefs_CFLAGS = -fobjc-arc -fblocks -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations
include $(THEOS_MAKE_PATH)/bundle.mk
