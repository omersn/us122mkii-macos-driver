#!/bin/bash
# build_us122d_v3.sh
#
# Builds the V3 driver: the low-latency IOKit daemon (us122d_ll.c -> us122d)
# and the Core Audio HAL plugin. V3 keeps V1's plugin + menu + shm contract and
# swaps only the USB transport to Apple's low-latency isochronous API (the jitter
# cure). Run it from anywhere; paths are resolved relative to this script.
#
# Output (placed in the repo root):
#   us122d                 the V3 daemon binary
#   src/plugin/US122.driver/Contents/MacOS/US122   the built plugin bundle binary
#
# Then follow the printed sudo install commands (or see INSTALL.md).

set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DAEMON_SRC="$ROOT/src/daemon/us122d_ll.c"
INC="$ROOT/src/daemon"
PLUGIN="$ROOT/src/plugin/US122.driver"
PLUGIN_SRC="$PLUGIN/Contents/MacOS/US122.c"

echo "[1/2] compiling V3 low-latency daemon (us122d)..."
clang "$DAEMON_SRC" -o "$ROOT/us122d" \
    -I"$INC" \
    -framework IOKit -framework CoreFoundation -lpthread \
    -Wall -O2
echo "  built: $ROOT/us122d"

echo "[2/2] compiling HAL plugin..."
cp "$INC/shmring.h" "$PLUGIN/Contents/MacOS/shmring.h"
clang -arch x86_64 -bundle -fno-common \
    -o "$PLUGIN/Contents/MacOS/US122" "$PLUGIN_SRC" \
    -I"$PLUGIN/Contents/MacOS" \
    -framework CoreAudio -framework CoreFoundation \
    -Wall
nm "$PLUGIN/Contents/MacOS/US122" 2>/dev/null | grep -q US122_Create \
    && echo "  built plugin: US122_Create present" \
    || echo "  WARNING: factory symbol US122_Create missing"

cat <<EOF

================ INSTALL (requires sudo) ================
# 0. clear old logs:
  sudo rm -f /var/log/us122d.log /var/log/us122d.err

# 1. V3 daemon binary (replaces any previous us122d):
  sudo cp "$ROOT/us122d" /usr/local/bin/us122d
  sudo chmod 755 /usr/local/bin/us122d

# 2. launchd daemon (reuses the existing plist):
  sudo cp "$ROOT/src/plist/net.senesh.us122d.plist" /Library/LaunchDaemons/
  sudo chown root:wheel /Library/LaunchDaemons/net.senesh.us122d.plist

# 3. HAL plugin:
  sudo rm -rf /Library/Audio/Plug-Ins/HAL/US122.driver
  sudo cp -R "$PLUGIN" /Library/Audio/Plug-Ins/HAL/

# 4. (re)start everything:
  sudo launchctl unload /Library/LaunchDaemons/net.senesh.us122d.plist 2>/dev/null
  sudo launchctl load   /Library/LaunchDaemons/net.senesh.us122d.plist
  sudo killall coreaudiod

# 5. menu app (user-session control):
  "$ROOT/src/build/build_us122menu.sh"   # then: open the built US122Menu.app

================ VERIFY ================
  tail -f /var/log/us122d.err          # health + jitter (US122_DEBUG=1 in plist)
  # launch the menu app, plug in the US-122MKII, pick it in Sound settings, play.
EOF
