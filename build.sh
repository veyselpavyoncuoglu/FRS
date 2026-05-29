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

# Detect stale imgui.h (WIP pre-release missing DrawCallback_* as struct members in ImGuiPlatformIO).
# The WIP version has #define ImDrawCallback_ResetRenderState (a macro), not a struct member.
# The final v1.92.8 release has: ImDrawCallback DrawCallback_ResetRenderState;
if ! grep -q 'ImDrawCallback DrawCallback_ResetRenderState' imgui/imgui.h 2>/dev/null; then
	echo "[!] imgui.h is outdated and incompatible with the downloaded backends."
	echo "    Run ./setup.sh to refresh all imgui files to v1.92.8."
	exit 1
fi

# Detect GLFW via pkg-config (most reliable) or by header file location.
glfw_find_header() {
	for p in /usr/include/GLFW/glfw3.h /usr/local/include/GLFW/glfw3.h; do
		[ -f "$p" ] && echo "$p" && return 0
	done
	return 1
}

glfw_detected() {
	pkg-config --exists glfw3 2>/dev/null || glfw_find_header >/dev/null 2>&1
}

if ! glfw_detected; then
	echo "[*] GLFW headers not found — installing..."
	if command -v apt-get >/dev/null 2>&1; then
		sudo apt-get update -qq --ignore-missing || true
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
	if ! glfw_detected; then
		echo "[!] GLFW headers still not found after install. Aborting."
		exit 1
	fi
fi

# Build compile/link flags.  pkg-config gives the canonical -I and -l flags;
# fall back to deriving the include root from the header path directly.
if pkg-config --exists glfw3 2>/dev/null; then
	GLFW_CFLAGS="$(pkg-config --cflags glfw3)"
	GLFW_FLAGS="$(pkg-config --libs glfw3)"
else
	GLFW_HDR="$(glfw_find_header)"
	GLFW_CFLAGS="-I$(dirname "$(dirname "$GLFW_HDR")")"
	GLFW_FLAGS="-lglfw"
fi

echo "[*] Building FRS..."
g++ -O2 -std=c++17 \
	-I imgui -I src $GLFW_CFLAGS \
	src/main_linux.cpp src/app.cpp src/ram_linux.cpp src/platform_linux.cpp \
	imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp \
	imgui/imgui_impl_glfw.cpp imgui/imgui_impl_opengl3.cpp \
	$GLFW_FLAGS -lGL -lpthread -ldl \
	-o FRS

echo "[*] Build succeeded: ./FRS"
