#!/usr/bin/env python3
"""Print the RetroAchievements probe log that the ARM7 card engine writes.

    python tools/ra_probe_read.py <SD>/_nds/nds-bootstrap/ramDump.bin

Records are 32 bytes in a 2048-entry ring at 0x01FF0000:
'RAT1', seq, vblank count, KEYINPUT, game frame count, 3 watched RAM words.
"""

import struct
import sys

OFFSET = 0x01FF0000
RECORDS = 2048
SIZE = 32
MAGIC = 0x31544152


def main(path: str) -> int:
    with open(path, "rb") as fh:
        fh.seek(OFFSET)
        blob = fh.read(RECORDS * SIZE)
    rows = []
    for i in range(len(blob) // SIZE):
        rec = struct.unpack_from("<8I", blob, i * SIZE)
        if rec[0] == MAGIC:
            rows.append(rec)
    if not rows:
        print("no probe records found")
        return 1
    rows.sort(key=lambda r: r[1])
    print(f"{len(rows)} records")
    print("   seq   vblank  keys  gameframe  watch0     watch1     watch2")
    for _, seq, frame, keys, gframe, w0, w1, w2 in rows:
        print(f"{seq:6d} {frame:8d}  {keys:04X} {gframe:10d}  {w0:08X}  {w1:08X}  {w2:08X}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
