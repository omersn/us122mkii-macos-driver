// US122Menu — menu-bar status & control for the US-122MKII V1 driver.
//
// Runs in the user session (NOT inside coreaudiod), so it can freely read the
// daemon's POSIX shared-memory segment and is fully debuggable.
//
// Shows a colored dot in the menu bar:
//   green   = device present AND streaming
//   amber   = device present but idle
//   hollow  = device absent (or hidden)
// Menu lets the user Show/Hide the virtual audio device via the user_hide flag.
//
// Build (10.13+):
//   swiftc -O -o US122Menu main.swift -framework Cocoa
// Packaged as an LSUIElement .app (no dock icon) — see Info.plist.

import Cocoa

// ---- shared-memory contract (must match shmring.h) ----
// Field byte offsets within the segment header:
//   magic=0 version=4 rate=8 running=12 dev_frames=16 device_present=24
//   user_hide=28 app_active=32 status=36 (96 bytes)
let SHM_NAME        = "/us122_shm"
let SHM_MAGIC: UInt32   = 0x55533132   // "US12"
let SHM_VERSION: UInt32 = 6
let OFF_MAGIC          = 0
let OFF_VERSION        = 4
let OFF_RATE           = 8
let OFF_RUNNING        = 12
let OFF_DEVICE_PRESENT = 24
let OFF_USER_HIDE      = 28
let OFF_APP_ACTIVE     = 32
let OFF_STATUS         = 36
let OFF_BUFFER_MS      = 132   // 36 + 96 (status) = 132
let STATUS_LEN         = 96
let HEADER_MAP_LEN     = 192  // header prefix incl. status + buffer_ms

final class ShmAccess {
    private var base: UnsafeMutableRawPointer?
    private var mapped = false

    /// Attach to the daemon's shm. Returns false if not available yet.
    @discardableResult
    func attachIfNeeded() -> Bool {
        if mapped { return true }
        // shm_open is variadic in Darwin and Swift can't call it, so we go
        // through a tiny non-variadic C shim (see shimopen.c / bridging.h).
        let fd = us122_shm_open_rw(SHM_NAME)
        if fd < 0 { return false }
        let p = mmap(nil, HEADER_MAP_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
        close(fd)
        if p == MAP_FAILED { return false }
        // validate magic/version so we don't read a stale/foreign segment
        let magic = p!.load(fromByteOffset: OFF_MAGIC, as: UInt32.self)
        let ver   = p!.load(fromByteOffset: OFF_VERSION, as: UInt32.self)
        if magic != SHM_MAGIC || ver != SHM_VERSION {
            munmap(p, HEADER_MAP_LEN)
            return false
        }
        base = p
        mapped = true
        return true
    }

    func detach() {
        if let b = base { munmap(b, HEADER_MAP_LEN); base = nil; mapped = false }
    }

    private func readU32(_ off: Int) -> UInt32? {
        guard let b = base else { return nil }
        return b.load(fromByteOffset: off, as: UInt32.self)
    }
    private func writeU32(_ off: Int, _ v: UInt32) {
        guard let b = base else { return }
        b.storeBytes(of: v, toByteOffset: off, as: UInt32.self)
    }

    var devicePresent: Bool { (readU32(OFF_DEVICE_PRESENT) ?? 0) != 0 }
    var running: Bool       { (readU32(OFF_RUNNING) ?? 0) != 0 }
    var rate: UInt32        { readU32(OFF_RATE) ?? 0 }
    var userHide: Bool {
        get { (readU32(OFF_USER_HIDE) ?? 0) != 0 }
        set { writeU32(OFF_USER_HIDE, newValue ? 1 : 0) }
    }
    var appActive: Bool {
        get { (readU32(OFF_APP_ACTIVE) ?? 0) != 0 }
        set { writeU32(OFF_APP_ACTIVE, newValue ? 1 : 0) }
    }
    var bufferMs: UInt32 {
        get { readU32(OFF_BUFFER_MS) ?? 0 }
        set { writeU32(OFF_BUFFER_MS, newValue) }
    }
    /// The daemon's last status line (NUL-terminated C string in shm).
    var statusString: String {
        guard let b = base else { return "" }
        let p = b.advanced(by: OFF_STATUS).assumingMemoryBound(to: CChar.self)
        return String(cString: p)
    }
}

enum DotState { case streaming, idle, absent, starting }

final class AppDelegate: NSObject, NSApplicationDelegate {
    let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    let shm = ShmAccess()
    var timer: Timer?

    // four info-only lines (all disabled / non-clickable)
    let lineDevice    = NSMenuItem(title: "Device: …",      action: nil, keyEquivalent: "")
    let lineDriver    = NSMenuItem(title: "macOS Driver: …", action: nil, keyEquivalent: "")
    let lineStreaming = NSMenuItem(title: "Streaming: …",    action: nil, keyEquivalent: "")
    let lineDaemon    = NSMenuItem(title: "Daemon: …",       action: nil, keyEquivalent: "")

    // buffer presets (ms). Lower = less latency, less stall tolerance.
    let bufferPresets: [UInt32] = [128, 256, 512, 1024, 1536]
    let bufferDefault: UInt32 = 512
    let bufferMenuItem = NSMenuItem(title: "Buffer", action: nil, keyEquivalent: "")
    let bufferSubmenu = NSMenu()

    // startup grace period: blink yellow + "please wait" to mask the acquire gap
    var startupUntil: Date = Date().addingTimeInterval(5.0)
    var blinkOn = false

    func applicationDidFinishLaunching(_ note: Notification) {
        // We are the on/off hub: tell the daemon to become ACTIVE, and SHOW the
        // virtual device (clear any prior hide) so opening the app makes the
        // device appear in macOS.
        shm.attachIfNeeded()
        shm.appActive = true
        shm.userHide = false

        startupUntil = Date().addingTimeInterval(5.0)
        buildMenu()
        refresh()
        // A scheduledTimer only runs in .default mode, so it STOPS while a menu
        // is open (the run loop switches to event-tracking mode) and the menu
        // appears frozen until reopened. Registering in .common mode makes it
        // keep firing during menu tracking, so the live view updates in place.
        let t = Timer(timeInterval: 0.5, repeats: true) { [weak self] _ in
            self?.refresh()
        }
        RunLoop.current.add(t, forMode: .common)
        timer = t
    }

    func applicationWillTerminate(_ note: Notification) {
        // Hub closing: HIDE the virtual device, then let the daemon go dormant.
        // Setting user_hide drives the plugin to remove the device from macOS.
        // The plugin's presence watcher polls ~4x/sec, so we give it a brief
        // moment to fire the coreaudiod notification before the process exits;
        // otherwise the device can linger until coreaudiod next polls.
        if shm.attachIfNeeded() {
            shm.userHide = true
            shm.appActive = false
            usleep(350000)   // ~350ms: let the watcher publish the removal
        }
    }

    func buildMenu() {
        let menu = NSMenu()
        lineDevice.isEnabled = false
        lineDriver.isEnabled = false
        lineStreaming.isEnabled = false
        lineDaemon.isEnabled = false
        menu.addItem(lineDevice)
        menu.addItem(lineDriver)
        menu.addItem(lineStreaming)
        menu.addItem(lineDaemon)
        menu.addItem(.separator())

        // Buffer submenu: discrete ms presets, checkmark on active, grayed while
        // streaming (changing buffer depth is only safe between streams).
        for ms in bufferPresets {
            let title = ms == bufferDefault ? "\(ms) ms (default)" : "\(ms) ms"
            let it = NSMenuItem(title: title, action: #selector(selectBuffer(_:)), keyEquivalent: "")
            it.target = self
            it.representedObject = ms
            bufferSubmenu.addItem(it)
        }
        bufferMenuItem.submenu = bufferSubmenu
        menu.addItem(bufferMenuItem)
        menu.addItem(.separator())

        let quit = NSMenuItem(title: "Quit", action: #selector(confirmQuit), keyEquivalent: "q")
        quit.target = self
        menu.addItem(quit)
        statusItem.menu = menu
    }

    @objc func selectBuffer(_ sender: NSMenuItem) {
        guard let ms = sender.representedObject as? UInt32 else { return }
        guard shm.attachIfNeeded() else { return }
        // only changeable while not streaming (menu is grayed then anyway)
        if shm.running { return }
        shm.bufferMs = ms
        refresh()
    }

    @objc func confirmQuit() {
        // mid-stream safety: warn before quitting (which drops the device) while
        // audio is actively streaming.
        if shm.attachIfNeeded() && shm.running {
            let a = NSAlert()
            a.messageText = "Audio is currently playing"
            a.informativeText = "Quitting will stop the device and interrupt playback. Quit anyway?"
            a.addButton(withTitle: "Quit")
            a.addButton(withTitle: "Cancel")
            NSApp.activate(ignoringOtherApps: true)
            if a.runModal() != .alertFirstButtonReturn { return }
        }
        NSApp.terminate(nil)
    }

    func refresh() {
        let attached = shm.attachIfNeeded()
        if attached { shm.appActive = true }   // keep asserting while we run
        let present  = attached && shm.devicePresent
        let running  = attached && shm.running
        let hidden   = attached && shm.userHide
        let inStartup = Date() < startupUntil

        // dot state
        let state: DotState
        if inStartup && !present { state = .starting }
        else if !present         { state = .absent }
        else if running          { state = .streaming }
        else                     { state = .idle }

        // blinking handled for the starting state
        blinkOn.toggle()
        statusItem.button?.image = dotImage(for: state, blinkOn: blinkOn)
        statusItem.button?.image?.isTemplate = false

        // ---- four info lines ----
        if state == .starting {
            lineDevice.title    = "Device: please wait…"
            lineDriver.title    = "macOS Driver: starting"
            lineStreaming.title = "Streaming: —"
            lineDaemon.title    = "Daemon: starting up"
            return
        }

        lineDevice.title = "Device: " + (present ? "Connected" : "Disconnected")
        lineDriver.title = "macOS Driver: " + ((attached && !hidden) ? "On" : "Off")
        if running {
            let r = Int(shm.rate)
            let grouped = NumberFormatter.localizedString(
                from: NSNumber(value: r), number: .decimal)
            lineStreaming.title = "Streaming: On @ \(grouped) Hz"
        } else {
            lineStreaming.title = "Streaming: Off"
        }
        let st = attached ? shm.statusString : ""
        _ = st   // daemon status string no longer shown (was stale/duplicative)
        lineDaemon.title = "Daemon: " + (attached ? "Running" : "Not running")

        // buffer submenu: checkmark active value, gray the whole thing while
        // streaming (buffer changes only apply between streams).
        let curBuf = attached ? shm.bufferMs : 0
        bufferMenuItem.isEnabled = attached && !running
        if running {
            // submenu unreachable while streaming, so surface the active value here
            bufferMenuItem.title = "Buffer: \(curBuf) ms"
        } else {
            bufferMenuItem.title = "Buffer"
        }
        for it in bufferSubmenu.items {
            it.isEnabled = attached && !running
            if let ms = it.representedObject as? UInt32 {
                it.state = (ms == curBuf) ? .on : .off
            }
        }
    }

    /// Render a small filled circle of the right color as the menu-bar icon.
    func dotImage(for state: DotState, blinkOn: Bool) -> NSImage {
        let size = NSSize(width: 14, height: 14)
        let img = NSImage(size: size)
        img.lockFocus()
        let rect = NSRect(x: 2, y: 2, width: 10, height: 10)
        let color: NSColor
        switch state {
        case .streaming: color = NSColor.systemGreen
        case .idle:      color = NSColor.systemOrange
        case .absent:    color = NSColor.systemGray
        case .starting:  color = blinkOn ? NSColor.systemYellow
                                         : NSColor.systemYellow.withAlphaComponent(0.25)
        }
        let path = NSBezierPath(ovalIn: rect)
        color.setFill()
        path.fill()
        img.unlockFocus()
        return img
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)   // LSUIElement-style: no dock icon
let delegate = AppDelegate()
app.delegate = delegate
app.run()
