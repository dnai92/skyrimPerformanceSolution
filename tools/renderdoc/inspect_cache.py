# Kaskaden-Cache-Diagnose: zeigt fuer eine RenderDoc-Aufnahme, was mit der Sonnen-Schattentextur
# (kSHADOWMAPS_ESRAM) und der Volumetric-Schattentextur passiert - in zeitlicher Reihenfolge:
#   - Clears (welche View = welche Ebene)
#   - Draws je Ebene (Anzahl zwischen Clear/Wechsel)
#   - Kopien (CopySubresourceRegion/CopyResource) von/zu diesen Texturen
#   - Stellen, an denen die Texturen zum LESEN gebunden werden (SRV) - mit Marker
# Damit laesst sich pruefen, ob unsere Zurueck-Kopie VOR dem Lesen liegt und ob ein Cache-Frame vorliegt.
#
# Aufruf: qrenderdoc.exe --python inspect_cache.py ; Parameter in inspect_cache.cfg (Zeile 1 = rdc, Zeile 2 = out)

import os
import traceback

import renderdoc as rd

try:
    _cfg = os.path.join(os.path.dirname(os.path.abspath(__file__)), "inspect_cache.cfg")
except NameError:
    _cfg = r"C:\dev\skyrim-perf\tools\renderdoc\inspect_cache.cfg"
_lines = [l.strip() for l in open(_cfg, encoding="utf-8-sig").read().splitlines() if l.strip()]
RDC_FILE, RDC_OUT = _lines[0], _lines[1]
TEX_NAMES = ["kSHADOWMAPS_ESRAM", "kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM"]

out = open(RDC_OUT, "w", encoding="utf-8")


def log(*args):
    out.write(" ".join(str(a) for a in args) + "\n")
    out.flush()


def sd_value(obj):
    for getter in ("AsResourceId", "AsString", "AsUInt", "AsInt", "AsFloat"):
        try:
            v = getattr(obj, getter)()
            if v not in ("", None):
                return v
        except Exception:
            pass
    return ""


def sd_ids(obj, acc):
    """Alle ResourceIds in einem Chunk (rekursiv)."""
    try:
        rid = obj.AsResourceId()
        if rid != rd.ResourceId.Null():
            acc.add(int(rid))
    except Exception:
        pass
    for i in range(obj.NumChildren()):
        sd_ids(obj.GetChild(i), acc)
    return acc


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
    names = {int(r.resourceId): r.name for r in resources}

    tex = {}
    for r in resources:
        if r.name in TEX_NAMES:
            tex[int(r.resourceId)] = r.name
    if not tex:
        # Engine-Ziele unbenannt -> ueber Groesse/Format erkennen (Sonne 2048x2048[2], Volumetric 256x256[2], R16_TYPELESS)
        for t in ctrl.GetTextures():
            if t.arraysize == 2 and "R16_TYPELESS" in t.format.Name():
                if t.width == 2048:
                    tex[int(t.resourceId)] = TEX_NAMES[0]
                elif t.width == 256:
                    tex[int(t.resourceId)] = TEX_NAMES[1]
    views = {}  # view-id -> (texname, beschreibung)
    for r in resources:
        parents = [int(p) for p in r.parentResources]
        for p in parents:
            if p in tex:
                first = size = "?"
                for ci in r.initialisationChunks:
                    ch = sdfile.chunks[ci]
                    stack = [ch]
                    while stack:
                        o = stack.pop()
                        if o.name == "FirstArraySlice":
                            first = sd_value(o)
                        if o.name == "ArraySize":
                            size = sd_value(o)
                        for i in range(o.NumChildren()):
                            stack.append(o.GetChild(i))
                views[int(r.resourceId)] = (tex[p], "%s Ebene %s+%s" % (r.name, first, size))
    log("Texturen:", tex)
    if not tex:
        cand = [r.name for r in resources if "shadow" in r.name.lower() or "ESRAM" in r.name]
        log("Keine Namens-Treffer. Ressourcen gesamt:", len(resources), "Kandidaten:", cand[:40])
        big = [(r.name, int(r.resourceId)) for r in resources if r.type == rd.ResourceType.Texture][:60]
        log("Erste Texturen:", big)
    for v, (t, d) in views.items():
        log("  View", v, t, d)
    watch = set(tex) | set(views)

    flat = []
    flatten(ctrl.GetRootActions(), [], sdfile, flat)

    log("\n== Zeitlicher Ablauf (nur Ereignisse mit Bezug zu den Texturen) ==")
    cur_view = None
    draws_in_view = 0

    def flush():
        nonlocal draws_in_view
        if draws_in_view:
            log("      ... %d Draws in %s" % (draws_in_view, views.get(cur_view, ("?", str(cur_view)))[1]))
        draws_in_view = 0

    for a, name, marker in flat:
        # alle API-Events dieser Action (auch State-Setzen davor) nach Bezuegen durchsuchen
        for ev in a.events:
            ch = sdfile.chunks[ev.chunkIndex]
            cname = ch.name
            ids = sd_ids(ch, set()) & watch
            if not ids:
                continue
            if "OMSetRenderTargets" in cname:
                for i in ids:
                    if i in views and i != cur_view:
                        flush()
                        cur_view = i
                continue
            if "SetShaderResources" in cname:
                flush()
                log("EID %6d LESEN  %-28s %s | %s" % (ev.eventId, cname.split("::")[-1], [names.get(i, i) for i in ids], marker[-110:]))
                continue
            if "Clear" in cname:
                flush()
                log("EID %6d CLEAR  %s | %s" % (ev.eventId, [views.get(i, (names.get(i, i), ""))[1] for i in ids], marker[-110:]))
                continue
            if "Copy" in cname:
                flush()
                allids = sd_ids(ch, set())
                log("EID %6d KOPIE  %s alle IDs %s | %s" % (ev.eventId, cname.split("::")[-1], [names.get(i, i) for i in allids], marker[-110:]))
                continue
        if a.flags & rd.ActionFlags.Drawcall and int(a.depthOut) in tex:
            draws_in_view += 1
    flush()
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
    analyze(ctrl)
    ctrl.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    log(traceback.format_exc())
finally:
    out.close()
    os._exit(0)
