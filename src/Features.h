#pragma once

// Status der Engine-Eingriffe: Jeder Eingriff mitten in Spielcode prueft vor dem Installieren, ob der Code an der
// Stelle so aussieht wie erwartet. Passt er nicht (andere Spielversion, andere Mod an derselben Stelle), wird nur
// dieses eine Feature abgeschaltet - nicht das ganze Plugin. Das Menue zeigt die abgeschalteten Features an.
namespace Features
{
	struct Entry
	{
		std::string name;    // englischer Name (Log + Menue)
		std::string nameDe;  // deutscher Name (Menue)
		bool        active;
		std::string reason;  // warum nicht aktiv
	};

	void Report(std::string_view a_name, std::string_view a_nameDe, bool a_active, std::string_view a_reason = {});

	std::vector<Entry> List();

	// Aufrufstelle pruefen: call rel32 (E8) - optional auf ein bestimmtes Ziel
	bool IsCall(std::uintptr_t a_site, std::uintptr_t a_target = 0) noexcept;

	// Spielversion ist die getestete (1.6.1170)?
	bool TestedVersion() noexcept;
}
