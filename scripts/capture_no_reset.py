import time
import serial
import sys
from datetime import datetime

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem21201"
duration = int(sys.argv[2]) if len(sys.argv) > 2 else 300
log_file = sys.argv[3] if len(sys.argv) > 3 else "/tmp/serial_capture.log"

ser = serial.Serial(port, 115200, timeout=0.05, write_timeout=1)
# Do NOT reset; release RTS/DTR so ESP32 keeps running
ser.dtr = False
ser.rts = False
ser.reset_input_buffer()

start = time.time()
with open(log_file, "w", encoding="utf-8") as f:
    while time.time() - start < duration:
        chunk = ser.read(4096)
        if chunk:
            f.write(chunk.decode("utf-8", "replace"))
            f.flush()
ser.close()
print(f"Captured to {log_file}")
