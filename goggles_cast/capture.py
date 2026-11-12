"""The capture program (native/gcast_capture.cpp): Desktop Duplication, the cursor and BGRA -> NV12 on the GPU,
straight into the hardware H.264 encoder, the frames to the goggles on USB. This starts it and reads its status
lines: "stat ..." once a second, "usb: ..." from the link to the goggles."""
from __future__ import annotations

import os
import subprocess
import threading
import time

from . import app_dir

CREATE_NO_WINDOW = 0x08000000


def exe_path() -> str | None:
    # from source: native\dist (the build, with its DLLs); installed: native\ next to GogglesCast.exe
    for cand in (os.path.join(app_dir(), "native", "dist", "gcast_capture.exe"),
                 os.path.join(app_dir(), "native", "gcast_capture.exe")):
        if os.path.isfile(cand):
            return cand
    return None


class Capture:
    def __init__(self, fps: int, mbps: float, on_line=None, log=print):
        self.fps, self.mbps = fps, mbps
        self.on_line = on_line or (lambda line: None)
        self.log = log
        self.proc: subprocess.Popen | None = None
        self.stat: dict[str, str] = {}   # the last "stat" line: fps, mbps, presents, usb_sent, usb_oversize, max_frame...
        self.stat_at = 0.0               # when it came
        self.tail: list[str] = []
        self.encoder = ""

    def start(self, wait: float = 1.5) -> None:
        exe = exe_path()
        if not exe:
            raise RuntimeError("the capture program is missing (native\\gcast_capture.exe)")
        # a VBV of one frame (every frame at most bitrate / fps: what keeps frames within what the goggles take),
        # the cursor in the picture, a key frame a second (the program adds intra refresh and SPS/PPS)
        args = [exe, "--width", "1920", "--height", "1080", "--fps", str(self.fps), "--bitrate", f"{self.mbps:g}",
                "--vbv-frames", "1", "--gop", str(self.fps), "--cursor", "1", "--quality", "speed",
                "--encoder", "auto"]
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                     bufsize=0, cwd=os.path.dirname(exe), creationflags=CREATE_NO_WINDOW)
        threading.Thread(target=self._read_err, daemon=True).start()
        time.sleep(wait)
        if not self.alive():
            raise RuntimeError("the capture did not start: " + " | ".join(self.tail[-3:]))

    def _read_err(self) -> None:
        for raw in iter(self.proc.stderr.readline, b""):
            line = raw.decode(errors="replace").strip()
            if line.startswith("stat "):
                self.stat = dict(kv.partition("=")[::2] for kv in line[5:].split())
                self.stat_at = time.time()
            elif line:
                if line.startswith("encoder ") and " did not open" not in line:
                    self.encoder = line.split()[1]
                self.tail = (self.tail + [line])[-20:]
                self.log("capture: " + line)
                self.on_line(line)

    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.stdin.close()   # the program exits when its input is closed
                self.proc.wait(2)
            except (OSError, subprocess.TimeoutExpired):
                self.proc.kill()
