"""Build ready-to-broadcast Tuya BLE beacon frames for this lamp's DPs
(data points), using the codec in tuya_beacon_codec.py.

Copied/adapted from the original research repo's lamp_command.py, with two
changes: (1) LOCAL_KEY/APP_KEY/SRC_ADDR/DST_ADDR — real secrets specific to
one device+account pairing — are no longer hardcoded here; they're imported
from wherever this project's config module loads them (from a gitignored
.env, see PLAN.md's "Secrets handling"). Wire up that import before this file
will actually run — the `from config import ...` line below is a placeholder
for whatever this project's real config module ends up being named/located.
(2) comments that pointed at the private research repo's own narrative doc
have been rewritten to stand on their own.

Everything else — the DP layouts, byte offsets, which bits mean which
color — is real reverse-engineered protocol knowledge and is unchanged.
"""

from tuya_beacon_codec import build_dp_payload, encode_frame

# Placeholder — replace with this project's real config import once it
# exists. Real values are specific to one device+account pairing and must
# never be hardcoded/committed; see PLAN.md's "Secrets handling".
from config import LOCAL_KEY, APP_KEY, SRC_ADDR, DST_ADDR  # noqa: F401 (see above)

DP_ID_SCENE = 73
DP_ID_COLOR = 11
SUB_CMD_P2DPS = 5      # dbpqbqp.bdqqbqd() — "P2Dps" DP-write command
LEAD_BYTE_P2DPS = 0x0B  # (bpqqdpq()=1 << 3) + 3, confirmed against a real capture

# scene_data (dpId=73) value byte[2]: pattern type.
PATTERN_STATIC = 0x00
PATTERN_JUMP = 0x01
PATTERN_GRADIENT = 0x02
PATTERN_FLASH = 0x03
PATTERN_BREATH = 0x04

# scene_data value byte[6]: bitmask of these, OR'd together. Derived from
# isolated single-bit real-device tests (send each bit alone, observe which
# color lights up) — trust these over any inference from preset tap-order,
# which was tried first and got two pairs swapped.
COLOR_RED = 0x01
COLOR_BLUE = 0x02
COLOR_GREEN = 0x04
COLOR_CYAN = 0x08
COLOR_YELLOW = 0x10
COLOR_PURPLE = 0x20
COLOR_WHITE = 0x40
COLORS_TRICOLOR = COLOR_RED | COLOR_GREEN | COLOR_BLUE
COLORS_FOUR_COLOR = COLOR_YELLOW | COLOR_CYAN | COLOR_PURPLE | COLOR_WHITE
COLORS_SEVEN_COLOR = 0x7F


def build_speed_brightness_frame(speed: int, brightness: int, sn: int, scene: int = 0x02) -> bytes:
    """speed and brightness are 0-100. sn is a 16-bit sequence number — pick a
    fresh, strictly-increasing value for every frame sent (see this
    project's SnCounter — the lamp silently drops anything that isn't higher
    than the last sn it accepted).

    Value byte[4] is speed, byte[5] is brightness — confirmed by an isolating
    real-device test: a static (non-animated) pattern, where speed can't have
    any visible effect, sent with brightness=5/speed=100 came out at FULL
    brightness, and brightness=100/speed=5 came out dim, which only makes
    sense if the bytes were swapped from what's documented in this docstring
    at face value. This function's own `speed=`/`brightness=` parameter
    meanings are correct as named; only the internal byte layout needed
    fixing.
    """
    assert 0 <= speed <= 100
    assert 0 <= brightness <= 100
    assert 0 <= sn <= 0xFFFF

    value = bytes([scene, 0x00, 0x01, 0x01, speed, brightness, 0x7F])
    plaintext = build_dp_payload(DP_ID_SCENE, dp_type=0, value=value)

    return encode_frame(
        plaintext=plaintext,
        local_key=LOCAL_KEY,
        app_key=APP_KEY,
        src_addr=SRC_ADDR,
        dst_addr=DST_ADDR,
        sn=sn.to_bytes(2, "big"),
        sub_cmd=SUB_CMD_P2DPS,
        lead_byte=LEAD_BYTE_P2DPS,
    )


def build_scene_effect_frame(
    pattern: int, colors: int, speed: int, brightness: int, sn: int, revision: int = 0x01
) -> bytes:
    """Drive one of the lamp's built-in effects (static/jump/gradient/flash/
    breath) over a chosen color bitmask, via the same scene_data DP as
    `build_speed_brightness_frame`.

    `revision` is byte[0] of the value — the first-party app bumps it once
    per distinct preset picked; whether the device actually requires it to
    be non-repeating is unconfirmed, but treat it the same way as `sn` (see
    this project's RevisionCounter) since a real unresponsive-device incident
    coincided with an ad-hoc test script resetting its own revision counter
    to 1 on every run. speed/brightness are 0-100, same byte-order note as
    `build_speed_brightness_frame` applies (byte[4]/byte[5] = speed/brightness).
    """
    assert pattern in (PATTERN_STATIC, PATTERN_JUMP, PATTERN_GRADIENT, PATTERN_FLASH, PATTERN_BREATH)
    assert 0 <= colors <= 0x7F
    assert 0 <= speed <= 100
    assert 0 <= brightness <= 100
    assert 0 <= sn <= 0xFFFF

    value = bytes([revision, 0x00, pattern, 0x01, speed, brightness, colors])
    plaintext = build_dp_payload(DP_ID_SCENE, dp_type=0, value=value)

    return encode_frame(
        plaintext=plaintext,
        local_key=LOCAL_KEY,
        app_key=APP_KEY,
        src_addr=SRC_ADDR,
        dst_addr=DST_ADDR,
        sn=sn.to_bytes(2, "big"),
        sub_cmd=SUB_CMD_P2DPS,
        lead_byte=LEAD_BYTE_P2DPS,
    )


def build_color_frame(hue: int, saturation: int, brightness: int, sn: int) -> bytes:
    """hue is 0-255 (degrees, NOT scaled to 360 — confirmed against real
    captures for hue=0/120/240 = red/green/blue; behavior above 255 is
    untested — Tuya's hue wheel is 0-359 but this DP only has one byte for
    it). saturation and brightness are 0-100 (percent). This is the lamp's
    arbitrary-static-color DP — no cycling/animation, unlike scene_data."""
    assert 0 <= hue <= 255
    assert 0 <= saturation <= 100
    assert 0 <= brightness <= 100
    assert 0 <= sn <= 0xFFFF

    value = bytes([0x00, hue, saturation, brightness])
    plaintext = build_dp_payload(DP_ID_COLOR, dp_type=0, value=value)

    return encode_frame(
        plaintext=plaintext,
        local_key=LOCAL_KEY,
        app_key=APP_KEY,
        src_addr=SRC_ADDR,
        dst_addr=DST_ADDR,
        sn=sn.to_bytes(2, "big"),
        sub_cmd=SUB_CMD_P2DPS,
        lead_byte=LEAD_BYTE_P2DPS,
    )


def hex_to_hue_sat_bri(hex_color: str):
    """'#RRGGBB' or 'RRGGBB' -> (hue 0-255, saturation 0-100, brightness 0-100).

    Hue is degrees DIRECTLY (confirmed: 240/120/0 = blue/green/red), not
    scaled to a 0-255 range — a single byte can't hold the full 0-359 wheel,
    so hues above 255 (roughly pink/magenta) are clipped to 255 here. Real
    device behavior above 255 is untested; this is a best-effort clamp, not
    a confirmed-correct mapping for that range.
    """
    import colorsys
    hex_color = hex_color.lstrip("#")
    r, g, b = (int(hex_color[i:i + 2], 16) / 255.0 for i in (0, 2, 4))
    h, s, v = colorsys.rgb_to_hsv(r, g, b)
    hue_deg = round(h * 360)
    return min(255, hue_deg), round(s * 100), round(v * 100)
