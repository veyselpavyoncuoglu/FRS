# Mem Reduct coverage

FRS took feature ideas from [Mem Reduct](https://github.com/henrypp/memreduct) 3.5.3 by Henry++. Mem Reduct is GPL-3.0 and FRS is MIT, so nothing was copied : no code, no strings, no struct layouts. FRS rebuilt the behaviour in its own code, the memory calls it uses are the ones FRS already had for its Memory page. This page goes through every Mem Reduct feature, 159 rows, and says what FRS does with it.

It is made from the coverage notes written while building, so the notes are short. When a note and the code disagree, the code is right. Feature names are Mem Reduct's. "New" in a note marks what was built for this work.

## Status words

- **done** : FRS has it. Where it lives in FRS, or a small difference, is in the note.
- **approx** : FRS covers it another way, the note says how. A few rows the build notes called done are approx here, because FRS does them in a clearly different way.
- **skipped** : not built, the note says why.

A note that starts with **Windows only** means Linux has no such thing in FRS : no tray icon, no global hotkey, no no-prompt shortcut, or a memory area Linux does not have.

The **Not run** column names paths that were written but never ran :

- **elevated** : the part that needs admin rights never ran. Every Windows test used a private test build that runs unelevated, and every clean step in testing was faked, never sent to the OS.
- **Linux** : the row has real Linux code and it never ran. The Linux build is compile and link checked with a cross compiler, there was no Linux machine to run it on. That holds for every Linux path in FRS, marked or not.
- **tray** : never shown in a real notification area. No test was allowed to put a balloon or an icon on the user's desktop. The note says whether the icon was drawn in a private harness or only read in code.

## Summary

| Section | Rows | Done | Approx | Skipped | Windows only | Not run elevated | Linux code not run | Not run in a real tray |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| [Main window](#main-window) | 17 | 12 | 2 | 3 | 0 | 1 | 14 | 0 |
| [Cleaning engine and clean areas](#cleaning-engine-and-clean-areas) | 13 | 10 | 1 | 2 | 3 | 9 | 7 | 0 |
| [Clean triggers and safety](#clean-triggers-and-safety) | 8 | 7 | 0 | 1 | 1 | 0 | 6 | 1 |
| [Notifications and reporting](#notifications-and-reporting) | 6 | 5 | 0 | 1 | 2 | 0 | 3 | 2 |
| [Tray icon](#tray-icon) | 18 | 17 | 1 | 0 | 16 | 0 | 2 | 9 |
| [Tray mouse and keyboard actions](#tray-mouse-and-keyboard-actions) | 5 | 3 | 1 | 1 | 4 | 0 | 0 | 0 |
| [Tray context menu items](#tray-context-menu-items) | 9 | 7 | 0 | 2 | 7 | 0 | 0 | 0 |
| [Main menu bar](#main-menu-bar) | 13 | 7 | 1 | 5 | 1 | 2 | 7 | 0 |
| [Accelerators](#accelerators) | 1 | 0 | 0 | 1 | 0 | 0 | 0 | 0 |
| [Settings window shell](#settings-window-shell) | 5 | 4 | 0 | 1 | 0 | 0 | 4 | 0 |
| [Settings > General](#settings--general) | 7 | 4 | 1 | 2 | 1 | 2 | 4 | 0 |
| [Settings > Memory cleaning](#settings--memory-cleaning) | 4 | 4 | 0 | 0 | 1 | 0 | 3 | 0 |
| [Settings > Appearance](#settings--appearance) | 7 | 7 | 0 | 0 | 7 | 0 | 0 | 6 |
| [Settings > Tray icon](#settings--tray-icon) | 3 | 3 | 0 | 0 | 2 | 0 | 1 | 1 |
| [Settings > Advanced](#settings--advanced) | 2 | 1 | 0 | 1 | 0 | 0 | 1 | 0 |
| [Startup, window and elevation](#startup-window-and-elevation) | 12 | 8 | 1 | 3 | 2 | 2 | 7 | 0 |
| [Command line](#command-line) | 7 | 5 | 0 | 2 | 0 | 2 | 5 | 0 |
| [Settings storage and portable mode](#settings-storage-and-portable-mode) | 2 | 2 | 0 | 0 | 0 | 0 | 2 | 0 |
| [Updates](#updates) | 4 | 0 | 0 | 4 | 0 | 0 | 0 | 0 |
| [Localization](#localization) | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 0 |
| [About, website, misc](#about-website-misc) | 4 | 1 | 0 | 3 | 0 | 0 | 1 | 0 |
| [Distribution](#distribution) | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 0 |
| [Known Mem Reduct quirks](#known-mem-reduct-quirks) | 8 | 0 | 0 | 8 | 0 | 0 | 0 | 0 |
| **Total** | **159** | **107** | **8** | **44** | **47** | **18** | **67** | **19** |

Done and approx together are the 115 rows the build notes called done. Windows only rows are not counted under Linux code not run, there is nothing of them on Linux to run.

## Only written, never run

Elevated : every real clean. The four methods, the five new areas, `-clean` and `-clean:full` only ran with faked steps. The Run at startup task and the no-prompt task at the highest run level : both only ran under test task names at the lowest run level, the user's own "FRS" task was never touched.

Tray : the balloon after a clean and the threshold colours on the icons (warning, critical, background instead of text) were only read in code. The other percent icon looks (font, smooth digits, text and background colour, no background, frame, round corners) were drawn in a private harness at 16, 20, 24 and 32 px, never in the real notification area. The tray menu opened from the keyboard was only checked with a synthetic message.

Portable : `-portable` itself can't run in a test build, which always uses its own settings folder. The task with `-portable` in it was registered and upgraded under a test task name.

Linux : all of it. Clean set areas, timer, pause, summary line, log file, `-clean`, `-help`, single instance through the lock file, window position, start minimized and portable mode.

## Biggest groups not built

- translations and language packs : FRS is English only
- update checks and installers : FRS ships as source, there is no binary to download
- website links : FRS runs elevated, a link opened from it would start the browser elevated
- Mem Reduct's menu bar, F1 and F2 keys and dark theme switch : FRS has a sidebar, the Ctrl+K palette and a dark only look
- a hidden freeze exclusion for automatic cleans and Mem Reduct's default areas : FRS runs exactly what you ticked and keeps its own default, Empty Working Sets only
- crash dumps and process hardening : the reasons are in the last rows below
- Mem Reduct's own bugs, listed at the end, were not copied

## Main window

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Main window | done | Linux | One resizable window with a sidebar of pages, not a fixed dialog. Always on top is an option |
| Memory list | done | Linux | Dashboard cards, Memory page, Page files tab on the Kernel page, Live view |
| Physical memory usage | done | Linux | Header RAM bar, Dashboard Physical card, status bar item (off by default) |
| Physical memory available | done | Linux | Header RAM bar and the status bar Available item. The header says "MB free" but the number is available memory, standby included, the same number Mem Reduct shows |
| Physical memory total | done | Linux | Dashboard Physical card "of total", Memory page strip |
| Page file usage | done | Linux | Kernel page, Page files tab, In use card. MCP `page_files` |
| Page file available | done | Linux | New : the In use card adds the free size, `page_files` gives `free_bytes` |
| Page file total | done | Linux | Page files Total size card |
| System working set usage | approx | Linux | File cache current and peak sizes on the Memory page Details, not a percent of the peak |
| System working set available | skipped |  | Peak minus current is not memory anyone can get. FRS shows current, peak, minimum and maximum instead |
| System working set total | done | Linux | File cache peak on the Memory page |
| Row colours by load | approx | Linux | Physical and commit numbers in the status bar and Live view turn orange and red past the thresholds. The header RAM bar has its own fixed green to red scale, the Dashboard cards keep one colour |
| Refresh rate | done | Linux | Sampling interval 0.1 to 10 s, Auto-Refresh, interval presets in the Ctrl+K palette |
| Clean memory button | done | elevated, Linux | New : Run clean set on the Dashboard runs the clean set, the four methods plus five areas you tick. Tested with faked steps |
| Clean one area from a list | done | Linux | Memory page Empty menu, one item per command, also in the Ctrl+K palette and MCP `memory_command` |
| Keyboard keys in the main window | skipped |  | Enter as a window wide clean key would fire cleans from text boxes. The Ctrl+K palette has "Clean : Run clean set", it asks first |
| Busy cursor | skipped |  | Cleans run on a worker thread with a progress bar, the window never freezes |

## Cleaning engine and clean areas

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Working set | approx | elevated, Linux | The clean set tick is the Dashboard's Empty Working Sets : process by process, protected processes are skipped. The single system call Mem Reduct makes is Empty working sets on the Memory page's Empty menu |
| System file cache | done | elevated, Linux | New in the clean set, off by default. Linux : sync, then drop the clean page cache |
| Modified file cache | done | elevated, Linux | New in the clean set, off by default. A stuck volume can hold it up to 20 s. Linux : syncfs on every block device and ZFS mount |
| Modified page list | done | elevated, Linux | Dashboard Flush Modified |
| Standby list | done | elevated, Linux | Dashboard Purge Standby |
| Standby list without priority | done | elevated | Windows only. New in the clean set as Priority 0 standby list, off by default. Linux has no standby priorities |
| Registry cache | done | elevated | Windows only. New in the clean set, off by default. Linux has no registry |
| Combine memory lists | done | elevated | Windows only. New in the clean set as Combine memory pages, off by default. On Linux combine starts KSM and leaves it on, a lasting change, so it stays on the Empty menu there |
| Default areas | skipped |  | FRS keeps its own default, Empty Working Sets only, so an existing Auto-Clean does not change. The new areas start off |
| Privileges | done | elevated | FRS turns on the debug, quota, working set, base priority and profiling privileges before the window opens. Linux runs as root through pkexec instead |
| Freed amount | done | Linux | New : a summary line after each clean, like `Clean : OK, freed 412 MB (available 3.10 GB -> 3.50 GB)`. `app_status` last_clean and MCP `clean` give the same number. When use went up it says "freed nothing, in use grew N MB" |
| Last clean time | done | Linux | New : `app_status` last_clean with time and source, and the tooltip of the cleaning item in the status bar |
| Clean without admin | skipped |  | FRS asks for admin at launch on both systems, a method without rights says why in the log |

## Clean triggers and safety

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Manual clean of the chosen areas | done | Linux | New : Run clean set on the Dashboard, the Ctrl+K palette, the tray and MCP `clean` with `selected` run your set |
| Confirm before cleaning | done | Linux | New : *Ask before a Dashboard clean*, off by default. The Dashboard buttons and Run clean set list the steps and ask first. Automatic, hotkey, tray, command line and MCP cleans never ask |
| Freeze warning | done | Linux | New : a line under the clean set warns that Purge Standby and Flush Modified in an automatic run can stall the machine. Empty all on the Memory page asks first |
| Global clean hotkey | done |  | Windows only. New, off by default. Ctrl+Shift+F12 by default instead of Mem Reduct's Ctrl+F1, which Office uses. Runs the clean set, logged as `[hotkey]`. Linux has no global hotkeys, Wayland has none |
| Clean above a usage level | done | Linux | Auto-Clean threshold as before. New : *Auto-Clean pause after a clean* (0 to 3600 s, 0 by default) stops back to back cleans while the machine stays above the threshold |
| Clean every N minutes | done | Linux | New, off by default, 1 to 1440 minutes. Counts from the start of the last clean, from launch or from turning it on. Runs with Auto-Refresh off. No countdown while the set is empty |
| Freeze exclusion for automatic cleans | skipped |  | FRS runs exactly the methods you ticked, a hidden exclusion would drop some of them. The warning line covers it |
| Report of automatic cleans | done | Linux, tray | New : the summary line for `[auto]` cleans, and the tray balloon when *Notify after a clean* is on. The balloon was only read in code |

## Notifications and reporting

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Balloon after a clean | done | tray | Windows only. New, off by default : the summary and the steps from the first tray icon that is up, the warning icon when a step failed, one every 30 s at most. Read in code only |
| Balloon sound | done | tray | Windows only. New : *Notification sound*, on by default. Read in code only |
| Result message box | done | Linux | New : `-clean` shows the steps in a message box, the warning icon when a step failed, nothing with `-silent`. Linux prints them. Tested through the test build's stand in file instead of a real box |
| No privileges balloon | skipped |  | FRS starts elevated on Windows, a method that fails says why in the log |
| Log clean results to a file | done | Linux | New : *Write the log to a file*, off by default. frs.log next to settings.ini, rotated at 1 MB. A file another program holds is retried for 10 s, then the Log card says writing failed until the option is toggled. The line of the change that turns it on or off lands in the file too. Tested with frs.log held open |
| Error log | done | Linux | Failed calls show red in the log, and go to frs.log when the log file is on |

## Tray icon

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Tray icon | done |  | Windows only. Off by default, icons for physical, commit and GPU memory |
| Usage value on the icon | done |  | Windows only. Percent as text. FRS rounds (69.6 % shows 70), Mem Reduct cuts it (69) |
| Icon drawing | done |  | Windows only. 32 bit icon with alpha at the small icon size |
| Font | done | tray | Windows only. New : Tahoma, Segoe UI, Consolas, Lucida Console or Arial. "100" steps the size down until it fits. Drawn in a private harness |
| Antialiasing | done | tray | Windows only. New : *Smooth digits*, on by default. Drawn in a private harness |
| Text colour | done | tray | Windows only. New colour picker, *automatic* follows the physical or commit graph colour as before. Drawn in a private harness |
| Background colour | done | tray | Windows only. New colour picker plus *without background*, *automatic* is black as before. Drawn in a private harness |
| Warning colour | done | tray | Windows only. New : with *Icons follow the highlight thresholds*, *automatic* is the theme's orange. Read in code only |
| Danger colour | done | tray | Windows only. New : the critical colour, *automatic* is the theme's red. Read in code only |
| Colour picker | done |  | Windows only. New : colour pickers on the Settings page. `set_settings` takes "#RRGGBB" and refuses anything else, `app_status` lists the string. An unset swatch shows the colour the icon uses |
| Warning level | done | Linux | Memory highlights *Warning at*, 80 % by default, Mem Reduct uses 70 |
| Danger level | done | Linux | *Critical at*, 90 % by default |
| Change background instead of text | done | tray | Windows only. New : *Thresholds color the background*. Read in code only |
| Transparent background | done |  | Windows only. *Transparent icons* |
| Border | done | tray | Windows only. New : *Frame around percent icons*, 1 px in the text colour. A 100 at 16 and 20 px draws without the frame, from 24 px its digits keep a free px inside it. Drawn in a private harness |
| Round corners | done | tray | Windows only. New : *Round percent icons*. Drawn in a private harness |
| Tooltip | approx |  | Windows only. Physical, commit and available memory. Page file and file cache are on the Kernel and Memory pages instead |
| DPI | done |  | Windows only. The tray is per monitor DPI aware. After a display scale change without signing out, the icon keeps its old size and Windows stretches it, cosmetic |

## Tray mouse and keyboard actions

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Single click action | approx |  | Windows only. A click opens the Live view (the GPU page for a GPU icon) or toggles the window, it never cleans. Cleaning on a click is given to the middle button |
| Middle click action | done |  | Windows only. New : *Middle click on an icon* : Nothing (the default), Show or hide, Clean set, Full Clean or Live view. Cleans start with no prompt and run only when the cursor is on the icon |
| Open Task Manager | skipped |  | The Processes page and the Live view show memory per process, the middle click can open the Live view |
| Keyboard select | done |  | Windows only. New : Enter on the icon brings FRS forward, it never hides it |
| Context menu | done |  | Windows only. Right click, or Shift+F10 and the Menu key. The keyboard path was only checked with a synthetic message |

## Tray context menu items

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Show or hide | done |  | Windows only. Show FRS / Hide FRS |
| Clean memory | done |  | Windows only. New : "Clean set" in the Clean submenu, grey while a clean runs or the set is empty |
| Clean areas submenu | done |  | Windows only. New : "Clean set areas" with the nine ticks |
| Clean when above submenu | done |  | Windows only. New : "Auto-clean" with Above 50 to 90 %, the current value when it is none of them, and an off item |
| Clean every submenu | done |  | Windows only. New : Every 10, 15, 30, 60 or 120 min, the current value, and an off item |
| Settings | done |  | Windows only. New : opens the Settings page and shows the window |
| Website | skipped |  | FRS runs elevated, a link opened from it would start the browser elevated |
| About | skipped |  | About is the Credits page, one click in the sidebar |
| Exit | done |  | Windows only. Exits also with Close to tray on |

## Main menu bar

FRS has no menu bar. These rows say where each item's job went.

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| File > Settings | done | Linux | Settings page in the sidebar, also through the Ctrl+K palette |
| File > Exit | done | Linux | Closing the window exits unless Close to tray is on, Exit is also in the tray menu |
| View > Always on top | done | Linux | *Always on top*. Linux : X11 only, Wayland ignores it |
| View > Use dark theme | skipped |  | FRS is dark only by design |
| View > Confirm memory cleaning | done | Linux | *Ask before a Dashboard clean*, see Confirm before cleaning |
| View > Language | skipped |  | FRS is English only |
| Settings > Load on system startup | done | elevated, Linux | Run at startup. New : the Windows task now starts on battery, keeps running on battery and past 3 days, at normal priority. A task made by an older FRS gets those settings once, its trigger and command stay, a task FRS did not make is left alone and logged. Tested on test tasks at the lowest run level. Linux : autostart entry, pkexec asks for the password at each login |
| Settings > Start minimized | done | Linux | New : *Start minimized* or `-minimized`, no tray icon needed. Without either FRS still follows a shortcut set to start minimized. Linux shows the window for an instant before it minimizes |
| Settings > Skip UAC prompt | approx | elevated | Windows only. New : *Create no-prompt shortcut* makes a desktop shortcut that starts FRS as admin through a scheduled task. Only that shortcut skips the prompt, other starts still ask. FRS asks first when FRS.exe is in a folder your account can write to. Tested with test task names and the shortcut in the test settings folder. On Linux a polkit rule would do it, that is the admin's choice |
| Settings > Check for updates periodically | skipped |  | FRS ships as source, there is no binary to download or install |
| Help > Website | skipped |  | A link opened from the elevated FRS would start the browser elevated |
| Help > Check for updates | skipped |  | Source only, nothing to install |
| Help > About | done | Linux | Credits page, Mem Reduct on its own Inspiration card |

## Accelerators

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Keyboard shortcuts | skipped |  | There is no About or Settings dialog for F1 and F2 to open, every page is one click in the sidebar or a Ctrl+K palette entry. Alt+F4 to hide is Close to tray |

## Settings window shell

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Settings window | done | Linux | The Settings page, changes apply and save at once |
| Page list | done | Linux | Option groups as cards under folding sections, with a filter box |
| Remember last page | skipped |  | Settings is one scrolling page, FRS opens on the Dashboard |
| Reset | done | Linux | New : *Reset settings...* asks first and keeps settings.ini.bak. MCP settings, Run at startup, the no-prompt shortcut, window size and position, table layouts and page priority rules stay |
| Close | done | Linux | Nothing to close, Settings is a page |

## Settings > General

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Always on top | done | Linux | *Always on top* |
| Load on system startup | done | elevated, Linux | See Settings > Load on system startup under Main menu bar |
| Start minimized | done | Linux | *Start minimized* |
| Confirm memory cleaning | done | Linux | *Ask before a Dashboard clean* |
| Skip UAC prompt | approx | elevated | Windows only. The no-prompt shortcut, see Settings > Skip UAC prompt under Main menu bar |
| Check for updates periodically | skipped |  | Source only, nothing to install |
| Language | skipped |  | FRS is English only |

## Settings > Memory cleaning

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Areas to clean | done | Linux | New : the four methods plus five area ticks under the Clean set heading of the Auto-Clean card. Linux shows the two areas it has |
| Clean when above | done | Linux | Auto-Clean threshold, with the new pause right under it |
| Clean every | done | Linux | New : *Clean every N minutes* |
| Global hotkey | done |  | Windows only. New : *Clean hotkey* with modifiers and key. `set_settings` takes the index or the name |

## Settings > Appearance

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Transparent background | done |  | Windows only. *Transparent icons* |
| Show border | done | tray | Windows only. The frame option, see Border under Tray icon |
| Round corners | done | tray | Windows only. See Round corners under Tray icon |
| Change background instead of text | done | tray | Windows only. *Thresholds color the background*. Read in code only |
| Antialiasing | done | tray | Windows only. *Smooth digits* |
| Font | done | tray | Windows only. See Font under Tray icon |
| Colours | done | tray | Windows only. Text, background, warning and critical colour pickers. The warning and critical colours on the icon were only read in code |

## Settings > Tray icon

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Colour levels | done | Linux | *Warning at* and *Critical at* in Memory highlights |
| Mouse actions | done |  | Windows only. *Single click icons*, *Icon click toggles visibility* and the new *Middle click on an icon* |
| Balloons | done | tray | Windows only. New : *Notify after a clean* and *Notification sound*. Read in code only |

## Settings > Advanced

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Allow standby and modified cleanup on automatic cleans | skipped |  | FRS has no hidden exclusion to lift, automatic cleans run what you ticked |
| Log results to a file | done | Linux | New : *Write the log to a file* in the Log section. `app_status` gives the path, null while it is off or after writing gave up |

## Startup, window and elevation

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Unelevated monitor mode | skipped |  | Windows FRS starts elevated by its manifest. Linux already runs unprivileged when you cancel the password prompt |
| Skip UAC task | approx | elevated, Linux | New : the no-prompt shortcut on Windows. The Startup card on both systems warns when FRS sits in a folder your account can write to. Linux counts your groups and what you own, it does not read ACLs |
| Load on system startup | done | elevated, Linux | See Settings > Load on system startup under Main menu bar |
| Start minimized | done | Linux | See Settings > Start minimized under Main menu bar |
| Close to tray | done |  | Windows only. Off by default, only while an icon is up |
| Minimize to tray | done |  | Windows only. Off by default, only while an icon is up |
| Always on top at start | done | Linux | Applied within 250 ms of start, whoever sets it |
| Single instance | done | Linux | New : a second start brings the running FRS forward, on its Memory page with `-sysinfo Memory`. One FRS per settings folder, *Allow more than one FRS* turns it off. A start while the other one closes and finishes a clean waits up to 10 s for it. Linux : a lock file, the second start says FRS already runs and asks no password. Tested with test builds, which never raise another window |
| Restart after update or reboot | skipped |  | Run at startup brings FRS back after a reboot. A restart Windows starts at sign in would meet a UAC prompt, FRS needs admin |
| Window position | done | Linux | New : FRS opens where it was closed while that spot is on a monitor, sized for that monitor's scale. A window closed maximized opens maximized, restore gives its normal size and spot. Tested on a second monitor at 100 %. Linux : X11 and XWayland, not native Wayland, kept inside the monitor's work area |
| Windows 11 frame tweaks | skipped |  | Window chrome cosmetics, FRS already uses the dark title bar |
| Allow shutdown | done | Linux | FRS lets the session end |

## Command line

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| -clean | done | elevated, Linux | New : runs your clean set and exits without opening the FRS window. Exit code 0 when every step worked, 1 when a step failed or the set is empty. The lines go to frs.log when the log file is on. Tested with faked steps while frs.log was held open. Linux : through pkexec, and from root's crontab it reads root's settings |
| -clean:full | done | elevated, Linux | New : the four methods plus every area this system has |
| -help | done | Linux | New : lists the switches in a message box on Windows, on stdout on Linux, where it needs no root |
| -minimized | done | Linux | New : starts minimized. Any case, with - or / |
| -portable | done | Linux | New : settings next to the exe. When the switch alone turned portable mode on, Run at startup, the no-prompt task and the Linux autostart entry pass it on. The switch itself was only read in code, a test build always uses its own settings folder |
| -install / -uninstall | skipped |  | Mem Reduct ignores them too |
| -nowow64 | skipped |  | Only in 32 bit Mem Reduct builds, FRS has no 32 bit build |

## Settings storage and portable mode

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Settings file | done | Linux | %APPDATA%\FRS\settings.ini, the XDG config folder on Linux |
| Portable mode | done | Linux | New : a file named FRS.portable next to the exe, or `-portable`. A folder FRS can't write to falls back to the normal one and logs it. Mem Reduct also goes portable when an ini sits next to the exe, FRS does not : a stray settings.ini would move your settings without asking |

## Updates

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Update check at start | skipped |  | FRS ships as source, there are no binary releases |
| Check for updates | skipped |  | Source only, nothing to install |
| Install update | skipped |  | No installer to download, and running a downloaded installer elevated is a risk FRS won't take |
| Language pack update | skipped |  | No translations |

## Localization

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Language pack | skipped |  | FRS is English only |
| Default language | skipped |  | FRS is English only |

## About, website, misc

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| About dialog | done | Linux | Credits page |
| Website link | skipped |  | A link opened from the elevated FRS would start the browser elevated. The Credits page shows the links as text |
| Crash handler | skipped |  | A dump of the elevated FRS holds the MCP token and every setting |
| Process hardening | skipped |  | FRS draws with Direct3D 11. The GPU driver, the WARP fallback that compiles shaders at run time and third party input methods need what those policies block |

## Distribution

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Installer and portable builds | skipped |  | FRS ships as source, the setup and build scripts build it. Releases are the owner's call |
| Signed binaries | skipped |  | No binaries are published to sign |

## Known Mem Reduct quirks

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Missing "*" on the standby list label | skipped |  | A Mem Reduct bug, not copied |
| Clean from the dropdown logged as command line | skipped |  | A Mem Reduct bug, not copied. Empty commands log under their own name |
| -clean ignores the chosen areas | skipped |  | A Mem Reduct bug, not copied : FRS `-clean` runs your clean set |
| Success balloon with the warning icon | skipped |  | A Mem Reduct bug, not copied : info icon, warning only when a step failed |
| Timer clean fires right away the first time | skipped |  | A Mem Reduct bug, not copied : the timer counts from launch or from turning it on |
| No check that danger is above warning | skipped |  | FRS checks critical first too, a warning level above critical just never shows. Nothing added |
| Threshold 0 cleans every 30 s | skipped |  | FRS keeps the threshold at 1 to 100 in Settings and `set_settings`. A hand edited file can still hold 0, left as it is |
| Always on top applies late from Settings | skipped |  | FRS applies it within 250 ms whoever sets it |
