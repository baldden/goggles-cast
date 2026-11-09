"""How much the goggles take over USB at 100 Hz: streams the main monitor as it is (no mode change) at 100 fps and the
given bitrate to the goggles on USB, and prints once a second the frames sent and the biggest frame size possible.

Every frame is at most bitrate / fps (a VBV of one frame); G2 Bridge found frames up to 49 KB shown and from about
65 KB frozen or black until the goggles were powered off and on. So: put something detailed and moving on the
monitor (an FPV video full screen), watch the goggles, and say whether the picture froze or went black.

    python tools/usb_bitrate_test.py 38 60      # 38 Mbit/s for 60 s
"""
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
sys.stdout.reconfigure(encoding="utf-8")
from goggles_cast import usbgoggles  # noqa: E402
from goggles_cast.capture import Capture  # noqa: E402


def main() -> None:
    mbps = float(sys.argv[1]) if len(sys.argv) > 1 else 38.0
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
    if not usbgoggles.present():
        sys.exit("The goggles are not on USB (USB-A to USB-C cable, More > DJI Virtual Flight open).")
    link = usbgoggles.LinkState()
    cap = Capture(100, mbps, link.on_line, print)
    cap.start()
    print(f"100 fps, {mbps:g} Mbit/s: a frame is at most {mbps * 1e6 / 8 / 100 / 1024:.1f} KB")
    t0 = time.time()
    try:
        while time.time() - t0 < seconds:
            time.sleep(1)
            st = cap.stat
            print(f"{time.time() - t0:4.0f} s  link {link.state:9}  sent {st.get('usb_sent', '?'):>4}/s  "
                  f"too big {st.get('usb_oversize', '?')}  {st.get('mbps', '?')} Mbit/s")
    finally:
        cap.stop()
    print("Done. Did the goggles freeze or go black at any point?")


if __name__ == "__main__":
    main()
