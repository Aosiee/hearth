#pragma once

// What the OS told the program, as a value: Platform::Pump collects them
// from every window, in the order they happened, and the program takes them
// with Platform::TakeEvents.

#include <cstdint>
#include <string>
#include <vector>

namespace hearth::platform {

	class Window;

	enum class EventType : uint8_t
	{
		WindowResize,       // Width, Height (the window's size in screen coordinates)
		WindowMove,         // X, Y
		WindowClose,        // the user asked to close the window
		WindowFocus,        // Focused
		WindowContentScale, // ScaleX, ScaleY
		CursorEnter,        // Entered
		Key,                // KeyCode, Scancode, KeyAction, Mods
		Char,               // Codepoint (the text typed, as Unicode)
		MouseButton,        // Button, KeyAction, Mods
		Scroll,             // ScrollX, ScrollY (wheel notches)
		CursorMove,         // CursorX, CursorY (screen coordinates in the window)
		Drop,               // Paths (files dropped on the window)
	};

	enum class Action : uint8_t { Release = 0, Press = 1, Repeat = 2 };

	struct Event
	{
		EventType Type = EventType::WindowClose;
		// The window it happened in.
		Window* Source = nullptr;

		int Width = 0, Height = 0;
		int X = 0, Y = 0;
		float ScaleX = 1.0f, ScaleY = 1.0f;
		bool Focused = false;
		bool Entered = false;
		int KeyCode = 0;
		int Scancode = 0;
		int Button = 0;
		Action KeyAction = Action::Release;
		int Mods = 0;
		uint32_t Codepoint = 0;
		double ScrollX = 0.0, ScrollY = 0.0;
		double CursorX = 0.0, CursorY = 0.0;
		std::vector<std::string> Paths;
	};

} // namespace hearth::platform
