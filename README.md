# projectMac-tuya-sync

OSC scene-stream (from `projectMac`) to Tuya BLE-beacon lamp bridge, relayed over an
ESP32. See `PLAN.md` for the full architecture and design decisions.

## Setup

```
uv sync --group dev
cp .env.example .env   # fill in your lamp's pairing keys, see below
uv run pytest tests/
```

## Getting your lamp's pairing keys

`LAMP_LOCAL_KEY` and `LAMP_APP_KEY` are specific to one device+account pairing, obtained
via the Smart Life app:

1. Log into the Smart Life app with the account your lamp is paired to.
2. `LAMP_LOCAL_KEY` is the device's standard Tuya Cloud API `local_key`.
3. `LAMP_APP_KEY` is the account/home-scoped Tuya "Beacon Mesh AppKey", obtained via the
   private `thing.m.device.beacon.mesh.create` mobile API call — not available through
   any public Tuya Cloud/Open API endpoint.
4. `LAMP_SRC_ADDR` / `LAMP_DST_ADDR` are 2-byte mesh addressing fields from a real
   captured beacon frame (e.g. via a BLE HCI snoop capture).

## Running

```
uv run uvicorn projectmac_tuya_sync.web.app:app --host 0.0.0.0 --port 8000
```

Reads `BRIDGE_SERIAL_PORT` from `.env` (set it to your ESP32's serial device path, e.g.
`/dev/tty.usbserial-xxxx`). Leave it unset/blank to run in dry-run mode (frames computed
but not sent — useful for testing the OSC listener / mapping / web panel without the
ESP32 attached).

Open `http://<mac's-lan-ip>:8000/` from any browser on the LAN (including a phone) for
live scene meters and manual color override.

**Before the first real-device send**: make sure `SnCounter`'s persisted state
(`.lamp_sn_state.json`) starts above whatever the lamp's real high-water mark already is
for `LAMP_SRC_ADDR` — see `PLAN.md`'s "Continuity with the old pipeline" for how to reset
that (send a command while the lamp is in pairing mode, or switch to an unused
`srcAddr`).

## ESP32 firmware

`esp32-bridge/` is a standalone ESP-IDF project (serial-only bridge, no protocol
knowledge). Flash it with `idf.py build flash monitor` from within that directory once
it's dropped into your ESP-IDF tool environment.
