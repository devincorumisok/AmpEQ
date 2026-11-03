TARGET ?= iphone:clang:latest:15.0
ARCHS = arm64
THEOS_PACKAGE_SCHEME = rootless
FINALPACKAGE = 1
INSTALL_TARGET_PROCESSES = Preferences
include $(THEOS)/makefiles/common.mk

TWEAK_NAME = AmpEQ AmpEQSB
AmpEQ_FILES = EQHook.c EQDSP.c AEQPlugins.c AmpObserver.c AmpEQSettings.c Relay.c
AmpEQ_CFLAGS = -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations
AmpEQ_FRAMEWORKS = CoreFoundation AudioToolbox
AmpEQ_LIBRARIES = m

AmpEQSB_FILES = SBVolume.m
AmpEQSB_CFLAGS = -fobjc-arc -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations
AmpEQSB_FRAMEWORKS = UIKit Foundation AVFoundation
AmpEQSB_LIBRARIES = substrate


include $(THEOS_MAKE_PATH)/tweak.mk
SUBPROJECTS += Prefs
include $(THEOS_MAKE_PATH)/aggregate.mk

