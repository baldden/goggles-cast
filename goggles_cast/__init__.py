"""Goggles Cast: a Windows PC's screen in DJI Goggles 2 / Integra over USB."""
import os
import sys

__version__ = "1.0.0"


def app_dir() -> str:
    """Where the program lives: next to GogglesCast.exe when installed, the repository when run from source."""
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
