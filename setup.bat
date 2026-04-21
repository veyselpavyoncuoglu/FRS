@echo off
cd /d "%~dp0"

:: ================================================================
:: Step 1 : Check / install g++ (MinGW-w64)
:: ================================================================
echo [*] Checking for g++...
where g++ >nul 2>&1
if not errorlevel 1 (
	echo [*] g++ already available, skipping MinGW install.
	goto :imgui
)

echo [*] g++ not found. Trying winget...
winget install --id=GnuWin32.GCC -e --accept-package-agreements --accept-source-agreements >nul 2>&1
where g++ >nul 2>&1
if not errorlevel 1 (
	echo [*] g++ installed via winget.
	goto :imgui
)

echo [*] winget failed or g++ still not in PATH.
echo [*] Downloading portable WinLibs MinGW-w64 from GitHub...
if not exist tools mkdir tools

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\download_mingw.ps1"
if errorlevel 1 (
	echo [!] Download failed. Install g++ manually and rerun setup.bat.
	pause
	exit /b 1
)

echo [*] Extracting MinGW-w64...
powershell -NoProfile -Command "Expand-Archive -Force 'tools\mingw64.zip' 'tools\'"
del tools\mingw64.zip

if not exist "%~dp0tools\mingw64\bin\g++.exe" (
	echo [!] Extraction failed or unexpected folder structure.
	pause
	exit /b 1
)

set "PATH=%~dp0tools\mingw64\bin;%PATH%"
echo [*] g++ ready at tools\mingw64\bin
echo [*] NOTE : Reopen any terminals for g++ to be in PATH globally.

:: ================================================================
:: Step 2 : Clone and copy ImGui
:: ================================================================
:imgui
echo [*] Cloning ImGui (shallow)...
if not exist imgui mkdir imgui

git clone --depth=1 https://github.com/ocornut/imgui.git _imgui_tmp
if errorlevel 1 (
	echo [!] git clone failed. Make sure git is in PATH.
	pause
	exit /b 1
)

echo [*] Copying required files...
copy _imgui_tmp\imgui.h              imgui\ >nul
copy _imgui_tmp\imgui.cpp            imgui\ >nul
copy _imgui_tmp\imgui_draw.cpp       imgui\ >nul
copy _imgui_tmp\imgui_tables.cpp     imgui\ >nul
copy _imgui_tmp\imgui_widgets.cpp    imgui\ >nul
copy _imgui_tmp\imgui_internal.h     imgui\ >nul
copy _imgui_tmp\imconfig.h           imgui\ >nul
copy _imgui_tmp\imstb_rectpack.h     imgui\ >nul
copy _imgui_tmp\imstb_textedit.h     imgui\ >nul
copy _imgui_tmp\imstb_truetype.h     imgui\ >nul
copy _imgui_tmp\backends\imgui_impl_win32.h   imgui\ >nul
copy _imgui_tmp\backends\imgui_impl_win32.cpp imgui\ >nul
copy _imgui_tmp\backends\imgui_impl_dx11.h    imgui\ >nul
copy _imgui_tmp\backends\imgui_impl_dx11.cpp  imgui\ >nul

echo [*] Cleaning up temp clone...
rmdir /s /q _imgui_tmp

echo [*] Done. ImGui files ready in imgui\
pause
