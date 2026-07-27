# M5NanoC6 Companion v4 Satellite

Version 0.1.1 firmware for using the M5Stack NanoC6 as a one-button Bitfocus
Companion v4 satellite.

## Included in v0.1.1

- Companion single-button surface (`KEY-PRESS`, `KEY-RELEASE`, RGB colour and brightness)
- Full 0–255 WS2812 colour range; no Atom Matrix brightness safety cap
- WiFiManager setup (hold the button for five seconds)
- Browser and Arduino OTA firmware updates
- NEC infrared transmission through `POST /api/ir/nec`
- ESP32-C6 802.15.4 capability discovery for Zigbee, Thread and Matter
- No Ethernet code

The Wi-Fi satellite build exposes the NanoC6's 802.15.4 capabilities, but does
not commission a Zigbee or Thread/Matter network. Those stacks require separate
build profiles and partition/role selections; `/api/radio` returns that state
explicitly rather than implying that a radio network is active.

## Install

Use `release/M5NanoC6-Companion-v4-Satellite-v0.1.1-factory.bin` for the first USB
installation. Hold the GPIO9 button while connecting USB-C to enter download
mode.

### First installation with ESPHome Web

1. Download the `*-factory.bin` file from the GitHub release.
2. Connect the NanoC6 using a USB data cable and open
   [ESPHome Web](https://web.esphome.io/).
3. Select **Connect**, choose the serial device, select **Install**, and choose
   the factory image.
4. Wait for flashing to finish, reconnect power, and complete setup.

### Updating over the air

1. Download the matching `*-upgrade.bin` application image.
2. Browse to `http://DEVICE-IP:9999/update`.
3. Select the upgrade image and wait for the automatic reboot. Do not remove
   power during the update.

Never upload the factory image through the port 9999 update page.

Configure Companion's satellite host and port in the setup portal. The AP name
and device ID are `M5NANOC6_<full Wi-Fi MAC>`.

The dashboard at `http://DEVICE-IP:9999/` shows the device ID, Wi-Fi and
Companion connection status, current IP, brightness, button state, most recent
incoming colour, and other troubleshooting data. It refreshes every two seconds.

## Companion control API

The companion-module-m5-satellites module includes matching actions.

- `GET /api/settings` — firmware, brightness, IR and radio capability state
- `POST /api/settings` — `{"brightness":100}`
- `POST /api/ir/nec` — `{"address":"0x00FF","command":"0x20DF","repeats":0}`
- `GET /api/radio` — radio capability/profile state

Addresses and commands may be decimal numbers or quoted `0x` hexadecimal.

## Build

Run `./build-release.sh`. It uses:

- Board: `m5stack:esp32:m5stack_nano_c6`
- Partition scheme: Minimal SPIFFS (1.9 MB app with OTA)
- USB CDC on boot: enabled
- Libraries: WiFiManager and Adafruit NeoPixel

## Hardware

GPIO 9 is the button, GPIO 20 is WS2812 data, GPIO 19 enables RGB power, and
GPIO 3 drives the built-in IR emitter.
