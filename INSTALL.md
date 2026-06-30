# Installing the US-122MKII macOS Driver (V3)

A from-scratch driver that makes the discontinued **TASCAM US-122MKII** work on
modern macOS. V3 uses Apple's low-latency isochronous USB API, so it streams
glitch-free even under heavy system load.

> [!IMPORTANT]
> **Will your unit work?** Some hardware revisions of the US-122MKII boot "cold"
> and need the host to upload firmware on every connect. **This driver does not
> upload firmware**, so it only supports units that present themselves ready.
> Plug the device in and run:
> ```bash
> system_profiler SPUSBDataType | grep -B2 -A6 -i tascam
> ```
> Supported if you see **Vendor ID `0x0644`** and **Product ID `0x8021`**.
> Anything else (different ID, appears only briefly, or not at all) is a
> firmware-upload revision this driver cannot initialize.

## Quick install (recommended)

From the repository root:

```bash
./install.sh
```

This guided installer does everything below for you: checks prerequisites and
that your unit is a supported revision, builds the daemon + plugin + menu app,
installs them (it asks once for your admin password), and verifies the
low-latency driver is actually running. Other modes:

```bash
./install.sh --dry-run     # check + build only, no install
./install.sh --uninstall   # remove the driver
```

The rest of this file is the manual procedure, for reference or if you prefer to
run each step yourself.

## Requirements

- **macOS 13 Ventura (Intel).** Built and tested on 13.7.8. Other versions and
  Apple Silicon are untested.
- **Xcode Command Line Tools** (for the compiler): `xcode-select --install`.
- That's it. V3 needs **no Homebrew and no libusb** (it talks to IOKit directly).

## Build

From the repository root:

```bash
# if you got this as a tarball, restore the execute bit first:
chmod +x src/build/*.sh

# build the V3 daemon + the Core Audio plugin:
src/build/build_us122d_v3.sh

# build the menu-bar control app:
src/build/build_us122menu.sh
```

The first script prints the exact `sudo` install commands for your paths. They are
also summarized below.

## Install (requires sudo / your password)

```bash
# 1. the daemon (USB engine, runs as root):
sudo cp us122d /usr/local/bin/us122d
sudo chmod 755 /usr/local/bin/us122d

# 2. the launchd service:
sudo cp src/plist/net.senesh.us122d.plist /Library/LaunchDaemons/
sudo chown root:wheel /Library/LaunchDaemons/net.senesh.us122d.plist

# 3. the Core Audio plugin:
sudo rm -rf /Library/Audio/Plug-Ins/HAL/US122.driver
sudo cp -R src/plugin/US122.driver /Library/Audio/Plug-Ins/HAL/

# 4. start the daemon and reload Core Audio:
sudo launchctl unload /Library/LaunchDaemons/net.senesh.us122d.plist 2>/dev/null
sudo launchctl load   /Library/LaunchDaemons/net.senesh.us122d.plist
sudo killall coreaudiod

# 5. install + launch the menu app (no sudo):
cp -R US122Menu.app /Applications/
open /Applications/US122Menu.app
```

## Use

1. Launch **US122Menu** (step 5). A dot appears in the menu bar. It is the on/off
   hub: the daemon is dormant until the menu app is running.
2. Plug in the US-122MKII. The menu dot shows device + driver status.
3. Open **System Settings > Sound** (or Audio MIDI Setup) and pick **US-122MKII**
   for output and/or input.
4. Play or record. The menu's Buffer submenu sets latency vs safety.

## Verify it is healthy

```bash
tail -f /var/log/us122d.err
```
You should see `streaming` and, with the default debug logging, per-half-second
lines with `gap_max` in the low single-digit milliseconds and `pb_err=0`. That is
the low-latency transport doing its job. To silence production logging, remove the
`US122_DEBUG` block from the plist and reload it.

## Uninstall

```bash
sudo launchctl unload /Library/LaunchDaemons/net.senesh.us122d.plist
sudo rm -f /Library/LaunchDaemons/net.senesh.us122d.plist
sudo rm -f /usr/local/bin/us122d
sudo rm -rf /Library/Audio/Plug-Ins/HAL/US122.driver
rm -rf /Applications/US122Menu.app
sudo killall coreaudiod
```

## Troubleshooting

- **Device not in Sound settings.** Make sure the menu app is running (it gates
  the daemon) and the device shows `0x8021` in `system_profiler SPUSBDataType`.
  If you just installed the plugin, `sudo killall coreaudiod` once more.
- **Plugin won't load.** The build links an ad-hoc code signature automatically;
  if Core Audio still refuses it, confirm the bundle copied fully and recopy.
- **"device not found" in the log.** The unit is unplugged, or it is a
  firmware-upload revision (see the compatibility note at the top).
- **Nothing plays but no errors.** Confirm US-122MKII is the selected *output*
  device and its hardware outputs are connected/monitored.

## Disclaimer

Unofficial, not affiliated with TASCAM/TEAC. Reverse-engineered, provided as-is,
no warranty. Use at your own risk.
