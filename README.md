# Goggles Cast

Streams a Windows PC's screen to DJI Goggles 2 / DJI Integra over a single USB cable.

For the stream the main monitor switches to 1920x1080 at the goggles' refresh rate (60 or 100 Hz). The GPU captures and encodes it as H.264, and the goggles show it the way they show DJI Virtual Flight. At Stop the monitor returns to its own mode.

## Requirements

- **PC:** Windows 10 or 11, 64-bit, with a GPU that has a hardware H.264 encoder: NVIDIA (NVENC), AMD (AMF) or Intel (Quick Sync).
- **Monitor:** a 1920x1080 mode at 60 Hz, and at 100 Hz for 100 Hz streaming. If the monitor has no 100 Hz mode at 1920x1080, the 100 Hz button is greyed out.
- **Goggles:** DJI Goggles 2 or DJI Integra.
- **Cable:** USB-A to USB-C, with the USB-A end in the PC. A USB-C to USB-C cable does not work: in Virtual Flight the goggles take the USB host role over it, and the PC cannot see them.
- **DJI USB driver:** installed with [DJI Assistant 2 (Consumer Drones Series)](https://www.dji.com/downloads/softwares/dji-assistant-2-consumer-drones-series). Assistant itself does not need to run while streaming.

## Install and run

1. Download `GogglesCast-Setup-<version>.exe` from Releases and run it. Everything the program needs is in it: no Python or anything else to install (only DJI's driver, above). No administrator rights are needed; it installs into the user profile.
2. Turn the goggles on and connect them to the PC with the USB-A to USB-C cable.
3. On the goggles open **More > DJI Virtual Flight**.
4. Start Goggles Cast. The window shows "Goggles connected".
5. Pick the refresh rate (60 / 100 Hz), then press **Start**. To stop, press **Stop** or **Ctrl+Alt+End** from any program.

The bitrate is set by itself to the most the goggles take over USB: 24 Mbit/s at 60 Hz, 32 Mbit/s at 100 Hz. A log is written to `stats\journal.txt` next to the program.

## Building from source

Only for building the installer yourself: Python 3.13 with PyInstaller, MSYS2 (UCRT64) for the capture program in `native/`, and Inno Setup 6.

```
powershell -ExecutionPolicy Bypass -File build.ps1 -Installer -Version 1.0.0
```

The installer is written to `installer\`.

## Who made it

The code was written by an AI, **Claude Opus 5.5** (Anthropic), in Claude Code, directed and tested by the repository owner.

## What it is built on

Written for this project:

- The protocol of DJI Virtual Flight with the goggles: session, heartbeats, simulator mode, video packet framing. Worked out from DJI's Virtual Flight app.
- The USB link to the goggles (`native/goggles_usb.h`), written in C++, and the window application.
- The capture and frame pacing in `native/gcast_capture.cpp`: a port of [Apollo](https://github.com/ClassicOldSong/Apollo)'s capture and encoder code (GPL-3.0, `native/LICENSE.apollo`).

From [G2 Bridge](https://github.com/Ammaar-Alam/g2-bridge) (GPL-3.0), which first showed that a PC can drive the goggles directly over USB:

- The route through DJI's USB driver.
- The PC's address on the link.
- The size limits: 978-byte video chunks and frames of at most about 56 KB.
- The bitrate limits above.

The packets were checked byte for byte against G2 Bridge's.

Because it includes GPL-3.0 code, Goggles Cast is distributed under GPL-3.0 (`LICENSE`).

The goggles' protocol belongs to DJI. This project is not affiliated with or endorsed by DJI.
