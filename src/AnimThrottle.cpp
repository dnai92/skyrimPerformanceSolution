#include "AnimThrottle.h"

#include "Config.h"

namespace AnimThrottle
{
	namespace
	{
		constexpr float kMaxCollected = 0.25f;  // spaetestens nach dieser gesammelten Zeit (s) wird aktualisiert

		struct Slot
		{
			std::atomic<const void*> actor{ nullptr };
			float                    collected = 0.0f;  // nur der Thread, der die Figur gerade aktualisiert, schreibt
			std::uint32_t            count = 0;
		};
		std::array<Slot, 2048> g_slots{};

		// Sichtfeld der Hauptkamera (vom Main-Thread je Frame gesetzt, von Update-Threads gelesen; ein Frame alt ist egal)
		struct View
		{
			float n[4][3]{};
			float d[4]{};
			float px = 0, py = 0, pz = 0;
			bool  valid = false;
		};
		std::array<View, 2>     g_views{};
		std::atomic<int>        g_viewIdx{ 0 };

		void UpdateView() noexcept
		{
			const int next = 1 - g_viewIdx.load(std::memory_order_relaxed);
			auto&     v = g_views[next];
			v.valid = false;
			const auto cam = RE::Main::WorldRootCamera();
			if (cam) {
				const auto& fr = cam->GetRuntimeData2().viewFrustum;
				const auto& r = cam->world.rotate;
				const auto& p = cam->world.translate;
				v.px = p.x;
				v.py = p.y;
				v.pz = p.z;
				const float w = fr.fRight - fr.fLeft, h = fr.fTop - fr.fBottom;
				if (!fr.bOrtho && w > 0.0f && h > 0.0f) {
					// Kamera-Spalte 0 = Blickrichtung, 1/2 = oben/rechts (Vorzeichen egal bei symmetrischem Frustum)
					const float D[3]{ r.entry[0][0], r.entry[1][0], r.entry[2][0] };
					const float U[3]{ r.entry[0][1], r.entry[1][1], r.entry[2][1] };
					const float R[3]{ r.entry[0][2], r.entry[1][2], r.entry[2][2] };
					const float a = std::max(std::abs(fr.fRight), std::abs(fr.fLeft)), t = std::max(std::abs(fr.fTop), std::abs(fr.fBottom));
					const float planes[4][3]{
						{ a * D[0] - R[0], a * D[1] - R[1], a * D[2] - R[2] },
						{ a * D[0] + R[0], a * D[1] + R[1], a * D[2] + R[2] },
						{ t * D[0] - U[0], t * D[1] - U[1], t * D[2] - U[2] },
						{ t * D[0] + U[0], t * D[1] + U[1], t * D[2] + U[2] },
					};
					v.valid = true;
					for (int i = 0; i < 4 && v.valid; ++i) {
						const float len = std::sqrt(planes[i][0] * planes[i][0] + planes[i][1] * planes[i][1] + planes[i][2] * planes[i][2]);
						if (!(len > 1e-4f)) {
							v.valid = false;
							break;
						}
						for (int k = 0; k < 3; ++k) {
							v.n[i][k] = planes[i][k] / len;
						}
						v.d[i] = -(v.n[i][0] * p.x + v.n[i][1] * p.y + v.n[i][2] * p.z);
					}
				}
			}
			g_viewIdx.store(next, std::memory_order_release);
		}

		// Huellkugel komplett ausserhalb des Sichtfelds (mit Rand fuer Bewegung im selben Frame)?
		bool OutsideView(const View& a_v, const RE::NiBound& a_b) noexcept
		{
			if (!a_v.valid) {
				return false;
			}
			const float r = a_b.radius + 200.0f;
			for (int i = 0; i < 4; ++i) {
				if (a_v.n[i][0] * a_b.center.x + a_v.n[i][1] * a_b.center.y + a_v.n[i][2] * a_b.center.z + a_v.d[i] < -r) {
					return true;
				}
			}
			return false;
		}

		std::atomic<std::uint64_t>            g_updated{ 0 }, g_skipped{ 0 }, g_hidden{ 0 }, g_far{ 0 }, g_frames{ 0 };
		std::chrono::steady_clock::time_point g_lastLog = std::chrono::steady_clock::now();

		// Platz der Figur (lineares Sondieren, beim ersten Mal belegen); nullptr = Tabelle voll
		Slot* SlotOf(const void* a_actor) noexcept
		{
			const auto h = (reinterpret_cast<std::uintptr_t>(a_actor) >> 4) * 0x9E3779B97F4A7C15ull;
			for (std::size_t i = 0; i < 16; ++i) {
				auto&       s = g_slots[(h + i) % g_slots.size()];
				const void* cur = s.actor.load(std::memory_order_acquire);
				if (cur == a_actor) {
					return &s;
				}
				if (!cur && s.actor.compare_exchange_strong(cur, a_actor, std::memory_order_acq_rel)) {
					s.collected = 0.0f;
					s.count = 0;
					return &s;
				}
				if (cur == a_actor) {
					return &s;
				}
			}
			return nullptr;
		}

		bool Excluded(RE::Actor* a_actor) noexcept
		{
			return a_actor->IsInCombat() || a_actor->IsPlayerTeammate() || a_actor->IsOnMount() || a_actor->IsBeingRidden() || a_actor->IsInKillMove() ||
			       a_actor->GetOccupiedFurniture();
		}

		// Wie oft aktualisieren? 1 = jeden Frame
		std::uint32_t Interval(RE::Actor* a_actor) noexcept
		{
			const auto& cfg = Config::animThrottle;
			const auto  obj = a_actor->Get3D();
			if (!obj) {
				return 1;
			}
			const auto& b = obj->worldBound;
			const auto& v = g_views[g_viewIdx.load(std::memory_order_acquire)];
			const float dx = b.center.x - v.px, dy = b.center.y - v.py, dz = b.center.z - v.pz;
			const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (dist < cfg.minDistance || Excluded(a_actor)) {
				return 1;
			}
			if (OutsideView(v, b)) {
				g_hidden.fetch_add(1, std::memory_order_relaxed);
				return cfg.hiddenInterval;
			}
			if (dist > cfg.farDistance) {
				g_far.fetch_add(1, std::memory_order_relaxed);
				return cfg.farInterval;
			}
			return 1;
		}

		struct UpdateAnimation
		{
			static void thunk(RE::Character* a_this, float a_delta)
			{
				if (!Config::animThrottle.enabled || !Config::masterEnabled.load(std::memory_order_relaxed) || !a_this) {
					func(a_this, a_delta);
					return;
				}
				const auto interval = Interval(a_this);
				Slot*      s = SlotOf(a_this);
				if (!s) {
					func(a_this, a_delta);
					g_updated.fetch_add(1, std::memory_order_relaxed);
					return;
				}
				s->collected += a_delta;
				if (interval > 1 && (++s->count % interval) != 0 && s->collected < kMaxCollected) {
					g_skipped.fetch_add(1, std::memory_order_relaxed);
					return;  // Zeit sammeln, naechstes Mal am Stueck
				}
				const float delta = s->collected;
				s->collected = 0.0f;
				func(a_this, delta);
				g_updated.fetch_add(1, std::memory_order_relaxed);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_Character[0] };
		UpdateAnimation::func = vtbl.write_vfunc(0x7D, UpdateAnimation::thunk);
		logger::info("Hook installed: Character::UpdateAnimation (vfunc 0x7D) - animation throttle (developer test)");
	}

	void OnFrame() noexcept
	{
		UpdateView();
		g_frames.fetch_add(1, std::memory_order_relaxed);
		const auto now = std::chrono::steady_clock::now();
		if (now - g_lastLog < std::chrono::seconds(10)) {
			return;
		}
		g_lastLog = now;
		const auto frames = g_frames.exchange(0);
		const auto updated = g_updated.exchange(0), skipped = g_skipped.exchange(0), hidden = g_hidden.exchange(0), farCount = g_far.exchange(0);
		if (!Config::animThrottle.enabled || frames == 0) {
			return;
		}
		const double f = static_cast<double>(frames);
		logger::info("[AnimThrottle] per frame: updated {:.1f}, skipped {:.1f} | throttled as hidden {:.1f}, as far {:.1f}", updated / f, skipped / f, hidden / f, farCount / f);
	}
}
