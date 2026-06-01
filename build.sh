#!/usr/bin/env bash
# Build & (optionally) install the Xone:4D AudioServerPlugIn bundle.
#
#   ./build.sh             — build the .driver, produces ./build/XoneHAL.driver
#   ./build.sh installer   — also build ./build/Xone:4D HAL Installer.app
#                            (a GUI front-end with Install/Uninstall buttons)
#   ./build.sh install     — build the .driver and install it to
#                            /Library/Audio/Plug-Ins/HAL/ (needs sudo). For
#                            distribution prefer the GUI installer.
#   ./build.sh clean       — remove build dir.
#
# Requires: Xcode command-line tools (clang++, swiftc, codesign).
set -euo pipefail

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUILD="${HERE}/build"
BUNDLE="${BUILD}/XoneHAL.driver"
APP="${BUILD}/Xone4D HAL Installer.app"
SRC=(
    "${HERE}/Xone4DHAL.cpp"
    "${HERE}/XoneUSBTransport.cpp"
    "${HERE}/PloytecCodec.cpp"
    "${HERE}/XoneMidi.cpp"
)

MODE="${1:-build}"
case "${MODE}" in
    clean)
        rm -rf "${BUILD}"
        echo "Cleaned."
        exit 0
        ;;
esac

mkdir -p "${BUNDLE}/Contents/MacOS"
cp "${HERE}/Info.plist" "${BUNDLE}/Contents/Info.plist"

# Detect host arch; build universal when supported.
ARCH_FLAGS="-arch arm64"
if [[ "$(uname -m)" == "x86_64" ]]; then
    ARCH_FLAGS="-arch x86_64"
fi
# If you want a universal binary, uncomment:
# ARCH_FLAGS="-arch arm64 -arch x86_64"

CXX_FLAGS=(
    -std=gnu++20
    -O2
    -g
    -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations
    -fvisibility=default
    ${ARCH_FLAGS}
    -bundle
    -framework CoreAudio
    -framework CoreMIDI
    -framework CoreFoundation
    -framework IOKit
)

echo "Compiling ${#SRC[@]} sources →  ${BUNDLE}/Contents/MacOS/XoneHAL"
clang++ "${CXX_FLAGS[@]}" -o "${BUNDLE}/Contents/MacOS/XoneHAL" "${SRC[@]}"

echo "Ad-hoc signing driver bundle"
codesign --force --sign - --timestamp=none "${BUNDLE}" >/dev/null

echo "Driver bundle ready: ${BUNDLE}"

# -----------------------------------------------------------------------------
# xone-midi-agent — separate user-session daemon that bridges USB MIDI bytes
# (sent over a Unix socket by the HAL) to a CoreMIDI virtual source. See
# MidiAgent/main.cpp for the rationale.
# -----------------------------------------------------------------------------
AGENT_BIN="${BUILD}/xone-midi-agent"
echo "Compiling MIDI agent → ${AGENT_BIN}"
clang++ -std=gnu++20 -O2 -g \
    -Wall -Wextra -Wno-unused-parameter \
    ${ARCH_FLAGS} \
    -framework CoreMIDI \
    -framework CoreFoundation \
    -framework IOKit \
    -o "${AGENT_BIN}" \
    "${HERE}/MidiAgent/main.cpp"
codesign --force --sign - --timestamp=none "${AGENT_BIN}" >/dev/null
echo "MIDI agent ready: ${AGENT_BIN}"

build_installer_app() {
    echo "Building GUI installer app"
    rm -rf "${APP}"
    mkdir -p "${APP}/Contents/MacOS" "${APP}/Contents/Resources"
    cp "${HERE}/Installer/Info.plist" "${APP}/Contents/Info.plist"

    # Embed the freshly built .driver inside the .app so a single double-click
    # is enough to install (the app prompts for admin once via osascript).
    cp -R "${BUNDLE}" "${APP}/Contents/Resources/"
    # Bundle the MIDI agent + launchd plist alongside; the Swift installer
    # copies them to /usr/local/bin and /Library/LaunchAgents/.
    cp "${AGENT_BIN}" "${APP}/Contents/Resources/"
    cp "${HERE}/MidiAgent/com.digitalkiss.xone.midi-agent.plist" \
        "${APP}/Contents/Resources/"

    # Build the Swift binary. swiftc expects -target rather than clang's -arch.
    SWIFT_TARGET="arm64-apple-macos11"
    if [[ "$(uname -m)" == "x86_64" ]]; then
        SWIFT_TARGET="x86_64-apple-macos11"
    fi
    swiftc \
        -target "${SWIFT_TARGET}" \
        -O \
        -framework Cocoa \
        -o "${APP}/Contents/MacOS/XoneHALInstaller" \
        "${HERE}/Installer/main.swift"

    codesign --force --deep --sign - --timestamp=none "${APP}" >/dev/null
    echo "Installer app ready: ${APP}"
}

case "${MODE}" in
    installer)
        build_installer_app
        ;;
    install)
        DEST="/Library/Audio/Plug-Ins/HAL"
        echo "Installing to ${DEST} (needs sudo)"
        sudo rm -rf "${DEST}/XoneHAL.bundle" "${DEST}/XoneHAL.driver"
        sudo cp -R "${BUNDLE}" "${DEST}/"
        echo "Restarting coreaudiod"
        sudo launchctl kickstart -k system/com.apple.audio.coreaudiod || \
            sudo killall coreaudiod || true
        echo "Done. Look for 'Xone:4D (Ozzy HAL)' in Audio MIDI Setup."
        ;;
esac
