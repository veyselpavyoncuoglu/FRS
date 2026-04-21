# FRS — Free RAM System

A lightweight Windows RAM cleaner with a native ImGui + DirectX 11 UI.  
Runs multiple cleaning methods in the background with real-time progress feedback.

> Because Microslop is too busy shipping another AI powered slop to bother freeing the 4 GB of RAM that Copilot, Teams, and Windows Search quietly ate while you weren't looking.

![License](https://img.shields.io/badge/License-MIT-gold.svg)
![Platform](https://img.shields.io/badge/Platform-Windows-blue.svg)

## Features

- **Flush Standby** — splits a large allocation across all CPU threads, touches every page in parallel, then frees it to flush the standby list
- **Empty Working Sets** — collects all running process IDs and trims their working sets in parallel across all CPU threads
- **Purge Standby** — calls `NtSetSystemInformation` (cmd 4) to force-purge the standby list
- **Flush Modified** — calls `NtSetSystemInformation` (cmd 3) to flush the modified page list
- **Full Clean** — runs all four methods in sequence
- **Auto-Refresh** — refreshes RAM stats automatically at a configurable interval
- **Auto-Clean** — triggers a clean automatically when RAM usage exceeds a configurable threshold; with a low threshold and only "Empty Working Sets" ticked it makes a great passive background cleaner that stays out of the way until memory pressure actually builds up
- **Persistent settings** — all settings are saved to `%APPDATA%\FRS\settings.ini` immediately on change and restored on next launch
- **Run at startup** — optional Task Scheduler task that launches FRS elevated at logon with no UAC prompt; toggled from the Settings tab (default off)
- Threaded cleaning — UI stays responsive during long operations
- Real-time progress bar and timestamped log with color-coded entries (green = success, red = error, yellow = auto-clean, gray = neutral)
- No freeze during window drag — render loop keeps running via a Win32 timer while the title bar is held
- UAC elevation prompt on launch (required for full effectiveness)
- No installer, no runtime DLLs — single portable `.exe`

## Requirements

- Windows 10 / 11 (64-bit)
- Administrator privileges (UAC prompt shown on launch)
- Internet connection for first-time setup (to download MinGW if needed)

## Building

### 1. Setup (first time only)

Run `setup.bat`. It will:

1. Check if `g++` is in `PATH`
2. If not, try `winget install MinGW` automatically
3. If that fails, download the latest WinLibs MinGW-w64 build from GitHub and extract it to `tools/mingw64/`
4. Clone ImGui from `https://github.com/ocornut/imgui` and copy the required source files into `imgui/`

```
setup.bat
```

### 2. Build

```
build.bat
```

Produces `FRS.exe` in the project root. No additional DLLs needed — the runtime is statically linked.

## Project Structure

```
FRS/
├── src/
│   ├── main.cpp          # Win32 window, DirectX 11, ImGui render loop, full UI
│   ├── ram.h             # RamStats struct + function declarations
│   └── ram.cpp           # RAM cleaning implementations
├── imgui/                # ImGui source (populated by setup.bat)
├── res/
│   ├── app.manifest      # UAC elevation manifest
│   └── app.rc            # Resource script embedding the manifest
├── tools/
│   └── download_mingw.ps1  # PowerShell script to download WinLibs MinGW
├── build.bat             # Compile script
└── setup.bat             # Dependency installer
```

## Technical Notes

- **Compiler**: g++ (MinGW-w64), C++17, `-O2 -mwindows -static-libgcc -static-libstdc++ -static`
- **UI**: [Dear ImGui](https://github.com/ocornut/imgui) with Win32 + DirectX 11 backend
- **Threading**: `std::thread` + `std::atomic` + `std::mutex` for thread-safe log updates and parallel RAM operations
- **Parallelism**: `flushStandby` and `emptyWorkSets` split work across `std::thread::hardware_concurrency()` threads
- **Settings**: plain `key=value` INI written to `%APPDATA%\FRS\settings.ini`; directory is created automatically on first launch
- **Log colors**: green for successful operations, red for errors/NTSTATUS codes, yellow/orange for auto-clean triggers, gray for neutral entries
- **Default Auto-Clean config**: threshold 20%, Empty Working Sets only — ready to enable as a passive background cleaner out of the box
- **Startup**: Task Scheduler task created via `schtasks /create /rl highest /sc onlogon`; no UAC prompt on subsequent boots since the task already holds the elevation grant
- **Privileges**: `SE_DEBUG_NAME` privilege enabled at startup for `EmptyWorkingSet` access to system processes

## License

This project is licensed under the [MIT License](LICENSE).