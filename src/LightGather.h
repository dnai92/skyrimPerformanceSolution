#pragma once

// Licht-Geometrie-Zuordnung drosseln.
// Die Engine durchsucht in jedem Frame fuer JEDES dynamische Punktlicht (getragene Fackeln, flackernde Lichter, ...)
// den Weltbaum nach Objekten im Lichtradius (ShadowSceneNode-Lichtschleife ID 106335 -> ID 106342 -> 106401/106400
// -> 108281/108303/108302). Am Weisslauf-Markt ~10 % der Hauptthread-Zeit (Tracy).
// Hat sich ein Licht seit der letzten Suche kaum bewegt und sein Radius kaum geaendert, behaelt es seine
// bisherige Objektliste; spaetestens nach fMaxAgeMs wird trotzdem neu gesucht.
namespace LightGather
{
	void Install();
}
