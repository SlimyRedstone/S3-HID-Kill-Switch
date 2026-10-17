# S3 KillSwitch — desktop client

Companion app for the ESP32-S3 NFC reader. It shows the UID of a scanned card,
decides whether it is authorised, and tells the reader to play the success or
error melody. An authorised card toggles a **kill switch**: it enables or
disables the keyboard and mouse you have selected, so the physical card arms or
disarms your input devices.

Built on the [IOMeeter](https://github.com/SlimyRedstone/IOMeeter) client — same
USB transport (libusb + a JSON framer), window (raylib + Clay), tray and
single-instance plumbing.

## What it does

* **Reads only `303A:9911`.** The ESP32-S3 dongle, nothing else on the bus.
* **Shows the scanned UID** and whether it is authorised, in the Cards tab.
* **Lists the HID keyboards and mice** present, in the Devices tab. Tick the
  ones the switch should control.
* **A valid card toggles the switch.** Disabled devices stop working; scanning
  the card again re-enables them. The reader plays success on an authorised
  card, error on an unknown one.
* **Fails safe.** If the reader is unplugged while devices are disabled, they
  are re-enabled at once — a card can no longer bring them back, so the app
  does. Quitting re-enables every selected device too.
* **Lives in the tray.** Closing the window hides it; the tray menu quits.

## Enabling/disabling devices needs administrator rights

Switching a device node is the same operation as Device Manager's right-click →
Disable / Enable (`SetupDiCallClassInstaller` with `DICS_DISABLE`/`DICS_ENABLE`).
Windows only allows it for an administrator, so the app ships a manifest that
requests elevation at launch (`resources/app.manifest`). Without elevation it
still runs and lists devices, but every toggle fails with access denied and the
reason is shown.

This is Windows-only. On other platforms `hidctl.c` is a stub: the window, the
USB link and the card display all work, but nothing is switched.

## Configuration

`config.json` beside the executable, written indented so it can be edited by
hand. A device listed under `devices` is a kill-switch target; matching is by
VID/PID, and `name` is for display.

```json
{
  "debug": false,
  "cards":   [ { "uid": "04A1B2C3" } ],
  "devices": [ { "name": "Keyboard", "usb": { "vid": "0x046D", "pid": "0xC31C" } } ]
}
```

* `debug` opens a console that logs every JSON document crossing the wire.
* A missing or malformed file is not an error: the app starts with empty lists.
* The file is saved when the window is hidden to the tray, and by the Save
  button; Reload re-reads it.

## Building

C99, CMake, `libusb-1.0` and `raylib`. Same prerequisites as IOMeeter.

### Windows

```
vcpkg install libusb:x64-mingw-dynamic raylib:x64-mingw-dynamic
build.bat          configure if needed, compile, then run
build.bat build    compile without running
build.bat clean    wipe the build directory and reconfigure
```

Launching prompts for elevation because of the manifest.

### Linux (interface only; the switch is a stub)

```
sudo apt install build-essential cmake pkg-config libusb-1.0-0-dev \
                 libraylib-dev libayatana-appindicator3-dev
./build.sh
```

## Protocol with the S3

```
S3  -> app: {"card":{"uid":"04A1B2C3","atqa":"0044","sak":"00","type":"Ultralight/NTAG"}}
app -> S3 : {"set":{"card":{"uid":"04A1B2C3","valid":true}}}
```

The `valid` flag is what the firmware turns into the success or error melody.
