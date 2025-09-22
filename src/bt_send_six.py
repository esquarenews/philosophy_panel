#!/usr/bin/env python3
import sys, time, serial, os

PORT = os.getenv("BT_PORT", "/dev/tty.MatrixPanel-SerialPort")  # adjust after pairing
BAUD = int(os.getenv("BT_BAUD", "115200"))

def to_six_by_ten(lines):
    fixed = []
    for i in range(6):
        ln = lines[i] if i < len(lines) else ""
        ln = ln.encode("ascii", errors="ignore").decode("ascii")
        ln = (ln + " " * 10)[:10]
        fixed.append(ln)
    return "\n".join(fixed) + "\n"

def main():
    if len(sys.argv) < 2:
        print("Usage: bt_send_six.py \"line1|line2|line3|line4|line5|line6\"")
        sys.exit(1)
    parts = sys.argv[1].split("|")
    payload = to_six_by_ten(parts)

    with serial.Serial(PORT, BAUD, timeout=2) as ser:
        ser.write(payload.encode("ascii"))
        ser.flush()
        time.sleep(0.2)

if __name__ == "__main__":
    main()