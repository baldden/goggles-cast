"""'Report a problem': the version, the PC's Windows and graphics card, a note from the person and the end of the
journal go to the project as an issue in a private reports repository. One button, no account: the program creates
the issue itself with a token that can only create issues there (written into the program when it is built, see
build.ps1; never in the source). The user's name and profile folder are taken out of the text first.
Without the token or without internet the report is saved on the Desktop instead."""
from __future__ import annotations

import json
import os
import platform
import re
import time
import urllib.error
import urllib.request

from . import __version__, display

REPORTS_API = "https://api.github.com/repos/baldden/goggles-cast-reports/issues"
BODY_MAX = 60000      # GitHub takes 65536 characters in an issue body
JOURNAL_LINES = 400


def _token() -> str:
    try:
        from . import _report_key   # written by build.ps1 from report_key.txt, not in the repository
    except ImportError:
        return ""
    return bytes(b ^ 0x5A for b in _report_key.KEY).decode()


def _clean(text: str) -> str:
    home = os.path.expanduser("~")
    user = os.environ.get("USERNAME", "")
    if home and len(home) > 3:
        text = text.replace(home, "%USERPROFILE%")
    if user and len(user) > 2:
        text = re.sub(re.escape(user), "<user>", text, flags=re.IGNORECASE)
    return text


def _journal(path: str) -> list[str]:
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            return fh.read().splitlines()[-JOURNAL_LINES:]
    except OSError:
        return []


def build(journal_path: str, note: str = "") -> tuple[str, str]:
    """(title, body) of the report."""
    lines = _journal(journal_path)
    gpu = next((ln.split("capture: adapter ", 1)[1] for ln in reversed(lines) if "capture: adapter " in ln), "?")
    enc = next((ln.split("capture: encoder ", 1)[1].split()[0] for ln in reversed(lines)
                if "capture: encoder " in ln and "did not open" not in ln), "?")
    w, h, hz = display.current()
    head = (f"**What the person says:** {note.strip() or '(nothing written)'}\n\n"
            f"- Goggles Cast {__version__}\n"
            f"- Windows {platform.version()} ({platform.machine()})\n"
            f"- Graphics card: {gpu}\n"
            f"- Encoder: {enc}\n"
            f"- Monitor now: {w}x{h} {hz} Hz\n\n"
            "Journal (stats\\journal.txt):\n")
    while True:   # as much of the journal's end as the issue takes
        body = head + "```\n" + "\n".join(lines) + "\n```\n"
        if len(body) <= BODY_MAX or not lines:
            break
        lines = lines[len(lines) // 4:]
    first = note.strip().splitlines()[0][:60] if note.strip() else ""
    title = f"{__version__}, {gpu.split(' (vendor')[0]}" + (f": {first}" if first else "")
    return _clean(title), _clean(body)


def send(journal_path: str, note: str = "") -> str:
    """Sends the report. Returns how it went, in words for the window: the issue's number, or where the file is."""
    title, body = build(journal_path, note)
    token = _token()
    if token:
        req = urllib.request.Request(REPORTS_API, data=json.dumps({"title": title, "body": body}).encode(),
                                     method="POST", headers={
                                         "Authorization": "Bearer " + token,
                                         "Accept": "application/vnd.github+json",
                                         "User-Agent": f"GogglesCast/{__version__}",
                                         "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                number = json.load(r).get("number")
            return f"sent #{number}"
        except (urllib.error.URLError, OSError, ValueError) as e:
            why = f"not sent ({e})"
    else:
        why = "not sent (this build has no report key)"
    desktop = os.path.join(os.path.expanduser("~"), "Desktop")
    path = os.path.join(desktop if os.path.isdir(desktop) else os.getcwd(),
                        time.strftime("GogglesCast-report-%Y%m%d-%H%M%S.txt"))
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(title + "\n\n" + body)
    return f"{why}; saved to {path}"
