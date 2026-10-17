#!/usr/bin/env bash
#
# Ubuntu counterpart of build.bat.
#
#   ./build.sh          configure if needed, compile, then run
#   ./build.sh clear    clear the screen, compile, then run
#   ./build.sh clean    wipe the build directory and reconfigure
#   ./build.sh build    compile without running
#   ./build.sh run      run without recompiling
#   ./build.sh help     this text
#
# Unlike the Windows build there is no vcpkg or MinGW here: libusb comes from
# the system package manager and CMake finds it through pkg-config.
#
#   sudo apt install build-essential cmake pkg-config libusb-1.0-0-dev \
#                    libpulse-dev libayatana-appindicator3-dev libxtst-dev
#                    libx11-dev
#
# raylib is also needed. libraylib-dev exists from Debian 12 and Ubuntu
# 23.04 onward; on anything older, build it from source. ./install.sh sorts
# all of this out and reports what it could not find.
#
# Talking to the device needs permission for the USB node. Install the udev
# rule once, then unplug and replug the device:
#
#   sudo ./install.sh --udev
#
# Do NOT run KillSwitch with sudo instead. That opens the device but cuts the
# process off from the desktop session's sound server, so the audio mixer
# reports itself unavailable and no volume is controlled.
#
# This builds and runs in place. To put KillSwitch in the application menu and
# give it a proper icon in the panel, run ./install.sh afterwards, which
# also installs it into ~/.KillSwitch.
#
set -uo pipefail

cd "$(dirname "$0")" || exit 1

BUILD_DIR=build

# CMake appends ".exe" on Windows only, so this is "main" here. The fallback
# covers a build directory left over from before that was made conditional.
exe_path() {
    if   [ -f "$BUILD_DIR/KillSwitch" ];     then printf '%s\n' "$BUILD_DIR/KillSwitch"
    elif [ -f "$BUILD_DIR/KillSwitch.exe" ]; then printf '%s\n' "$BUILD_DIR/KillSwitch.exe"
    else return 1
    fi
}

require() {
    command -v "$1" >/dev/null 2>&1 && return 0
    echo "$1 not found. Install it with:" >&2
    echo "    sudo apt install $2" >&2
    exit 1
}

usage() {
    echo "Usage: ./build.sh [command]"
    echo
    echo "  (none)   configure if needed, compile, then run"
    echo "  clear    clear the console, compile, then run"
    echo "  clean    wipe the build directory and reconfigure"
    echo "  build    compile without running"
    echo "  run      run without recompiling"
    echo "  help     show this text"
}

# Development headers, checked by their pkg-config name.
require_pkg() {
    pkg-config --exists "$1" && return 0
    echo "$1 development files not found. Install them with:" >&2
    echo "    sudo apt install $2" >&2
    exit 1
}

# A build directory left behind by a run under sudo belongs to root, so cmake
# cannot write its cache as you. Nothing here runs as root, so it can only be
# reported.
check_build_owner() {
    [ -d "$BUILD_DIR" ] || return 0
    [ -w "$BUILD_DIR" ] && return 0

    echo "$BUILD_DIR is not writable: it was created by a run under sudo." >&2
    echo "Remove it with:" >&2
    echo "    sudo rm -rf \"$PWD/$BUILD_DIR\"" >&2
    exit 1
}

setup() {
    check_build_owner
    require cmake cmake
    require pkg-config pkg-config

    require_pkg libusb-1.0 libusb-1.0-0-dev

    # Without this CMake quietly builds a stub mixer: the interface runs, the
    # faders move, and nothing changes volume. Mint does not ship it.
    require_pkg libpulse libpulse-dev

    # The tray icon is optional, so this only warns.
    if ! pkg-config --exists ayatana-appindicator3-0.1; then
        echo "libayatana-appindicator not found: there will be no tray icon." >&2
        echo "Install it with:" >&2
        echo "    sudo apt install libayatana-appindicator3-dev" >&2
        echo >&2
    fi

    # Likewise the macro pad: without XTEST it still edits and saves, it just
    # cannot type anything.
    if ! pkg-config --exists xtst; then
        echo "libXtst not found: macro keys will not be able to type." >&2
        echo "Install it with:" >&2
        echo "    sudo apt install libxtst-dev" >&2
        echo >&2
    fi

    # And the running-application list, which is what makes profiles follow
    # whatever is in front.
    if ! pkg-config --exists x11; then
        echo "libX11 not found: profiles will not switch on their own." >&2
        echo "Install it with:" >&2
        echo "    sudo apt install libx11-dev" >&2
        echo >&2
    fi

    rm -rf "$BUILD_DIR"
    cmake -S . -B "$BUILD_DIR" || exit 1
}

# CMake only looks for its optional dependencies when it configures. A build
# directory created before they were installed therefore keeps the stub mixer
# and no tray icon, however many times it is rebuilt.
warn_stale_pulse() {
    [ -f "$BUILD_DIR/CMakeCache.txt" ] || return 0

    if ! grep -q '^PULSE_FOUND:INTERNAL=1$' "$BUILD_DIR/CMakeCache.txt"; then
        echo "WARNING: this build directory was configured without PulseAudio," >&2
        echo "so per-application volume is disabled. Reconfigure with:" >&2
        echo "    ./build.sh clean && ./build.sh" >&2
        echo >&2
    fi

    if ! grep -q '^APPINDICATOR_FOUND:INTERNAL=1$' "$BUILD_DIR/CMakeCache.txt"; then
        echo "WARNING: this build directory was configured without AppIndicator," >&2
        echo "so there will be no tray icon. Reconfigure with:" >&2
        echo "    ./build.sh clean && ./build.sh" >&2
        echo >&2
    fi
}

# An ELF binary carries no icon of its own, so the file manager is told which
# one to use through GVFS metadata. Nemo, Nautilus and Caja honour this; it is
# per-user and survives rebuilds of the same path.
set_file_icon() {
    command -v gio >/dev/null 2>&1 || return 0

    local exe icon
    exe=$(exe_path) || return 0
    icon="$PWD/resources/icon.png"
    [ -f "$icon" ] || return 0

    gio set "$PWD/$exe" metadata::custom-icon "file://$icon" 2>/dev/null
    return 0
}

compile() {
    check_build_owner
    warn_stale_pulse
    echo "Compiling main..."
    cmake --build "$BUILD_DIR" || exit 1
    set_file_icon
    echo "Compiling done !"
    printf '\n\n\n'
}

run() {
    local exe
    if ! exe=$(exe_path); then
        echo "Failed to compile !" >&2
        exit 1
    fi

    "./$exe"
    local status=$?

    # libusb reports a permission problem as a failure to open the device.
    if [ $status -ne 0 ] && [ "$(id -u)" -ne 0 ]; then
        echo >&2
        echo "If the device was found but could not be opened, install the" >&2
        echo "USB permission rule and replug the device:" >&2
        echo "    sudo ./install.sh --udev" >&2
        echo "Do not use sudo to work around it: the audio mixer needs to run" >&2
        echo "as your own user." >&2
    fi
    return $status
}

case "${1-}" in
    "")
        # Configure only when there is no cache, then build and run.
        [ -f "$BUILD_DIR/CMakeCache.txt" ] || setup
        compile
        run
        ;;
    clear)
        clear
        compile
        run
        ;;
    build)
        # Compile without launching, which is what install.sh wants.
        [ -f "$BUILD_DIR/CMakeCache.txt" ] || setup
        compile
        ;;
    clean)
        setup
        echo "Configured. Run ./build.sh to compile."
        ;;
    run)
        run
        ;;
    help|-h|--help|/?)
        usage
        ;;
    *)
        echo "Unknown option: $1" >&2
        echo >&2
        usage
        exit 1
        ;;
esac
