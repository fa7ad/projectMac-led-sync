"""Persistent, monotonically-increasing counters for lamp commands.

The lamp enforces strict sn monotonicity per source address — it silently
drops any command whose sn isn't higher than the last one it accepted. sn
must therefore keep increasing across the whole pairing lifetime, not just
within one process run. (Putting the lamp back into pairing mode resets its
side of this — see PLAN.md's "Continuity with the old pipeline" — so this
project's counters are safe to start fresh at 1 as long as that's done
first.)

The scene_data byte[0] "revision" field is suspected to need the same
treatment: every ad-hoc test script that restarted its own in-process
`revision = 1` counter on each run was sending repeated/non-monotonic
revision values across separate invocations, which coincided with the lamp
falling into an unresponsive autonomous-cycling state that only cleared with
a physical power cycle. RevisionCounter below persists it the same way
SnCounter already persists sn, so it never repeats across process runs
either.

Copied unmodified from the original research repo — already generic, no
secrets, no changes needed.
"""

from __future__ import annotations

import json
from pathlib import Path

DEFAULT_STATE_FILE = Path(__file__).parent / ".lamp_sn_state.json"
DEFAULT_REVISION_STATE_FILE = Path(__file__).parent / ".lamp_revision_state.json"


class _PersistentCounter:
    """Shared implementation: wraps at `modulus`, skipping 0 (reserved/
    ambiguous in both the sn and revision fields as far as we know)."""

    def __init__(self, path: Path, key: str, modulus: int):
        self.path = path
        self.key = key
        self.modulus = modulus
        self._value = self._load()

    def _load(self) -> int:
        try:
            return int(json.loads(self.path.read_text())[self.key])
        except Exception:
            return 0

    def next(self) -> int:
        self._value = (self._value + 1) % self.modulus
        if self._value == 0:
            self._value = 1
        self.path.write_text(json.dumps({self.key: self._value}))
        return self._value


class SnCounter(_PersistentCounter):
    """sn is a 2-byte field (header8 bytes 4-5) -> wraps mod 0x10000."""

    def __init__(self, path: Path = DEFAULT_STATE_FILE):
        super().__init__(path, key="sn", modulus=0x10000)


class RevisionCounter(_PersistentCounter):
    """scene_data byte0 "revision" is a 1-byte field -> wraps mod 256."""

    def __init__(self, path: Path = DEFAULT_REVISION_STATE_FILE):
        super().__init__(path, key="revision", modulus=256)
