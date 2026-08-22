"""抓取 COM6 串口日志，用于观察小智设备启动状态"""
import serial
import sys
import time

PORT = "COM6"
BAUD = 115200
DURATION = int(sys.argv[1]) if len(sys.argv) > 1 else 60
OUT = sys.argv[2] if len(sys.argv) > 2 else r"D:\ai_work\xiaozhi-esp32-music-player\serial_log.txt"

end = time.time() + DURATION
lines = []
try:
    ser = serial.Serial(PORT, BAUD, timeout=1)
except Exception as e:
    print(f"open {PORT} failed: {e}")
    sys.exit(1)

print(f"capturing {PORT} for {DURATION}s ...")
try:
    while time.time() < end:
        data = ser.readline()
        if data:
            try:
                line = data.decode("utf-8", errors="replace").rstrip()
            except Exception:
                continue
            print(line)
            lines.append(line)
finally:
    ser.close()
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"--- saved {len(lines)} lines to {OUT}")
