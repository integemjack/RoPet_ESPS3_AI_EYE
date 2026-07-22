import serial, time, sys
s = serial.Serial("COM20", 115200, timeout=0.1)
print("=== C5 COM20 18s (S3 should be sending PING/OTA repeatedly) ===", flush=True)
end = time.time() + 18
while time.time() < end:
    d = s.read(4096)
    if d:
        sys.stdout.write(d.decode('utf-8','replace')); sys.stdout.flush()
s.close()
