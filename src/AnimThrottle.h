#pragma once

// Experiment (Entwickler-Test, Standard AUS): Animation ferner bzw. nicht sichtbarer NPCs seltener aktualisieren.
// Hook auf Character::UpdateAnimation (vfunc 0x7D). Bei uebersprungenen Frames wird die Zeit gesammelt und beim
// naechsten Update am Stueck nachgereicht - Bewegung (Root Motion) und Ablauf bleiben gleich, nur in groesseren
// Schritten. Nie gedrosselt: nahe Figuren, Kampf, Begleiter, Reiten, Killmoves, Moebel (Sitzen, Szenen).
namespace AnimThrottle
{
	void Install();

	// Einmal pro Frame (Main-Thread): Zaehler auswerten, Log alle 10 s
	void OnFrame() noexcept;
}
