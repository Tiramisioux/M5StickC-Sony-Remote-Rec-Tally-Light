# M5StickC Sony Remote Tally

Direct-hardware Arduino implementation of a Sony Wi-Fi tally and record controller for the original `M5StickC`.

This project:

- joins the Sony camera's Wi-Fi network
- polls the legacy Sony Camera Remote API on the camera
- toggles movie recording with `BtnA`
- uses the built-in red LED on `GPIO10` as the tally light
- renders connection, camera, and battery state on the built-in display
- keeps camera Wi-Fi credentials out of tracked source

The current verified camera body is a Sony `a7S II` using the older `Smart Remote Embedded` workflow. Other Sony bodies may work if they expose the same Camera Remote API behavior, but that is not what this repo currently claims as verified.

## Features

- direct hardware path with no `M5StickC.h` dependency
- non-blocking Wi-Fi and camera HTTP workflow so button handling stays responsive
- retained-state row redraws to avoid visible full-screen display blinking
- compile-time UI font selection: built-in font, `DIN2014 Regular`, or `DIN2014 Bold`
- compile-time display dimming default
- background theme cycling with persisted preference storage
- green `READY`, red `RECORD`, magenta linked `NET`, and compact camera-state labels
- battery icon with red and yellow low-charge thresholds
- charge-state socket indicator beside the battery icon
- optional record elapsed time appended to the `RECORD` line
- optional lowest-line debug/runtime estimate display

## Verified hardware contract

- board: original `M5StickC`
- `BtnA` on `GPIO37`: record toggle
- `BtnB` on `GPIO39`: short press toggles the display, long press cycles background themes
- red LED on `GPIO10`: record tally
- display: primary status surface
- PMU: direct `AXP192` access for display brightness and battery telemetry

## Repo layout

- [m5stickc_sony_remote_tally_arduino/m5stickc_sony_remote_tally_arduino.ino](/Users/patrikeriksson/Documents/codex/governed-systems/implementation-repos/hardware/m5stickc-sony-remote-tally/m5stickc_sony_remote_tally_arduino/m5stickc_sony_remote_tally_arduino.ino): main sketch
- [m5stickc_sony_remote_tally_arduino/config.example.h](/Users/patrikeriksson/Documents/codex/governed-systems/implementation-repos/hardware/m5stickc-sony-remote-tally/m5stickc_sony_remote_tally_arduino/config.example.h): local config template
- [m5stickc_sony_remote_tally_arduino/config_fallback.h](/Users/patrikeriksson/Documents/codex/governed-systems/implementation-repos/hardware/m5stickc-sony-remote-tally/m5stickc_sony_remote_tally_arduino/config_fallback.h): compile-safe fallback when `config.h` is missing
- [.gitignore](/Users/patrikeriksson/Documents/codex/governed-systems/implementation-repos/hardware/m5stickc-sony-remote-tally/.gitignore): excludes local `config.h`

## Requirements

- original `M5StickC`
- Arduino IDE with the M5Stack ESP32 board package, or the bundled `arduino-cli`
- USB data cable for upload
- Sony camera with `Smart Remote Embedded`

## Camera setup

On the camera:

1. Power on the camera.
2. Open the Sony application that exposes the legacy Wi-Fi remote API.
3. On the verified `a7S II`, that means launching `Smart Remote Embedded`.
4. Wait for the camera to show its Wi-Fi SSID and password.
5. Keep the camera in that remote-control mode while using the tally unit.

What the sketch expects from the camera:

- the camera creates its own Wi-Fi network
- the camera answers on `http://192.168.122.1:8080/sony/camera`
- `getEvent` returns `cameraStatus`
- `startMovieRec`, `stopMovieRec`, and optionally `startRecMode` are available as needed

## Local configuration

Do not put Wi-Fi credentials in tracked source.

Create a local-only config file:

```bash
cd "/Users/patrikeriksson/Documents/codex/governed-systems/implementation-repos/hardware/m5stickc-sony-remote-tally"
cp m5stickc_sony_remote_tally_arduino/config.example.h m5stickc_sony_remote_tally_arduino/config.h
```

Edit `config.h` and set:

- `M5_SONY_WIFI_SSID` to the exact SSID shown on the camera
- `M5_SONY_WIFI_PASSWORD` to the password shown by the camera remote app

`config.h` is intentionally ignored by git and should stay local.

If `config.h` is missing, the sketch still compiles by using `config_fallback.h`, but the device will stop in an `EDIT CFG` state instead of trying to connect.

## Build

Compile with the Arduino IDE bundled CLI:

```bash
'/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli' compile \
  --fqbn m5stack:esp32:m5stack_stickc \
  --build-path /tmp/m5stickc_sony_remote_build \
  "/Users/patrikeriksson/Documents/codex/governed-systems/implementation-repos/hardware/m5stickc-sony-remote-tally/m5stickc_sony_remote_tally_arduino"
```

## Upload

Upload with Arduino IDE or `arduino-cli upload` for your active serial port while selecting board `M5StickC` from the M5Stack ESP32 package.

Typical Arduino IDE flow:

1. Open the sketch folder.
2. Select board `M5StickC`.
3. Select the correct serial port.
4. Upload.

## Runtime behavior

On boot:

- the board checks whether `config.h` contains real Wi-Fi values
- if config is missing, the display shows an `EDIT CFG` error state and the board does not try to connect
- if config is valid, the board scans for the camera SSID and joins that Wi-Fi network
- it then starts polling the camera state

Display behavior:

- top line: title and battery state
- ready line: `READY` or `RECORD` with optional elapsed time
- network line: `NET` status
- camera line: camera status summary
- footer line: optional debug/runtime summary

Controls:

- `BtnA`: toggle recording
- `BtnB` short press: toggle display on or off
- `BtnB` long press: cycle background theme

Record workflow:

- `IDLE` -> `startMovieRec`
- `MovieRecording` -> `stopMovieRec`
- if the camera first reports `NotReady` and exposes `startRecMode`, the sketch will call `startRecMode` and retry

## Important compile-time options

Near the top of the sketch:

- `DIM_DISPLAY`
- `UI_FONT_MODE`
- `UI_FONT_SCALE`
- `SHOW_RECORDING_TIME`
- `SHOW_INFO_LINE`
- `BATTERY_CAPACITY_MAH`

## Troubleshooting

`EDIT CFG` on screen:

- create `config.h`
- copy the exact SSID and password from the camera

Wrong colors on screen:

- this direct display path depends on RGB565 byte swapping before SPI transfer
- keep the current `panelColor()` handling in the display driver

Buttons feel unresponsive:

- this repo uses a non-blocking camera workflow to keep button handling responsive
- if responsiveness regresses, inspect the camera job and HTTP state-machine path before changing debounce values again

Build works but the device does not connect:

- verify the camera is still inside `Smart Remote Embedded`
- verify the SSID and password exactly match the camera display
- verify the camera is still exposing the legacy Wi-Fi API path

## Current limitations

- this repo is specific to the original `M5StickC`
- the display, PMU, and button assumptions are not generic across other M5Stick variants
- the verified camera path is the older Sony `Smart Remote Embedded` / Camera Remote API route
- elapsed recording time is measured from when the device first observes recording, not from camera-internal absolute take start
