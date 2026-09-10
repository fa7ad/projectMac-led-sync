from projectmac_tuya_sync.lamp_command_protocol import (
    COLOR_BLUE,
    COLOR_RED,
    COLORS_FOUR_COLOR,
    COLORS_RED_GREEN,
    PATTERN_BREATH,
    PATTERN_FLASH,
    PATTERN_GRADIENT,
    VALID_COLORS_BY_PATTERN,
)
from projectmac_tuya_sync.mapping import color_follow, combined_tick, pattern_follow
from projectmac_tuya_sync.osc_listener import SceneState


def test_color_follow_scales_hsv_fractions_to_dp_ranges():
    state = SceneState(vibrant_hsv=(0.5, 0.8, 0.9))
    assert color_follow(state) == (180, 80, 90)


def test_color_follow_does_not_clip_purple_magenta_hues():
    # dp=11's hue is a full 0-359 16-bit field, not a clipped single byte --
    # confirmed via a real BLE capture of the Smart Life app's color wheel
    # (2026-09-10), see build_color_frame's docstring.
    state = SceneState(vibrant_hsv=(300 / 360, 1.0, 1.0))
    assert color_follow(state) == (300, 100, 100)


def test_pattern_follow_picks_pattern_by_bpm_band():
    # BPM_MIN=70, BPM_MAX=160, 4 equal bands: breath, gradient, jump, flash
    slow = pattern_follow(SceneState(visual_bpm=80.0))[0]
    fast = pattern_follow(SceneState(visual_bpm=155.0))[0]
    assert slow == PATTERN_BREATH
    assert fast == PATTERN_FLASH
    assert slow != fast  # the whole point: different tempos get different patterns


def test_pattern_follow_uses_visual_bpm_not_tempo_bpm():
    state = SceneState(tempo_bpm=80.0, visual_bpm=155.0)
    pattern, _colors, _speed, _brightness = pattern_follow(state)
    assert pattern == PATTERN_FLASH  # visual_bpm (155, fast band) must win, not tempo_bpm (80)


def test_pattern_follow_single_color_pattern_gets_one_bit():
    # breath is a single-color pattern -- pure red should quantize to just COLOR_RED
    state = SceneState(visual_bpm=80.0, vibrant_hsv=(0.0, 1.0, 1.0))
    _pattern, colors, _speed, _brightness = pattern_follow(state)
    assert colors == COLOR_RED


def test_pattern_follow_multi_color_pattern_uses_a_confirmed_valid_combo():
    # gradient is a multi-color pattern -- orange (30 deg) is nearest red
    # among all reference hues, and red is in the RGB family, so it brackets
    # to the RED|GREEN pairwise combo (0-120 deg arc) -- one of the 6 combos
    # confirmed valid for jump/gradient via the app's own preset menus
    # (2026-09-10), not an arbitrary/untested pair.
    state = SceneState(visual_bpm=100.0, vibrant_hsv=(30 / 360, 1.0, 1.0))
    pattern, colors, _speed, _brightness = pattern_follow(state)
    assert pattern == PATTERN_GRADIENT
    assert colors == COLORS_RED_GREEN
    assert colors in VALID_COLORS_BY_PATTERN[PATTERN_GRADIENT]


def test_pattern_follow_multi_color_pattern_falls_back_to_four_color_in_cmyk_arc():
    # yellow (60 deg) is in the CMYK family -- no pairwise sub-combos are
    # confirmed valid there, so it falls back to four-color.
    state = SceneState(visual_bpm=100.0, vibrant_hsv=(60 / 360, 1.0, 1.0))
    _pattern, colors, _speed, _brightness = pattern_follow(state)
    assert colors == COLORS_FOUR_COLOR


def test_flash_allows_blue_confirmed_on_real_hardware():
    # the Smart Life app's menu is missing a solo blue preset for flash, but
    # a direct real-device test (bypassing the app) confirmed the lamp
    # flashes blue just fine -- a UI restriction, not a firmware one.
    assert COLOR_BLUE in VALID_COLORS_BY_PATTERN[PATTERN_FLASH]
    state = SceneState(visual_bpm=155.0, vibrant_hsv=(240 / 360, 1.0, 1.0))  # flash band, pure blue
    pattern, colors, _speed, _brightness = pattern_follow(state)
    assert pattern == PATTERN_FLASH
    assert colors == COLOR_BLUE


def test_combined_tick_uses_pattern_for_a_preset_color():
    state = SceneState(vibrant_hsv=(0.0, 1.0, 1.0))  # pure red -- matches a dp=73 preset exactly
    assert combined_tick(state)[0] == "pattern"


def test_combined_tick_uses_color_for_a_gap_hue():
    state = SceneState(vibrant_hsv=(90 / 360, 1.0, 1.0))  # chartreuse -- no dp=73 preset covers it
    kind, params = combined_tick(state)
    assert kind == "color"
    assert params[0] == 90


def test_combined_tick_treats_low_saturation_as_covered_by_white():
    state = SceneState(vibrant_hsv=(90 / 360, 0.05, 1.0))  # same gap hue, but near-white
    assert combined_tick(state)[0] == "pattern"


def test_combined_tick_uses_pattern_for_true_purple_hue():
    state = SceneState(vibrant_hsv=(300 / 360, 1.0, 1.0))  # true purple -- matches dp=73's PURPLE bit
    assert combined_tick(state)[0] == "pattern"
