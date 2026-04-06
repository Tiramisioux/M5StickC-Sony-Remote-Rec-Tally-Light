# M5StickC Sony Remote Tally

Direct-hardware Arduino tally and record controller for the original `M5StickC` and Sony cameras that still expose the older Wi-Fi `Camera Remote API`.

## What This Does

- joins the camera's Wi-Fi network
- polls the legacy Sony camera API for record state
- toggles movie recording with `BtnA`
- uses the built-in red LED on `GPIO10` as the tally light
- shows connection, camera, and battery state on the built-in display

## Hardware

Tested hardware for this repo:

- camera body: Sony `a7S II`
- board: original `M5StickC`
- camera workflow: `Smart Remote Embedded`
- local Wi-Fi credentials stay in an ignored `config.h`

## Quick Setup

1. On the camera, launch `Smart Remote Embedded`.
2. Note the Wi-Fi SSID and password shown by the camera.
3. Copy the local config template and add the exact credentials.
4. Keep the camera in that remote-control mode while using the tally unit.

## Install With Arduino IDE

1. Open Arduino IDE.
2. Install the `M5Stack` ESP32 board package if it is not already installed.
3. Open the sketch folder `m5stickc_sony_remote_tally_arduino`.
4. Select board `M5StickC`.
5. Copy the config template:

   ```bash
   cp m5stickc_sony_remote_tally_arduino/config.example.h \
     m5stickc_sony_remote_tally_arduino/config.h
   ```

6. Edit `config.h` and enter the exact Sony camera Wi-Fi SSID and password shown by `Smart Remote Embedded`.
7. Connect the original `M5StickC` over USB.
8. Select the correct serial port in Arduino IDE.
9. Click `Upload`.

The sketch will still compile without `config.h` by using the fallback config, but the device will stop at `EDIT CFG` until real credentials are added.

## Runtime Behavior

- if `config.h` is missing, the display shows `EDIT CFG` and the board does not try to connect
- `BtnA` toggles recording
- `BtnB` short press toggles the display on or off
- `BtnB` long press cycles background themes
- the display shows battery state, `READY` or `RECORD`, network status, and camera status

## Troubleshooting

- `EDIT CFG` on screen: create `config.h` and copy the exact SSID and password from the camera
- build succeeds but the device does not connect: confirm the camera is still inside `Smart Remote Embedded` and still exposing the legacy Wi-Fi API path
- wrong colors on screen: keep the current RGB565 byte-swap handling in the display driver

## Limitations

- specific to the original `M5StickC`
- verified only on the older Sony `Smart Remote Embedded` / `Camera Remote API` route
- elapsed recording time starts when the device first observes recording, not from a camera-internal absolute start
