"""Scene-stream -> lamp-parameter mapping. Three modes: continuous
color-follow (v1), rate-matched pattern (v2), and combined (v3) — dp=11
(arbitrary color, full 0-359 hue wheel) only earns its place in combined
mode for hues dp=73's fixed 7-color palette can't represent; everything
else rides the rate-matched pattern (see PLAN.md).

The BPM-band pattern selection and per-pattern speed calibration below are
ported from the original research plugin
(~/tuya-sunset-lamp-research/projectmac-sunset-sync/src/projectmac_sunset_sync/plugin.py)
rather than re-derived — real, tap-tempo-measured values against this exact
physical lamp.

dp=73's color bitmask does NOT accept arbitrary bit combinations -- only a
fixed preset list per pattern, re-verified directly against the Smart Life
app's own preset menus (2026-09-10) and encoded as
`lamp_command_protocol.VALID_COLORS_BY_PATTERN` (enforced there too, in
`build_scene_effect_frame`). The quantization below is built to only ever
produce values from that table.
"""

from .lamp_command_protocol import (
    COLOR_BLUE,
    COLOR_CYAN,
    COLOR_GREEN,
    COLOR_PURPLE,
    COLOR_RED,
    COLOR_WHITE,
    COLOR_YELLOW,
    COLORS_FOUR_COLOR,
    COLORS_SEVEN_COLOR,
    COLORS_TRICOLOR,
    PATTERN_BREATH,
    PATTERN_FLASH,
    PATTERN_GRADIENT,
    PATTERN_JUMP,
    VALID_COLORS_BY_PATTERN,
)
from .osc_listener import SceneState

BPM_MIN, BPM_MAX = 70.0, 160.0
SPEED_MIN, SPEED_MAX = 10, 100

# 4 equal-width BPM bands over BPM_MIN-BPM_MAX, slowest to fastest: breath,
# gradient, jump, flash. An aesthetic choice (not derived from the speed
# calibration below), keeps calm/slow music on the gentlest effect and
# busy/fast music on the most energetic one.
_BPM_BAND_WIDTH = (BPM_MAX - BPM_MIN) / 4
_PATTERN_BANDS = [
    (BPM_MIN + 1 * _BPM_BAND_WIDTH, PATTERN_BREATH),
    (BPM_MIN + 2 * _BPM_BAND_WIDTH, PATTERN_GRADIENT),
    (BPM_MIN + 3 * _BPM_BAND_WIDTH, PATTERN_JUMP),
    (BPM_MAX + 1, PATTERN_FLASH),  # +1 so bpm == BPM_MAX still lands in this band
]

# jump/gradient cycle *between* colors, so they get a 2-color bracket instead
# of one dominant color; breath/flash pulse/strobe a single color, so they
# get the nearest single-color quantization.
_MULTI_COLOR_PATTERNS = (PATTERN_JUMP, PATTERN_GRADIENT)

# Reference hues in true degrees (0-360) for each fixed dp=73 color bit --
# a separate concept from dp=11's hue field (also full 0-359 now, see
# build_color_frame), since dp=73 only offers 7 discrete color bits, not an
# arbitrary hue. red/green/blue confirmed directly against the device;
# yellow/cyan/purple by construction (a 3-channel RGB LED, so those "colors"
# are just two base channels driven at once), hence the even 60-degree
# spacing.
_HUE_REFERENCE = [
    (0.0, COLOR_RED),
    (60.0, COLOR_YELLOW),
    (120.0, COLOR_GREEN),
    (180.0, COLOR_CYAN),
    (240.0, COLOR_BLUE),
    (300.0, COLOR_PURPLE),
]
_WHITE_SATURATION_THRESHOLD = 0.15  # fraction, 0.0-1.0 (vibrant_hsv's own units)
_GAP_THRESHOLD_DEG = 25  # combined-mode only: ponytail, eyeballed vs. the 60-degree spacing

# Real per-pattern calibration: (speed, cycles/min) at two speed values,
# measured directly against the physical lamp with a tap-tempo app -- speed
# scales linearly, so 2 points/pattern fully determines each line.
_SPEED_CALIBRATION = {
    PATTERN_BREATH: ((10, 2.0), (100, 255.0)),
    PATTERN_GRADIENT: ((10, 2.0), (100, 255.0)),
    PATTERN_JUMP: ((10, 30.0), (100, 3000.0)),
    PATTERN_FLASH: ((10, 22.0), (100, 3000.0)),
}


def _circular_distance(a: float, b: float) -> float:
    d = abs(a - b) % 360
    return min(d, 360 - d)


def _speed_for_bpm(bpm: float, pattern: int) -> int:
    """Invert that pattern's own calibrated (speed -> cycles/min) line to
    find the speed value that makes it cycle at `bpm`."""
    (s1, c1), (s2, c2) = _SPEED_CALIBRATION[pattern]
    slope = (c2 - c1) / (s2 - s1)
    intercept = c1 - slope * s1
    speed = (bpm - intercept) / slope
    return round(min(max(speed, SPEED_MIN), SPEED_MAX))


def _pattern_for_bpm(bpm: float) -> int:
    for band_max, pattern in _PATTERN_BANDS:
        if bpm < band_max:
            return pattern
    return PATTERN_FLASH


# dp=73's color bits split into two families around the hue wheel, matching
# the two confirmed named combos (tricolor / four-color).
_RGB_FAMILY = (COLOR_RED, COLOR_GREEN, COLOR_BLUE)
_RGB_HUE_REFERENCE = [(0.0, COLOR_RED), (120.0, COLOR_GREEN), (240.0, COLOR_BLUE)]


def _hue_sat_to_color_bits(hue_deg: float, saturation: float, pattern: int) -> int:
    """Nearest single reference color valid for this pattern -- for
    patterns that show one color (static/breath/flash). Derives its
    candidate set from VALID_COLORS_BY_PATTERN rather than assuming all
    patterns support the same solo colors, since that's not guaranteed --
    the Smart Life app's own menus already omit a valid preset in one case
    (flash + blue), which turned out to be a UI restriction, not a
    firmware one; a future pattern-specific gap that *is* real would be
    handled here the same way, via the table."""
    if saturation < _WHITE_SATURATION_THRESHOLD:
        return COLOR_WHITE
    valid = VALID_COLORS_BY_PATTERN[pattern]
    candidates = [(hue, bit) for hue, bit in _HUE_REFERENCE if bit in valid]
    _, bits = min(candidates, key=lambda ref: _circular_distance(hue_deg, ref[0]))
    return bits


def _hue_sat_to_color_combo(hue_deg: float, saturation: float) -> int:
    """One of the confirmed-valid combos for jump/gradient (identical valid
    sets): the pairwise RGB combo bracketing hue_deg when it's nearer the
    red/green/blue arc (finer-grained, 3 real combos available there), else
    four-color (no pairwise sub-combos confirmed in the yellow/cyan/purple
    arc); seven-color for low-saturation input."""
    if saturation < _WHITE_SATURATION_THRESHOLD:
        return COLORS_SEVEN_COLOR
    _, nearest_bit = min(_HUE_REFERENCE, key=lambda ref: _circular_distance(hue_deg, ref[0]))
    if nearest_bit not in _RGB_FAMILY:
        return COLORS_FOUR_COLOR
    n = len(_RGB_HUE_REFERENCE)
    for i in range(n):
        lo_hue, lo_bit = _RGB_HUE_REFERENCE[i]
        hi_hue, hi_bit = _RGB_HUE_REFERENCE[(i + 1) % n]
        span = (hi_hue - lo_hue) % 360
        offset = (hue_deg - lo_hue) % 360
        if offset < span:
            return lo_bit | hi_bit
    return COLORS_TRICOLOR  # unreachable, evenly-spaced 360-degree coverage


def _covered_by_pattern_palette(hue_deg: float, saturation: float) -> bool:
    """Combined-mode-only gate: is hue_deg close enough to one of dp=73's
    fixed colors that its nearest-fit quantization is good enough, vs. worth
    spending a precise dp=11 frame on?"""
    if saturation < _WHITE_SATURATION_THRESHOLD:
        return True
    return any(_circular_distance(hue_deg, ref_hue) < _GAP_THRESHOLD_DEG for ref_hue, _bit in _HUE_REFERENCE)


def color_follow(state: SceneState) -> tuple[int, int, int]:
    """scene/vibrant HSV (each 0.0-1.0) -> (hue 0-359, saturation 0-100,
    brightness 0-100) for build_color_frame."""
    h, s, v = state.vibrant_hsv
    hue = round(h * 360) % 360
    return hue, round(s * 100), round(v * 100)


def pattern_follow(state: SceneState) -> tuple[int, int, int, int]:
    """-> (pattern, colors, speed, brightness) for build_scene_effect_frame.
    Pattern is picked by BPM band (breath/gradient/jump/flash, slowest to
    fastest), speed by that pattern's own calibrated speed->cycles/minute
    curve, and colors quantized from the current dominant hue."""
    bpm = state.rate_bpm
    pattern = _pattern_for_bpm(bpm)
    speed = _speed_for_bpm(bpm, pattern)
    h, s, _v = state.vibrant_hsv
    hue_deg = h * 360
    colors = (
        _hue_sat_to_color_combo(hue_deg, s)
        if pattern in _MULTI_COLOR_PATTERNS
        else _hue_sat_to_color_bits(hue_deg, s, pattern)
    )
    brightness = round(state.brightness * 100) if state.brightness else 100
    return pattern, colors, speed, brightness


def combined_tick(state: SceneState):
    """Returns ("color", (hue, sat, bri)) when the current dominant color
    falls in a gap dp=73's fixed palette can't represent, else ("pattern",
    (pattern, colors, speed, brightness)) to keep the rate-matched cycle
    running."""
    h, s, _v = state.vibrant_hsv
    if _covered_by_pattern_palette(h * 360, s):
        return "pattern", pattern_follow(state)
    return "color", color_follow(state)
