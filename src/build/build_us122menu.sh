#!/bin/bash
# build_us122menu.sh — build the US-122MKII menu-bar status/control app.
# Produces US122Menu.app, a menu-bar-only (LSUIElement) agent.
#
# Run from the directory containing US122Menu/main.swift and US122Menu/Info.plist.

set -e

APP="US122Menu.app"
SRC="US122Menu/main.swift"
PLIST="US122Menu/Info.plist"

echo "[1/3] compiling Swift menu-bar app (+ C shim)..."
# swiftc does NOT compile .c sources; passing a .c straight to swiftc sends raw
# source bytes to the linker. Compile the shim with clang first, link the .o.
clang -c US122Menu/shimopen.c -o /tmp/shimopen.o
swiftc -O -o /tmp/US122Menu \
    US122Menu/main.swift /tmp/shimopen.o \
    -import-objc-header US122Menu/bridging.h \
    -framework Cocoa
echo "  built binary"

echo "[2/3] assembling $APP bundle..."
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp /tmp/US122Menu "$APP/Contents/MacOS/US122Menu"
chmod +x "$APP/Contents/MacOS/US122Menu"
cp "$PLIST" "$APP/Contents/Info.plist"
echo "  assembled $APP"

echo "[3/3] done."
echo
echo "================ RUN (test) ================"
echo "  open $APP"
echo "  # a colored dot appears in the menu bar:"
echo "  #   green  = device connected & streaming"
echo "  #   amber  = device connected, idle"
echo "  #   hollow = device not connected (or daemon down)"
echo "  # the menu has Show/Hide Audio Device + Quit."
echo
echo "================ INSTALL (optional, run at login) ================"
echo "  cp -R $APP /Applications/"
echo "  # then add /Applications/$APP to System Settings > Login Items,"
echo "  # or it auto-starts after you open it once and keep it."
echo
echo "================ UNINSTALL ================"
echo "  # quit from its menu, then:"
echo "  rm -rf /Applications/$APP"
