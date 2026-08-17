"""Reset the board and capture its console for a fixed window, then exit.

Used instead of `idf.py monitor` because that never returns, which makes it
useless for scripted flash-and-check cycles.

    python tools/monitor.py [seconds] [COMxx]
"""
import sys
import time

import serial

SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0
PORT = sys.argv[2] if len(sys.argv) > 2 else "COM4"
BAUD = 115200

s = serial.Serial(PORT, BAUD, timeout=0.2)

# DTR drives IO0, RTS drives EN. Keep IO0 high so the app boots rather than the
# ROM loader, and pulse EN low to reset.
s.setDTR(False)
s.setRTS(True)
time.sleep(0.12)
s.setRTS(False)

deadline = time.time() + SECONDS
buf = bytearray()
while time.time() < deadline:
    chunk = s.read(4096)
    if chunk:
        buf += chunk

# Release both control lines before closing. Leaving RTS asserted holds the chip
# in reset, and the next flash then fails with "No serial data received" on a
# port that looks perfectly healthy. This cost half an hour once already.
s.setRTS(False)
s.setDTR(False)
time.sleep(0.1)
s.close()

sys.stdout.write(buf.decode("utf-8", errors="replace"))
sys.stdout.write(f"\n--- captured {len(buf)} bytes in {SECONDS:.0f}s ---\n")
