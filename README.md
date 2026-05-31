# FRS — Free RAM System

A lightweight cross-platform RAM cleaner with a native ImGui UI.  
Runs multiple cleaning methods in the background with real-time progress feedback.

> Because Microslop is too busy shipping another AI powered slop to bother freeing the 4 GB of RAM that Copilot, Teams, and Windows Search quietly ate while you weren't looking.

![License](https://img.shields.io/badge/License-MIT-gold.svg)
![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-blue.svg)

## Features

- **Flush Standby** — splits a large allocation across all CPU threads, touches every page in parallel, then frees it to flush the standby list
- **Empty Working Sets** — collects all running process IDs and trims their working sets in parallel across all CPU threads
- **Purge Standby** — calls `NtSetSystemInformation` (cmd 4) to force-purge the standby list
- **Flush Modified** — calls `NtSetSystemInformation` (cmd 3) to flush the modified page list
- **Full Clean** — runs all four methods in sequence
- **Auto-Refresh** — refreshes RAM stats automatically at a configurable interval
- **Auto-Clean** — triggers a clean automatically when RAM usage exceeds a configurable threshold; with a low threshold and only "Empty Working Sets" ticked it makes a great passive background cleaner that stays out of the way until memory pressure actually builds up
- **Privacy tab** — apply and revert ~60 registry-based privacy/telemetry tweaks across 10 categories (Telemetry, Activity History, Location, Advertising ID, Tailored Experiences, Feedback, Ink & Typing, Clipboard History, App Compat, Cortana). Each tweak shows live status (ON / PARTIAL / OFF) and can be toggled individually or all at once. Ported from [hellzerg/optimizer](https://github.com/hellzerg/optimizer). Requires Administrator.
- **Persistent settings** — all settings are saved on change and restored on next launch (`%APPDATA%\FRS\settings.ini` on Windows, `$XDG_CONFIG_HOME/FRS/settings.ini` — usually `~/.config/FRS/settings.ini` — on Linux)
- **Run at startup** — optional auto-start at login, toggled from the Settings tab (default off). Windows uses a Task Scheduler task that launches FRS elevated with no UAC prompt; Linux drops a `.desktop` entry in `~/.config/autostart/`
- Threaded cleaning — UI stays responsive during long operations
- Real-time progress bar and timestamped log with color-coded entries (green = success, red = error, yellow = auto-clean, gray = neutral)
- No freeze during window drag — the render loop keeps running while the window is resized/moved
- Elevation indicator — shows `[ADMIN]` when running with the privileges needed for full effectiveness
- No installer — single portable executable (statically linked on Windows)

## Platform support

FRS runs on **Windows** (Win32 + DirectX 11 backend) and **Linux** (GLFW + OpenGL 3 backend). The UI, settings, logging and threaded cleaning are shared, identical code; only the windowing backend and the RAM primitives differ per OS.

### How the four methods map on Linux

| Method | Windows | Linux |
| --- | --- | --- |
| Flush Standby | parallel allocate → touch → free (forces standby-list flush) | identical: parallel `mmap` → touch → `munmap` to create reclaim pressure |
| Empty Working Sets | `EmptyWorkingSet` on every process | `process_madvise(MADV_PAGEOUT)` over each process's `/proc/<pid>/maps` regions (kernel ≥ 5.10) |
| Purge Standby | `NtSetSystemInformation` cmd 4 | `sync` + drop clean page cache via `/proc/sys/vm/drop_caches` (needs root) |
| Flush Modified | `NtSetSystemInformation` cmd 3 | `sync()` — flush modified/dirty pages to disk |

RAM stats come from `GlobalMemoryStatusEx` on Windows and `/proc/meminfo` (`MemAvailable`) on Linux. Methods that need root degrade gracefully and report the reason in the log instead of crashing.

## Requirements

**Windows**
- Windows 10 / 11 (64-bit)
- Administrator privileges (UAC prompt shown on launch)
- Internet connection for first-time setup (to download MinGW if needed)

**Linux**
- A C++17 compiler (g++/clang), GLFW3 dev headers, OpenGL
- Run as root (`sudo`) for the cache-dropping methods to take effect
- Internet connection for first-time setup (to clone ImGui)

## Building

### Windows

#### 1. Setup (first time only)

Run `setup.bat`. It will:

1. Check if `g++` is in `PATH`
2. If not, try `winget install MinGW` automatically
3. If that fails, download the latest WinLibs MinGW-w64 build from GitHub and extract it to `tools/mingw64/`
4. Clone ImGui from `https://github.com/ocornut/imgui` and copy the required source files into `imgui/`

```
setup.bat
```

#### 2. Build

```
build.bat
```

Produces `FRS.exe` in the project root. No additional DLLs needed — the runtime is statically linked.

### Linux

#### 1. Setup (first time only)

```
./setup.sh
```

Installs the build toolchain and GLFW dev headers via your distro's package manager (apt/dnf/pacman/zypper), then clones ImGui and copies the core files plus the GLFW/OpenGL3 backends into `imgui/`.

#### 2. Build

```
./build.sh
```

Produces `./FRS` in the project root. Run it with `sudo ./FRS` so the standby/cache methods can take effect.

## Project Structure

```
FRS/
├── src/
│   ├── app.h / app.cpp        # Shared platform-agnostic core: UI, settings, logging, threaded cleaning
│   ├── platform.h             # Thin OS-abstraction interface (settings I/O, startup, privileges)
│   ├── platform_win.cpp       # Windows platform impl (AppData path, schtasks startup, SE_DEBUG)
│   ├── platform_linux.cpp     # Linux platform impl (XDG config path, autostart .desktop)
│   ├── ram.h                  # RamStats struct + cleaning function declarations (platform-neutral)
│   ├── ram.cpp                # Windows RAM cleaning implementations
│   ├── ram_linux.cpp          # Linux RAM cleaning implementations
│   ├── privacy.h              # Privacy tab interface + platform backend declarations
│   ├── privacy.cpp            # Platform-neutral privacy tweak logic and ImGui tab rendering
│   ├── privacy_win.cpp        # Windows registry backend (RegCreateKeyExW, RegSetValueExW, …)
│   ├── privacy_linux.cpp      # Linux stub (privacy tweaks are Windows-only)
│   ├── main.cpp               # Windows backend: Win32 window + DirectX 11 render loop
│   └── main_linux.cpp         # Linux backend: GLFW window + OpenGL 3 render loop
├── imgui/                     # ImGui source + backends (populated by setup.bat / setup.sh)
├── res/
│   ├── app.manifest           # UAC elevation manifest (Windows)
│   └── app.rc                 # Resource script embedding the manifest
├── tools/
│   └── download_mingw.ps1     # PowerShell script to download WinLibs MinGW (Windows)
├── build.bat / setup.bat      # Windows compile + dependency scripts
└── build.sh  / setup.sh       # Linux compile + dependency scripts
```

## Technical Notes

- **Compiler**: g++ — Windows: MinGW-w64, C++17, `-O2 -mwindows -static-libgcc -static-libstdc++ -static`; Linux: C++17, `-O2`, linked against GLFW + OpenGL
- **UI**: [Dear ImGui](https://github.com/ocornut/imgui) — Win32 + DirectX 11 backend on Windows, GLFW + OpenGL 3 backend on Linux. The same `App` core drives both, so the UI is byte-for-byte identical across platforms.
- **Threading**: `std::thread` + `std::atomic` + `std::mutex` for thread-safe log updates and parallel RAM operations
- **Parallelism**: `flushStandby` and `emptyWorkSets` split work across `std::thread::hardware_concurrency()` threads on both platforms
- **Settings**: plain `key=value` INI; directory created automatically on first save (`%APPDATA%\FRS` / `~/.config/FRS`)
- **Log colors**: green for successful operations, red for errors/NTSTATUS codes, yellow/orange for auto-clean triggers, gray for neutral entries
- **Default Auto-Clean config**: threshold 20%, Empty Working Sets only — ready to enable as a passive background cleaner out of the box
- **Startup**: Windows uses a Task Scheduler task via `schtasks /create /rl highest /sc onlogon` (no UAC prompt on subsequent boots); Linux uses an XDG autostart `.desktop` file
- **Privileges**: Windows enables `SE_DEBUG_NAME` at startup for `EmptyWorkingSet` access to system processes; Linux checks `geteuid() == 0`

## Credits

| What | Who / Where |
| --- | --- |
| **FRS** — original author and developer | Nems1337 |
| **Dear ImGui** — the entire UI | [Omar Cornut](https://github.com/ocornut) — [ocornut/imgui](https://github.com/ocornut/imgui) (MIT) |
| **Privacy & telemetry tweaks** — registry op patterns ported to C++ | [hellzerg/optimizer](https://github.com/hellzerg/optimizer) `EnhancePrivacy` / `CompromisePrivacy` (GPL-3.0) |

The privacy tweak list was ported from hellzerg/optimizer's `OptimizeHelper.cs`. Only pure registry read/write/delete operations were carried over; service-Start edits, scheduled-task scripts, and any other side-effecting shell commands were intentionally excluded so every tweak reverts cleanly to Windows defaults.

## License

This project is licensed under the [MIT License](LICENSE).