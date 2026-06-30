#!/bin/bash
#
# install.sh - guided installer for the TASCAM US-122MKII macOS driver (V3).
#
# Builds the low-latency daemon, the Core Audio plugin, and the menu app, then
# installs them (asking once for your admin password) and verifies V3 is live.
#
# Usage:
#   ./install.sh            full guided install
#   ./install.sh --dry-run  check prerequisites + build, but do not install
#   ./install.sh --uninstall  remove the driver
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLIST="/Library/LaunchDaemons/net.senesh.us122d.plist"
HAL="/Library/Audio/Plug-Ins/HAL/US122.driver"
BIN="/usr/local/bin/us122d"
APP="/Applications/US122Menu.app"

# ---- pretty output ----
if [ -t 1 ]; then B=$'\033[1m'; G=$'\033[32m'; Y=$'\033[33m'; R=$'\033[31m'; C=$'\033[36m'; Z=$'\033[0m'; else B=""; G=""; Y=""; R=""; C=""; Z=""; fi
step(){ echo; echo "${B}${C}==> $*${Z}"; }
ok(){   echo "  ${G}OK${Z} $*"; }
warn(){ echo "  ${Y}!${Z}  $*"; }
die(){  echo "  ${R}x${Z}  $*" >&2; exit 1; }
ask(){  # ask "question" -> returns 0 for yes
    local a; read -r -p "  ${B}$1${Z} [y/N] " a < /dev/tty || a=""
    [[ "$a" =~ ^[Yy]$ ]]
}

DRYRUN=0; UNINSTALL=0
for a in "$@"; do
    case "$a" in
        --dry-run) DRYRUN=1 ;;
        --uninstall) UNINSTALL=1 ;;
        *) die "unknown option: $a" ;;
    esac
done

[ "$(id -u)" -eq 0 ] && die "Do not run with sudo. Run as yourself; I will ask for your password only for the install step."

echo "${B}TASCAM US-122MKII macOS driver - V3 installer${Z}"

# ===================== UNINSTALL =====================
if [ "$UNINSTALL" -eq 1 ]; then
    step "Uninstalling"
    ask "Remove the V3 driver (daemon, plugin, menu app)?" || die "cancelled"
    sudo -v || die "need admin rights"
    sudo launchctl unload "$PLIST" 2>/dev/null || true
    sudo rm -f "$PLIST" "$BIN"
    sudo rm -rf "$HAL"
    rm -rf "$APP"
    sudo killall coreaudiod 2>/dev/null || true
    osascript -e 'quit app "US122Menu"' 2>/dev/null || true
    ok "removed."
    exit 0
fi

# ===================== 1. PREREQUISITES =====================
step "Step 1/5  Checking prerequisites"
osver="$(sw_vers -productVersion 2>/dev/null || echo '?')"
case "$osver" in
    13.*) ok "macOS $osver (Ventura, the tested target)" ;;
    *)    warn "macOS $osver - V3 was built/tested on 13.x Ventura; other versions are untested." ;;
esac
[ "$(uname -m)" = "x86_64" ] || warn "arch $(uname -m) - tested on Intel (x86_64) only."
if xcode-select -p >/dev/null 2>&1; then ok "Xcode Command Line Tools present"; else
    die "Xcode Command Line Tools missing. Run:  xcode-select --install   then re-run this."
fi

# ===================== 2. DEVICE CHECK =====================
step "Step 2/5  Checking your US-122MKII"
usb="$(system_profiler SPUSBDataType 2>/dev/null)"
if echo "$usb" | grep -q "0x8021"; then
    ok "Supported unit found (Vendor 0x0644 / Product 0x8021, firmware resident)."
elif echo "$usb" | grep -qi "0x0644"; then
    warn "A TEAC/TASCAM device is present but NOT at product id 0x8021."
    warn "That is likely a firmware-upload hardware revision, which THIS DRIVER CANNOT INITIALIZE."
    ask "Install anyway?" || die "cancelled"
else
    warn "No US-122MKII detected right now. You can install and plug it in afterward,"
    warn "but only units that enumerate as 0x0644/0x8021 will work."
    ask "Continue?" || die "cancelled"
fi

# ===================== 3. BUILD =====================
step "Step 3/5  Building (daemon + plugin + menu app)"
chmod +x "$ROOT"/src/build/*.sh "$ROOT"/install.sh 2>/dev/null || true
"$ROOT/src/build/build_us122d_v3.sh" >/tmp/us122_build.log 2>&1 || { tail -20 /tmp/us122_build.log; die "daemon/plugin build failed (see /tmp/us122_build.log)"; }
ok "daemon (us122d) + Core Audio plugin built"
"$ROOT/src/build/build_us122menu.sh" >>/tmp/us122_build.log 2>&1 || { tail -20 /tmp/us122_build.log; die "menu app build failed (see /tmp/us122_build.log)"; }
ok "menu app (US122Menu.app) built"
strings "$ROOT/us122d" | grep -q "V3 low-latency" || die "built daemon is not V3 - aborting."
ok "verified built daemon is V3 (low-latency, native IOKit, no libusb)"

if [ "$DRYRUN" -eq 1 ]; then step "Dry run complete"; ok "Prereqs + build OK. Re-run without --dry-run to install."; exit 0; fi

# ===================== 4. INSTALL =====================
step "Step 4/5  Installing (needs your admin password)"
ask "Install V3 now? This replaces any previous us122d daemon." || die "cancelled"
sudo -v || die "need admin rights"
sudo rm -f /var/log/us122d.log /var/log/us122d.err
sudo cp "$ROOT/us122d" "$BIN" && sudo chmod 755 "$BIN"                     || die "daemon copy failed"
sudo cp "$ROOT/src/plist/net.senesh.us122d.plist" "$PLIST" && sudo chown root:wheel "$PLIST" || die "plist copy failed"
sudo rm -rf "$HAL" && sudo cp -R "$ROOT/src/plugin/US122.driver" "$HAL"    || die "plugin copy failed"
sudo launchctl unload "$PLIST" 2>/dev/null || true
sudo launchctl load   "$PLIST"                                            || die "launchctl load failed"
sudo killall coreaudiod 2>/dev/null || true
rm -rf "$APP" && cp -R "$ROOT/US122Menu.app" "$APP"                        || die "menu app copy failed"
ok "installed: daemon, launchd service, plugin, menu app"

# ===================== 5. VERIFY =====================
step "Step 5/5  Verifying V3 is live"
open "$APP" 2>/dev/null || true   # launches the menu app -> wakes the daemon
ok "launched US122Menu (the on/off hub)"
# give the daemon a moment to start and log
for _ in $(seq 1 10); do grep -q "starting" /var/log/us122d.err 2>/dev/null && break; sleep 0.5; done
if grep -q "V3 low-latency" /var/log/us122d.err 2>/dev/null; then
    ok "daemon log confirms ${G}V3 low-latency${Z} is running"
elif grep -q "build6-buffer" /var/log/us122d.err 2>/dev/null; then
    die "the OLD V1 daemon is running - the install did not take. Check $BIN."
else
    warn "could not read the daemon log yet; check:  tail -f /var/log/us122d.err"
fi

step "Done"
echo "  Plug in the US-122MKII, open ${B}System Settings > Sound${Z}, pick ${B}US-122MKII${Z}, and play."
echo "  Health/jitter log:  ${C}tail -f /var/log/us122d.err${Z}"
echo "  Menu bar dot is your on/off + status + buffer control."
echo "  Uninstall any time:  ${C}./install.sh --uninstall${Z}"
