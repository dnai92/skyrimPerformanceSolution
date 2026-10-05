#pragma once

// Texture Streaming - Stufe 0 (nur messen, keine Aenderung am Bild):
// Einmal pro Sekunde wird ein Frame ausgewertet: fuer jede sichtbare Textur (alle Texturen der Materialien sichtbarer
// Objekte) wird berechnet, wie gross sie beim naechsten Objekt auf dem Bildschirm erscheint und welche Mip-Stufe damit
// wirklich gebraucht wird. Der 10-s-Bericht zeigt VRAM der sichtbaren Texturen und wie viel Streaming sparen wuerde.
//
// Benoetigte Aufloesung ~ Bildschirm-Durchmesser des Objekts x Sicherheitsfaktor. Konservativ: Texturen, die sich auf
// einem Objekt mehrfach wiederholen, brauchen in Wahrheit noch weniger.
namespace TextureStream
{
	// Aus BSLightingShaderProperty::GetRenderPasses (Hauptszene), nur in Mess-Frames aktiv
	void OnGeometry(RE::BSGeometry* a_geometry, RE::BSLightingShaderProperty* a_property) noexcept;

	// Einmal pro Frame (Main-Thread)
	void OnFrame();
}
