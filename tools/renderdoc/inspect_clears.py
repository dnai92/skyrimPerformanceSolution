# Prueft in einer RenderDoc-Aufnahme, welche Array-Slices die ClearDepthStencilView-Aufrufe auf einer
# Depth-Textur treffen (z. B. kSHADOWMAPS_ESRAM der Sonnenkaskaden) und in welche Slices die Draws zeichnen.
#
# Aufruf: qrenderdoc.exe --python inspect_clears.py
# Parameter ueber inspect_clears.cfg neben dem Skript: Zeile 1 = rdc, Zeile 2 = out, Zeile 3 = Texturname

import os
import traceback

import renderdoc as rd

try:
    _cfg = os.path.join(os.path.dirname(os.path.abspath(__file__)), "inspect_clears.cfg")
except NameError:
    _cfg = r"C:\dev\skyrim-perf\tools\renderdoc\inspect_clears.cfg"
_lines = [l.strip() for l in open(_cfg, encoding="utf-8-sig").read().splitlines() if l.strip()]
RDC_FILE, RDC_OUT, TEX_NAME = _lines[0], _lines[1], _lines[2]

out = open(RDC_OUT, "w", encoding="utf-8")


def log(*args):
    out.write(" ".join(str(a) for a in args) + "\n")
    out.flush()


def dump_sd(obj, depth=0, maxdepth=6):
    """Strukturierte Daten (SDObject) als Text, begrenzt."""
    lines = []
    name = obj.name
    val = ""
    if obj.NumChildren() == 0:
        for getter in ("AsString", "AsUInt", "AsInt", "AsFloat", "AsResourceId"):
            try:
                v = getattr(obj, getter)()
                if v not in ("", None):
                    val = v
                    break
            except Exception:
                pass
    lines.append("  " * depth + "%s = %s" % (name, val))
    if depth < maxdepth:
        for i in range(obj.NumChildren()):
            lines.extend(dump_sd(obj.GetChild(i), depth + 1, maxdepth))
    return lines


def flatten(actions, markers, sdfile, result):
    for a in actions:
        name = a.GetName(sdfile)
        if a.children:
            flatten(a.children, markers + [name], sdfile, result)
        else:
            result.append((a, name, "/".join(markers)))


def analyze(ctrl):
    sdfile = ctrl.GetStructuredFile()
    resources = ctrl.GetResources()
    names = {r.resourceId: r.name for r in resources}

    target = None
    for r in resources:
        if r.name == TEX_NAME:
            target = r
    if not target:
        log("Textur nicht gefunden:", TEX_NAME)
        return
    log("Textur:", TEX_NAME, target.resourceId)

    # Alle Views dieser Textur: Erzeugungs-Chunk ausgeben (enthaelt FirstArraySlice/ArraySize)
    views = {}
    for r in resources:
        if target.resourceId in list(r.parentResources):
            views[r.resourceId] = r
            log("\n== View", r.name, r.resourceId, "Typ", r.type)
            for ci in r.initialisationChunks:
                chunk = sdfile.chunks[ci]
                for l in dump_sd(chunk, 0, 5):
                    log("   ", l)

    # Clears und erste Draws je Pass auf diese Textur
    flat = []
    flatten(ctrl.GetRootActions(), [], sdfile, flat)
    log("\n== Clears auf Views dieser Textur")
    for a, name, marker in flat:
        if not (a.flags & rd.ActionFlags.Clear):
            continue
        for ev in a.events:
            chunk = sdfile.chunks[ev.chunkIndex]
            text = "\n".join(dump_sd(chunk, 0, 3))
            if "Directional Light Shadowmaps" in marker:
                log("EID", a.eventId, name, "|", marker)
                for l in dump_sd(chunk, 0, 3):
                    log("   ", l)

    # Welche DSV ist bei Draws gebunden (Stichprobe: Wechsel der View)
    log("\n== Gebundene Depth-Views bei Draws (nur Wechsel)")
    last = None
    for a, name, marker in flat:
        if not (a.flags & rd.ActionFlags.Drawcall):
            continue
        if "Directional Light Shadowmaps" not in marker:
            continue
        dsv = a.depthOut
        if dsv != last:
            ctrl.SetFrameEvent(a.eventId, False)
            st = ctrl.GetD3D11PipelineState()
            dt = st.outputMerger.depthTarget
            info = {k: getattr(dt, k) for k in ("view", "resource", "firstSlice", "numSlices", "firstMip") if hasattr(dt, k)}
            log("EID", a.eventId, names.get(dsv, str(dsv)), info, "|", marker[-80:])
            last = dsv
    log("fertig")


def main():
    log("Lade", RDC_FILE)
    if "pyrenderdoc" in globals():
        ctx = pyrenderdoc  # noqa: F821 - von qrenderdoc bereitgestellt
        ctx.LoadCapture(RDC_FILE, rd.ReplayOptions(), RDC_FILE, False, True)
        ctx.Replay().BlockInvoke(analyze)
        return
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(RDC_FILE, "", None) != rd.ResultCode.Succeeded:
        log("OpenFile fehlgeschlagen")
        return
    status, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ResultCode.Succeeded:
        log("OpenCapture fehlgeschlagen", status)
        return
    try:
        analyze(ctrl)
    except Exception:
        log(traceback.format_exc())
    ctrl.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    log(traceback.format_exc())
finally:
    out.close()
    os._exit(0)
