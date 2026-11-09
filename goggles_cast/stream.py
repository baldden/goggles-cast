"""One stream: the main monitor at the goggles' 1920x1080 and refresh rate (the GPU capture follows the monitor's own
refresh, so it must run at exactly the goggles' rate), the capture program sending to the goggles, everything back at
stop()."""
from __future__ import annotations

from . import display, usbgoggles
from .capture import Capture
from .i18n import t


class Stream:
    def __init__(self, hz: int, log=print):
        self.hz, self.log = hz, log
        self.link = usbgoggles.LinkState()
        self.capture: Capture | None = None
        self.switched = False
        self.note = ""   # shown in the window when the monitor could not give what was chosen

    def start(self) -> None:
        mode = display.switch(1920, 1080, self.hz)
        if not mode and self.hz != 60:
            mode = display.switch(1920, 1080, 60)
            if mode:
                self.note = t(f"Монитор не умеет {self.hz} Гц в 1920×1080: идёт 60 Гц.",
                              f"The monitor cannot do {self.hz} Hz at 1920x1080: streaming at 60 Hz.")
        if mode:
            self.switched = True
            self.hz = mode[2]
            self.log(f"Monitor for the stream: {mode[0]}x{mode[1]} {mode[2]} Hz")
        else:
            w, h, hz = display.current()
            self.hz = min(self.hz, hz)
            self.note = t(f"Монитор не умеет 1920×1080: беру экран как есть ({w}×{h}, {hz} Гц).",
                          f"The monitor cannot do 1920x1080: taking the screen as it is ({w}x{h}, {hz} Hz).")
        if self.note:
            self.log(self.note)
        mbps = usbgoggles.max_bitrate(self.hz)
        self.capture = Capture(self.hz, mbps, self.link.on_line, self.log)
        try:
            self.capture.start()
        except Exception:
            self.stop()
            raise
        self.log(f"Streaming {self.hz} Hz, {mbps:g} Mbit/s, encoder {self.capture.encoder or '?'}")

    def alive(self) -> bool:
        return self.capture is not None and self.capture.alive()

    def stop(self) -> None:
        if self.capture:
            self.capture.stop()
        if self.switched:
            display.restore()
            self.switched = False
