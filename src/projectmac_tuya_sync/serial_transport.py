"""Writes frames to the ESP32 over the existing serial wire contract: one
line per frame, 52 ASCII hex chars + '\\n'. See esp32-bridge/main/main.c."""

import serial


class SerialTransport:
    def __init__(self, port: str, baudrate: int = 115200):
        self._conn = serial.Serial(port, baudrate)

    def send_frame(self, frame: bytes) -> None:
        assert len(frame) == 26, f"expected a 26-byte frame, got {len(frame)}"
        self._conn.write(frame.hex().encode("ascii") + b"\n")

    def close(self) -> None:
        self._conn.close()
