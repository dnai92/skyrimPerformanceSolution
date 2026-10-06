#include "Features.h"

#include <mutex>

namespace Features
{
	namespace
	{
		std::mutex         g_lock;
		std::vector<Entry> g_entries;
	}

	void Report(std::string_view a_name, std::string_view a_nameDe, bool a_active, std::string_view a_reason)
	{
		{
			std::scoped_lock lock(g_lock);
			g_entries.push_back({ std::string(a_name), std::string(a_nameDe), a_active, std::string(a_reason) });
		}
		if (!a_active) {
			logger::warn("Feature abgeschaltet: {} - {}", a_name, a_reason);
		}
	}

	std::vector<Entry> List()
	{
		std::scoped_lock lock(g_lock);
		return g_entries;
	}

	bool IsCall(std::uintptr_t a_site, std::uintptr_t a_target) noexcept
	{
		const auto* code = reinterpret_cast<const std::uint8_t*>(a_site);
		if (code[0] != 0xE8) {
			return false;
		}
		return a_target == 0 || a_site + 5 + *reinterpret_cast<const std::int32_t*>(a_site + 1) == a_target;
	}

	bool TestedVersion() noexcept
	{
		return REL::Module::get().version() == REL::Version{ 1, 6, 1170, 0 };
	}

	bool VerifiedVersion() noexcept
	{
		const auto v = REL::Module::get().version();
		return v == REL::Version{ 1, 5, 97, 0 } || v == REL::Version{ 1, 6, 640, 0 } || v == REL::Version{ 1, 7, 104, 0 };
	}
}
