"""Capture the automatic benchmark, or a short normal-boot log (needs pyserial)."""
import argparse
import time
from pathlib import Path
import serial

parser = argparse.ArgumentParser()
parser.add_argument('port')
parser.add_argument('--normal', action='store_true')
parser.add_argument('--output', help='Log filename inside tests/results')
args = parser.parse_args()
root = Path(__file__).parent / 'results'
root.mkdir(exist_ok=True)
path = root / (args.output or ('rhodes-normal.log' if args.normal else 'rhodes-stress.log'))
port = serial.Serial()
port.port = args.port
port.baudrate = 115200
port.timeout = 1
port.dtr = False
port.rts = False
port.open()
end = time.monotonic() + (10 if args.normal else 100)
complete = False
with port, path.open('w') as log:
    while time.monotonic() < end:
        line = port.readline().decode('utf-8', errors='replace')
        if not line:
            continue
        log.write(line)
        log.flush()
        if args.normal or '[Rhodes' in line or 'panic' in line or 'ERROR' in line:
            print(line.strip(), flush=True)
        if '[Rhodes stress] DONE' in line:
            complete = True
            break
if not args.normal and not complete:
    raise SystemExit('Timed out before benchmark completion; inspect capture')
