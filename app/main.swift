// Tether.app -- a menu bar front end for tetherd.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Launch the app and it starts tetherd, which waits for the phone and brings
// up the network interface. The menu bar icon shows the state; "Quit Tether"
// disconnects and removes the interface.
//
// tetherd needs root. On first launch (and after an update) the app asks for
// an administrator password once and installs:
//   /Library/PrivilegedHelperTools/tetherd   a root-owned copy of the daemon
//   /etc/sudoers.d/tetherd                   lets this user run exactly that
//                                            binary through sudo, no password
// After that the app runs it with `sudo -n` and needs no further prompts.

import AppKit
import SwiftUI

let helperPath = "/Library/PrivilegedHelperTools/tetherd"
let sudoersPath = "/etc/sudoers.d/tetherd"
let bootDaemonPlist = "/Library/LaunchDaemons/org.tetherd.daemon.plist"
let logURL = FileManager.default.homeDirectoryForCurrentUser
    .appendingPathComponent("Library/Logs/Tether.log")

// MARK: - Helpers

/// Runs a command and returns (exit status, combined output).
@discardableResult
func run(_ path: String, _ args: [String]) -> (Int32, String) {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: path)
    p.arguments = args
    let pipe = Pipe()
    p.standardOutput = pipe
    p.standardError = pipe
    p.standardInput = FileHandle.nullDevice
    do { try p.run() } catch { return (-1, "\(error)") }
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    p.waitUntilExit()
    return (p.terminationStatus, String(decoding: data, as: UTF8.self))
}

/// Single-quotes a string for /bin/sh.
func shellQuote(_ s: String) -> String {
    "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
}

/// Runs a shell script as root, after the standard macOS password prompt.
/// Returns nil on success, or an error message (including "User canceled").
func runAsAdmin(_ script: String, prompt: String) -> String? {
    let source = "do shell script \(appleScriptString(script)) "
        + "with prompt \(appleScriptString(prompt)) with administrator privileges"
    var error: NSDictionary?
    NSAppleScript(source: source)?.executeAndReturnError(&error)
    if let error = error {
        return (error[NSAppleScript.errorMessage] as? String) ?? "unknown error"
    }
    return nil
}

func appleScriptString(_ s: String) -> String {
    "\"" + s.replacingOccurrences(of: "\\", with: "\\\\")
        .replacingOccurrences(of: "\"", with: "\\\"") + "\""
}

// MARK: - Privileged helper installation

enum Setup {
    static var bundledHelper: String? {
        Bundle.main.path(forResource: "tetherd", ofType: nil)
    }

    /// True if the installed helper matches the bundled one and sudo will
    /// run it without a password.
    static func isReady() -> Bool {
        guard let bundled = bundledHelper,
              let a = FileManager.default.contents(atPath: bundled),
              let b = FileManager.default.contents(atPath: helperPath),
              a == b else { return false }
        return run("/usr/bin/sudo", ["-n", "-l", helperPath]).0 == 0
    }

    /// Installs the helper and the sudoers rule. Returns nil on success.
    static func install() -> String? {
        guard let bundled = bundledHelper else {
            return "The app bundle has no tetherd inside it. Rebuild with `make app`."
        }
        let user = NSUserName()
        // Validate the rule with visudo before putting it in place: a broken
        // file in sudoers.d would break sudo for everyone.
        let rule = "\(user) ALL=(root) NOPASSWD: \(helperPath)"
        let script = """
            set -e
            install -d -o root -g wheel -m 755 /Library/PrivilegedHelperTools
            install -o root -g wheel -m 755 \(shellQuote(bundled)) \(shellQuote(helperPath))
            tmp=$(mktemp /tmp/tetherd-sudoers.XXXXXX)
            printf '%s\\n' \(shellQuote("# Installed by Tether.app: lets \(user) start tetherd.")) \(shellQuote(rule)) > "$tmp"
            /usr/sbin/visudo -c -q -f "$tmp"
            install -o root -g wheel -m 440 "$tmp" \(shellQuote(sudoersPath))
            rm -f "$tmp"
            """
        return runAsAdmin(script, prompt:
            "Tether needs to install its helper so it can create the tethering network interface.")
    }

    static func uninstall() -> String? {
        runAsAdmin("rm -f \(shellQuote(helperPath)) \(shellQuote(sudoersPath))",
                   prompt: "Tether wants to remove its helper.")
    }

    static var bootDaemonInstalled: Bool {
        FileManager.default.fileExists(atPath: bootDaemonPlist)
    }

    static func removeBootDaemon() -> String? {
        runAsAdmin("""
            launchctl bootout system/org.tetherd.daemon 2>/dev/null || true
            rm -f \(shellQuote(bootDaemonPlist)) /usr/local/sbin/tetherd
            """, prompt: "Tether wants to remove the tetherd boot-time daemon.")
    }
}

// MARK: - tetherd process

enum TetherState: Equatable {
    case stopped
    case starting
    case waiting                          // for the phone
    case connecting(String)               // device name
    case connected(String, String)        // device name, interface
    case failed(String)
}

final class Daemon {
    private var process: Process?
    private var stdinPipe: Pipe?
    private var buffer = Data()
    private var stopRequested = false
    private var lastError: String?
    private var logHandle: FileHandle?

    var onStateChange: ((TetherState) -> Void)?
    /// tetherd made the phone the Mac's main connection (true) or stopped (false).
    var onPrimaryChange: ((Bool) -> Void)?
    private(set) var state: TetherState = .stopped {
        didSet { if state != oldValue { onStateChange?(state) } }
    }
    var isRunning: Bool { process?.isRunning ?? false }

    func start(verbose: Bool, primary: Bool) {
        guard !isRunning else { return }
        openLog()
        stopRequested = false
        lastError = nil
        buffer.removeAll()

        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/sudo")
        p.arguments = ["-n", helperPath, "--stdin-eof"]
            + (verbose ? ["--verbose"] : []) + (primary ? ["--primary"] : [])
        let out = Pipe()
        let input = Pipe()
        p.standardOutput = out
        p.standardError = out
        p.standardInput = input
        out.fileHandleForReading.readabilityHandler = { [weak self] h in
            let data = h.availableData
            if data.isEmpty { h.readabilityHandler = nil; return }   // EOF
            DispatchQueue.main.async { self?.consume(data) }
        }
        p.terminationHandler = { [weak self] proc in
            DispatchQueue.main.async { self?.exited(proc.terminationStatus) }
        }
        do {
            try p.run()
        } catch {
            state = .failed("Cannot start tetherd: \(error.localizedDescription)")
            return
        }
        process = p
        stdinPipe = input
        state = .starting
    }

    /// Closing tetherd's stdin makes it tear down the interface and exit.
    func stop() {
        guard isRunning else { return }
        stopRequested = true
        try? stdinPipe?.fileHandleForWriting.close()
        stdinPipe = nil
    }

    /// Stops and waits (up to `timeout`) for tetherd to exit.
    func stopAndWait(timeout: TimeInterval) {
        stop()
        let deadline = Date().addingTimeInterval(timeout)
        while isRunning && Date() < deadline {
            RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        }
    }

    private func consume(_ data: Data) {
        guard !data.isEmpty else { return }
        logHandle?.write(data)
        buffer.append(data)
        while let nl = buffer.firstIndex(of: UInt8(ascii: "\n")) {
            let line = String(decoding: buffer[buffer.startIndex..<nl], as: UTF8.self)
            buffer.removeSubrange(buffer.startIndex...nl)
            handle(line: line)
        }
    }

    private func handle(line: String) {
        if line.contains("tetherd[error]:") {
            lastError = line.components(separatedBy: "tetherd[error]: ").last
        }
        if line.contains("is now the main connection") {
            onPrimaryChange?(true)
        } else if line.contains("is no longer the main connection") {
            onPrimaryChange?(false)
        }
        if line.contains("waiting for an RNDIS device") {
            state = .waiting
        } else if let m = line.firstMatch(#"found RNDIS device '(.*)' \("#) {
            state = .connecting(m[0])
        } else if let m = line.firstMatch(#"tethering through '(.*)' is running on (\S+)"#) {
            state = .connected(m[0], m[1])
        } else if line.contains("stopping tethering through") && !stopRequested {
            state = .waiting
        } else if line.contains("could not start tethering with") {
            state = .failed(lastError ?? "Could not start tethering.")
        } else if line.contains("a password is required") || line.contains("sudo:") {
            lastError = line
        }
    }

    private func exited(_ status: Int32) {
        process = nil
        stdinPipe = nil
        if stopRequested || status == 0 {
            state = .stopped
        } else {
            state = .failed(lastError ?? "tetherd exited with status \(status).")
        }
    }

    private func openLog() {
        guard logHandle == nil else { return }
        let fm = FileManager.default
        if !fm.fileExists(atPath: logURL.path) {
            fm.createFile(atPath: logURL.path, contents: nil)
        }
        logHandle = try? FileHandle(forWritingTo: logURL)
        logHandle?.seekToEndOfFile()
    }
}

extension String {
    /// Capture groups of the first match of `pattern`, or nil.
    func firstMatch(_ pattern: String) -> [String]? {
        guard let re = try? NSRegularExpression(pattern: pattern),
              let m = re.firstMatch(in: self, range: NSRange(startIndex..., in: self))
        else { return nil }
        return (1..<m.numberOfRanges).compactMap {
            Range(m.range(at: $0), in: self).map { String(self[$0]) }
        }
    }
}

// MARK: - App

/// What the panel shows. The app delegate owns it and keeps it current.
final class TetherModel: ObservableObject {
    @Published var state: TetherState = .stopped
    @Published var address: String?
    @Published var running = false
    @Published var verbose = UserDefaults.standard.bool(forKey: "verbose") {
        didSet { UserDefaults.standard.set(verbose, forKey: "verbose") }
    }
    /// The setting: route the Mac's traffic through the phone, ahead of Wi-Fi.
    @Published var preferPhone = UserDefaults.standard.object(forKey: "preferPhone") as? Bool ?? true {
        didSet { UserDefaults.standard.set(preferPhone, forKey: "preferPhone") }
    }
    /// The fact: tetherd has made the phone the main connection.
    @Published var isPrimary = false

    /// From the moment the phone is found until macOS has an address for it.
    var isLoading: Bool {
        switch state {
        case .connecting: return true
        case .connected: return address == nil
        default: return false
        }
    }

    var connect: () -> Void = {}
    var disconnect: () -> Void = {}
    var toggleVerbose: () -> Void = {}
    var togglePreferPhone: () -> Void = {}
    var openLog: () -> Void = {}
    var removeHelper: () -> Void = {}
    var quit: () -> Void = {}
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    private let daemon = Daemon()
    private let model = TetherModel()
    private var statusItem: NSStatusItem!
    private let popover = NSPopover()
    private var addressTimer: Timer?

    func applicationDidFinishLaunching(_ notification: Notification) {
        model.connect = { [weak self] in self?.connect() }
        model.disconnect = { [weak self] in self?.daemon.stop() }
        model.toggleVerbose = { [weak self] in self?.toggleVerbose() }
        model.togglePreferPhone = { [weak self] in self?.togglePreferPhone() }
        model.openLog = { NSWorkspace.shared.open(logURL) }
        model.removeHelper = { [weak self] in self?.uninstallHelper() }
        model.quit = { NSApp.terminate(nil) }

        popover.behavior = .transient
        popover.animates = true
        let host = NSHostingController(rootView: PanelView(model: model))
        // Report the SwiftUI content's size to the popover, so it resizes
        // (and stays below the menu bar) when rows appear or disappear.
        host.sizingOptions = [.preferredContentSize]
        popover.contentViewController = host

        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        statusItem.button?.target = self
        statusItem.button?.action = #selector(togglePanel)
        daemon.onStateChange = { [weak self] state in self?.stateChanged(state) }
        daemon.onPrimaryChange = { [weak self] primary in self?.model.isPrimary = primary }
        refreshIcon()
        if let preview = ProcessInfo.processInfo.environment["TETHER_PREVIEW"] {
            showPreview(preview)
        } else {
            connect()
        }
    }

    /// For checking the panel's design without a phone or the helper:
    /// TETHER_PREVIEW=waiting|linking|online|error Tether.app/Contents/MacOS/Tether
    private func showPreview(_ which: String) {
        let phone = "Redmi Note 13 NFC"
        switch which {
        case "waiting": model.state = .waiting
        case "linking": model.state = .connected(phone, "feth0")
        case "error": model.state = .failed("The RNDIS control interface is already in use.")
        default:
            model.state = .connected(phone, "feth0")
            model.address = "10.191.118.95"
            model.isPrimary = true
        }
        model.running = which != "error"
        refreshIcon()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { self.togglePanel() }
        // TETHER_PREVIEW_GEOMETRY=1: report whether the open panel fits on
        // screen, before and after the content grows (the address arrives).
        if ProcessInfo.processInfo.environment["TETHER_PREVIEW_GEOMETRY"] != nil {
            func report(_ when: String) {
                guard let window = self.popover.contentViewController?.view.window,
                      let screen = window.screen else { return }
                let content = self.popover.contentViewController!.view
                print("\(when): window \(window.frame) visible top \(screen.visibleFrame.maxY) "
                      + "content \(content.frame.size) fitting \(content.fittingSize) "
                      + "contentTop \(window.convertToScreen(content.convert(content.bounds, to: nil)).maxY) "
                      + "fits=\(window.convertToScreen(content.convert(content.bounds, to: nil)).maxY <= screen.visibleFrame.maxY && content.frame.height >= content.fittingSize.height - 1)")
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
                report("before")
                self.model.address = "10.191.118.95"
                self.model.isPrimary = true
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 3) { report("after"); exit(0) }
        }
        // TETHER_PREVIEW_SNAPSHOT=<file.png>: save a picture of the panel and quit.
        if let path = ProcessInfo.processInfo.environment["TETHER_PREVIEW_SNAPSHOT"] {
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
                // Glass needs the window server, so this shows layout and
                // text but not the glass itself.
                let renderer = ImageRenderer(content: PanelView(model: self.model)
                    .background(LinearGradient(colors: [.teal, .indigo],
                                               startPoint: .top, endPoint: .bottom)))
                renderer.scale = 2
                if let image = renderer.nsImage, let tiff = image.tiffRepresentation,
                   let rep = NSBitmapImageRep(data: tiff) {
                    try? rep.representation(using: .png, properties: [:])?
                        .write(to: URL(fileURLWithPath: path))
                }
                exit(0)
            }
        }
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        daemon.stopAndWait(timeout: 5)
        return .terminateNow
    }

    @objc func togglePanel() {
        if popover.isShown {
            popover.performClose(nil)
        } else if let button = statusItem.button, let content = popover.contentViewController {
            // Size it before showing: positioned at a stale size, the popover
            // would hang off the top of the screen.
            content.view.layoutSubtreeIfNeeded()
            popover.contentSize = content.view.fittingSize
            NSApp.activate(ignoringOtherApps: true)
            popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
        }
    }

    // MARK: Actions

    func connect() {
        if Setup.bootDaemonInstalled && !offerToRemoveBootDaemon() {
            return
        }
        if !Setup.isReady() {
            if let error = Setup.install() {
                if !error.contains("User canceled") {
                    alert("Could not install the helper", error)
                }
                stateChanged(.failed("The helper is not installed."))
                return
            }
            guard Setup.isReady() else {
                alert("The helper was installed but sudo still asks for a password",
                      "Check that /etc/sudoers includes /private/etc/sudoers.d.")
                return
            }
        }
        daemon.start(verbose: model.verbose, primary: model.preferPhone)
        model.running = daemon.isRunning
    }

    /// Settings are tetherd arguments, so a change restarts it.
    private func restartIfRunning() {
        if daemon.isRunning {
            daemon.stopAndWait(timeout: 5)
            daemon.start(verbose: model.verbose, primary: model.preferPhone)
            model.running = daemon.isRunning
        }
    }

    private func toggleVerbose() {
        model.verbose.toggle()
        restartIfRunning()
    }

    private func togglePreferPhone() {
        model.preferPhone.toggle()
        restartIfRunning()
    }

    private func uninstallHelper() {
        popover.performClose(nil)
        daemon.stopAndWait(timeout: 5)
        if let error = Setup.uninstall(), !error.contains("User canceled") {
            alert("Could not remove the helper", error)
        }
    }

    /// The `make install` boot daemon would grab the phone first.
    private func offerToRemoveBootDaemon() -> Bool {
        let a = NSAlert()
        a.messageText = "The tetherd boot-time daemon is installed"
        a.informativeText = "It already runs tetherd in the background and holds the phone, "
            + "so the app cannot use it. Remove the daemon and let the app control tethering?"
        a.addButton(withTitle: "Remove Daemon")
        a.addButton(withTitle: "Cancel")
        NSApp.activate(ignoringOtherApps: true)
        guard a.runModal() == .alertFirstButtonReturn else { return false }
        if let error = Setup.removeBootDaemon() {
            if !error.contains("User canceled") { alert("Could not remove the daemon", error) }
            return false
        }
        return true
    }

    private func alert(_ title: String, _ text: String) {
        let a = NSAlert()
        a.messageText = title
        a.informativeText = text
        NSApp.activate(ignoringOtherApps: true)
        a.runModal()
    }

    // MARK: State

    private func stateChanged(_ state: TetherState) {
        model.state = state
        model.running = daemon.isRunning
        if case .connected = state {} else { model.isPrimary = false }
        addressTimer?.invalidate()
        model.address = nil
        if case .connected(_, let iface) = state {
            pollAddress(iface)
            // .common so the poll also runs while the panel is being tracked.
            let timer = Timer(timeInterval: 1, repeats: true) { [weak self] _ in
                self?.pollAddress(iface)
            }
            RunLoop.main.add(timer, forMode: .common)
            addressTimer = timer
        }
        refreshIcon()
    }

    private func pollAddress(_ iface: String) {
        DispatchQueue.global().async {
            let (status, out) = run("/usr/sbin/ipconfig", ["getifaddr", iface])
            let ip = out.trimmingCharacters(in: .whitespacesAndNewlines)
            DispatchQueue.main.async {
                guard case .connected(_, let current) = self.model.state, current == iface else { return }
                if status == 0 && !ip.isEmpty {
                    self.model.address = ip
                    self.addressTimer?.invalidate()   // got it; stop polling
                }
                self.refreshIcon()
            }
        }
    }

    private func refreshIcon() {
        let symbol: String
        switch model.state {
        case .connected, .connecting: symbol = "personalhotspot"
        case .failed: symbol = "exclamationmark.triangle"
        case .stopped: symbol = "cable.connector.slash"
        default: symbol = "cable.connector"
        }
        if let image = NSImage(systemSymbolName: symbol, accessibilityDescription: "Tether") {
            image.isTemplate = true
            statusItem.button?.image = image
        }
        statusItem.button?.appearsDisabled = model.state == .stopped
        statusItem.button?.toolTip = "Tether: " + model.state.summary(address: model.address)
        updateAnimation()
    }

    // MARK: Menu bar loading animation

    private var animationTimer: Timer?
    private var animationStart = Date()

    /// While loading, the menu bar icon breathes.
    private func updateAnimation() {
        if model.isLoading {
            guard animationTimer == nil else { return }
            animationStart = Date()
            let timer = Timer(timeInterval: 1.0 / 30, repeats: true) { [weak self] _ in
                guard let self = self else { return }
                let t = Date().timeIntervalSince(self.animationStart)
                let wave = 0.5 - 0.5 * cos(t * 2 * .pi / 1.4)   // 0 -> 1 -> 0 every 1.4 s
                self.statusItem.button?.alphaValue = CGFloat(1 - 0.75 * wave)
            }
            RunLoop.main.add(timer, forMode: .common)
            animationTimer = timer
        } else {
            animationTimer?.invalidate()
            animationTimer = nil
            statusItem.button?.alphaValue = 1
        }
    }
}

extension TetherState {
    func summary(address: String?) -> String {
        switch self {
        case .stopped: return "Disconnected"
        case .starting: return "Starting"
        case .waiting: return "Waiting for phone"
        case .connecting(let d): return "Connecting to \(d)"
        case .connected(let d, _): return "Connected to \(d)" + (address.map { " (\($0))" } ?? "")
        case .failed(let e): return "Error: \(e)"
        }
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
