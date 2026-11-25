#pragma once

// Key, mouse-button, modifier and gamepad codes. Their values are GLFW's,
// which hearth/platform passes through unchanged, so a host whose own codes
// already use GLFW's values (TinyEngine's TE_KEY_*) needs no table; the suite
// checks every value against GLFW's header.

namespace hearth::platform {

	// Keyboard keys (layout-independent: the US-layout key at that position).
	namespace key {
		constexpr int Unknown = -1;
		constexpr int Space = 32;
		constexpr int Apostrophe = 39;
		constexpr int Comma = 44;
		constexpr int Minus = 45;
		constexpr int Period = 46;
		constexpr int Slash = 47;
		constexpr int Num0 = 48;
		constexpr int Num1 = 49;
		constexpr int Num2 = 50;
		constexpr int Num3 = 51;
		constexpr int Num4 = 52;
		constexpr int Num5 = 53;
		constexpr int Num6 = 54;
		constexpr int Num7 = 55;
		constexpr int Num8 = 56;
		constexpr int Num9 = 57;
		constexpr int Semicolon = 59;
		constexpr int Equal = 61;
		constexpr int A = 65;
		constexpr int B = 66;
		constexpr int C = 67;
		constexpr int D = 68;
		constexpr int E = 69;
		constexpr int F = 70;
		constexpr int G = 71;
		constexpr int H = 72;
		constexpr int I = 73;
		constexpr int J = 74;
		constexpr int K = 75;
		constexpr int L = 76;
		constexpr int M = 77;
		constexpr int N = 78;
		constexpr int O = 79;
		constexpr int P = 80;
		constexpr int Q = 81;
		constexpr int R = 82;
		constexpr int S = 83;
		constexpr int T = 84;
		constexpr int U = 85;
		constexpr int V = 86;
		constexpr int W = 87;
		constexpr int X = 88;
		constexpr int Y = 89;
		constexpr int Z = 90;
		constexpr int LeftBracket = 91;
		constexpr int Backslash = 92;
		constexpr int RightBracket = 93;
		constexpr int GraveAccent = 96;
		constexpr int World1 = 161;
		constexpr int World2 = 162;
		constexpr int Escape = 256;
		constexpr int Enter = 257;
		constexpr int Tab = 258;
		constexpr int Backspace = 259;
		constexpr int Insert = 260;
		constexpr int Delete = 261;
		constexpr int Right = 262;
		constexpr int Left = 263;
		constexpr int Down = 264;
		constexpr int Up = 265;
		constexpr int PageUp = 266;
		constexpr int PageDown = 267;
		constexpr int Home = 268;
		constexpr int End = 269;
		constexpr int CapsLock = 280;
		constexpr int ScrollLock = 281;
		constexpr int NumLock = 282;
		constexpr int PrintScreen = 283;
		constexpr int Pause = 284;
		constexpr int F1 = 290;
		constexpr int F2 = 291;
		constexpr int F3 = 292;
		constexpr int F4 = 293;
		constexpr int F5 = 294;
		constexpr int F6 = 295;
		constexpr int F7 = 296;
		constexpr int F8 = 297;
		constexpr int F9 = 298;
		constexpr int F10 = 299;
		constexpr int F11 = 300;
		constexpr int F12 = 301;
		constexpr int F13 = 302;
		constexpr int F14 = 303;
		constexpr int F15 = 304;
		constexpr int F16 = 305;
		constexpr int F17 = 306;
		constexpr int F18 = 307;
		constexpr int F19 = 308;
		constexpr int F20 = 309;
		constexpr int F21 = 310;
		constexpr int F22 = 311;
		constexpr int F23 = 312;
		constexpr int F24 = 313;
		constexpr int F25 = 314;
		constexpr int Keypad0 = 320;
		constexpr int Keypad1 = 321;
		constexpr int Keypad2 = 322;
		constexpr int Keypad3 = 323;
		constexpr int Keypad4 = 324;
		constexpr int Keypad5 = 325;
		constexpr int Keypad6 = 326;
		constexpr int Keypad7 = 327;
		constexpr int Keypad8 = 328;
		constexpr int Keypad9 = 329;
		constexpr int KeypadDecimal = 330;
		constexpr int KeypadDivide = 331;
		constexpr int KeypadMultiply = 332;
		constexpr int KeypadSubtract = 333;
		constexpr int KeypadAdd = 334;
		constexpr int KeypadEnter = 335;
		constexpr int KeypadEqual = 336;
		constexpr int LeftShift = 340;
		constexpr int LeftControl = 341;
		constexpr int LeftAlt = 342;
		constexpr int LeftSuper = 343;
		constexpr int RightShift = 344;
		constexpr int RightControl = 345;
		constexpr int RightAlt = 346;
		constexpr int RightSuper = 347;
		constexpr int Menu = 348;
	}

	// Mouse buttons.
	namespace mouse {
		constexpr int Button1 = 0;
		constexpr int Button2 = 1;
		constexpr int Button3 = 2;
		constexpr int Button4 = 3;
		constexpr int Button5 = 4;
		constexpr int Button6 = 5;
		constexpr int Button7 = 6;
		constexpr int Button8 = 7;
		constexpr int Left = Button1;
		constexpr int Right = Button2;
		constexpr int Middle = Button3;
	}

	// Modifier bits, as a key or button event carries them.
	namespace mod {
		constexpr int Shift = 0x0001;
		constexpr int Control = 0x0002;
		constexpr int Alt = 0x0004;
		constexpr int Super = 0x0008;
		constexpr int CapsLock = 0x0010;
		constexpr int NumLock = 0x0020;
	}

	// Gamepad buttons (the standard mapping).
	namespace gamepad {
		constexpr int A = 0;
		constexpr int B = 1;
		constexpr int X = 2;
		constexpr int Y = 3;
		constexpr int LeftBumper = 4;
		constexpr int RightBumper = 5;
		constexpr int Back = 6;
		constexpr int Start = 7;
		constexpr int Guide = 8;
		constexpr int LeftThumb = 9;
		constexpr int RightThumb = 10;
		constexpr int DpadUp = 11;
		constexpr int DpadRight = 12;
		constexpr int DpadDown = 13;
		constexpr int DpadLeft = 14;
	}

	// Gamepad axes (the standard mapping).
	namespace axis {
		constexpr int LeftX = 0;
		constexpr int LeftY = 1;
		constexpr int RightX = 2;
		constexpr int RightY = 3;
		constexpr int LeftTrigger = 4;
		constexpr int RightTrigger = 5;
	}

} // namespace hearth::platform
