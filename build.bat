@echo off
cd /d "%~dp0"

:: Use local MinGW if g++ not in PATH
where g++ >nul 2>&1
if errorlevel 1 (
	if exist "%~dp0tools\mingw64\bin\g++.exe" (
		echo [*] Using local MinGW at tools\mingw64\bin
		set "PATH=%~dp0tools\mingw64\bin;%PATH%"
	) else (
		echo [!] g++ not found. Run setup.bat first.
		pause
		exit /b 1
	)
)

echo [*] Compiling resource...
pushd res
windres app.rc -O coff -o app.res
if errorlevel 1 ( popd & echo [!] windres failed & pause & exit /b 1 )
popd

if not exist build mkdir build

echo [*] Building FRS...
g++ -O2 -std=c++17 -DUNICODE -D_UNICODE -mwindows -static-libgcc -static-libstdc++ -static ^
	-I imgui -I src ^
	src\main.cpp src\app.cpp src\ram.cpp src\platform_win.cpp ^
	imgui\imgui.cpp imgui\imgui_draw.cpp imgui\imgui_tables.cpp imgui\imgui_widgets.cpp ^
	imgui\imgui_impl_win32.cpp imgui\imgui_impl_dx11.cpp ^
	res\app.res ^
	-ld3d11 -ldxgi -ld3dcompiler -lpsapi -luser32 -lgdi32 -lshell32 -ldwmapi -lkernel32 ^
	-o FRS.exe

if errorlevel 1 ( echo [!] Build failed & pause & exit /b 1 )
echo [*] Build succeeded: FRS.exe
pause