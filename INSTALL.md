# Xone:4D HAL — Build and Install Guide

A user-space CoreAudio driver for the **Allen & Heath Xone:4D** USB DJ mixer on
macOS 15 (Tahoe) and newer. Ships two pieces, both installed in one click from
the bundled GUI installer:

1. **`XoneHAL.driver`** — CoreAudio HAL plug-in providing the 8-channel audio
   device at 44.1 / 48 / 88.2 / 96 kHz.
   Installed into `/Library/Audio/Plug-Ins/HAL/`.
2. **`xone-midi-agent`** — a small user-session daemon that owns a CoreMIDI
   virtual source named **"Xone:4D"**.
   Installed into `/usr/local/bin/` with a LaunchAgent in
   `/Library/LaunchAgents/`.

The split exists because on macOS Tahoe calling `MIDIClientCreate` from inside
`coreaudiod` (which is where HAL plug-ins live) deadlocks `MIDIServer`. To work
around this the HAL forwards raw MIDI bytes over a Unix-domain socket
(`/tmp/com.digitalkiss.xone.midi`) and the agent re-emits them through
CoreMIDI in a normal user-session process.

---

## 1. Building

Only Xcode Command Line Tools are required (`clang++`, `swiftc`, `codesign`):

```bash
cd XoneHAL
./build.sh installer
```

Output:

```
build/
├── XoneHAL.driver               ← the HAL plug-in bundle
├── xone-midi-agent              ← the MIDI agent binary
└── Xone4D HAL Installer.app     ← GUI installer that bundles both
```

Other `build.sh` modes:

| Command                | What it does                                                                |
| ---------------------- | --------------------------------------------------------------------------- |
| `./build.sh`           | Build `XoneHAL.driver` + `xone-midi-agent` (no GUI installer).              |
| `./build.sh installer` | The above plus the GUI installer `.app`.                                    |
| `./build.sh install`   | Build and `sudo`-copy the driver into `/Library/Audio/Plug-Ins/HAL/` (dev). |
| `./build.sh clean`     | Remove the `build/` directory.                                              |

The script ad-hoc signs every output (`codesign --sign -`) and picks
`arm64` / `x86_64` based on `uname -m`.

---

## 2. Installing on another Mac

### Via the GUI installer (recommended for end users)

1. Copy `Xone4D HAL Installer.app` to the target Mac (Telegram / AirDrop / USB).
2. Strip the quarantine attribute (otherwise Gatekeeper will block it):
   ```bash
   xattr -cr "/path/to/Xone4D HAL Installer.app"
   ```
3. Launch the app → click **"Install Driver"** → enter the admin password.
4. The installer will:
   - copy the `.driver` into `/Library/Audio/Plug-Ins/HAL/`,
   - copy `xone-midi-agent` into `/usr/local/bin/`,
   - copy the LaunchAgent plist into `/Library/LaunchAgents/`,
   - restart `coreaudiod`,
   - `launchctl bootstrap` the agent into the current user's GUI session.
5. Re-plug the Xone:4D USB cable.
6. Open **Audio MIDI Setup** — both an audio device named **"Xone:4D"** and a
   MIDI source by the same name should appear.

The **"Uninstall Driver"** button reverses everything.

### Manually (development workflow)

```bash
cd XoneHAL
./build.sh install      # driver + restart coreaudiod (requires sudo)

# Install the MIDI agent:
sudo cp build/xone-midi-agent /usr/local/bin/
sudo cp MidiAgent/com.digitalkiss.xone.midi-agent.plist /Library/LaunchAgents/
sudo chown root:wheel /usr/local/bin/xone-midi-agent \
                     /Library/LaunchAgents/com.digitalkiss.xone.midi-agent.plist
launchctl bootstrap gui/$(id -u) \
    /Library/LaunchAgents/com.digitalkiss.xone.midi-agent.plist
```

---

## 3. Traktor MIDI mapping

The rotary encoders on the Xone:4D are **relative** controls (7-bit two's
complement deltas), not absolute pot positions. If you map them as
`Knob/Fader: Direct (Absolute)` the value will jump around (e.g. "1, 100, 1,
23…") because Traktor will read the signed step values as raw absolute CCs.
Map them properly:

1. Traktor → **Preferences → Controller Manager**.
2. **Add… → Generic MIDI** → set **In-Port** to `Xone:4D`.
3. For each encoder:
   - **Type of Controller**: `Encoder`
   - **Encoder Mode**: `7Fh/01h`
   - **Interaction Mode**: `Relative` (or `Inc`/`Dec` depending on what you map it to)
4. Faders stay as `Fader/Knob: Direct` — they send absolute 0…127.
5. Buttons are standard Note On/Off, map as `Button`.

---

## 4. Quick diagnostics

Agent log (the agent writes to both `os_log` and stdout; stdout is captured by
launchd into a file, which is the most reliable source):

```bash
tail -f /tmp/xone-midi-agent.out.log
```

Sanity checks:

```bash
# Driver bundle is in place
ls -la /Library/Audio/Plug-Ins/HAL/XoneHAL.driver/Contents/MacOS/XoneHAL

# Agent is registered with launchd and running
launchctl print gui/$(id -u)/com.digitalkiss.xone.midi-agent | head -10

# IPC socket exists
ls -la /tmp/com.digitalkiss.xone.midi

# USB device is seen by the system
system_profiler SPUSBDataType | grep -A3 -i "xone"

# Virtual CoreMIDI source created by the agent
system_profiler SPMIDIDataType | grep -A2 -i xone
```

HAL-side log (lives inside `coreaudiod`):

```bash
log stream --info --predicate 'eventMessage CONTAINS "[Xone4D HAL]"'
```

If `xone-midi-agent.out.log` is growing but CoreMIDI does not see a "Xone:4D"
source, the agent likely got dropped from the GUI session. Restart it:

```bash
launchctl kickstart -k gui/$(id -u)/com.digitalkiss.xone.midi-agent
```

---

## 5. Full uninstall

Either click **"Uninstall Driver"** in the installer app, or run:

```bash
sudo launchctl bootout gui/$(id -u)/com.digitalkiss.xone.midi-agent 2>/dev/null || true
sudo rm -f /Library/LaunchAgents/com.digitalkiss.xone.midi-agent.plist
sudo rm -f /usr/local/bin/xone-midi-agent
sudo rm -rf /Library/Audio/Plug-Ins/HAL/XoneHAL.driver
sudo launchctl kickstart -k system/com.apple.audio.coreaudiod
```
