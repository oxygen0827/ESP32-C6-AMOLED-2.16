import time
import serial

port = "/dev/cu.usbmodem21201"
ser = serial.Serial(port, 115200, timeout=0.05, write_timeout=1)
ser.dtr = False
ser.rts = True
time.sleep(0.15)
ser.rts = False
ser.close()
time.sleep(1.0)
ser = serial.Serial(port, 115200, timeout=0.05, write_timeout=1)
ser.reset_input_buffer()
start = time.time()
data = bytearray()
while time.time() - start < 75:
    chunk = ser.read(4096)
    if chunk:
        data.extend(chunk)
ser.close()
text = data.decode("utf-8", "replace")
open("/tmp/clare-net-8192-reset.log", "w", encoding="utf-8").write(text)
print(text)
print("\n--- bytes", len(data), "---")
