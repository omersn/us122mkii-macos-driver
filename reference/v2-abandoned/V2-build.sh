#!/bin/bash
# V2-build.sh  -  build the V2 single-process US-122MKII driver.
#
# V2 is ONE bundle: the AudioServerPlugIn with the IOKit USB engine compiled
# in. No helper daemon, no libusb, no launchd, no shared memory. That is the
# whole point of V2.
#
# Run from the directory containing V2-US122.driver/.

set -e

BUNDLE="V2-US122.driver"
MACOS="$BUNDLE/Contents/MacOS"
OUT="$MACOS/V2-US122"

echo "[1/1] compiling V2 plugin + IOKit USB engine..."
# Both translation units (plugin object model + engine) compiled together.
# The engine pulls in V2-usbengine-iokit.inc via #include.
clang -arch x86_64 -bundle -fno-common \
    -o "$OUT" \
    "$MACOS/V2-US122.c" \
    "$MACOS/V2-usbengine.c" \
    -I"$MACOS" \
    -framework CoreAudio \
    -framework CoreFoundation \
    -framework IOKit \
    -Wall
echo "  built: $OUT"
nm "$OUT" 2>/dev/null | grep -q "US122_Create" && echo "  US122_Create present" || echo "  WARNING: factory symbol missing"

echo
echo "================ INSTALL (requires sudo) ================"
echo "  sudo cp -R $BUNDLE /Library/Audio/Plug-Ins/HAL/"
echo "  sudo killall coreaudiod"
echo
echo "Then select 'US-122MKII (V2)' in Sound settings and play audio."
echo "No daemon, no launchd: the plugin owns the device directly."
echo
echo "================ VERIFY ================"
echo "  log show --last 2m --predicate 'eventMessage CONTAINS \"V2 US122\"'"
echo "  log show --last 2m --predicate 'eventMessage CONTAINS \"V2 us122 engine\"'"
echo
echo "================ UNINSTALL ================"
echo "  sudo rm -rf /Library/Audio/Plug-Ins/HAL/$BUNDLE && sudo killall coreaudiod"
echo
echo "NOTE: V2 talks to USB via IOKit from inside coreaudiod. If macOS denies"
echo "raw USB access to the plugin, the engine logs the failure and the device"
echo "shows as not-alive. See V2-NOTES.md for the entitlement/permission"
echo "discussion and the on-hardware validation checklist."
