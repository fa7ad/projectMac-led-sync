"""Loads this lamp's pairing secrets from the environment (a gitignored `.env`
at the project root). Never hardcode these values in source.
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
