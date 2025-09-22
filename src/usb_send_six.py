import sys
import serial
import time

USB_PORT = "/dev/cu.usbserial-0001"   # adjust if needed
BAUD = 115200

def send_text(msg):
    # Ensure 6 lines
    lines = [s.strip() for s in msg.split("|")]
    while len(lines) < 6:
        lines.append("")
    body = "\n".join(lines[:6]) + "\n"

    with serial.Serial(USB_PORT, BAUD, timeout=1) as ser:
        time.sleep(2)  # wait for ESP32 reset on port open
        ser.write(body.encode("utf-8"))
        ser.flush()
        print("Sent over USB.")

if __name__ == "__main__":
    msg = " ".join(sys.argv[1:])
    send_text(msg)