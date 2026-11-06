#!/usr/bin/env python3
"""recv.py [PORT] [--forza] -- the telemetry stream's example receiver (docs/TELEMETRY.md): once a second the rate and the last
packet. Start it, then set ENHANCEMENTS > SYSTEM > TELEMETRY OUTPUT to LOCAL (or ACZ_TELEMETRY=127.0.0.1:20777)."""
import json
import socket
import struct
import sys
import time

args = [a for a in sys.argv[1:] if not a.startswith('--')]
port = int(args[0]) if args else 20777
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('', port))
s.settimeout(1.0)
print('listening on UDP port', port)
n, t0, last = 0, time.monotonic(), None
while True:
    try:
        last = s.recv(2048)
        n += 1
    except socket.timeout:
        pass
    now = time.monotonic()
    if now - t0 >= 1.0:
        if last is None:
            print('nothing yet')
        elif last[:1] == b'{':
            p = json.loads(last)
            print('%5.1f/s  %s' % (n / (now - t0), json.dumps(p)))
        else:   # the Forza Dash packet: IsRaceOn at 0, the speed at byte 256, the aircraft id (CarOrdinal) at byte 212
            print('%5.1f/s  forza %d bytes: on %d, speed %.1f m/s, aircraft %d' % (n / (now - t0), len(last),
                  struct.unpack_from('<i', last, 0)[0], struct.unpack_from('<f', last, 244)[0],
                  struct.unpack_from('<i', last, 212)[0]))
        n, t0 = 0, now
