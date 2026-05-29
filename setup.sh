#!/usr/bin/env bash
# Linux setup for FRS. Installs build dependencies (compiler + GLFW dev headers)
# and fetches the ImGui source plus the GLFW/OpenGL3 backends into imgui/.
set -e
cd "$(dirname "$0")"

# ----------------------------------------------------------------------------
# Step 1 : build toolchain + GLFW development package
# ----------------------------------------------------------------------------
need() { command -v "$1" >/dev/null 2>&1; }

if ! need g++ || ! need git; then
	echo "[*] Installing build tools..."
fi

install_pkgs() {
	if   need apt-get; then sudo apt-get update && sudo apt-get install -y "$@"
	elif need dnf;     then sudo dnf install -y "$@"
	elif need pacman;  then sudo pacman -S --noconfirm "$@"
	elif need zypper;  then sudo zypper install -y "$@"
	else
		echo "[!] No supported package manager found."
		echo "    Please install: a C++ compiler, git, and GLFW3 development headers."
		return 1
	fi
}

# Package names differ per distro family; try the common ones.
if need apt-get; then
	install_pkgs build-essential git libglfw3-dev libgl1-mesa-dev || true
elif need dnf; then
	install_pkgs gcc-c++ git glfw-devel mesa-libGL-devel || true
elif need pacman; then
	install_pkgs base-devel git glfw mesa || true
elif need zypper; then
	install_pkgs gcc-c++ git glfw-devel Mesa-libGL-devel || true
else
	echo "[!] Unknown distro — install g++, git and GLFW3 dev headers manually."
fi

if ! need g++; then
	echo "[!] g++ still not found after install attempt. Aborting."
	exit 1
fi

# Warn loudly if GLFW dev headers still aren't present after the install attempt.
if [ ! -f /usr/include/GLFW/glfw3.h ] && [ ! -f /usr/local/include/GLFW/glfw3.h ]; then
	echo "[!] GLFW development headers not found after install attempt."
	echo "    Try manually: sudo apt-get install libglfw3-dev"
	exit 1
fi

# ----------------------------------------------------------------------------
# Step 2 : clone ImGui and copy the files we need (core + GLFW/OpenGL3 backends)
# ----------------------------------------------------------------------------
echo "[*] Cloning ImGui (shallow)..."
mkdir -p imgui
rm -rf _imgui_tmp
git clone --depth=1 https://github.com/ocornut/imgui.git _imgui_tmp

echo "[*] Copying required files..."
for f in imgui.h imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp \
         imgui_internal.h imconfig.h imstb_rectpack.h imstb_textedit.h imstb_truetype.h; do
	cp "_imgui_tmp/$f" imgui/
done
# Cross-platform backends for the Linux build.
cp _imgui_tmp/backends/imgui_impl_glfw.h      imgui/
cp _imgui_tmp/backends/imgui_impl_glfw.cpp    imgui/
cp _imgui_tmp/backends/imgui_impl_opengl3.h   imgui/
cp _imgui_tmp/backends/imgui_impl_opengl3.cpp imgui/
cp _imgui_tmp/backends/imgui_impl_opengl3_loader.h imgui/

echo "[*] Cleaning up temp clone..."
rm -rf _imgui_tmp

echo "[*] Done. Run ./build.sh to compile FRS."
