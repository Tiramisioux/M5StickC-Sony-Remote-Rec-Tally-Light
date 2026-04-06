# M5StickC Sony Remote Recording and Tally Light

Wireless recording button and tally light for Sony cameras using M5StickC, using the Sony `Camera Remote API`.

## How it works

- M5StickC joins the camera's Wi-Fi network
- polls the legacy Sony camera API for record state
- use Button A to toggle recording start/stop
- built-in red LED on `GPIO10` lights up as the tally light
- shows connection, camera, and battery state on the built-in display

## Hardware

Tested hardware:

- camera body: Sony a7S II
- board: original M5StickC
- camera workflow: `Smart Remote Embedded`

## Setup

1. On the camera, go to `MENU > Application List > Smart Remote Embedded`and start the camera app.
2. Note the Wi-Fi SSID and password shown by the camera.

3. Open Arduino IDE.
4. Install the `M5Stack` ESP32 board package if it is not already installed.
5. Open the sketch folder `m5stickc_sony_remote_tally_arduino`.
6. Select board `M5StickC`.
7. Copy the config template:

   ```bash
   cp m5stickc_sony_remote_tally_arduino/config.example.h \
     m5stickc_sony_remote_tally_arduino/config.h
   ```

8. Edit `config.h` and enter the exact Sony camera Wi-Fi SSID and password shown by `Smart Remote Embedded` in step 2 above.
9. Connect the original `M5StickC` over USB.
10. Select the correct serial port in Arduino IDE.
11. Click `Upload`.

The sketch will still compile without `config.h` by using the fallback config, but the device will stop at `EDIT CFG` until real credentials are added.

## Runtime Behavior

- if `config.h` is missing, the display shows `EDIT CFG` and the board does not try to connect
- `BtnA` toggles recording
- `BtnB` short press toggles the display on or off
- `BtnB` long press cycles background themes
- the display shows battery state, `READY` or `RECORD`, network status, and camera status

## Troubleshooting

- screen displays `EDIT CFG`: create `config.h` and copy the exact SSID and password from the camera
- build succeeds but the device does not connect: confirm the camera is still inside `Smart Remote Embedded` and still exposing the legacy Wi-Fi API path
- verified only on the older Sony `Smart Remote Embedded` / `Camera Remote API` route
- elapsed recording time starts when the device first observes recording, not from a camera-internal absolute start
