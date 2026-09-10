from projectmac_tuya_sync.osc_listener import (
    HUE_HOLD_THRESHOLD_SECONDS,
    SceneState,
    _select_rate_bpm,
)


def test_select_rate_bpm_prefers_visual_before_the_hold_threshold():
    assert _select_rate_bpm(0.0, tempo_bpm=90.0, visual_bpm=140.0) == 140.0


def test_select_rate_bpm_falls_back_to_tempo_once_held_past_threshold():
    held = HUE_HOLD_THRESHOLD_SECONDS + 0.1
    assert _select_rate_bpm(held, tempo_bpm=90.0, visual_bpm=140.0) == 90.0


def test_set_vibrant_hsv_resets_hold_timer_on_a_genuine_hue_change():
    state = SceneState()
    state.set_vibrant_hsv(0.0, 1.0, 1.0)  # hue=0
    held_after_first = 0.0  # freshly reset
    assert _select_rate_bpm(held_after_first, 90.0, 140.0) == 140.0

    # a small drift within tolerance shouldn't reset the hold timer
    since_before = state._hue_hold_since
    state.set_vibrant_hsv(0.01, 1.0, 1.0)  # hue=3.6, well within HUE_HOLD_TOLERANCE_DEG
    assert state._hue_hold_since == since_before

    # a genuinely different hue should reset it
    state.set_vibrant_hsv(0.5, 1.0, 1.0)  # hue=180, well outside tolerance
    assert state._hue_hold_since != since_before
