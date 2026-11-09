"""The main monitor at the goggles' picture size (1920x1080) and refresh rate for the stream: the encoder then takes
the screen as it is, on the GPU, with nothing to crop or scale, and every picture once.

The switch is a normal, dynamic mode change (as the Settings app makes, without saving it to the registry): Windows
keeps every window on the screen. A game's temporary mode (CDS_FULLSCREEN) would leave windows off the smaller
screen. The user's own mode stays in the registry: restore() at Stop, and at the next start after a crash
(restore_if_left())."""
from __future__ import annotations

import ctypes
import struct
import sys
from ctypes import wintypes

CDS_FULLSCREEN = 0x4
DISP_CHANGE_SUCCESSFUL = 0
DM_PELSWIDTH, DM_PELSHEIGHT, DM_DISPLAYFREQUENCY = 0x80000, 0x100000, 0x400000


class DEVMODEW(ctypes.Structure):
    _fields_ = [("dmDeviceName", wintypes.WCHAR * 32), ("dmSpecVersion", wintypes.WORD),
                ("dmDriverVersion", wintypes.WORD), ("dmSize", wintypes.WORD), ("dmDriverExtra", wintypes.WORD),
                ("dmFields", wintypes.DWORD), ("dmPositionX", ctypes.c_long), ("dmPositionY", ctypes.c_long),
                ("dmDisplayOrientation", wintypes.DWORD), ("dmDisplayFixedOutput", wintypes.DWORD),
                ("dmColor", ctypes.c_short), ("dmDuplex", ctypes.c_short), ("dmYResolution", ctypes.c_short),
                ("dmTTOption", ctypes.c_short), ("dmCollate", ctypes.c_short), ("dmFormName", wintypes.WCHAR * 32),
                ("dmLogPixels", wintypes.WORD), ("dmBitsPerPel", wintypes.DWORD), ("dmPelsWidth", wintypes.DWORD),
                ("dmPelsHeight", wintypes.DWORD), ("dmDisplayFlags", wintypes.DWORD),
                ("dmDisplayFrequency", wintypes.DWORD)]


def modes() -> list[tuple[int, int, int]]:
    """(width, height, Hz) the main monitor offers."""
    if sys.platform != "win32":
        return []
    d = DEVMODEW()
    d.dmSize = ctypes.sizeof(DEVMODEW)
    out, i = set(), 0
    while ctypes.windll.user32.EnumDisplaySettingsW(None, i, ctypes.byref(d)):
        out.add((d.dmPelsWidth, d.dmPelsHeight, d.dmDisplayFrequency))
        i += 1
    return sorted(out)


def current() -> tuple[int, int, int]:
    d = DEVMODEW()
    d.dmSize = ctypes.sizeof(DEVMODEW)
    ctypes.windll.user32.EnumDisplaySettingsW(None, -1, ctypes.byref(d))   # ENUM_CURRENT_SETTINGS
    return d.dmPelsWidth, d.dmPelsHeight, d.dmDisplayFrequency


def has_mode(w: int, h: int, hz: int) -> bool:
    return (w, h, hz) in modes()


def switch(w: int, h: int, hz: int) -> tuple[int, int, int] | None:
    """Main monitor -> exactly w x h at hz (the goggles' screens run at 60 or 100 Hz, following the video's frame rate,
    so any other rate would show unevenly). Returns the new mode, None when the monitor has no such mode."""
    if not has_mode(w, h, hz):
        return None
    if current() == (w, h, hz):
        return w, h, hz
    d = DEVMODEW()
    d.dmSize = ctypes.sizeof(DEVMODEW)
    d.dmPelsWidth, d.dmPelsHeight, d.dmDisplayFrequency = w, h, hz
    d.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY
    r = ctypes.windll.user32.ChangeDisplaySettingsExW(None, ctypes.byref(d), None, 0, None)   # dynamic, not saved
    return (w, h, hz) if r == DISP_CHANGE_SUCCESSFUL else None


def restore() -> None:
    """Back to the mode saved in the registry (the user's own)."""
    if sys.platform == "win32":
        ctypes.windll.user32.ChangeDisplaySettingsExW(None, None, None, 0, None)


def restore_if_left() -> bool:
    """The main monitor is not in the user's own (registry) mode: a stream ended without Stop (crash, killed). Put it
    back; True when it was needed."""
    if sys.platform != "win32":
        return False
    d = DEVMODEW()
    d.dmSize = ctypes.sizeof(DEVMODEW)
    if not ctypes.windll.user32.EnumDisplaySettingsW(None, -2, ctypes.byref(d)):   # ENUM_REGISTRY_SETTINGS
        return False
    if (d.dmPelsWidth, d.dmPelsHeight, d.dmDisplayFrequency) == current():
        return False
    restore()
    return True


def _mode(name: str | None) -> DEVMODEW | None:
    d = DEVMODEW()
    d.dmSize = ctypes.sizeof(DEVMODEW)
    return d if ctypes.windll.user32.EnumDisplaySettingsW(name, -1, ctypes.byref(d)) else None
