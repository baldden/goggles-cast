"""The goggles on this PC's USB port (native/goggles_usb.h sends the video; the route is G2 Bridge's,
github.com/Ammaar-Alam/g2-bridge): whether they are plugged in, whether DJI's driver is there, the bitrate they take,
and the link's state from the capture program's "usb:" lines."""
from __future__ import annotations

import ctypes
import os

VID_PID = "USB\\VID_2CA3&PID_0020"   # the goggles as a USB device (USB-A to USB-C cable)
# The goggles show frames up to 49 KB; from about 65 KB they froze or stayed black until a power cycle (G2 Bridge
# docs/PROTOCOL.md). Every frame is at most bitrate / fps (a VBV of one frame), so the rate is what keeps it under:
# G2 Bridge's Windows rates (24 Mbps at 60 fps = 50 KB a frame, 32 at 100 = 40 KB).
MAX_MBPS = {60: 24, 75: 30, 90: 32, 100: 32}


def present() -> bool:
    """The goggles are on USB now (cfgmgr32: present devices of the USB enumerator)."""
    try:
        cfg = ctypes.WinDLL("cfgmgr32")
    except OSError:
        return False
    flags = 0x1 | 0x100   # CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT
    size = ctypes.c_ulong(0)
    if cfg.CM_Get_Device_ID_List_SizeW(ctypes.byref(size), "USB", flags) != 0 or not size.value:
        return False
    buf = ctypes.create_unicode_buffer(size.value)
    if cfg.CM_Get_Device_ID_ListW("USB", buf, size, flags) != 0:
        return False
    return any(i.upper().startswith(VID_PID) for i in buf[:size.value].split("\0"))


def driver_ok() -> bool:
    """DJI's USB driver for the goggles (DJI Assistant 2 installs it; goggles_usb.h loads this DLL)."""
    root = os.environ.get("SystemRoot", r"C:\Windows")
    return any(os.path.exists(os.path.join(root, d, "libusb0_device.dll")) for d in ("System32", "Sysnative"))


def max_bitrate(fps: int) -> float:
    return float(MAX_MBPS.get(int(fps), 24))


class LinkState:
    """waiting -> connected -> ready, from the capture program's "usb:" lines (goggles_usb.h)."""

    def __init__(self) -> None:
        self.state = "waiting"
        self.device = ""

    def on_line(self, line: str) -> None:
        if not line.startswith("usb:"):
            return
        if "goggles on USB" in line or "handshake again" in line:
            self.state = "connected"
        elif "ignore simulator mode" in line:
            self.state = "need_vf"   # the goggles answer, but More > DJI Virtual Flight is not open on them
        elif "driver not found" in line:
            self.state = "no_driver"
        elif "waiting for the goggles" in line:
            self.state = "waiting"
        elif "ready, video" in line:
            self.state = "ready"
        elif line.startswith("usb: goggles '"):
            self.device = line.split("'")[1]
