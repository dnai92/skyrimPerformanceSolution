# Wertet eine RenderDoc-Aufnahme (.rdc) eines Skyrim-Frames aus: Render-Paesse und Draw Calls pro Pass.
#
# Aufruf (nutzt das in RenderDoc eingebettete Python):
#   qrenderdoc.exe --python analyze_passes.py
# Parameter ueber Umgebungsvariablen (alternativ analyze_passes.cfg neben dem Skript: Zeile 1 = rdc, Zeile 2 = out):
#   RDC_FILE = Pfad zur .rdc-Datei
#   RDC_OUT  = Pfad der Text-Ausgabe
#
# Pass-Grenzen: Wechsel der Render-Targets (Depth + Color), Clear-Aufrufe und Debug-Marker.
# Fuer den ersten Draw jedes Passes wird der D3D11-Zustand abgefragt (Viewport, DSV-Slice).

import os
import sys
import traceback

import renderdoc as rd

RDC_FILE = os.environ.get("RDC_FILE", "")
RDC_OUT = os.environ.get("RDC_OUT", "")
# qrenderdoc setzt __file__ nicht immer -> fester Fallback-Pfad
try:
    _cfg = os.path.join(os.path.dirname(os.path.abspath(__file__)), "analyze_passes.cfg")
except NameError:
    _cfg = r"C:\dev\skyrim-perf\tools\renderdoc\analyze_passes.cfg"
if (not RDC_FILE or not RDC_OUT) and os.path.exists(_cfg):
    _lines = [l.strip() for l in open(_cfg, encoding="utf-8-sig").read().splitlines() if l.strip()]
    RDC_FILE, RDC_OUT = _lines[0], _lines[1]

out = open(RDC_OUT, "w", encoding="utf-8")


def log(*args):
    out.write(" ".join(str(a) for a in args) + "\n")
    out.flush()


def flatten(actions, markers, sdfile, result):
    for a in actions:
        name = a.GetName(sdfile)
        if a.children:
            flatten(a.children, markers + [name], sdfile, result)
        else:
            result.append((a, name, "/".join(markers)))


def analyze(ctrl):
    log("Replay geladen, analysiere ...")
    sdfile = ctrl.GetStructuredFile()
    textures = {t.resourceId: t for t in ctrl.GetTextures()}
    names = {r.resourceId: r.name for r in ctrl.GetResources()}

    def tex_desc(rid):
        if rid == rd.ResourceId.Null():
            return "-"
        t = textures.get(rid)
        n = names.get(rid, str(rid))
        if not t:
            return n
        arr = "[%d]" % t.arraysize if t.arraysize > 1 else ""
        return "%s %dx%d%s %s" % (n, t.width, t.height, arr, t.format.Name())

    flat = []
    flatten(ctrl.GetRootActions(), [], sdfile, flat)

    passes = []
    cur = None
    pending_clear = None
    total_draws = 0
    total_dispatch = 0

    for a, name, marker in flat:
        is_draw = bool(a.flags & rd.ActionFlags.Drawcall)
        is_dispatch = bool(a.flags & rd.ActionFlags.Dispatch)
        is_clear = bool(a.flags & rd.ActionFlags.Clear)

        if is_clear:
            pending_clear = name
            continue
        if is_dispatch:
            total_dispatch += 1
            continue
        if not is_draw:
            continue

        total_draws += 1
        colors = tuple(o for o in a.outputs if o != rd.ResourceId.Null())
        key = (a.depthOut, colors, marker)
        if cur is None or key != cur["key"] or pending_clear is not None:
            cur = {
                "key": key,
                "first_eid": a.eventId,
                "marker": marker,
                "depth": a.depthOut,
                "colors": colors,
                "clear": pending_clear,
                "draws": 0,
                "instances": 0,
                "indices": 0,
            }
            passes.append(cur)
            pending_clear = None
        cur["draws"] += 1
        cur["instances"] += max(1, a.numInstances)
        cur["indices"] += a.numIndices

    # Kein SetFrameEvent pro Pass: jedes Setzen spielt den Frame bis dorthin nach (quadratischer Aufwand)
    for p in passes:
        p["viewport"] = "-"
        p["slice"] = -1

    depth_only = sum(p["draws"] for p in passes if not p["colors"] and p["depth"] != rd.ResourceId.Null())
    log("Datei:", RDC_FILE)
    log("Actions: %d | Draws: %d | davon Depth-only: %d | Dispatches: %d | Paesse: %d" % (len(flat), total_draws, depth_only, total_dispatch, len(passes)))
    log("")

    # Zusammenfassung pro Depth-Target (Schattenkarten erkennt man an Aufloesung/Array)
    log("== Draws pro Depth-Target (nur Depth-only-Paesse) ==")
    per_depth = {}
    for p in passes:
        if p["colors"] or p["depth"] == rd.ResourceId.Null():
            continue
        e = per_depth.setdefault(p["depth"], [0, 0, set()])
        e[0] += p["draws"]
        e[1] += 1
        e[2].add(p["slice"])
    for rid, (d, n, slices) in sorted(per_depth.items(), key=lambda kv: -kv[1][0]):
        log("%7d Draws in %3d Paessen  Slices %s  %s" % (d, n, sorted(slices), tex_desc(rid)))
    log("")

    log("== Alle Paesse (Reihenfolge im Frame) ==")
    log("%6s %6s %8s %-5s %-22s %-6s %s" % ("EID", "Draws", "Inst", "Typ", "Viewport", "Slice", "Depth / Color / Marker / Clear"))
    for p in passes:
        typ = "DEPTH" if not p["colors"] else "COLOR"
        log("%6d %6d %8d %-5s %-22s %-6s D=%s | C=%s | M=%s | %s" % (
            p["first_eid"], p["draws"], p["instances"], typ, p["viewport"], p["slice"],
            tex_desc(p["depth"]), ",".join(tex_desc(c) for c in p["colors"]) or "-",
            p["marker"] or "-", p["clear"] or ""))

def run_in_ui(ctx):
    # In qrenderdoc: Aufnahme ueber den UI-Kontext laden und im Replay-Thread auswerten
    log("Lade", RDC_FILE)
    ctx.LoadCapture(RDC_FILE, rd.ReplayOptions(), RDC_FILE, False, True)
    ctx.Replay().BlockInvoke(analyze)


def run_standalone():
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(RDC_FILE, "", None) != rd.ResultCode.Succeeded:
        log("OpenFile fehlgeschlagen")
        return
    status, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ResultCode.Succeeded:
        log("OpenCapture fehlgeschlagen:", status)
        return
    analyze(ctrl)
    ctrl.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()


try:
    log("Skript gestartet")
    if "pyrenderdoc" in globals():
        run_in_ui(pyrenderdoc)  # noqa: F821 - von qrenderdoc bereitgestellt
    else:
        run_standalone()
    log("Fertig")
except Exception:
    log(traceback.format_exc())
finally:
    out.close()
    os._exit(0)
