"""Reset the board and capture N seconds of boot log via USB-Serial/JTAG."""
import serial
import time
import sys

out_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/clare_boot.log"
duration = float(sys.argv[2]) if len(sys.argv) > 2 else 200

ser = serial.Serial()
ser.port = "/dev/cu.usbmodem1101"
ser.baudrate = 115200
ser.timeout = 0.2
ser.open()
ser.dtr = False
ser.rts = True
time.sleep(0.1)
ser.rts = False
ser.reset_input_buffer()
deadline = time.time() + duration
chunks = []
while time.time() < deadline:
    data = ser.read(4096)
    if data:
        chunks.append(data)
ser.close()
with open(out_path, "wb") as f:
    f.write(b"".join(chunks))
print(f"captured {sum(len(c) for c in chunks)} bytes -> {out_path}")
