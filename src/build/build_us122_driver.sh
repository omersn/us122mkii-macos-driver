#!/bin/bash
# build_us122_driver.sh  (step 4: full integration)
# Builds the HAL plugin bundle (with real shm IO), the helper daemon, and
# installs everything including the launchd plist.
#
# Run from the directory containing US122.driver/, us122d.c, shmring.h,
# and net.senesh.us122d.plist.

set -e

BUNDLE="US122.driver"
SRC="$BUNDLE/Contents/MacOS/US122.c"
OUT="$BUNDLE/Contents/MacOS/US122"

LIBUSB_INC="$(brew --prefix libusb)/include/libusb-1.0"
LIBUSB_LIB="$(brew --prefix libusb)/lib"

cp shmring.h "$BUNDLE/Contents/MacOS/shmring.h"

echo "[1/3] compiling HAL plugin (with shm IO)..."
clang -arch x86_64 -bundle -fno-common \
    -o "$OUT" "$SRC" \
    -I"$BUNDLE/Contents/MacOS" \
    -framework CoreAudio -framework CoreFoundation \
    -Wall
echo "  built: $OUT"
nm "$OUT" 2>/dev/null | grep -q "US122_Create" && echo "  US122_Create present" || echo "  WARNING: factory symbol missing"

echo "[2/3] compiling helper daemon us122d..."
clang us122d.c -o us122d -lpthread \
    -I"$LIBUSB_INC" -L"$LIBUSB_LIB" -lusb-1.0 \
    -Wall
echo "  built: us122d"

echo "[3/3] done building."
echo
echo "================ INSTALL (requires sudo) ================"
echo "# 0. clear old logs so this install starts fresh:"
echo "  sudo rm -f /var/log/us122d.log /var/log/us122d.err"
echo
echo "# 1. helper daemon binary:"
echo "  sudo cp us122d /usr/local/bin/us122d"
echo "  sudo chmod 755 /usr/local/bin/us122d"
echo
echo "# 2. launchd daemon:"
echo "  sudo cp net.senesh.us122d.plist /Library/LaunchDaemons/"
echo "  sudo chown root:wheel /Library/LaunchDaemons/net.senesh.us122d.plist"
echo "  sudo launchctl load /Library/LaunchDaemons/net.senesh.us122d.plist"
echo
echo "# 3. HAL plugin:"
echo "  sudo cp -R $BUNDLE /Library/Audio/Plug-Ins/HAL/"
echo "  sudo killall coreaudiod"
echo
echo "================ VERIFY ================"
echo "  sudo launchctl list | grep us122d"
echo "  cat /var/log/us122d.log"
echo "  # then select US-122MKII in Sound settings and play audio."
echo
echo "================ UNINSTALL ================"
echo "  sudo launchctl unload /Library/LaunchDaemons/net.senesh.us122d.plist"
echo "  sudo rm /Library/LaunchDaemons/net.senesh.us122d.plist /usr/local/bin/us122d"
echo "  sudo rm -rf /Library/Audio/Plug-Ins/HAL/$BUNDLE && sudo killall coreaudiod"
