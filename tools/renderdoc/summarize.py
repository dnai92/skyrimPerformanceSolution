"""Fasst die Ausgabe von analyze_passes.py zusammen: Draws pro Render-Phase, Accumulator, Objekt.

Aufruf: python summarize.py <renderdoc_passes_XXXX.txt> [topN]
"""

import re
import sys
from collections import Counter, defaultdict

ROW = re.compile(r"^\s*\d+\s+(\d+)\s+\d+\s+(DEPTH|COLOR)\s+\S+\s+-?\d+\s+D=(.*?) \| C=(.*?) \| M=(.*?) \| ")
LEAF = re.compile(r"^\[(\w+):\w+\]\s*\([^)]*\)\s*<\d+>\s*(.*)$")
ACC_INDEX = re.compile(r"\[(-?\d+)\]")


def object_name(leaf):
    m = LEAF.match(leaf)
    name = m.group(2) if m else leaf
    name = re.sub(r":\d+$", "", name)   # Sub-Geometrie-Index
    return name.strip()


def main():
    path = sys.argv[1]
    top_n = int(sys.argv[2]) if len(sys.argv) > 2 else 30

    rows = []
    for line in open(path, encoding="utf-8"):
        m = ROW.match(line)
        if not m:
            continue
        marker = re.sub(r" -> Data/ShaderCache/.*$", "", m.group(5))
        segs = marker.split("/")
        rows.append({
            "draws": int(m.group(1)),
            "depth": m.group(3).split(" ")[0],
            "phase": segs[1] if len(segs) > 1 else segs[0],
            "acc": (ACC_INDEX.search(segs[2]).group(1) if len(segs) > 2 and ACC_INDEX.search(segs[2]) else "-"),
            "list": re.sub(r"\s*[<(\[].*", "", segs[3]) if len(segs) > 3 else "-",
            "shader": (LEAF.match(segs[-1]).group(1) if LEAF.match(segs[-1]) else "-"),
            "obj": object_name(segs[-1]),
        })

    total = sum(r["draws"] for r in rows)
    print(f"Draws gesamt: {total}\n")

    by_phase = Counter()
    for r in rows:
        by_phase[r["phase"]] += r["draws"]
    print("== Draws pro Phase ==")
    for k, v in by_phase.most_common(15):
        print(f"{v:7d}  {v * 100 / total:5.1f}%  {k}")

    for phase in [p for p, _ in by_phase.most_common(5)]:
        sub = [r for r in rows if r["phase"] == phase]
        n = sum(r["draws"] for r in sub)
        print(f"\n######## {phase} ({n} Draws)")

        acc = Counter()
        lst = Counter()
        shader = Counter()
        depth = Counter()
        obj = Counter()
        for r in sub:
            acc[r["acc"]] += r["draws"]
            lst[r["list"]] += r["draws"]
            shader[r["shader"]] += r["draws"]
            depth[r["depth"]] += r["draws"]
            obj[r["obj"]] += r["draws"]

        print("-- Accumulator-Index:", ", ".join(f"[{k}]={v}" for k, v in sorted(acc.items(), key=lambda kv: -kv[1])[:12]))
        print("-- Renderliste:", ", ".join(f"{k}={v}" for k, v in lst.most_common(8)))
        print("-- Shader:", ", ".join(f"{k}={v}" for k, v in shader.most_common(8)))
        print("-- Depth-Target:", ", ".join(f"{k}={v}" for k, v in depth.most_common(5)))
        print(f"-- {len(obj)} verschiedene Objekte, Top {top_n}:")
        for k, v in obj.most_common(top_n):
            print(f"   {v:6d}  {k}")


if __name__ == "__main__":
    main()
