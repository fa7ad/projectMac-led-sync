"""FastAPI control panel: live scene-stream meters + manual color override.
Reachable from a phone on the LAN for live-performance punch-ins."""

from __future__ import annotations

import asyncio
import os
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from ..lamp_command_protocol import (
    PATTERN_BREATH,
    PATTERN_FLASH,
    PATTERN_GRADIENT,
    PATTERN_JUMP,
    PATTERN_STATIC,
    VALID_COLORS_BY_PATTERN,
    hex_to_hue_sat_bri,
)
from ..osc_listener import SceneState, start_osc_server
from ..pipeline import MODES, Pipeline
from ..serial_transport import SerialTransport

STATIC_DIR = Path(__file__).parent / "static"

PATTERN_NAMES = {
    "static": PATTERN_STATIC,
    "jump": PATTERN_JUMP,
    "gradient": PATTERN_GRADIENT,
    "flash": PATTERN_FLASH,
    "breath": PATTERN_BREATH,
}
PATTERN_NAMES_BY_VALUE = {v: k for k, v in PATTERN_NAMES.items()}

OSC_HOST = os.environ.get("BRIDGE_OSC_HOST", "0.0.0.0")
OSC_PORT = int(os.environ.get("BRIDGE_OSC_PORT", "9000"))
SERIAL_PORT = os.environ.get("BRIDGE_SERIAL_PORT")  # e.g. /dev/tty.usbserial-...; unset = dry run


@asynccontextmanager
async def lifespan(app: FastAPI):
    state = SceneState()
    transport = SerialTransport(SERIAL_PORT) if SERIAL_PORT else None
    pipeline = Pipeline(state, transport)

    osc_transport = await start_osc_server(state, OSC_HOST, OSC_PORT, pipeline.on_state_changed)

    app.state.scene = state
    app.state.pipeline = pipeline

    yield

    osc_transport.close()
    if transport is not None:
        transport.close()


app = FastAPI(lifespan=lifespan)
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


@app.get("/")
async def index():
    return FileResponse(STATIC_DIR / "index.html")


@app.websocket("/ws")
async def ws_endpoint(websocket: WebSocket):
    await websocket.accept()
    pipeline: Pipeline = websocket.app.state.pipeline
    state: SceneState = websocket.app.state.scene

    async def send_updates():
        while True:
            hue, saturation, brightness = pipeline.current_color()
            await websocket.send_json({
                "vibrant_hsv": state.vibrant_hsv,
                "rate_bpm": state.rate_bpm,
                "preset_name": state.preset_name,
                "override_active": pipeline.override.active,
                "override_kind": pipeline.override.kind,
                "override_pattern": PATTERN_NAMES_BY_VALUE.get(pipeline.override.pattern),
                "override_speed": pipeline.override.speed,
                "override_colors": pipeline.override.colors,
                "mode": pipeline.mode,
                "hue": hue,
                "saturation": saturation,
                "brightness": brightness,
            })
            await asyncio.sleep(0.2)

    async def receive_commands():
        while True:
            msg = await websocket.receive_json()
            if msg.get("type") == "override_set":
                hue, saturation, brightness = hex_to_hue_sat_bri(msg["color"])
                pipeline.override.active = True
                pipeline.override.kind = "color"
                pipeline.override.hue = hue
                pipeline.override.saturation = saturation
                pipeline.override.brightness = brightness
            elif msg.get("type") == "override_pattern_set" and msg.get("pattern") in PATTERN_NAMES:
                pattern = PATTERN_NAMES[msg["pattern"]]
                colors = int(msg.get("colors", pipeline.override.colors))
                if colors not in VALID_COLORS_BY_PATTERN[pattern]:
                    continue  # not a real preset for this pattern -- ignore rather than send garbage
                pipeline.override.active = True
                pipeline.override.kind = "pattern"
                pipeline.override.pattern = pattern
                pipeline.override.colors = colors
                pipeline.override.speed = int(msg.get("speed", pipeline.override.speed))
                pipeline.override.brightness = int(msg.get("brightness", pipeline.override.brightness))
            elif msg.get("type") == "override_release":
                pipeline.override.active = False
            elif msg.get("type") == "mode_set" and msg.get("mode") in MODES:
                pipeline.mode = msg["mode"]
            else:
                continue
            pipeline.on_state_changed()  # don't wait for the next OSC message to react

    sender = asyncio.create_task(send_updates())
    receiver = asyncio.create_task(receive_commands())
    try:
        await asyncio.wait([sender, receiver], return_when=asyncio.FIRST_COMPLETED)
    except WebSocketDisconnect:
        pass
    finally:
        sender.cancel()
        receiver.cancel()
