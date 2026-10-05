#pragma once

// Texture Streaming - Stufe 1: Texturen ferner Objekte im VRAM verkleinern, bei Annaeherung neu laden.
//
// Bedarf: In kleinen Zeitscheiben (fBudgetMs pro Frame) wird die ganze geladene Szene durchlaufen - auch was hinter
// der Kamera liegt. Je Textur zaehlt das naechste Objekt, das sie benutzt: benoetigte Kantenlaenge ~ Bildschirm-
// Durchmesser des Objekts x Sicherheitsfaktor (mindestens fMinEdge).
//
// Verkleinern (Main-Thread, Millisekunden): neue Textur ohne die oberen Mip-Stufen anlegen, die kleineren Stufen
// per GPU-Kopie uebernehmen, im BSGraphics::Texture austauschen. Erst nach zwei Durchlaeufen mit geringem Bedarf.
// Vergroessern (Hintergrund-Thread): DDS aus Datei/BSA lesen (BSResource), Textur mit den benoetigten Stufen anlegen,
// Austausch im Main-Thread (SKSE-Task, laeuft auch in Menues). Vorher wird der Datei-Kopf geprueft (Groesse, Format,
// Mip-Anzahl); nur Texturen, die sich garantiert wieder herstellen lassen, werden verkleinert.
//
// Nie veraendert: Dateien auf der Platte; Pfade aus sExclude (Oberflaeche, Karten, LOD, Schriften, Buecher ...).
// Inventar-/Handels-/Schmiede-Vorschau: Texturen der gezeigten Modelle werden sofort in voller Groesse geladen.
//
// Stufe 3 - gleich verkleinert laden: Das Spiel laedt DDS-Dateien ueber eine DirectXTK-Variante (ID 77533 ->
// 77539 CreateTextureFromDDS) mit Parameter maxsize, den es immer auf 0 (= unbegrenzt) setzt. Wir merken uns je Pfad
// die zuletzt benoetigte Kantenlaenge (auch ueber Spielsitzungen, SkyrimPerf_TextureSizes.txt im SKSE-Log-Ordner)
// und geben sie beim naechsten Laden als maxsize mit -> die oberen Mip-Stufen werden gar nicht erst gelesen/angelegt.
// Kette: NiSourceTexture-Ladefunktion ID 108531 (+0x44) -> ID 77301 (+0x62) -> ID 77533.
namespace TextureStream
{
	// Lade-Hooks + gemerkte Groessen (beim Laden des Plugins)
	void Install();

	// Einmal pro Frame (Main-Thread)
	void OnFrame();

	// VRAM-Belegung des Spiels und Budget laut Windows (Bytes; 0 = unbekannt)
	void GetVram(std::uint64_t& a_usage, std::uint64_t& a_budget);
}
