"""Wires listener state -> manual override -> mapping -> frame building ->
serial transport, triggered directly by incoming OSC updates (event-driven,
not polled on a fixed timer) — ported from the original research plugin's
proven design (~/tuya-sunset-lamp-research/code/lamp_sync_plugin.py): only
send when the computed target actually differs from what was last sent, so
a burst of hues landing in the same pattern/color bracket collapses to one
command instead of several redundant restarts, and the effective send rate
tracks how fast the real signal is changing rather than an arbitrary poll
interval. A send that gets rate-limited isn't lost — since `_last_sent`
only updates on an actual send, the next update recomputing the same target
retries it.

Manual override is just fields on Pipeline (active + color) rather than its
own module — when active, it wins outright over the automatic mapping
output, per PLAN.md's manual-override layer.
"""

from __future__ import annotations

import logging
import time
from dataclasses import dataclass

from . import mapping
from .lamp_command_protocol import (
    COLOR_RED,
    PATTERN_STATIC,
    build_color_frame,
    build_scene_effect_frame,
)
from .osc_listener import SceneState
from .serial_transport import SerialTransport
from .sn_counter import RevisionCounter, SnCounter

logger = logging.getLogger(__name__)

MODES = ("color", "pattern", "combined")
OVERRIDE_KINDS = ("color", "pattern")

# Protects the lamp's BLE receiver from being overwhelmed. The exact ceiling
# is unconfirmed on this lamp (earlier research guessed ~4-6/sec, but that
# estimate looks conservative) -- 10/sec pending a real test of how high it
# can actually go.
MIN_SEND_INTERVAL_SECONDS = 0.1


@dataclass
class ManualOverride:
    """Two independent manual controls -- dp=11 (color) and dp=73
    (pattern) -- since only one can actually drive the lamp at a time,
    `kind` picks which; `active` is the shared on/off switch for whichever
    was set most recently."""

    active: bool = False
    kind: str = "color"  # "color" | "pattern"
    # dp=11
    hue: int = 0
    saturation: int = 100
    brightness: int = 100
    # dp=73 (brightness is shared with dp=11 above)
    pattern: int = PATTERN_STATIC
    colors: int = COLOR_RED  # a valid solo preset for the default pattern (static)
    speed: int = 50


class _RateLimiter:
    def __init__(self, min_interval: float):
        self.min_interval = min_interval
        self._last_send = 0.0

    def allow(self) -> bool:
        now = time.monotonic()
        if now - self._last_send >= self.min_interval:
            self._last_send = now
            return True
        return False


class Pipeline:
    def __init__(self, state: SceneState, transport: SerialTransport | None):
        self.state = state
        self.override = ManualOverride()
        self.transport = transport
        self.mode = "combined"
        self._sn_counter = SnCounter()
        self._revision_counter = RevisionCounter()
        self._rate_limiter = _RateLimiter(MIN_SEND_INTERVAL_SECONDS)
        self._last_sent = None  # dedup key: the ("color"|"pattern", params) last actually sent

    def current_color(self) -> tuple[int, int, int]:
        """Best-effort color preview for the web panel — reflects the sensed
        dominant color whenever nothing carries an arbitrary hue directly
        (pattern mode, or a pattern override)."""
        if self.override.active and self.override.kind == "color":
            return self.override.hue, self.override.saturation, self.override.brightness
        return mapping.color_follow(self.state)

    def _target(self) -> tuple[str, tuple]:
        if self.override.active:
            if self.override.kind == "color":
                return "color", (self.override.hue, self.override.saturation, self.override.brightness)
            return "pattern", (self.override.pattern, self.override.colors, self.override.speed, self.override.brightness)
        if self.mode == "color":
            return "color", mapping.color_follow(self.state)
        if self.mode == "pattern":
            return "pattern", mapping.pattern_follow(self.state)
        return mapping.combined_tick(self.state)

    def _build_frame(self, kind: str, params: tuple) -> bytes:
        if kind == "color":
            hue, saturation, brightness = params
            return build_color_frame(hue, saturation, brightness, sn=self._sn_counter.next())
        pattern, colors, speed, brightness = params
        return build_scene_effect_frame(
            pattern, colors, speed, brightness,
            sn=self._sn_counter.next(), revision=self._revision_counter.next(),
        )

    def on_state_changed(self) -> None:
        """Call after any OSC update, or an override/mode change from the
        GUI, that could change the lamp's target output."""
        target = self._target()
        if target == self._last_sent:
            return
        if not self._rate_limiter.allow():
            return
        frame = self._build_frame(*target)
        if self.transport is not None:
            self.transport.send_frame(frame)
        else:
            logger.debug("no serial port configured, dropping frame: %s", frame.hex())
        self._last_sent = target
