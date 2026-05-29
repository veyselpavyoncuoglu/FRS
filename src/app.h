#pragma once

// Platform-agnostic application core. Holds all UI state, settings, logging and
// the threaded cleaning logic. Both backends (Win32+DX11, GLFW+OpenGL3) create
// one App, call init() once, then update() + drawUI() each frame, and otherwise
// stay out of the way. No OS or graphics headers belong here.
struct App {
	App();
	~App();
	App(const App&) = delete;
	App& operator=(const App&) = delete;

	// Load persisted settings + query startup state. Call once after construction.
	void init();

	// Advance timers: auto-refresh polling and auto-clean triggering. Call once
	// per frame before drawUI().
	void update();

	// Emit the full ImGui UI (assumes a frame has been started by the backend).
	void drawUI();

	struct State;       // defined in app.cpp
	State* s;
};
