#pragma once

// Schatten-Cache fester Lichter (Test, [LightShadowCache] bEnabled).
// Fackeln, Kamine, Lampen: Ihre Schattenkarten werden jeden Frame komplett neu gezeichnet, obwohl sich bei einem
// ruhenden Licht fast nichts aendert (Drachenfeste: ~3.400 Meshes je Karte, ~4 ms GPU fuer alle Fackelschatten).
// Pro Karte wird eine Kopie mit nur den unbeweglichen Meshes gehalten. In jedem Frame wird sie in den gerade
// zugeteilten Platz der Schattenkarten-Sammlung kopiert, gezeichnet werden nur noch Figuren, an Figuren haengende
// Meshes, Effekte und gelernte Bewegliche. Flackernde (sich bewegende) Lichter werden nie zwischengespeichert.
// Neu aufgebaut wird, wenn sich ein unbewegliches Mesh bewegt (es gilt ab dann als beweglich), hinzukommt oder
// wegfaellt. Ablauf in der Engine (ID 107604, je Schattenkarte): Platz zuteilen, leeren, zeichnen (ID 106436),
// Matrix berechnen - der Cache haengt sich an den Zeichen-Aufruf.
namespace LightShadowCache
{
	// Aufrufstelle des Zeichnens patchen (Plugin-Start)
	void Install();

	// Frame-Beginn (Main-Thread): letzten Frame auswerten, Modus je Schattenkarte fuer diesen Frame festlegen
	void OnFrame();

	// Culling-Jobs: Mesh kommt in die Schattenkarte dieser Kamera. true = nicht zeichnen (steckt im Cache)
	bool FilterAppend(const RE::NiCamera* a_camera, RE::BSGeometry& a_geom) noexcept;

	// Bildausgabe (IDXGISwapChain::Present, jedes gezeichnete Bild - auch in Pause/Menues)
	void OnPresent() noexcept;

	// Ladebildschirm / Hauptmenue: alles verwerfen
	void Reset();

	// Analyse-Bericht
	void Report();
}
