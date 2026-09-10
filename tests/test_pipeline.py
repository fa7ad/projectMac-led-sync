from projectmac_tuya_sync.osc_listener import SceneState
from projectmac_tuya_sync.pipeline import Pipeline


class _FakeTransport:
    def __init__(self):
        self.sent = []

    def send_frame(self, frame):
        self.sent.append(frame)


def test_on_state_changed_skips_redundant_sends_within_the_same_bracket():
    state = SceneState(vibrant_hsv=(0.0, 1.0, 1.0))  # pure red
    transport = _FakeTransport()
    pipeline = Pipeline(state, transport)
    pipeline.mode = "color"

    pipeline.on_state_changed()
    pipeline.on_state_changed()  # same hue/sat/bri -- should not resend
    assert len(transport.sent) == 1


def test_on_state_changed_sends_again_when_target_actually_changes():
    state = SceneState(vibrant_hsv=(0.0, 1.0, 1.0))
    transport = _FakeTransport()
    pipeline = Pipeline(state, transport)
    pipeline.mode = "color"
    pipeline._rate_limiter.min_interval = 0  # isolate dedup from the rate gate for this test

    pipeline.on_state_changed()
    state.vibrant_hsv = (0.5, 1.0, 1.0)  # genuinely different color
    pipeline.on_state_changed()
    assert len(transport.sent) == 2


def test_on_state_changed_rate_limits_rapid_genuine_changes():
    state = SceneState(vibrant_hsv=(0.0, 1.0, 1.0))
    transport = _FakeTransport()
    pipeline = Pipeline(state, transport)
    pipeline.mode = "color"

    pipeline.on_state_changed()
    state.vibrant_hsv = (0.5, 1.0, 1.0)
    pipeline.on_state_changed()  # too soon after the first real send -- gated
    assert len(transport.sent) == 1
