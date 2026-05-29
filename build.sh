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

# Ensure the GLFW development headers are present; install them if not.
GLFW_HEADER=""
for p in /usr/include/GLFW/glfw3.h /usr/local/include/GLFW/glfw3.h; do
	[ -f "$p" ] && GLFW_HEADER="$p" && break
done

if [ -z "$GLFW_HEADER" ]; then
	echo "[*] GLFW headers not found — attempting to install libglfw3-dev..."
	if command -v apt-get >/dev/null 2>&1; then
		sudo apt-get install -y libglfw3-dev libgl1-mesa-dev
	elif command -v dnf >/dev/null 2>&1; then
		sudo dnf install -y glfw-devel mesa-libGL-devel
	elif command -v pacman >/dev/null 2>&1; then
		sudo pacman -S --noconfirm glfw mesa
	elif command -v zypper >/dev/null 2>&1; then
		sudo zypper install -y glfw-devel Mesa-libGL-devel
	else
		echo "[!] No supported package manager found."
		echo "    Install GLFW3 dev headers manually (e.g. libglfw3-dev)."
		exit 1
	fi
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
