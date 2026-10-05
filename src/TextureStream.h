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
namespace TextureStream
{
	// Einmal pro Frame (Main-Thread)
	void OnFrame();
}
