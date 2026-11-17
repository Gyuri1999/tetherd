# tetherd -- userspace RNDIS USB tethering daemon for macOS.
#
#   make                 build build/tetherd
#   sudo make install    install the binary and a launchd daemon that starts it at boot
#   sudo make uninstall  stop the daemon and remove both
#   make app             build build/Tether.app, a menu bar app that starts
#                        tethering when opened and stops it when quit
#   make install-app     copy Tether.app to /Applications
#   make clean
#
# Needs only the Xcode command line tools. Builds universal binaries (Intel
# and Apple silicon) by default; for this Mac only: make ARCHS=$(uname -m)

ARCHS      ?= x86_64 arm64
MIN_MACOS  ?= 11.0
PREFIX     ?= /usr/local
LABEL      := org.tetherd.daemon
PLIST      := /Library/LaunchDaemons/$(LABEL).plist

CXX        := xcrun clang++
CXXFLAGS   := -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter \
              -mmacosx-version-min=$(MIN_MACOS) $(foreach a,$(ARCHS),-arch $(a))
LDFLAGS    := -framework IOKit -framework CoreFoundation -framework SystemConfiguration

BUILD      := build
SRCS       := src/main.cpp src/RNDISDevice.cpp src/FakeEthernet.cpp src/RNDISProtocolCore.cpp
HDRS       := $(wildcard src/*.h)

APP        := $(BUILD)/Tether.app
APPDIR     ?= /Applications
SWIFTC     := xcrun swiftc
SWIFTFLAGS := -O -framework AppKit -framework SwiftUI
APP_SRCS   := app/main.swift app/Panel.swift
# The app's Liquid Glass UI needs macOS 26; tetherd itself still runs on 11.
APP_MIN_MACOS ?= 26.0
ICTOOL     := /Applications/Icon Composer.app/Contents/Executables/ictool
LSREGISTER := /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister

all: $(BUILD)/tetherd

$(BUILD)/tetherd: $(SRCS) $(HDRS) Makefile
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(SRCS) $(LDFLAGS) -o $@

install: $(BUILD)/tetherd
	install -d $(PREFIX)/sbin
	install -m 755 $(BUILD)/tetherd $(PREFIX)/sbin/tetherd
	sed 's|__PREFIX__|$(PREFIX)|g' launchd/$(LABEL).plist > $(PLIST)
	chown root:wheel $(PLIST) && chmod 644 $(PLIST)
	-launchctl bootout system/$(LABEL) 2>/dev/null
	launchctl bootstrap system $(PLIST)
	@echo "tetherd installed and started. Log: /var/log/tetherd.log"

uninstall:
	-launchctl bootout system/$(LABEL) 2>/dev/null
	rm -f $(PLIST) $(PREFIX)/sbin/tetherd

app: $(APP)

# The icon is a Liquid Glass Icon Composer document, app/Tether.icon. Icon
# Composer's ictool renders it; without Icon Composer, MakeIcon draws a flat
# version of the same design.
$(BUILD)/Tether.icns: app/MakeIcon.swift app/Tether.icon/icon.json app/Tether.icon/Assets/chain.png
	@mkdir -p $(BUILD)
	$(SWIFTC) -O app/MakeIcon.swift -o $(BUILD)/makeicon
	rm -rf $(BUILD)/Tether.iconset
	if [ -x "$(ICTOOL)" ]; then \
	  "$(ICTOOL)" app/Tether.icon --export-image --output-file $(BUILD)/icon-tile.png \
	    --platform macOS --rendition Default --width 824 --height 824 --scale 1 >/dev/null && \
	  $(BUILD)/makeicon --pad $(BUILD)/icon-tile.png $(BUILD)/Tether.iconset; \
	else \
	  echo "Icon Composer not found; using the flat icon"; \
	  $(BUILD)/makeicon $(BUILD)/Tether.iconset; \
	fi
	iconutil -c icns $(BUILD)/Tether.iconset -o $@
	rm -rf $(BUILD)/Tether.iconset $(BUILD)/makeicon $(BUILD)/icon-tile.png

$(APP): $(BUILD)/tetherd $(BUILD)/Tether.icns $(APP_SRCS) app/Info.plist Makefile
	rm -rf $@
	mkdir -p $@/Contents/MacOS $@/Contents/Resources
	for a in $(ARCHS); do \
	  $(SWIFTC) $(SWIFTFLAGS) -target $$a-apple-macos$(APP_MIN_MACOS) $(APP_SRCS) \
	    -o $(BUILD)/Tether-$$a || exit 1; \
	done
	lipo -create $(foreach a,$(ARCHS),$(BUILD)/Tether-$(a)) -output $@/Contents/MacOS/Tether
	rm -f $(foreach a,$(ARCHS),$(BUILD)/Tether-$(a))
	cp $(BUILD)/tetherd $@/Contents/Resources/tetherd
	cp $(BUILD)/Tether.icns $@/Contents/Resources/Tether.icns
	sed 's|__MIN_MACOS__|$(APP_MIN_MACOS)|' app/Info.plist > $@/Contents/Info.plist
	codesign --force --sign - $@/Contents/Resources/tetherd
	codesign --force --sign - $@
	$(LSREGISTER) -f $@   # so Finder shows the current icon, not a cached one

install-app: $(APP)
	rm -rf $(APPDIR)/Tether.app
	cp -R $(APP) $(APPDIR)/
	$(LSREGISTER) -f $(APPDIR)/Tether.app
	@echo "Installed $(APPDIR)/Tether.app"

clean:
	rm -rf $(BUILD)

.PHONY: all app install-app install uninstall clean
