"""One stream: the main monitor at the goggles' 1920x1080 and refresh rate (the GPU capture follows the monitor's own
refresh, so it must run at exactly the goggles' rate), the capture program sending to the goggles, everything back at
stop().

While it runs, a watcher (once a second):
- starts the capture again when it ends by itself (exit code 3: Windows took the screen away, after a mode change, a
  full-screen game, the UAC prompt; 1.0.0 left it stopped and the goggles black);
- steps the bitrate down when frames still come out too big for the goggles (they are dropped, not sent);
- writes what goes to the goggles into the journal every 5 seconds."""
from __future__ import annotations

import threading
import time

from . import display, usbgoggles
from .capture import Capture
from .i18n import t

RESTARTS_PER_MINUTE = 5   # more than this and the stream gives up with a message
OVERSIZE_SECONDS = 3      # seconds in a row with too-big frames before the bitrate steps down
STEP_MBPS, MIN_MBPS = 4, 12
SUMMARY_EVERY = 5.0


class Stream:
    def __init__(self, hz: int, log=print):
        self.hz, self.log = hz, log
        self.link = usbgoggles.LinkState()
        self.capture: Capture | None = None
        self.switched = False
        self.note = ""     # shown in the window when the monitor could not give what was chosen
        self.failed = ""   # set when the capture could not be kept running
        self.mbps = 0.0
        self._stop = threading.Event()
        self._lock = threading.Lock()

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
            self._settle(mode)
        else:
            w, h, hz = display.current()
            self.hz = min(self.hz, hz)
            self.note = t(f"Монитор не умеет 1920×1080: беру экран как есть ({w}×{h}, {hz} Гц).",
                          f"The monitor cannot do 1920x1080: taking the screen as it is ({w}x{h}, {hz} Hz).")
        if self.note:
            self.log(self.note)
        self.mbps = usbgoggles.max_bitrate(self.hz)
        try:
            self._launch()
        except Exception:
            self.stop()
            raise
        threading.Thread(target=self._watch, daemon=True).start()

    def _settle(self, mode) -> None:
        """A monitor takes a moment to switch (an external one 1-3 s): the capture starts once Windows reports the new
        mode, then half a second more, so it does not start into the switch and lose the screen at once."""
        t0 = time.time()
        while time.time() - t0 < 4 and display.current() != tuple(mode):
            time.sleep(0.1)
        time.sleep(0.5)

    def _launch(self) -> None:
        cap = Capture(self.hz, self.mbps, self.link.on_line, self.log)
        cap.start()
        with self._lock:
            self.capture = cap
        self.log(f"Streaming {self.hz} Hz, {self.mbps:g} Mbit/s, encoder {cap.encoder or '?'}")

    def _watch(self) -> None:
        restarts: list[float] = []
        oversize_run = 0
        last_summary = time.time()
        sent = oversize = 0
        biggest = 0
        seen = 0.0   # the status line already counted
        while not self._stop.wait(1.0):
            cap = self.capture
            if cap is None:
                continue
            if not cap.alive():
                code = cap.proc.returncode if cap.proc else None
                now = time.time()
                restarts = [r for r in restarts if now - r < 60] + [now]
                if len(restarts) > RESTARTS_PER_MINUTE:
                    self.failed = t("Захват экрана останавливается снова и снова.",
                                    "The screen capture keeps stopping.")
                    self.log(f"Capture ended (code {code}) {len(restarts)} times in a minute: giving up. "
                             f"Last lines: {' | '.join(cap.tail[-3:])}")
                    return
                self.log(f"Capture ended (code {code}): starting it again. Last lines: {' | '.join(cap.tail[-3:])}")
                self.link.state = "connected"
                try:
                    self._launch()
                except Exception as e:  # noqa: BLE001
                    self.log(f"Capture restart failed: {e}")
                continue
            st, at = cap.stat, cap.stat_at
            if at == seen:
                continue   # no new second yet
            seen = at
            s_sent, s_over = int(st.get("usb_sent", 0) or 0), int(st.get("usb_oversize", 0) or 0)
            sent += s_sent
            oversize += s_over
            biggest = max(biggest, int(st.get("max_frame", 0) or 0))
            oversize_run = oversize_run + 1 if s_over else 0
            if oversize_run >= OVERSIZE_SECONDS and self.mbps - STEP_MBPS >= MIN_MBPS:
                self.mbps -= STEP_MBPS
                oversize_run = 0
                self.log(f"Frames too big for the goggles for {OVERSIZE_SECONDS} s: bitrate down to {self.mbps:g} Mbit/s")
                cap.stop()
                try:
                    self._launch()
                except Exception as e:  # noqa: BLE001
                    self.log(f"Capture restart failed: {e}")
                continue
            now = time.time()
            if now - last_summary >= SUMMARY_EVERY:
                dt = now - last_summary
                self.log(f"Last {dt:.0f} s: link {self.link.state}, frames to the goggles {sent / dt:.1f}/s, "
                         f"too big {oversize}, biggest {biggest / 1024:.1f} KB, encoded {st.get('fps', '?')} fps "
                         f"{st.get('mbps', '?')} Mbit/s, encoder {cap.encoder or '?'}")
                last_summary, sent, oversize, biggest = now, 0, 0, 0

    def sending(self) -> bool:
        """Frames reach the goggles now (the last second's count, not only the link's state)."""
        cap = self.capture
        return bool(cap and cap.alive() and cap.stat_at > time.time() - 2.5
                    and int(cap.stat.get("usb_sent", 0) or 0) > 0)

    def dropping(self) -> bool:
        """Frames are made but dropped as too big for the goggles."""
        cap = self.capture
        return bool(cap and cap.stat_at > time.time() - 2.5 and int(cap.stat.get("usb_oversize", 0) or 0) > 0)

    def alive(self) -> bool:
        return not self.failed and self.capture is not None

    def stop(self) -> None:
        self._stop.set()
        if self.capture:
            self.capture.stop()
        if self.switched:
            display.restore()
            self.switched = False
