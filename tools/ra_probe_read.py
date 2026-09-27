#!/usr/bin/env python3
"""Print the RetroAchievements diagnostics the ARM7 card engine writes.

    python tools/ra_probe_read.py <SD>/_nds/nds-bootstrap/ramDump.bin

Also accepts just the 64KB probe ring (curl -r 33488896-33554431 ...).
Probe records are 32 bytes in a 2048-entry ring at 0x01FF0000, one per
second.  With the whole file, pending unlock records (not yet moved to
sd:/_nds/ra/unlocks.log by the loader) are listed too.
"""

import struct
import sys

PROBE_OFFSET = 0x01FF0000
UNLOCK_OFFSET = 0x01FE0000
RECORDS = 2048
SIZE = 32
RAT1 = 0x31544152
RAT2 = 0x32544152
RAT3 = 0x33544152
WATCH = ("076a2c", "07b3b0", "17dd38", "17dd1c", "17dd20", "17dd30")
RAU1 = 0x31554152
STATES = {0: "loading", 1: "running", 0xFFFF: "off"}


def probes3(blob: bytes) -> int:
    rows = [struct.unpack_from("<16I", blob, i * 64) for i in range(len(blob) // 64)]
    rows = sorted((r for r in rows if r[0] == RAT3), key=lambda r: r[1])
    print(f"{len(rows)} probe records")
    print("   seq   vblank keys state   achs parsed errs unl lines  max  eval skip  variant  magic    "
          + " ".join(f"{w:>8s}" for w in WATCH))
    for r in rows:
        _, seq, frame, ks, ap, eu, lm, fs, cfg, magic = r[:10]
        prio = ("during", "always", "off", "?")[(cfg >> 1) & 3]
        variant = f"{'wram' if cfg >> 31 else 'main'}/{prio}/{max(1, (cfg >> 8) & 0xFF)}"
        state = STATES.get(ks >> 16, hex(ks >> 16))
        print(f"{seq:6d} {frame:8d} {ks & 0xFFFF:04X} {state:7s} {ap & 0xFFFF:4d} {ap >> 16:6d} {eu & 0xFFFF:4d}"
              f" {eu >> 16:3d} {lm & 0xFFFF:5d} {lm >> 16:4d} {fs & 0xFFFF:5d} {fs >> 16:4d}  {variant:14s} {magic:08X} "
              + " ".join(f"{v:08X}" for v in r[10:]))
    return 0 if rows else 1


def probes(blob: bytes) -> int:
    if any(struct.unpack_from("<I", blob, i * 64)[0] == RAT3 for i in range(len(blob) // 64)):
        return probes3(blob)
    rows = [struct.unpack_from("<8I", blob, i * SIZE) for i in range(len(blob) // SIZE)]
    rows = sorted((r for r in rows if r[0] in (RAT1, RAT2)), key=lambda r: r[1])
    if not rows:
        print("no probe records found")
        return 1
    print(f"{len(rows)} probe records")
    print("   seq   vblank  keys  state    achs parsed errs unlocks  lines max  evaluated skipped")
    for r in rows:
        if r[0] == RAT1:
            print(f"{r[1]:6d} {r[2]:8d}  {r[3]:04X}  (milestone 1 record)")
            continue
        _, seq, frame, ks, ap, eu, lm, fs = r
        state = STATES.get(ks >> 16, hex(ks >> 16))
        print(f"{seq:6d} {frame:8d}  {ks & 0xFFFF:04X}  {state:8s} {ap & 0xFFFF:4d} {ap >> 16:6d}"
              f" {eu & 0xFFFF:4d} {eu >> 16:7d}  {lm & 0xFFFF:5d} {lm >> 16:3d} {fs & 0xFFFF:9d} {fs >> 16:7d}")
    return 0


def unlocks(blob: bytes) -> None:
    found = []
    for i in range(len(blob) // 64):
        magic, seq, ach, game, points, frame = struct.unpack_from("<6I", blob, i * 64)
        if magic == RAU1:
            rtc = blob[i * 64 + 24:i * 64 + 32]
            md5 = blob[i * 64 + 32:i * 64 + 64].decode("ascii", "replace")
            found.append((seq, ach, game, points, frame, rtc, md5))
    print(f"\n{len(found)} pending unlock records")
    for seq, ach, game, points, frame, rtc, md5 in sorted(found):
        print(f"  #{seq} achievement {ach} ({points} pts) game {game} {md5} frame {frame}"
              f" 20{rtc[0]:02d}-{rtc[1]:02d}-{rtc[2]:02d} {rtc[4]:02d}:{rtc[5]:02d}:{rtc[6]:02d}")


def main(path: str) -> int:
    with open(path, "rb") as fh:
        fh.seek(0, 2)
        whole = fh.tell() != RECORDS * SIZE
        fh.seek(PROBE_OFFSET if whole else 0)
        rc = probes(fh.read(RECORDS * SIZE))
        if whole:
            fh.seek(UNLOCK_OFFSET)
            unlocks(fh.read(0x10000))
    return rc


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
