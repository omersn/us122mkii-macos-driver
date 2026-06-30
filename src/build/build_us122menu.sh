#!/bin/bash
# build_us122menu.sh - build the US-122MKII menu-bar status/control app.
# Produces US122Menu.app (a menu-bar-only LSUIElement agent) in the repo root.
#
# Run it from anywhere; paths resolve relative to this script. Do NOT use sudo
# (this builds a user-session app; sudo would make it root-owned).

set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MENU="$ROOT/src/menu/US122Menu"
APP="$ROOT/US122Menu.app"
TMPD="$(mktemp -d)"; trap 'rm -rf "$TMPD"' EXIT   # private temp; avoids /tmp ownership clashes

echo "[1/3] compiling Swift menu-bar app (+ C shim)..."
# swiftc does NOT compile .c sources; compile the shim with clang first, link the .o.
clang -c "$MENU/shimopen.c" -o "$TMPD/shimopen.o"
swiftc -O -o "$TMPD/US122Menu" \
    "$MENU/main.swift" "$TMPD/shimopen.o" \
    -import-objc-header "$MENU/bridging.h" \
    -framework Cocoa
echo "  built binary"

echo "[2/3] assembling US122Menu.app bundle..."
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp "$TMPD/US122Menu" "$APP/Contents/MacOS/US122Menu"
chmod +x "$APP/Contents/MacOS/US122Menu"
cp "$MENU/Info.plist" "$APP/Contents/Info.plist"
echo "  assembled: $APP"

cat <<EOF

[3/3] done.

================ RUN / INSTALL (no sudo) ================
  open "$APP"                       # test it: a dot appears in the menu bar
  cp -R "$APP" /Applications/       # install for everyday use, then open it once
EOF
