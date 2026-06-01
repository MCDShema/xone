import Cocoa

// HAL plug-ins are not System Extensions — they are .driver bundles copied to
// /Library/Audio/Plug-Ins/HAL/. We do everything in a single osascript call
// with administrator privileges so the user gets a single auth prompt.

let app = NSApplication.shared
app.setActivationPolicy(.regular)

let delegate = AppDelegate()
app.delegate = delegate
app.run()

class AppDelegate: NSObject, NSApplicationDelegate {

    var window: NSWindow!
    var statusLabel: NSTextField!
    var progressIndicator: NSProgressIndicator!
    var installButton: NSButton!
    var uninstallButton: NSButton!
    var iconView: NSImageView!

    let halDest = "/Library/Audio/Plug-Ins/HAL/XoneHAL.driver"

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildUI()
        refreshState()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        return true
    }

    // MARK: - UI

    func buildUI() {
        let w: CGFloat = 480
        let h: CGFloat = 360

        window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: w, height: h),
            styleMask: [.titled, .closable, .miniaturizable],
            backing: .buffered,
            defer: false
        )
        window.title = "Xone:4D HAL Installer"
        window.center()
        window.isReleasedWhenClosed = false

        let bg = NSView(frame: NSRect(x: 0, y: 0, width: w, height: h))
        bg.wantsLayer = true
        bg.layer?.backgroundColor = NSColor(red: 0.11, green: 0.11, blue: 0.13, alpha: 1.0).cgColor
        window.contentView = bg

        // Top red bar
        let bar = NSView(frame: NSRect(x: 0, y: h - 4, width: w, height: 4))
        bar.wantsLayer = true
        bar.layer?.backgroundColor = NSColor(red: 0.80, green: 0.12, blue: 0.23, alpha: 1.0).cgColor
        bg.addSubview(bar)

        // App icon
        if let img = NSImage(named: NSImage.applicationIconName) {
            iconView = NSImageView(frame: NSRect(x: w/2 - 40, y: h - 110, width: 80, height: 80))
            iconView.image = img
            bg.addSubview(iconView)
        }

        // Title
        let title = makeLabel("Xone:4D HAL Driver", size: 22, bold: true, color: .white)
        title.frame = NSRect(x: 0, y: h - 148, width: w, height: 28)
        title.alignment = .center
        bg.addSubview(title)

        let sub = makeLabel("Allen & Heath | CoreAudio HAL plug-in", size: 12, bold: false, color: NSColor(white: 0.6, alpha: 1))
        sub.frame = NSRect(x: 0, y: h - 170, width: w, height: 18)
        sub.alignment = .center
        bg.addSubview(sub)

        let div = NSView(frame: NSRect(x: 40, y: h - 185, width: w - 80, height: 1))
        div.wantsLayer = true
        div.layer?.backgroundColor = NSColor(white: 0.25, alpha: 1).cgColor
        bg.addSubview(div)

        statusLabel = makeLabel("", size: 13, bold: false, color: NSColor(white: 0.75, alpha: 1))
        statusLabel.frame = NSRect(x: 20, y: 130, width: w - 40, height: 60)
        statusLabel.alignment = .center
        statusLabel.maximumNumberOfLines = 3
        bg.addSubview(statusLabel)

        progressIndicator = NSProgressIndicator(frame: NSRect(x: w/2 - 16, y: 100, width: 32, height: 32))
        progressIndicator.style = .spinning
        progressIndicator.isHidden = true
        progressIndicator.appearance = NSAppearance(named: .darkAqua)
        bg.addSubview(progressIndicator)

        installButton = NSButton(frame: NSRect(x: w/2 - 210, y: 30, width: 200, height: 36))
        installButton.title = "Install Driver"
        installButton.bezelStyle = .rounded
        installButton.wantsLayer = true
        installButton.layer?.backgroundColor = NSColor(red: 0.80, green: 0.12, blue: 0.23, alpha: 1.0).cgColor
        installButton.layer?.cornerRadius = 8
        installButton.contentTintColor = .white
        installButton.font = NSFont.systemFont(ofSize: 14, weight: .semibold)
        installButton.target = self
        installButton.action = #selector(installDriver)
        bg.addSubview(installButton)

        uninstallButton = NSButton(frame: NSRect(x: w/2 + 10, y: 30, width: 200, height: 36))
        uninstallButton.title = "Uninstall Driver"
        uninstallButton.bezelStyle = .rounded
        uninstallButton.wantsLayer = true
        uninstallButton.layer?.backgroundColor = NSColor(white: 0.22, alpha: 1).cgColor
        uninstallButton.layer?.cornerRadius = 8
        uninstallButton.contentTintColor = .white
        uninstallButton.font = NSFont.systemFont(ofSize: 14, weight: .semibold)
        uninstallButton.target = self
        uninstallButton.action = #selector(uninstallDriver)
        bg.addSubview(uninstallButton)

        let ver = makeLabel("v0.1  •  CoreAudio HAL", size: 10, bold: false, color: NSColor(white: 0.35, alpha: 1))
        ver.frame = NSRect(x: 0, y: 6, width: w, height: 14)
        ver.alignment = .center
        bg.addSubview(ver)
    }

    func makeLabel(_ text: String, size: CGFloat, bold: Bool, color: NSColor) -> NSTextField {
        let lbl = NSTextField(labelWithString: text)
        lbl.font = bold ? NSFont.boldSystemFont(ofSize: size) : NSFont.systemFont(ofSize: size)
        lbl.textColor = color
        lbl.isBezeled = false
        lbl.isEditable = false
        lbl.drawsBackground = false
        return lbl
    }

    func setStatus(_ text: String, color: NSColor) {
        statusLabel.stringValue = text
        statusLabel.textColor = color
    }

    func setBusy(_ busy: Bool) {
        progressIndicator.isHidden = !busy
        if busy { progressIndicator.startAnimation(nil) }
        else    { progressIndicator.stopAnimation(nil) }
        installButton.isEnabled   = !busy
        uninstallButton.isEnabled = !busy
    }

    func refreshState() {
        let installed = FileManager.default.fileExists(atPath: halDest)
        if installed {
            setStatus("Driver installed in /Library/Audio/Plug-Ins/HAL/.\nClick \u{201C}Install\u{201D} to reinstall or \u{201C}Uninstall\u{201D} to remove.", color: NSColor(red: 0.2, green: 0.9, blue: 0.4, alpha: 1))
        } else {
            setStatus("Driver is not installed.\nClick \u{201C}Install\u{201D} to activate XoneHAL.", color: NSColor(white: 0.75, alpha: 1))
        }
        uninstallButton.isEnabled = installed
    }

    // MARK: - Actions

    @objc func installDriver() {
        guard let bundledDriver = Bundle.main.path(forResource: "XoneHAL", ofType: "driver") else {
            setStatus("❌ Driver bundle was not found in the installer's Resources.", color: NSColor(red: 1, green: 0.3, blue: 0.3, alpha: 1))
            return
        }
        // MIDI agent + LaunchAgent plist are bundled alongside the .driver
        // so the same admin prompt installs the whole MIDI subsystem too.
        let bundledAgent = Bundle.main.path(forResource: "xone-midi-agent", ofType: nil)
        let bundledPlist = Bundle.main.path(forResource: "com.digitalkiss.xone.midi-agent",
                                            ofType: "plist")
        let agentDest    = "/usr/local/bin/xone-midi-agent"
        let plistDest    = "/Library/LaunchAgents/com.digitalkiss.xone.midi-agent.plist"
        let user         = NSUserName()

        setBusy(true)
        setStatus("Requesting administrator privileges\u{2026}", color: NSColor(white: 0.75, alpha: 1))

        var lines: [String] = [
            "rm -rf '/Library/Audio/Plug-Ins/HAL/XoneHAL.bundle' '\(halDest)';",
            "cp -R '\(bundledDriver)' '/Library/Audio/Plug-Ins/HAL/';",
            "chown -R root:wheel '\(halDest)';",
            // kickstart -k tells launchd to re-spawn coreaudiod, but on
            // Tahoe it sometimes returns before the old process actually
            // exits — so the freshly-copied .driver bundle is not picked up
            // and the device stays invisible until a manual killall. Belt
            // and braces: kickstart, then SIGKILL anything still alive
            // under the coreaudiod name. launchd respawns it immediately
            // with the new bundle.
            "launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || true;",
            "sleep 1;",
            "killall -9 coreaudiod 2>/dev/null || true;",
        ]
        if let a = bundledAgent, let p = bundledPlist {
            lines.append(contentsOf: [
                "mkdir -p /usr/local/bin /Library/LaunchAgents;",
                "cp '\(a)' '\(agentDest)';",
                "chown root:wheel '\(agentDest)';",
                "chmod 0755 '\(agentDest)';",
                "cp '\(p)' '\(plistDest)';",
                "chown root:wheel '\(plistDest)';",
                "chmod 0644 '\(plistDest)';",
                // Bootstrap the LaunchAgent into the current user's GUI session.
                "launchctl bootout gui/$(id -u \(user))/com.digitalkiss.xone.midi-agent 2>/dev/null || true;",
                "launchctl bootstrap gui/$(id -u \(user)) '\(plistDest)' || true;",
            ])
        }
        let script = lines.joined(separator: "\n")

        runAsAdmin(shell: script, friendly: "driver install") { ok, msg in
            self.setBusy(false)
            if ok {
                self.setStatus("✅ Driver and MIDI agent installed.", color: NSColor(red: 0.2, green: 0.9, blue: 0.4, alpha: 1))
            } else {
                self.setStatus("❌ \(msg)", color: NSColor(red: 1, green: 0.3, blue: 0.3, alpha: 1))
            }
            self.refreshState()
        }
    }

    @objc func uninstallDriver() {
        let agentDest = "/usr/local/bin/xone-midi-agent"
        let plistDest = "/Library/LaunchAgents/com.digitalkiss.xone.midi-agent.plist"
        let user      = NSUserName()

        setBusy(true)
        setStatus("Requesting administrator privileges\u{2026}", color: NSColor(white: 0.75, alpha: 1))

        let script = """
        rm -rf '/Library/Audio/Plug-Ins/HAL/XoneHAL.bundle' '\(halDest)';
        launchctl bootout gui/$(id -u \(user))/com.digitalkiss.xone.midi-agent 2>/dev/null || true;
        rm -f '\(plistDest)' '\(agentDest)';
        launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || true;
        sleep 1;
        killall -9 coreaudiod 2>/dev/null || true;
        """

        runAsAdmin(shell: script, friendly: "driver uninstall") { ok, msg in
            self.setBusy(false)
            if ok {
                self.setStatus("🗑  Driver uninstalled.", color: NSColor(white: 0.85, alpha: 1))
            } else {
                self.setStatus("❌ \(msg)", color: NSColor(red: 1, green: 0.3, blue: 0.3, alpha: 1))
            }
            self.refreshState()
        }
    }

    // MARK: - Privileged shell via AppleScript

    func runAsAdmin(shell: String, friendly: String, completion: @escaping (Bool, String) -> Void) {
        // AppleScript will show a Touch ID / password prompt once.
        let escaped = shell
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
        let osa = "do shell script \"\(escaped)\" with administrator privileges"

        DispatchQueue.global(qos: .userInitiated).async {
            var errInfo: NSDictionary?
            let script = NSAppleScript(source: osa)
            let result = script?.executeAndReturnError(&errInfo)

            DispatchQueue.main.async {
                if let err = errInfo {
                    let code = (err[NSAppleScript.errorNumber] as? Int) ?? -1
                    let msg  = (err[NSAppleScript.errorMessage] as? String) ?? "unknown error"
                    if code == -128 {
                        completion(false, "cancelled by user")
                    } else {
                        completion(false, "\(friendly) error: \(msg)")
                    }
                } else {
                    _ = result
                    completion(true, "")
                }
            }
        }
    }
}
