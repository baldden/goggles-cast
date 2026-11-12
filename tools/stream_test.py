"""The stream as the window starts it, on this PC, with or without the goggles on USB. Checks what a user would notice:
the main monitor at 1920x1080 and the goggles' rate during the stream, no window left off the smaller screen, the
encoder giving every new picture (a test window presents at the rate), and the monitor back as it was after Stop.
Switches the PC's monitor for a while.

    python tools/stream_test.py          # 100 Hz (when the monitor has it) and 60 Hz
    python tools/stream_test.py 60       # one rate only
"""
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
sys.stdout.reconfigure(encoding="utf-8")
from goggles_cast import display  # noqa: E402
from goggles_cast.stream import Stream  # noqa: E402

FAILS: list[str] = []
PRESENT = os.path.join(ROOT, "native", "present_test.exe")   # a window that presents every vsync, like a game


def check(ok: bool, what: str) -> None:
    print(("OK   " if ok else "FAIL ") + what, flush=True)
    if not ok:
        FAILS.append(what)


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long), ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


def windows_off_screen(w: int, h: int) -> list:
    """Visible top-level windows (not tool windows, not minimized) whose centre is off a w x h screen."""
    u = ctypes.windll.user32
    out = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def each(hwnd, _):
        if u.IsWindowVisible(hwnd) and not u.IsIconic(hwnd) and not u.GetWindow(hwnd, 4) \
                and not u.GetWindowLongW(hwnd, -20) & 0x80:
            cloaked = wintypes.DWORD()
            ctypes.windll.dwmapi.DwmGetWindowAttribute(hwnd, 14, ctypes.byref(cloaked), 4)
            r = RECT()
            u.GetWindowRect(hwnd, ctypes.byref(r))
            cx, cy = (r.left + r.right) // 2, (r.top + r.bottom) // 2
            if not cloaked.value and not (0 <= cx < w and 0 <= cy < h):
                out.append((r.left, r.top, r.right, r.bottom))
        return True
    u.EnumWindows(each, 0)
    return out


def run(hz: int, seconds: float = 6.0) -> None:
    print(f"=== monitor 1920x1080 {hz} Hz for the stream", flush=True)
    mode0 = display.current()
    lines: list[str] = []
    s = Stream(hz, lines.append)
    game = None
    try:
        s.start()
        check(display.current() == (1920, 1080, hz), f"monitor during the stream 1920x1080 {hz} Hz: {display.current()}")
        time.sleep(1)
        check(not windows_off_screen(1920, 1080), f"no window off the 1920x1080 screen: {windows_off_screen(1920, 1080)}")
        game = subprocess.Popen([PRESENT, str(seconds + 3), str(hz)], stdout=subprocess.DEVNULL)
        time.sleep(seconds)
        st = s.capture.stat
        new, fps = float(st.get("presents", 0)), float(st.get("fps", 0))
        check(new >= hz * 0.95, f"new pictures from Windows {new:.0f}/s with a game at {hz}/s")
        check(fps >= hz * 0.95, f"frames encoded {fps:.1f}/s")
        check(s.alive() and s.capture.alive(), f"capture running, link to the goggles: {s.link.state}")
    finally:
        if game and game.poll() is None:
            game.kill()
        s.stop()
    time.sleep(2)
    check(display.current() == mode0, f"after Stop the monitor as it was: {display.current()} (was {mode0})")


def main() -> None:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)   # as the window: monitors and windows in physical pixels
    ctypes.windll.kernel32.SetThreadExecutionState(0x80000000 | 0x00000002)   # no screen saver between runs
    for hz in [int(a) for a in sys.argv[1:] if a.isdigit()] or [100, 60]:
        if display.has_mode(1920, 1080, hz):
            run(hz)
        else:
            print(f"=== {hz} Hz: the monitor has no 1920x1080 at {hz} Hz, skipped (greyed out in the window)")
    print("RESULT: " + ("all passed" if not FAILS else f"{len(FAILS)} FAILED: " + "; ".join(FAILS)))
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
