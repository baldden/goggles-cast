"""User-facing language: Russian or English.

The first start follows the Windows display language (Russian -> Russian, anything else -> English); a choice made in
the window is saved in settings.json and wins from then on. Texts are written in place as t("Русский", "English"), so a
string can never be missing in one of the languages. The journal and code comments are always English.
"""
from __future__ import annotations

import json
import os
import sys

LANGS = [("ru", "Русский"), ("en", "English")]
_lang = "en"


def _state_file() -> str:
    from . import app_dir
    return os.path.join(app_dir(), "settings.json")


def system_lang() -> str:
    """'ru' when the Windows display language is Russian, else 'en'."""
    if sys.platform == "win32":
        try:
            import ctypes
            return "ru" if ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3FF == 0x19 else "en"
        except (OSError, AttributeError):
            pass
    import locale
    return "ru" if (locale.getlocale()[0] or "").lower().startswith(("ru", "russian")) else "en"


def load() -> str:
    """Set the language from settings.json, or from the system when nothing was chosen yet."""
    global _lang
    saved = None
    # the language picked in the installer (install_lang, written by installer.iss) wins once, then it is the saved one
    marker = os.path.join(os.path.dirname(_state_file()), "install_lang")
    try:
        with open(marker, encoding="utf-8") as f:
            picked = f.read().strip()
        os.remove(marker)
        if picked in dict(LANGS):
            save(picked)
            return _lang
    except OSError:
        pass
    try:
        with open(_state_file(), encoding="utf-8") as f:
            saved = json.load(f).get("lang")
    except (OSError, ValueError):
        pass
    _lang = saved if saved in dict(LANGS) else system_lang()
    return _lang


def save(lang: str) -> None:
    """Remember the user's choice (keeps the other settings in settings.json)."""
    global _lang
    _lang = lang
    path = _state_file()
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        data = {}
    data["lang"] = lang
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f)
    except OSError:
        pass


def lang() -> str:
    return _lang


def t(ru: str, en: str) -> str:
    return ru if _lang == "ru" else en
