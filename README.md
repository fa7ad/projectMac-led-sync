# projectMac-tuya-sync

Drives a Tuya BLE-beacon smart lamp from [projectMac](https://github.com/fa7ad/projectMac)'s
OSC scene stream. Everything runs on an ESP32-C3: it receives projectMac's OSC over WiFi,
maps the scene's colors and tempo to lamp commands, encrypts them into Tuya beacon frames,
and broadcasts them over BLE. It also serves a web control panel. No host process needed,
and it runs fine off a battery.

The repo root is the ESP-IDF project:

```
main/main.c            WiFi, OSC listener, pipeline, sn counters, BLE advertising, web server
main/lamp.c            Tuya codec, DP frame builders, scene -> lamp mapping (pure C)
main/index.html        web panel, embedded into the firmware
main/Kconfig.projbuild menuconfig options (WiFi, lamp keys, counters)
test/test_lamp.c       host-side tests for lamp.c
```

## Lamp pairing keys

Specific to one device + account pairing, obtained via the Smart Life app:

- **local_key**: the device's standard Tuya Cloud API `local_key` (16 ASCII chars).
- **AppKey**: the account/home-scoped Tuya "Beacon Mesh AppKey" (32 hex chars), from the
  private `thing.m.device.beacon.mesh.create` mobile API call. It isn't available through
  any public Tuya Cloud/Open API endpoint.
- **Source / destination address**: 2-byte mesh addressing fields (4 hex chars each) from a
  real captured beacon frame, e.g. via a BLE HCI snoop capture.

## Build and flash

With ESP-IDF activated (EIM installs: `source ~/.espressif/tools/activate_idf_v6.1.fish`):

```
idf.py set-target esp32c3     # first time only
idf.py menuconfig             # Lamp bridge → WiFi, pairing keys, starting sn/revision
idf.py build flash monitor
```

Everything set in menuconfig lives in the gitignored `sdkconfig`. The board registers as
`lamp` with your router's DHCP. On connect the monitor logs its IP; reserve that IP in your
router's DHCP settings.

**sn continuity**: the lamp ignores any frame whose sequence number isn't above the highest
it has accepted from that source address. The firmware keeps its counters in NVS. On first
boot (empty NVS) it starts from menuconfig's "Starting sn", which must be above the lamp's
current high-water mark. If that's unknown, put the lamp in pairing mode or switch to an
unused source address. sn is 16-bit and wraps after 65535, at which point the lamp stops
accepting frames until you do one of those.

## Use

- In projectMac's Settings → Scene Stream, set **Destination IP** to the board's IP and the
  port to `9000`. Don't use a broadcast address: many routers drop broadcast to WiFi clients.
- Open `http://lamp.home/` (or the IP) from any browser on the LAN:
  - live scene readout: color, the bpm driving the lamp, audio and visual bpm, preset name
  - lamp mode: **Color** (follow projectMac's dominant color), **Pattern** (built-in
    effect picked by tempo, speed matched to the beat), or **Combined** (pattern, plus
    exact color for hues the effect palette can't show)
  - manual override: hue/saturation/brightness sliders, "Latch current color", or a
    pattern effect with color preset, speed (shown in bpm), tap tempo, and ÷2/×2

## Tests

`lamp.c` has no ESP-IDF dependencies:

```
cc -Wall -Werror -o /tmp/test_lamp test/test_lamp.c main/lamp.c -lm && /tmp/test_lamp
```

It covers golden frames, valid color presets, the mapping modes, the slow-motion-measured
speed timing, and the bpm hold logic.

## Protocol notes

The reverse-engineering details live as comments next to the code in `main/lamp.c`
(frame layout, DP byte orders, valid color presets, measured effect timing) and
`main/main.c` (sn/revision rules, BLE advertising format).
