#!/usr/bin/env bash
# Build FRS for Linux (GLFW + OpenGL3 backend).
set -e
cd "$(dirname "$0")"

if ! command -v g++ >/dev/null 2>&1; then
	echo "[!] g++ not found. Run ./setup.sh first."
	exit 1
fi

if [ ! -f imgui/imgui_impl_glfw.cpp ]; then
	echo "[!] ImGui GLFW backend missing. Run ./setup.sh first."
	exit 1
fi

# GLFW/GL link flags via pkg-config when available, else sane defaults.
if pkg-config --exists glfw3 2>/dev/null; then
	GLFW_FLAGS="$(pkg-config --cflags --libs glfw3)"
else
	GLFW_FLAGS="-lglfw"
fi

echo "[*] Building FRS..."
g++ -O2 -std=c++17 \
	-I imgui -I src \
	src/main_linux.cpp src/app.cpp src/ram_linux.cpp src/platform_linux.cpp \
	imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp \
	imgui/imgui_impl_glfw.cpp imgui/imgui_impl_opengl3.cpp \
	$GLFW_FLAGS -lGL -lpthread -ldl \
	-o FRS

echo "[*] Build succeeded: ./FRS"
