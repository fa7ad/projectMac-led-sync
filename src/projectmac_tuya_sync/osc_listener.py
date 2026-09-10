"""Listens for projectMac's `/projectmac/...` scene-stream OSC messages and
keeps the latest values in a plain mutable SceneState — everything else
(mapping, web panel) just reads from it.
"""

from __future__ import annotations

import asyncio
import time
from dataclasses import dataclass, field

from pythonosc.dispatcher import Dispatcher
from pythonosc.osc_server import AsyncIOOSCUDPServer

# How long the dominant hue has to sit still before rate_bpm switches from
# visual/bpm to tempo/bpm -- a static/held scene has no on-screen strobe to
# track, so falling back to the audio's own beat keeps the lamp flashing
# rhythmically at that held color instead of sitting on a stale visual rate.
HUE_HOLD_TOLERANCE_DEG = 8.0
HUE_HOLD_THRESHOLD_SECONDS = 2.0


def _circular_distance(a: float, b: float) -> float:
    d = abs(a - b) % 360
    return min(d, 360 - d)


def _select_rate_bpm(hue_held_seconds: float, tempo_bpm: float, visual_bpm: float) -> float:
    if hue_held_seconds > HUE_HOLD_THRESHOLD_SECONDS:
        return tempo_bpm
    return visual_bpm


@dataclass
class SceneState:
    vibrant_hsv: tuple[float, float, float] = (0.0, 0.0, 0.0)
    muted_hsv: tuple[float, float, float] = (0.0, 0.0, 0.0)
    average_hsv: tuple[float, float, float] = (0.0, 0.0, 0.0)
    brightness: float = 0.0
    tempo_bpm: float = 120.0
    visual_bpm: float = 120.0
    tempo_phase: float = 0.0
    visual_phase: float = 0.0
    audio_bass: float = 0.0
    audio_mid: float = 0.0
    audio_treble: float = 0.0
    preset_name: str = ""

    _hue_hold_reference_deg: float = field(default=0.0, repr=False, compare=False)
    _hue_hold_since: float = field(default_factory=time.monotonic, repr=False, compare=False)

    def set_vibrant_hsv(self, h: float, s: float, v: float) -> None:
        hue_deg = h * 360
        if _circular_distance(hue_deg, self._hue_hold_reference_deg) > HUE_HOLD_TOLERANCE_DEG:
            self._hue_hold_reference_deg = hue_deg
            self._hue_hold_since = time.monotonic()
        self.vibrant_hsv = (h, s, v)

    @property
    def rate_bpm(self) -> float:
        """visual/bpm normally wins (tracks on-screen strobe activity that
        has nothing to do with the music's actual tempo), but if the
        dominant hue has been holding steady for a while, there's no visual
        strobe to track -- fall back to tempo/bpm (audio) instead (see
        PLAN.md)."""
        held = time.monotonic() - self._hue_hold_since
        return _select_rate_bpm(held, self.tempo_bpm, self.visual_bpm)


def build_dispatcher(state: SceneState, on_update) -> Dispatcher:
    """State is captured via closure — pythonosc calls each handler as
    callback(address, *osc_args), no fixed-args indirection needed.

    `on_update` fires after every message, so the pipeline can react
    immediately (event-driven, not polled) — see pipeline.Pipeline."""

    def hsv(attr):
        def handler(addr, h, s, v):
            setattr(state, attr, (h, s, v))
            on_update()
        return handler

    def scalar(attr):
        def handler(addr, value):
            setattr(state, attr, value)
            on_update()
        return handler

    def vibrant_hsv(addr, h, s, v):
        state.set_vibrant_hsv(h, s, v)
        on_update()

    d = Dispatcher()
    d.map("/projectmac/scene/vibrant", vibrant_hsv)
    d.map("/projectmac/scene/muted", hsv("muted_hsv"))
    d.map("/projectmac/scene/average", hsv("average_hsv"))
    d.map("/projectmac/scene/brightness", scalar("brightness"))
    d.map("/projectmac/tempo/bpm", scalar("tempo_bpm"))
    d.map("/projectmac/visual/bpm", scalar("visual_bpm"))
    d.map("/projectmac/tempo/phase", scalar("tempo_phase"))
    d.map("/projectmac/visual/phase", scalar("visual_phase"))
    d.map("/projectmac/audio/bass", scalar("audio_bass"))
    d.map("/projectmac/audio/mid", scalar("audio_mid"))
    d.map("/projectmac/audio/treble", scalar("audio_treble"))
    d.map("/projectmac/preset/name", scalar("preset_name"))
    return d


async def start_osc_server(state: SceneState, host: str, port: int, on_update) -> asyncio.DatagramTransport:
    """Binds the UDP port on the running asyncio loop. Returns the transport
    — call transport.close() to stop listening."""
    dispatcher = build_dispatcher(state, on_update)
    server = AsyncIOOSCUDPServer((host, port), dispatcher, asyncio.get_running_loop())
    transport, _protocol = await server.create_serve_endpoint()
    return transport
