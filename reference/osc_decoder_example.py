"""Minimal OSC 1.0 message decoder, copied from the old research repo's throwaway
lamp_sync_plugin.py — hand-rolling this is genuinely small; a real dependency
(e.g. python-osc) is also a perfectly fine, low-risk choice instead. Only
handles the `f` (float32) and would need extending for `s` (string) and bang
(no-arg) messages, both of which this project's wire format actually uses
(see osc-wire-format.md) — this is a starting point, not complete.
"""

import struct


def parse_osc_message(data: bytes) -> tuple[str, list[float]]:
    """Decode one OSC 1.0 message: an OSC-string address, an OSC-string type
    tag (`,` followed by one `f` per arg), then each arg as a big-endian
    float32 — every string padded with nulls to a 4-byte boundary."""

    def read_osc_string(buf: bytes, offset: int) -> tuple[str, int]:
        end = buf.index(b"\x00", offset)
        s = buf[offset:end].decode("utf-8")
        next_offset = (end + 4) & ~3  # advance past the null(s) to the next 4-byte boundary
        return s, next_offset

    address, offset = read_osc_string(data, 0)
    type_tag, offset = read_osc_string(data, offset)
    if not type_tag.startswith(","):
        raise ValueError(f"malformed OSC type tag: {type_tag!r}")

    args = []
    for tag in type_tag[1:]:
        if tag != "f":
            raise ValueError(f"unsupported OSC arg type {tag!r} in {type_tag!r}")
        (value,) = struct.unpack(">f", data[offset:offset + 4])
        args.append(value)
        offset += 4

    return address, args
