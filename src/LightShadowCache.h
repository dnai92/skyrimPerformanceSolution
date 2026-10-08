#pragma once

// Schatten-Cache fester Lichter (Test, [LightShadowCache] bEnabled).
// Fackeln, Kamine, Lampen: Ihre Schattenkarten werden jeden Frame komplett neu gezeichnet, obwohl sich bei einem
// ruhenden Licht fast nichts aendert (Drachenfeste: ~3.400 Meshes je Karte, ~4 ms GPU fuer alle Fackelschatten).
// Pro Karte wird eine Kopie mit nur den unbeweglichen Meshes gehalten. In jedem Frame wird sie in den gerade
// zugeteilten Platz der Schattenkarten-Sammlung kopiert, gezeichnet werden nur noch Figuren, an Figuren haengende
// Meshes, Effekte und gelernte Bewegliche. Eingesammelt wird alles wie ohne Cache, weggelassen wird erst beim
// einzelnen Draw: das Einsammeln hat Nebenwirkungen (bis 1.0.33 ging dadurch das Licht aus).
// 1.0.35 filterte schon bei der Uebergabe in die Zeichenliste (RegisterObject, ID 106567) - Licht flackerte ebenso:
// die Engine wertet die Zeichenliste der Karte noch fuer etwas anderes aus. Deshalb bleibt es beim Draw-Filter. Flackernde (sich bewegende) Lichter werden nie zwischengespeichert.
// Neu aufgebaut wird, wenn sich ein unbewegliches Mesh bewegt (es gilt ab dann als beweglich), hinzukommt oder
// wegfaellt. Ablauf in der Engine (ID 107604, je Schattenkarte): Platz zuteilen, leeren, zeichnen (ID 106436),
// Matrix berechnen - der Cache haengt sich an den Zeichen-Aufruf.
namespace LightShadowCache
{
	// Aufrufstelle des Zeichnens patchen (Plugin-Start)
	void Install();

	// Frame-Beginn (Main-Thread): letzten Frame auswerten, Modus je Schattenkarte fuer diesen Frame festlegen
	void OnFrame();

	// Einzelner Draw (RenderPassImmediately, Render-Thread) waehrend eine Schattenkarte gezeichnet wird.
	// true = nicht zeichnen (steckt im Cache bzw. Figur im Aufbau-Frame)
	bool SkipDraw(const RE::BSRenderPass& a_pass) noexcept;

	// BSUtilityShader::SetupGeometry: jeder Utility-Draw (Abdeckungs-Kontrolle des Draw-Filters)
	void OnUtilityDraw() noexcept;

	// Draw-Filter (RenderPassImmediately-Aufrufstellen) installiert - ohne ihn bleibt der Cache aus
	void SetDrawFilterInstalled() noexcept;

	// Bildausgabe (IDXGISwapChain::Present, jedes gezeichnete Bild - auch in Pause/Menues)
	void OnPresent() noexcept;

	// Ladebildschirm / Hauptmenue: alles verwerfen
	void Reset();

	// Neuaufbauten seit dem letzten Aufruf (Ruckler-Zeile)
	std::uint32_t TakeFrameBuilds() noexcept;

	// Analyse-Bericht
	void Report();
}
