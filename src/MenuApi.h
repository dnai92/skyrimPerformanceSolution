#pragma once

// Eigene, schlanke Anbindung an SKSE Menu Framework (optionale Laufzeit-Abhaengigkeit).
// Das Framework exportiert Dear-ImGui-Funktionen unter ihren cimgui-Namen (igButton, igCheckbox, ...) und einige
// eigene Funktionen (AddSectionItem, GetMenuFrameworkVersion). Wir holen nur die benoetigten per GetProcAddress;
// fehlt das Framework oder eine Funktion, passiert nichts. Tastencodes: Dear ImGui (MIT), Version des Frameworks.

#include <cstdarg>
#include <filesystem>
#include <string>

namespace MenuApi
{
	namespace detail
	{
		inline HMODULE Module() noexcept
		{
			static HMODULE module = GetModuleHandleW(L"SKSEMenuFramework");
			return module;
		}

		template <class F>
		F Get(const char* a_name) noexcept
		{
			const auto module = Module();
			return module ? reinterpret_cast<F>(GetProcAddress(module, a_name)) : nullptr;
		}
	}

	// ---- Framework ----

	using RenderFunction = void(__stdcall*)();

	inline bool IsInstalled()
	{
		std::error_code ec;
		return std::filesystem::exists("Data/SKSE/Plugins/SKSEMenuFramework.dll", ec);
	}

	inline float Version()
	{
		static const auto f = detail::Get<float (*)()>("GetMenuFrameworkVersion");
		return f ? f() : 0.0f;
	}

	// a_path: "Bereich/Eintrag" (Schraegstrich trennt die Ebenen)
	inline void AddSectionItem(const std::string& a_path, RenderFunction a_render)
	{
		static const auto f = detail::Get<void (*)(const char*, RenderFunction)>("AddSectionItem");
		if (f) {
			f(a_path.c_str(), a_render);
		}
	}

	// ---- Bedienelemente (Dear ImGui ueber das Framework) ----

	struct Vec2
	{
		float x = 0.0f, y = 0.0f;
	};

	// Dear ImGui ImGuiKey (benannte Tasten ab 512)
	enum Key : int
	{
		Key_None = 0,
		Key_Tab = 512,
		Key_LeftArrow,
		Key_RightArrow,
		Key_UpArrow,
		Key_DownArrow,
		Key_PageUp,
		Key_PageDown,
		Key_Home,
		Key_End,
		Key_Insert,
		Key_Delete,
		Key_Backspace,
		Key_Space,
		Key_Enter,
		Key_Escape,
		Key_LeftCtrl,
		Key_LeftShift,
		Key_LeftAlt,
		Key_LeftSuper,
		Key_RightCtrl,
		Key_RightShift,
		Key_RightAlt,
		Key_RightSuper,
		Key_Menu,
		Key_0 = 536,
		Key_9 = 545,
		Key_A = 546,
		Key_Z = 571,
		Key_F1 = 572,
		Key_F24 = 595,
		Key_Apostrophe = 596,
		Key_Comma,
		Key_Minus,
		Key_Period,
		Key_Slash,
		Key_Semicolon,
		Key_Equal,
		Key_LeftBracket,
		Key_Backslash,
		Key_RightBracket,
		Key_GraveAccent,
		Key_CapsLock,
		Key_ScrollLock,
		Key_NumLock,
		Key_PrintScreen,
		Key_Pause,
		Key_Keypad0 = 612,
		Key_Keypad9 = 621,
	};

	inline bool Button(const char* a_label)
	{
		static const auto f = detail::Get<bool (*)(const char*, Vec2)>("igButton");
		return f && f(a_label, Vec2{});
	}

	inline bool Checkbox(const char* a_label, bool* a_value)
	{
		static const auto f = detail::Get<bool (*)(const char*, bool*)>("igCheckbox");
		return f && f(a_label, a_value);
	}

	inline bool SliderFloat(const char* a_label, float* a_value, float a_min, float a_max, const char* a_format = "%.3f")
	{
		static const auto f = detail::Get<bool (*)(const char*, float*, float, float, const char*, int)>("igSliderFloat");
		return f && f(a_label, a_value, a_min, a_max, a_format, 0);
	}

	inline void SameLine()
	{
		static const auto f = detail::Get<void (*)(float, float)>("igSameLine");
		if (f) {
			f(0.0f, -1.0f);
		}
	}

	inline void SeparatorText(const char* a_label)
	{
		static const auto f = detail::Get<void (*)(const char*)>("igSeparatorText");
		if (f) {
			f(a_label);
		}
	}

	inline void TextUnformatted(const char* a_text)
	{
		static const auto f = detail::Get<void (*)(const char*, const char*)>("igTextUnformatted");
		if (f) {
			f(a_text, nullptr);
		}
	}

	namespace detail
	{
		using FormatV = void (*)(const char*, va_list);

		inline void CallV(const char* a_export, const char* a_fmt, va_list a_args)
		{
			if (const auto f = Get<FormatV>(a_export)) {
				f(a_fmt, a_args);
			}
		}
	}

	inline void Text(const char* a_fmt, ...)
	{
		va_list args;
		va_start(args, a_fmt);
		detail::CallV("igTextV", a_fmt, args);
		va_end(args);
	}

	inline void TextWrapped(const char* a_fmt, ...)
	{
		va_list args;
		va_start(args, a_fmt);
		detail::CallV("igTextWrappedV", a_fmt, args);
		va_end(args);
	}

	inline void BulletText(const char* a_fmt, ...)
	{
		va_list args;
		va_start(args, a_fmt);
		detail::CallV("igBulletTextV", a_fmt, args);
		va_end(args);
	}

	inline void SetTooltip(const char* a_fmt, ...)
	{
		va_list args;
		va_start(args, a_fmt);
		detail::CallV("igSetTooltipV", a_fmt, args);
		va_end(args);
	}

	inline bool IsItemHovered()
	{
		static const auto f = detail::Get<bool (*)(int)>("igIsItemHovered");
		return f && f(0);
	}

	inline void PushID(const char* a_id)
	{
		static const auto f = detail::Get<void (*)(const char*)>("igPushID_Str");
		if (f) {
			f(a_id);
		}
	}

	inline void PopID()
	{
		static const auto f = detail::Get<void (*)()>("igPopID");
		if (f) {
			f();
		}
	}

	inline bool IsKeyPressed(Key a_key, bool a_repeat)
	{
		static const auto f = detail::Get<bool (*)(int, bool)>("igIsKeyPressed_Bool");
		return f && f(a_key, a_repeat);
	}

	inline void SetNextFrameWantCaptureKeyboard(bool a_capture)
	{
		static const auto f = detail::Get<void (*)(bool)>("igSetNextFrameWantCaptureKeyboard");
		if (f) {
			f(a_capture);
		}
	}

	// Erste in diesem Frame gedrueckte benannte Taste (Tab .. Ziffernblock 9), sonst Key_None
	inline Key FindPressedKey()
	{
		for (int k = Key_Tab; k <= Key_Keypad9; ++k) {
			if (IsKeyPressed(static_cast<Key>(k), false)) {
				return static_cast<Key>(k);
			}
		}
		return Key_None;
	}
}
