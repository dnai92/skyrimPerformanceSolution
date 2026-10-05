"""Texturbedarf pro Mod (nur lesen): lose DDS + Texturen in BSA-Archiven im Vortex-Staging.
Aktiv = Dateien sind per Hardlink ins Spiel deployed (st_nlink > 1).
Aufruf: python texture_report.py [staging] > report.txt"""
import os, struct, sys
from collections import defaultdict

STAGING = sys.argv[1] if len(sys.argv) > 1 else r"E:\Skyrim\Vortex Mods\skyrimse"


def dds_dims(head):
    if len(head) >= 20 and head[:4] == b"DDS ":
        h, w = struct.unpack_from("<II", head, 12)
        return w, h
    return None


def bucket(dims):
    if not dims:
        return "?"
    m = max(dims)
    return "8K+" if m >= 8192 else "4K" if m >= 4096 else "2K" if m >= 2048 else "klein"


def scan_bsa(path, stats):
    with open(path, "rb") as f:
        head = f.read(36)
        if head[:4] != b"BSA\0":
            return
        magic, ver, off, flags, nf, nfile, fnl, flnl, ftype = struct.unpack("<4sIIIIIIII", head)
        rec = 24 if ver == 105 else 16
        f.seek(off)
        counts = [struct.unpack("<QI", f.read(rec)[:12])[1] for _ in range(nf)]
        entries = []
        for c in counts:
            n = f.read(1)[0]
            folder = f.read(n).rstrip(b"\0").decode("latin-1").lower()
            for _ in range(c):
                h, size, o = struct.unpack("<QII", f.read(16))
                entries.append((folder, size, o))
        names = f.read(flnl).split(b"\0")
        compressed_default = bool(flags & 4)
        embed = bool(flags & 0x100)
        for i, (folder, size, o) in enumerate(entries):
            name = names[i].decode("latin-1").lower() if i < len(names) else ""
            if not name.endswith(".dds"):
                continue
            comp = compressed_default ^ bool(size & 0x40000000)
            size &= 0x3FFFFFFF
            dims = None
            if not comp:
                f.seek(o)
                skip = 0
                if embed:
                    skip = f.read(1)[0]
                    f.seek(o + 1 + skip)
                dims = dds_dims(f.read(20))
            stats["bytes"] += size
            stats["count"] += 1
            stats[bucket(dims) if dims else ("?" if comp else "?")] += 1
            if dims and max(dims) >= 4096:
                stats["big_bytes"] += size


def main():
    rows = []
    for mod in sorted(os.listdir(STAGING)):
        root = os.path.join(STAGING, mod)
        if not os.path.isdir(root):
            continue
        stats = defaultdict(int)
        deployed = None
        for dp, dn, fn in os.walk(root):
            for name in fn:
                p = os.path.join(dp, name)
                low = name.lower()
                if low.endswith((".dds", ".bsa")):
                    try:
                        st = os.stat(p)
                    except OSError:
                        continue
                    if deployed is None:
                        deployed = st.st_nlink > 1
                    if low.endswith(".dds"):
                        with open(p, "rb") as f:
                            dims = dds_dims(f.read(20))
                        stats["bytes"] += st.st_size
                        stats["count"] += 1
                        stats[bucket(dims)] += 1
                        if dims and max(dims) >= 4096:
                            stats["big_bytes"] += st.st_size
                    else:
                        try:
                            scan_bsa(p, stats)
                        except Exception as e:  # defekte/unbekannte Archive ueberspringen
                            stats["bsa_errors"] += 1
        if stats["count"]:
            rows.append((mod, bool(deployed), stats))
    rows.sort(key=lambda r: r[2]["bytes"], reverse=True)
    total = sum(r[2]["bytes"] for r in rows if r[1])
    print(f"Aktive Mods mit Texturen: {sum(1 for r in rows if r[1])} | Texturdaten aktiv gesamt: {total / 2**30:.1f} GB")
    print(f"{'GB':>6} {'4K+ GB':>7} {'Anzahl':>7} {'8K+':>5} {'4K':>6} {'2K':>6} {'?':>6}  aktiv  Mod")
    for mod, dep, s in rows[:70]:
        print(f"{s['bytes'] / 2**30:6.2f} {s['big_bytes'] / 2**30:7.2f} {s['count']:7d} {s['8K+']:5d} {s['4K']:6d} {s['2K']:6d} {s['?']:6d}  {'ja' if dep else 'nein':5}  {mod}")


if __name__ == "__main__":
    main()
