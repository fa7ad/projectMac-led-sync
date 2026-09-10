"""Pattern to follow for this project's real config module — adapted from
projectmac-sunset-sync/src/projectmac_sunset_sync/config.py (already public/
shareable, same secrets-handling requirement as this project). Loads this
specific lamp's pairing secrets from the environment (a gitignored .env at
the project root, or real env vars) rather than hardcoding them in source.

The `Path(...).resolve().parent.parent.parent` walk below is copied as-is
from the original, which assumed a specific src-layout depth
(src/<pkg>/config.py -> project root is 3 parents up) — adjust the number of
`.parent`s to match wherever this project's real config module ends up
living once the package layout is decided.
"""

import os
from pathlib import Path

from dotenv import load_dotenv

load_dotenv(Path(__file__).resolve().parent.parent.parent / ".env")


def _require(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(
            f"{name} is not set -- copy .env.example to .env and fill in "
            "your lamp's pairing keys (see README.md)"
        )
    return value


LOCAL_KEY = _require("LAMP_LOCAL_KEY").encode()
APP_KEY = bytes.fromhex(_require("LAMP_APP_KEY"))
SRC_ADDR = bytes.fromhex(_require("LAMP_SRC_ADDR"))
DST_ADDR = bytes.fromhex(_require("LAMP_DST_ADDR"))
