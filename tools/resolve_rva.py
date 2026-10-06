"""Rechnet Skyrim-Adressen aus Tracy-Auswertungen in Address-Library-IDs um.

Aufruf:
    python resolve_rva.py <sampling.txt> <image_base_hex> [Thread-Name-Teil] [top]

  sampling.txt      Ausgabe von tracy-sampling-export
  image_base_hex    Basisadresse von SkyrimSE.exe aus dem SkyrimPerf.log ("Skyrim-Basisadresse 0x...")
  Thread-Name-Teil  Standard "Main thread"
  top               Anzahl Zeilen je Liste (Standard 25)

Ausgabe: je Funktions-Adresse die Address-Library-ID (naechster Eintrag unterhalb) und der Abstand dazu.
IDs lassen sich in CommonLib (RELOCATION_ID(SE, AE)) und Community Shaders nachschlagen.
"""
import os
import re
import struct
import sys

# Address Library: Umgebungsvariable SKYRIM_VERSIONLIB oder relativ zum Spielordner
LIB = os.environ.get("SKYRIM_VERSIONLIB", r"Data\SKSE\Plugins\versionlib-1-6-1170-0.bin")


def load_library(path):
    d = open(path, "rb").read()
    pos = 0

    def rd(fmt):
        nonlocal pos
        size = struct.calcsize(fmt)
        v = struct.unpack_from("<" + fmt, d, pos)[0]
        pos += size
        return v

    fmt = rd("i")
    assert fmt == 2, "nur Format 2 unterstuetzt (war %d)" % fmt
    pos += 16  # Version
    name_len = rd("i")
    pos += name_len
    pointer_size = rd("i")
    count = rd("i")
    entries = []
    prev_id = 0
    prev_off = 0
    for _ in range(count):
        t = rd("B")
        lo, hi = t & 0xF, t >> 4
        if lo == 0:
            i = rd("Q")
        elif lo == 1:
            i = prev_id + 1
        elif lo == 2:
            i = prev_id + rd("B")
        elif lo == 3:
            i = prev_id - rd("B")
        elif lo == 4:
            i = prev_id + rd("H")
        elif lo == 5:
            i = prev_id - rd("H")
        elif lo == 6:
            i = rd("H")
        elif lo == 7:
            i = rd("I")
        else:
            raise ValueError("unbekannter Typ")
        tmp = prev_off // pointer_size if (hi & 8) else prev_off
        h = hi & 7
        if h == 0:
            off = rd("Q")
        elif h == 1:
            off = tmp + 1
        elif h == 2:
            off = tmp + rd("B")
        elif h == 3:
            off = tmp - rd("B")
        elif h == 4:
            off = tmp + rd("H")
        elif h == 5:
            off = tmp - rd("H")
        elif h == 6:
            off = rd("H")
        else:
            off = rd("I")
        if hi & 8:
            off *= pointer_size
        entries.append((off, i))
        prev_id, prev_off = i, off
    entries.sort()
    return entries


def lookup(entries, offsets, rva):
    import bisect

    k = bisect.bisect_right(offsets, rva) - 1
    if k < 0:
        return None, None
    return entries[k][1], rva - entries[k][0]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    path = sys.argv[1]
    base = int(sys.argv[2], 16)
    thread = sys.argv[3] if len(sys.argv) > 3 else "Main thread"
    top = int(sys.argv[4]) if len(sys.argv) > 4 else 25
    entries = load_library(LIB)
    offsets = [e[0] for e in entries]

    text = open(path, encoding="utf-8-sig", errors="replace").read()
    start = text.index("######## Thread " + thread)
    end = text.find("######## Thread", start + 10)
    sec = text[start : end if end > 0 else None]

    def parts(title):
        i = sec.find(title)
        if i < 0:
            return []
        block = sec[i:].split("\n-- ")[0].splitlines()[1:]
        return block

    for title in [m for m in re.findall(r"-- Top \d+ Funktionen (?:exklusiv|inklusiv)", sec)][:2]:
        print("==", title.strip("- "))
        n = 0
        for line in parts(title):
            m = re.match(r"\s+(?:exkl|inkl)\s+([\d.]+)%\s+(?:exkl|inkl)\s+([\d.]+)%\s+SkyrimSE\.exe!@0x([0-9a-fA-F]+)", line)
            if not m:
                continue
            addr = int(m.group(3), 16)
            rva = addr - base
            i, delta = lookup(entries, offsets, rva)
            print("  %5s%% / %5s%%  RVA 0x%X  ->  ID %s +0x%X" % (m.group(1), m.group(2), rva, i, delta if delta is not None else 0))
            n += 1
            if n >= top:
                break


if __name__ == "__main__":
    main()
