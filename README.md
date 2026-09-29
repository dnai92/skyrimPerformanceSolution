# SkyrimPerf

SKSE-Profiling-Plugin für Skyrim AE (getestet gegen 1.6.1170). Misst die CPU-Seite des Main-Threads,
um Performance-Engpässe gezielt zu finden, statt zu raten.

## Was gemessen wird

| Messpunkt | Hook (vtable, Address Library) | Bedeutung |
|---|---|---|
| Frame | `PlayerCharacter::Update` (0xAD) | 1× pro Spiel-Frame → Frame-Grenze |
| Player Update | `PlayerCharacter::Update` (0xAD) | Update des Spielers |
| NPC Actor::Update | `Character::Update` (0xAD) | Summe aller aktiven NPCs pro Frame (in Tracy mit Namen) |
| Papyrus VM Update | `VirtualMachine::Update` (0x04) | Skriptausführung pro Frame |
| Papyrus Tasklets | `VirtualMachine::UpdateTasklets` (0x05) | Latente Native-Calls |
| VM overstressed | `VirtualMachine::SetOverstressed` (0x06) | Papyrus-Überlast-Ereignisse |
| Cell-Loads, Menüs | Event-Sinks | Markierungen in der Tracy-Zeitleiste |

Keine festen Offsets, nur vtable-Einträge aus CommonLibSSE-NG → robust gegenüber Spielupdates.

## Ausgaben

1. **Log** – `Documents\My Games\Skyrim Special Edition\SKSE\SkyrimPerf.log`: alle 10 s eine Zusammenfassung
   (FPS, Frame avg/p99/max, Anteil jeder Zone an der Frame-Zeit).
2. **CSV** – `...\SKSE\SkyrimPerf.csv`: dieselben Werte maschinenlesbar (wird bei jedem Spielstart neu angelegt).
3. **Tracy** (optional) – Live-Zeitleiste. Tracy-Profiler **v0.14.x** starten
   (https://github.com/wolfpld/tracy/releases), Spiel starten, im Profiler auf *Connect* (localhost).
   Solange kein Profiler verbunden ist, sammelt das Plugin keine Tracy-Daten (`TRACY_ON_DEMAND`).

**Nicht erfasst:** GPU-Zeiten, Render-Submission, FSMP/CBPC-Physik. Deren Anteil steckt in
„Frame-Zeit minus gemessene Zonen“. Für CPU vs. GPU: Intel PresentMon („GPU Busy“).

## Bauen

Voraussetzungen: VS 2022 Build Tools (C++), vcpkg unter `C:\dev\vcpkg`.

```bat
git submodule update --init --recursive
build.cmd
powershell -File package.ps1
```

Ergebnis: `dist\SkyrimPerf-<version>.zip` → in Vortex per Drag & Drop als Mod installieren.
Zum Deaktivieren die Mod in Vortex ausschalten.
