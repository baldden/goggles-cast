"""Goggles Cast's window: whether the goggles are on USB, the refresh rate, Start / Stop, and what the stream does,
in words. For the stream the main monitor goes to the goggles' 1920x1080 at their rate and comes back at Stop.
The journal: stats\\journal.txt next to the program.
"""
from __future__ import annotations

import ctypes
import json
import os
import queue
import threading
import time
import tkinter as tk
from tkinter import font as tkfont

from . import app_dir, display, i18n, usbgoggles
from .capture import exe_path
from .i18n import t
from .stream import Stream

i18n.load()

STATE_FILE = os.path.join(app_dir(), "settings.json")

HZ = (60, 100)            # the goggles' 60 or 100 Hz
from .usbgoggles import MAX_MBPS   # the bitrate: always the most the goggles take over USB at the rate
WIDTH = 340               # the window, in screen points at 100 % scaling

# colours: one dark theme, one accent
BG, CARD, CHIP, LINE = "#0f1115", "#181b21", "#22262e", "#2c313b"
TEXT, MUTED, DIM = "#eef0f3", "#8b93a1", "#5d6470"
ACCENT, ACCENT_HI = "#3b82f6", "#5b9bff"
GREEN, AMBER, RED = "#22c55e", "#f59e0b", "#ef4444"
FONT = "Segoe UI"


def _round_rect(c: tk.Canvas, x0, y0, x1, y1, r, **kw):
    pts = [x0 + r, y0, x1 - r, y0, x1, y0, x1, y0 + r, x1, y1 - r, x1, y1, x1 - r, y1, x0 + r, y1, x0, y1, x0, y1 - r,
           x0, y0 + r, x0, y0]
    return c.create_polygon(pts, smooth=True, **kw)


class Segmented(tk.Frame):
    """A row of choices, the chosen one filled with the accent colour."""

    def __init__(self, parent, app: "SimpleApp", options: list[tuple[object, str]], var: tk.Variable, command=None):
        super().__init__(parent, bg=CHIP, padx=app.px(3), pady=app.px(3))
        self.var, self.command, self.enabled = var, command, True
        self.off: set = set()   # values this PC cannot give
        self.items: list[tuple[object, tk.Label]] = []
        for i, (value, label) in enumerate(options):
            lb = tk.Label(self, text=label, bg=CHIP, fg=MUTED, font=(FONT, app.f(10)), pady=app.px(5), cursor="hand2")
            lb.grid(row=0, column=i, sticky="nsew")
            self.columnconfigure(i, weight=1, uniform="seg")
            lb.bind("<Button-1>", lambda e, v=value: self._pick(v))
            lb.bind("<Enter>", lambda e, w=lb, v=value: self.enabled and v not in self.off and self.var.get() != v
                    and w.configure(fg=TEXT))
            lb.bind("<Leave>", lambda e: self.refresh())
            self.items.append((value, lb))
        self.refresh()

    def _pick(self, value) -> None:
        if not self.enabled or self.var.get() == value or value in self.off:
            return
        self.var.set(value)
        self.refresh()
        if self.command:
            self.command()

    def relabel(self, labels: list[str]) -> None:
        for (_, lb), text in zip(self.items, labels):
            lb.configure(text=text)

    def set_enabled(self, on: bool) -> None:
        self.enabled = on
        self.refresh()

    def refresh(self) -> None:
        for value, lb in self.items:
            sel = self.var.get() == value
            can = self.enabled and value not in self.off
            lb.configure(bg=ACCENT if sel and self.enabled else LINE if sel else CHIP,
                         fg="white" if sel and self.enabled else TEXT if sel else (MUTED if can else DIM),
                         cursor="hand2" if can and not sel else "")


class BigButton(tk.Canvas):
    """The Start / Stop button: rounded, filled, with hover."""

    def __init__(self, parent, app: "SimpleApp", command):
        self.h = app.px(50)
        super().__init__(parent, height=self.h, bg=BG, highlightthickness=0, bd=0, cursor="hand2")
        self.app, self.command = app, command
        self.text, self.kind, self.hover = "", "go", False   # kind: go, stop, off
        self.bind("<Configure>", lambda e: self.draw())
        self.bind("<Enter>", lambda e: self._hover(True))
        self.bind("<Leave>", lambda e: self._hover(False))
        self.bind("<Button-1>", lambda e: self.kind != "off" and self.command())

    def _hover(self, on: bool) -> None:
        self.hover = on
        self.draw()

    def set(self, text: str, kind: str) -> None:
        self.text, self.kind = text, kind
        self.configure(cursor="hand2" if kind != "off" else "")
        self.draw()

    def draw(self) -> None:
        self.delete("all")
        w = self.winfo_width()
        if w < 10:
            return
        fill = {"go": ACCENT_HI if self.hover else ACCENT, "stop": "#f26464" if self.hover else RED,
                "off": CHIP}[self.kind]
        _round_rect(self, 1, 1, w - 1, self.h - 1, self.app.px(12), fill=fill, outline=fill)
        self.create_text(w / 2, self.h / 2, text=self.text, fill="white" if self.kind != "off" else DIM,
                         font=(FONT, self.app.f(13), "bold"))


def _monitor_dpi(x: int, y: int) -> float | None:
    """The scale of the monitor at a point, as DPI (96 = 100 %)."""
    try:
        from ctypes import wintypes
        mon = ctypes.windll.user32.MonitorFromPoint(wintypes.POINT(x, y), 2)   # MONITOR_DEFAULTTONEAREST
        dx, dy = ctypes.c_uint(), ctypes.c_uint()
        if ctypes.windll.shcore.GetDpiForMonitor(mon, 0, ctypes.byref(dx), ctypes.byref(dy)) == 0:
            return float(dx.value)
    except (AttributeError, OSError):
        pass
    return None


def _dpi_aware() -> None:
    """Sharp, correctly sized on high-DPI screens (e.g. 2880x1800 at 200 %); before Tk starts."""
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)   # per monitor
    except (AttributeError, OSError):
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except (AttributeError, OSError):
            pass


class Window(tk.Tk):
    def __init__(self) -> None:
        _dpi_aware()
        super().__init__()
        self.title("Goggles Cast")
        self.configure(bg=BG)
        self.dpi = self.winfo_fpixels("1i")
        self.tk.call("tk", "scaling", self.dpi / 72.0)
        for name in ("TkDefaultFont", "TkTextFont", "TkMenuFont", "TkHeadingFont"):
            try:
                tkfont.nametofont(name).configure(family=FONT, size=10)
            except tk.TclError:
                pass
        self.session: Stream | None = None
        self.ready = False   # the goggles are on USB with DJI's driver
        self.capture_ok = exe_path() is not None
        self.uiq: queue.Queue = queue.Queue()
        self._closing = False
        os.makedirs(os.path.join(app_dir(), "stats"), exist_ok=True)
        self.log_path = os.path.join(app_dir(), "stats", "journal.txt")
        if display.restore_if_left():   # the last stream ended without Stop: the monitor back in the user's mode
            self.log("The monitor was left in the stream's mode (no Stop last time): put back")
        self._build()
        self.update_idletasks()
        sw, sh = self.winfo_screenwidth(), self.winfo_screenheight()
        self.geometry(f"+{max(0, sw - self.winfo_reqwidth() - self.px(32))}+{max(0, sh // 6)}")
        self.resizable(False, False)
        self._dark_title_bar()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(100, self._pump)
        if not self.capture_ok:
            self._show(t("Нет программы захвата экрана", "The screen capture program is missing"),
                       t("Переустанови Goggles Cast.", "Reinstall Goggles Cast."), RED)
        threading.Thread(target=self._detect_loop, daemon=True).start()
        threading.Thread(target=self._hotkey_loop, daemon=True).start()

    def px(self, n: float) -> int:
        return int(round(n * self.dpi / 96))

    def f(self, size: float) -> int:
        """Font size in points (Tk scales points by the screen's DPI itself)."""
        return int(size)

    # ------------------------------------------------------------------ UI
    def _build(self) -> None:
        saved = {}
        try:
            with open(STATE_FILE, encoding="utf-8") as fh:
                saved = json.load(fh)
        except (OSError, ValueError):
            pass
        self.saved = saved
        pad = self.px(16)
        root = tk.Frame(self, bg=BG, width=self.px(WIDTH), padx=pad, pady=self.px(14))
        root.pack(fill="both", expand=True)
        root.columnconfigure(0, weight=1, minsize=self.px(WIDTH) - 2 * pad)
        self.root_frame = root

        # header: name, language
        head = tk.Frame(root, bg=BG)
        head.grid(row=0, column=0, sticky="ew")
        tk.Label(head, text="Goggles Cast", bg=BG, fg=TEXT, font=(FONT, 15, "bold")).pack(side="left")
        self.btn_lang = self._link(head, {"ru": "RU", "en": "EN"}.get(i18n.lang(), "EN"), self._lang_changed, size=10)
        self.btn_lang.pack(side="right")

        # status card: a coloured dot, what is going on, what to do
        card = tk.Frame(root, bg=CARD, padx=self.px(14), pady=self.px(12))
        card.grid(row=1, column=0, sticky="ew", pady=(self.px(14), self.px(14)))
        card.columnconfigure(1, weight=1)
        d = self.px(10)
        self.dot = tk.Canvas(card, width=d, height=d, bg=CARD, highlightthickness=0, bd=0)
        self.dot.grid(row=0, column=0, sticky="w", padx=(0, self.px(10)))
        self.dot_id = self.dot.create_oval(0, 0, d - 1, d - 1, fill=MUTED, outline="")
        wrap = self.px(WIDTH) - 2 * pad - 2 * self.px(14) - d - self.px(10)
        self.lbl_title = tk.Label(card, text=t("Ищу очки…", "Looking for the goggles…"), bg=CARD, fg=TEXT,
                                  font=(FONT, 12, "bold"), anchor="w", justify="left", wraplength=wrap)
        self.lbl_title.grid(row=0, column=1, sticky="w")
        self.lbl_sub = tk.Label(card, text="", bg=CARD, fg=MUTED, font=(FONT, 10), anchor="w", justify="left",
                                wraplength=wrap)
        self.lbl_sub.grid(row=1, column=1, sticky="w", pady=(self.px(3), 0))

        wrap_all = self.px(WIDTH) - 2 * pad
        row = 2
        self.v_hz = tk.IntVar(value=saved.get("hz") if saved.get("hz") in HZ else 60)
        self._caption(root, t("Частота очков", "Goggles' refresh rate"), row)
        self.seg_hz = Segmented(root, self, [(v, f"{v} {t('Гц', 'Hz')}") for v in HZ], self.v_hz, self._hz_changed)
        self.seg_hz.grid(row=row + 1, column=0, sticky="ew")
        self.lbl_hz = tk.Label(root, bg=BG, fg=AMBER, font=(FONT, 9), anchor="w", justify="left", wraplength=wrap_all)
        self.lbl_hz.grid(row=row + 2, column=0, sticky="w", pady=(self.px(4), 0))
        # the main monitor itself goes to 1920x1080 at the rate: only rates it has
        self.seg_hz.off = {hz for hz in HZ if not display.has_mode(1920, 1080, hz)}
        if self.seg_hz.off:
            self.lbl_hz.configure(text=t("Монитор не умеет 1920×1080 при ", "The monitor cannot do 1920x1080 at ")
                                  + ", ".join(f"{hz} {t('Гц', 'Hz')}" for hz in sorted(self.seg_hz.off)))
            if self.v_hz.get() in self.seg_hz.off:
                self.v_hz.set(next((hz for hz in HZ if hz not in self.seg_hz.off), 60))
            self.seg_hz.refresh()
        if not self.lbl_hz.cget("text"):
            self.lbl_hz.grid_remove()
        row += 3
        self._caption(root, t("Монитор ПК", "PC monitor"), row)
        tk.Label(root, text=t("На время стрима переключается на 1920×1080 и частоту очков, после «Стоп» возвращается.",
                              "Switches to 1920x1080 at the goggles' rate for the stream, back at Stop."),
                 bg=BG, fg=MUTED, font=(FONT, 9), anchor="w", justify="left", wraplength=wrap_all
                 ).grid(row=row + 1, column=0, sticky="w")
        row += 2
        self.lbl_bitrate = tk.Label(root, bg=BG, fg=MUTED, font=(FONT, 9), anchor="w", justify="left",
                                    wraplength=wrap_all)
        self.lbl_bitrate.grid(row=row, column=0, sticky="w", pady=(self.px(10), 0))
        self._relabel_bitrate()
        row += 1
        self.btn = BigButton(root, self, self._toggle)
        self.btn.grid(row=row, column=0, sticky="ew", pady=(self.px(16), 0))
        self.btn.set(t("Старт", "Start"), "off")
        tk.Label(root, text=t("Ctrl+Alt+End — стоп из любой программы", "Ctrl+Alt+End stops it from any program"),
                 bg=BG, fg=DIM, font=(FONT, 9)).grid(row=row + 1, column=0, sticky="w", pady=(self.px(8), 0))

    def _bitrate(self) -> int:
        return int(MAX_MBPS.get(int(self.v_hz.get()), MAX_MBPS[60]))

    def _relabel_bitrate(self) -> None:
        self.lbl_bitrate.configure(text=t(f"Битрейт {self._bitrate()} Мбит/с — максимум, который очки берут по USB",
                                          f"Bitrate {self._bitrate()} Mbit/s, the most the goggles take over USB"))

    def _hz_changed(self) -> None:
        self._relabel_bitrate()
        self._save()

    def _caption(self, parent, text: str, row: int) -> None:
        tk.Label(parent, text=text, bg=BG, fg=MUTED, font=(FONT, 9, "bold"), anchor="w"
                 ).grid(row=row, column=0, sticky="w", pady=(self.px(10), self.px(4)))

    def _link(self, parent, text: str, command, size: int = 10) -> tk.Label:
        lb = tk.Label(parent, text=text, bg=BG, fg=MUTED, font=(FONT, size), cursor="hand2")
        lb.bind("<Button-1>", lambda e: command())
        lb.bind("<Enter>", lambda e: lb.configure(fg=TEXT))
        lb.bind("<Leave>", lambda e: lb.configure(fg=MUTED))
        return lb

    def _dark_title_bar(self) -> None:
        """Windows 10/11: the title bar dark too, in the window's own colour."""
        try:
            hwnd = ctypes.windll.user32.GetParent(self.winfo_id())
            on = ctypes.c_int(1)
            ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 20, ctypes.byref(on), 4)   # DWMWA_USE_IMMERSIVE_DARK_MODE
            r, g, b = int(BG[1:3], 16), int(BG[3:5], 16), int(BG[5:7], 16)
            col = ctypes.c_int(r | g << 8 | b << 16)
            ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 35, ctypes.byref(col), 4)  # DWMWA_CAPTION_COLOR (11)
        except (AttributeError, OSError):
            pass

    def _place(self, x: int, y: int, width: int | None) -> None:
        """Put the window at a point of a monitor (width given: top right of a monitor that wide), sized for that
        monitor's own scale: Tk sizes everything once, for the monitor it started on (a 200 % laptop window came out
        twice too big on the goggles' 100 % monitor; 2026-11-09)."""
        dpi = _monitor_dpi(x + 1, y + 1)
        if dpi and abs(dpi - self.dpi) > 1:
            self.dpi = dpi
            self.tk.call("tk", "scaling", dpi / 72.0)
            self.root_frame.destroy()
            self._build()
            self._sync_button()
            if getattr(self, "_status", None):
                self._show(*self._status)
            self.update_idletasks()
        if width is not None:
            x = x + width - self.winfo_reqwidth() - self.px(24)
            y = y + self.px(24)
        self.geometry(f"+{x}+{y}")

    # ------------------------------------------------------------------ helpers
    def log(self, msg: str) -> None:
        line = time.strftime("%Y-%m-%d %H:%M:%S ") + msg
        try:
            with open(self.log_path, "a", encoding="utf-8") as fh:
                fh.write(line + "\n")
        except OSError:
            pass

    def ui(self, fn) -> None:
        self.uiq.put(fn)

    def _pump(self) -> None:
        try:
            while True:
                self.uiq.get_nowait()()
        except queue.Empty:
            pass
        self.after(150, self._pump)

    def _show(self, title: str, sub: str, color: str) -> None:
        """The status card (from any thread)."""
        def apply() -> None:
            self._status = (title, sub, color)
            self.lbl_title.configure(text=title)
            self.lbl_sub.configure(text=sub)
            self.lbl_sub.grid() if sub else self.lbl_sub.grid_remove()
            self.dot.itemconfigure(self.dot_id, fill=color)
        self.ui(apply)

    def _set_ready(self, ready: bool, title: str, sub: str, color: str) -> None:
        self.ready = ready
        if self.session is None:
            self._show(title, sub, color)
        self.ui(self._sync_button)

    def _sync_button(self) -> None:
        if self.session:
            self.btn.set(t("Стоп", "Stop"), "stop")
        else:
            self.btn.set(t("Старт", "Start"), "go" if self.ready and self.capture_ok else "off")
        for seg in (self.seg_hz,):
            seg.set_enabled(self.session is None)

    def _save(self) -> None:
        self.saved.update({"hz": int(self.v_hz.get()), "lang": i18n.lang()})
        try:
            with open(STATE_FILE, "w", encoding="utf-8") as fh:
                json.dump(self.saved, fh)
        except OSError:
            pass

    def _hotkey_loop(self) -> None:
        """Ctrl+Alt+End stops the stream from any program (a fullscreen game hides this window)."""
        from ctypes import wintypes
        user32 = ctypes.windll.user32
        if not user32.RegisterHotKey(None, 1, 0x0001 | 0x0002 | 0x4000, 0x23):   # Ctrl+Alt+End, no repeat
            self.log("Ctrl+Alt+End is taken by another program")
        msg = wintypes.MSG()
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            if msg.message == 0x0312 and msg.wParam == 1 and self.session:   # WM_HOTKEY
                self.log("Ctrl+Alt+End: stop")
                self.ui(self._toggle)

    # ------------------------------------------------------------------ background: the goggles
    def _detect_loop(self) -> None:
        while not self._closing:
            if self.session is None:
                if not usbgoggles.present():
                    self._set_ready(False, t("Очки не подключены", "Goggles not connected"),
                                    t("Подключи очки к ПК кабелем USB-A → USB-C и включи их.",
                                      "Plug the goggles into the PC with a USB-A to USB-C cable and turn them on."),
                                    MUTED)
                elif not usbgoggles.driver_ok():
                    self._set_ready(False, t("Нужен драйвер DJI", "DJI's USB driver is needed"),
                                    t("Очки подключены, но драйвера нет. Установи DJI Assistant 2 (Consumer Drones "
                                      "Series): он ставит драйвер.",
                                      "The goggles are plugged in, but the driver is missing. Install DJI Assistant 2 "
                                      "(Consumer Drones Series): it installs the driver."), RED)
                else:
                    self._set_ready(True, t("Очки подключены", "Goggles connected"),
                                    t("На очках открой More → DJI Virtual Flight и нажми «Старт».",
                                      "On the goggles open More > DJI Virtual Flight, then press Start."), GREEN)
            time.sleep(2)

    # ------------------------------------------------------------------ actions
    def _toggle(self) -> None:
        if self.session:
            sess, self.session = self.session, None
            if getattr(self, "_home", None):
                self.after(2500, lambda h=self._home: self.session is None and self._place(*h))
            threading.Thread(target=sess.stop, daemon=True).start()
            self.log("Stopped")
            self._sync_button()
            self._show(t("Остановлено", "Stopped"), "", MUTED)
            return
        if not self.capture_ok or not self.ready:
            return
        self._save()
        self._home = (self.winfo_x(), self.winfo_y(), None)
        sess = Stream(int(self.v_hz.get()), self.log)
        self.session = sess
        self._sync_button()
        self._show(t("Запускаюсь…", "Starting…"), "", AMBER)
        self.log(f"Start: {sess.hz} Hz")
        threading.Thread(target=self._monitor, args=(sess,), daemon=True).start()

        def work() -> None:
            try:
                sess.start()
            except Exception as e:  # noqa: BLE001
                self.log(f"Start failed: {e}")
                sess.stop()
                if self.session is sess:
                    self.session = None
                    self.ui(self._sync_button)
                    self._show(t("Не удалось запустить", "Could not start"), str(e), RED)
        threading.Thread(target=work, daemon=True).start()

    def _monitor(self, sess: Stream) -> None:
        """Twice a second: what the stream is doing, in words (the goggles answer nothing but their link state)."""
        started = time.time()
        moved = False
        while self.session is sess and not self._closing:
            try:
                if not moved and sess.switched:
                    # the monitor is 1920x1080 now; Windows kept the window on the screen, this puts it in the same
                    # corner of the new size (back where it was at Stop)
                    m = display._mode(None)
                    if m is not None:
                        moved = True
                        self.ui(lambda m=m: self._place(m.dmPositionX, m.dmPositionY, m.dmPelsWidth))
                where = sess.note or t(f"Монитор ПК на 1920×1080, {sess.hz} Гц до «Стоп».",
                                       f"The PC's monitor at 1920x1080, {sess.hz} Hz until Stop.")
                state = sess.link.state
                if sess.capture is not None and not sess.alive() and time.time() - started > 3:
                    self._show(t("Захват экрана остановился", "The screen capture stopped"),
                               t("Нажми «Стоп», потом «Старт».", "Press Stop, then Start."), RED)
                elif state == "ready":
                    self._show(t("Идёт трансляция", "Streaming"), where, GREEN)
                elif state == "need_vf":
                    self._show(t("Открой Virtual Flight на очках", "Open Virtual Flight on the goggles"),
                               "More → DJI Virtual Flight", AMBER)
                elif state == "no_driver":
                    self._show(t("Нужен драйвер DJI", "DJI's USB driver is needed"),
                               t("Установи DJI Assistant 2 (Consumer Drones Series).",
                                 "Install DJI Assistant 2 (Consumer Drones Series)."), RED)
                elif state == "connected":
                    self._show(t("Очки на связи", "Goggles answer"),
                               t("Включаю приём видео…", "Switching their video input on…"), AMBER)
                else:
                    self._show(t("Жду очки по USB…", "Waiting for the goggles on USB…"),
                               t("Проверь кабель USB-A → USB-C.", "Check the USB-A to USB-C cable."), AMBER)
            except Exception as e:  # noqa: BLE001
                self.log(f"Status check: {e!r}")
            time.sleep(0.5)

    def _lang_changed(self) -> None:
        """The other language: saved, and the window starts again in it (not during a stream)."""
        if self.session:
            return
        new = "en" if i18n.lang() == "ru" else "ru"
        i18n.save(new)
        self.log("Language: " + new + ", restarting the window")
        self.restart = True   # main() starts the program again once this window is gone
        self._on_close()

    def _on_close(self) -> None:
        self._closing = True
        if self.session:
            self.session.stop()
        self.destroy()


def main() -> None:
    app = Window()
    app.mainloop()
    if getattr(app, "restart", False):   # a new language: the same program again, after this one let go of its hotkeys
        import subprocess
        import sys
        args = [sys.executable] + sys.argv[1:] if getattr(sys, "frozen", False) else [sys.executable, "-m", "goggles_cast"] + sys.argv[1:]
        subprocess.Popen(args, cwd=os.getcwd())
