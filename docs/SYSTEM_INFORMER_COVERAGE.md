# System Informer coverage

FRS brings in the RAM related features of [System Informer](https://github.com/winsiderss/systeminformer) (SI from here on). This page goes through all of them, 1707 rows grouped the way the work was planned, and says for each one what FRS does with it.

It is made from the coverage notes written while building each part, so the notes are short. When a note and the code disagree, the code is right. Feature names are SI's. A name in backticks is a tool or field name from SI's own MCP plugin, AgentTools.

## Status words

- **done** : FRS has it and it works like SI's.
- **approx** : FRS has it in another form or does it another way, the note says how.
- **skipped** : not built, the note says why.
- **impossible** : can't be done without SI's kernel driver, see [below](#why-some-things-are-impossible).

The **Not run** column names paths that were written but never ran :

- **elevated** : the part that needs admin rights or reaches other users' processes never ran. All Windows testing used an unelevated test build on processes started for the test.
- **Linux** : the row has real Linux code and it never ran. The Linux build is compile and link checked with a cross compiler, there was no Linux machine to run it on. That holds for every Linux path in FRS, marked or not.

## Summary

| Section | Rows | Done | Approx | Skipped | Impossible | Not run elevated | Linux code not run |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| [1. System memory](#1-system-memory) | 276 | 158 | 26 | 91 | 1 | 12 | 183 |
| [2. Process memory list](#2-process-memory-list) | 112 | 79 | 18 | 15 | 0 | 12 | 42 |
| [3. Process memory details](#3-process-memory-details) | 110 | 87 | 6 | 15 | 2 | 11 | 66 |
| [4. Virtual memory](#4-virtual-memory) | 417 | 303 | 29 | 79 | 6 | 17 | 154 |
| [5. Kernel memory](#5-kernel-memory) | 121 | 62 | 15 | 44 | 0 | 5 | 62 |
| [6. Working set watch](#6-working-set-watch) | 18 | 13 | 4 | 1 | 0 | 3 | 14 |
| [7. GPU and .NET memory](#7-gpu-and-net-memory) | 111 | 72 | 9 | 30 | 0 | 8 | 37 |
| [8. Tray and status surfaces](#8-tray-and-status-surfaces) | 142 | 87 | 12 | 43 | 0 | 16 | 59 |
| [9. Options](#9-options) | 63 | 21 | 17 | 25 | 0 | 3 | 15 |
| [10. MCP](#10-mcp) | 337 | 211 | 71 | 54 | 1 | 0 | 0 |
| **Total** | **1707** | **1093** | **207** | **397** | **10** | **87** | **632** |


The MCP rows carry no Not run marks. Every MCP tool ran on Windows in an unelevated build, none ran elevated or on Linux.

## Why some things are impossible

SI does a few things through its own kernel driver. FRS can't use that driver : the driver only answers programs it has verified, it checks the signature of the calling program's file and refuses every request from anything else. FRS is not signed by System Informer, so even with the driver installed every call would be refused. FRS has no driver of its own and won't get one.

Without the driver there is no user mode way to :

- read the memory of a protected process (PP or PPL). FRS spots them and says so instead of failing with an error code
- read the paged pool limit and other kernel globals. N/A in the UI, null over MCP
- list the views of a section across all processes. The Mapped by list finds file backed views by path only
- read kernel module memory, so no kernel image coherency check
- list heaps of a protected process, read its shared working set split, trim it on its own or dump it

A few SI features that use the driver had a user mode way worth trying :

- the job object of a process : works without the driver by scanning the handle table for job handles, as long as FRS can open the job (approx)
- emptying the compression store : FRS opens the Memory Compression process and trims it from user mode. Windows denies that open without admin rights, with admin rights it never ran
- the system store trim request : not built, it is dead code in SI and could only be tested by trimming the real store

## Biggest groups not built

- SMBIOS records other than the RAM ones (types 16 to 20 are shown), ACPI memory tables, CPU cache sizes
- the cache latency benchmark, NPU memory, live kernel dumps and image coherency : borderline for a RAM tool, left out for this update
- SI's older strings dialog : FRS has one strings UI, the Strings tab
- rare minidump flags : the Custom preset offers the common ones
- undocumented memory commands that SI has no menu for
- process control (end, freeze), not RAM
- SI window chrome : dialog placement, opacity, customize dialogs. FRS keeps everything in one window with pages and tabs
- the parts of SI's MCP plugin built on named pipes, sessions, client elicitation and resources : FRS serves plain HTTP with a token

## Only written, never run

Elevated : the success path of most Empty commands (only the modified list ran, it needs no admin on Windows 11 build 26200), trimming and page priority on other users' processes, job lookup and quota limits of processes FRS can only open elevated, heaps, locks and dumps of other users' processes, IFEO page priority writes to HKLM, the tray and taskbar overlay as the elevated FRS shows them.

Linux : all of it. Memory page from /proc and sysfs, processes and cgroups, regions from maps and smaps, strings from /proc/pid/mem, dumps through gcore, WS Watch through perf, GPU through DRM fdinfo, slab caches, vmalloc and swap on the Kernel page.

## 1. System memory

### Data feed and privileges

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| System performance snapshot | done | Linux |  |
| Physical page count | done | Linux | Re-read every tick so hot added RAM shows |
| Commit and physical history | done | Linux | MB floats in Snap&lt;SysHist&gt;, runs while the page is closed |
| Refresh source | approx | Linux | Own sampler job on memRefreshMs, not a provider event |
| Privilege setup | done | elevated, Linux |  |
| Sample time text | done | Linux | "Unknown time" fallback |
| Unused memory counters | done | Linux | More counters fold on Details |
| Physical used definition | done | Linux | Explained in the Physical memory (?) tooltip |

### Entry points

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| System information | approx | Linux | FRS has no View menu : sidebar page Memory, or the Ctrl+K palette |
| Start Task Manager, Resource Monitor, Performance Monitor | skipped |  | Only launches external tools |
| -sysinfo Memory | done | Linux | Opens FRS on the Memory page. On Linux the pkexec relaunch passes the argument on |

### Summary row

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Memory section registration | approx | Linux | No summary view, Memory is a sidebar page |
| Summary graph | approx | Linux | Both graphs are on the Memory page Overview, small ones in the header |
| Summary tooltip | done | Linux | Label added |
| Panel title | done | Linux |  |
| Panel subtitle | approx | Linux | Physical and Commit stat cards with percent of total as the big number |
| Panel subtitle overflow | done | Linux | Cards drop " of total" when short of room |

### Graphs

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Memory heading | done | Linux | Shell header shows the page title |
| Installed memory label | done | Linux | "total" fallback |
| Commit charge caption | done | Linux |  |
| Commit charge graph | done | Linux |  |
| Commit graph overlay | approx | Linux | On the caption row, on the graph the line ran through the text |
| Commit graph max label | approx | Linux | The limit is in the caption row, same reason |
| Commit graph tooltip | done | Linux | Plus top process by private bytes |
| Physical memory caption | done | Linux |  |
| Physical memory graph | done | Linux |  |
| Physical graph overlay | approx | Linux | On the caption row, on the graph the line ran through the text |
| Physical graph max label | approx | Linux | The total is in the caption row, same reason |
| Tooltip Physical memory line | done | Linux | Plus top process by working set |
| Tooltip Compressed memory line | done | Linux | Labeled "now" |
| Tooltip Total compressed line | done | Linux | Labeled "now" |
| Tooltip Total memory saved line | done | Linux | Labeled "now", also on Details |
| Tooltip without compression | done | Linux |  |
| Graph layout | approx | Linux | Stacked, heights follow the window so it all fits at 900x600 |

### Composition bar

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Memory composition bar | done | Linux | Cached per tick, N/A case shows used and available plus the reason instead of an empty bar |
| Bar segment In use | done | Linux |  |
| Bar segment Modified | done | Linux |  |
| Bar segment Modified (no write) | done | Linux |  |
| Bar segment Standby | done | Linux |  |
| Bar segment Zeroed | done | Linux |  |
| Bar segment Free | done | Linux |  |
| Bar tooltip | done | Linux | ui::stackBar, plus a legend |

### Panel : commit charge

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Current | done | Linux |  |
| Peak | done | Linux | Linux labeled Peak since FRS started |
| Limit | done | Linux |  |

### Panel : physical memory

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Current | done | Linux |  |
| Total | done | Linux |  |
| Reserved | done | Linux |  |
| Cache WS | done | Linux |  |
| Kernel WS | done | Linux | Linux KernelStack |
| Driver WS | done | Linux | Linux loaded module size, tooltip says so |

### Panel : paged pool

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Working set | done | Linux | Linux N/A with a tooltip pointing at SReclaimable |
| Virtual size | done | Linux | Linux N/A |
| Allocs delta | approx | Linux | Real allocation count per second, SI shows a size change under this name. Windows keeps 0 here on this build, so it reads N/A |
| Frees delta | approx | Linux | Per second instead of per tick. N/A on this Windows build |
| Paged pool limit | impossible |  | Kernel global, only the driver can read it. Shows N/A, null over MCP |

### Panel : non-paged pool

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Usage | done | Linux | Linux SUnreclaim |
| Limit | approx | Linux | Documented formula (RAM or 16 TB), not read from the kernel, which needs the driver |
| Allocs delta | approx | Linux | Real allocation count per second, the size change has its own row. N/A on this Windows build |
| Frees delta | approx | Linux | Per second instead of per tick. N/A on this Windows build |

### Panel : paging

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Page faults delta | approx | Linux | Per second, not per tick |
| Page reads delta | approx | Linux | Per second, pages not I/Os, matches perfmon Pages Input/sec and the MCP description says so |
| Pagefile writes delta | approx | Linux | Per second |
| Mapped writes delta | approx | Linux | Per second |

### Panel : mapped IO

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Mapped reads | approx | Linux | Per second |
| Mapped writes | approx | Linux | Per second |

### Memory lists

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Memory list query | done | elevated, Linux | On Windows 11 it answers without admin rights too |
| Zeroed | done | Linux |  |
| Free | done | Linux |  |
| Modified | done | Linux |  |
| Modified no-write | done | Linux |  |
| Modified pagefile | done | Linux |  |
| Standby | done | Linux |  |
| Priority 0 | done | Linux |  |
| Priority 1 | done | Linux |  |
| Priority 2 | done | Linux |  |
| Priority 3 | done | Linux |  |
| Priority 4 | done | Linux |  |
| Priority 5 | done | Linux |  |
| Priority 6 | done | Linux |  |
| Priority 7 | done | Linux |  |
| More button | approx | Linux | No window to open, the Memory lists tab holds every row |
| Empty button | done | Linux | Empty button above the tabs, on every tab |
| Memory Lists window | approx | Linux | A sub tab, not a modeless window |
| Window position | skipped |  | No such window in FRS |
| Close button | approx | Linux | No window to close |
| Bad | done | Linux | Red when not 0 |
| Repurposed header | done | Linux |  |
| Repurposed total | done | Linux | Linux `pgsteal_file` |
| Repurposed priority 0 | done | Linux |  |
| Repurposed priority 1 | done | Linux |  |
| Repurposed priority 2 | done | Linux |  |
| Repurposed priority 3 | done | Linux |  |
| Repurposed priority 4 | done | Linux |  |
| Repurposed priority 5 | done | Linux |  |
| Repurposed priority 6 | done | Linux |  |
| Repurposed priority 7 | done | Linux |  |
| Compression store statistics | done | Linux | Linux zram `mm_stat`, then zswap |

### Empty commands

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Empty menu | done | Linux | Tips and the Empty all confirm follow the OS |
| Combine memory pages | done | elevated, Linux | Never sent to the OS in testing, only through a test hook. Linux : turns KSM on until reboot |
| Empty system file cache | done | elevated, Linux | Reports size before and after. Only the refused path ran |
| Empty registry cache | done | elevated, Linux | Never sent to the OS in testing, only through a test hook |
| Empty working sets | done | elevated, Linux | Only the refused path ran |
| Empty modified page list | done | Linux | Windows 11 build 26200 runs it even without admin rights |
| Empty modified file cache | done | elevated, Linux | Per volume results and 20 s timeout. Flush path not |
| Empty standby list | done | elevated, Linux | Only the refused path ran |
| Empty priority 0 standby list | done | elevated, Linux | Only the refused path ran |
| Empty all | done | elevated, Linux | Refuses without admin like SI, runs in SI order. Tried only through a test hook |
| Command progress dialog | done | Linux | Modal, Close only after the worker ended |
| Progress display | done | Linux | Bar for several commands, moving bar for one |
| Live log text | done | Linux | appLog and `memory_command` result |
| Cancel | done | Linux | Stops between commands, dialog stays until the worker ends |
| Failure text | done | Linux | FormatMessage from ntdll, one line, hex fallback |
| Elevation fallback | skipped |  | FRS already runs elevated |
| No confirmation on Empty commands | done | Linux | The Empty menu runs at once like SI, Empty all and every Ctrl+K entry ask first |
| Empty compression store | approx | elevated, Linux | Direct open plus a working set trim from user mode. Without admin it reports not available, with admin it never ran. Linux compacts zram |
| Candidate: open Memory Compression from user mode | done | elevated | Windows denies the open without admin. With admin it was never tried |
| Candidate: system store trim request | skipped |  | Dead code in SI, unverified, and only testable by trimming the real store |

### Hidden commands, no SI UI

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| MemoryCaptureAccessedBits | skipped |  | Undocumented memory command with no SI menu, not built |
| MemoryCaptureAndResetAccessedBits | skipped |  | Undocumented memory command with no SI menu, not built |
| Combine flags | skipped |  | Undocumented memory command with no SI menu, not built |
| Per process combine | skipped |  | Undocumented memory command with no SI menu, not built |
| Store high memory priority | skipped |  | Undocumented memory command with no SI menu, not built |
| File cache limits | skipped |  | Undocumented command with no SI menu, not built |
| Memory classes never queried | skipped |  | Undocumented memory command with no SI menu, not built |

### Hardware

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| SMBIOS memory device scan | done | Linux | One parser in smbios.cpp, read once on the sampler thread |
| Slots used | done | Linux | System memory arrays only, video and flash arrays no longer count |
| Form factor | done | Linux | Per stick, Mixed when they differ |
| Technology | done | Linux | Same |
| Memory type | done | Linux | Same, DDR2-FM-DIMM typo not copied |
| Speed | done | Linux | Highest across sticks, bit 31 of the extended field masked |
| DRAM graph | skipped |  | DRAM energy |
| DRAM tooltip | skipped |  | DRAM energy |
| Resource Memory range | skipped |  | Device address ranges |
| Resource Large memory range | skipped |  | Device address ranges |
| DRAM | skipped |  | DRAM energy |
| Memory related boot log parameters | skipped |  | Boot log |

### SMBIOS RAM records

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| SMBIOS window | approx | Linux | Hardware tab, RAM record types only, one fold per record, Copy all |
| Handle | done | Linux |  |
| Physical array handle | done | Linux |  |
| Memory error information handle | done | Linux | 0xFFFE and 0xFFFF decoded, SI prints them as hex |
| Total width | done | Linux |  |
| Data width | done | Linux |  |
| Size | done | Linux | 0xFFFF unknown (SI would print 32767 KB), bit 31 of extended masked |
| Form factor | done | Linux |  |
| Device set | done | Linux |  |
| Device locator | done | Linux |  |
| Bank locator | done | Linux |  |
| Memory type | done | Linux | 0x1F uses the spec name Logical non-volatile device, not SI wording |
| Type detail | done | Linux |  |
| Speed | done | Linux |  |
| Configured speed | done | Linux |  |
| Manufacturer | done | Linux |  |
| Serial number | done | Linux | UI shows it |
| Asset tag | done | Linux |  |
| Part number | done | Linux |  |
| Rank | done | Linux |  |
| Minimum voltage | done | Linux |  |
| Maximum voltage | done | Linux |  |
| Configured voltage | done | Linux |  |
| Technology | done | Linux |  |
| Mode capabilities | done | Linux |  |
| Firmware version | done | Linux |  |
| Module manufacturer ID | done | Linux |  |
| Module product ID | done | Linux |  |
| Subsystem controller manufacturer ID | done | Linux |  |
| Subsystem controller product ID | done | Linux |  |
| Non-volatile size | done | Linux |  |
| Volatile size | done | Linux |  |
| Logical size | done | Linux | CacheSize at 0x44 added |
| PMIC0 manufacturer ID | done | Linux |  |
| PMIC0 revision | done | Linux |  |
| RCD manufacturer ID | done | Linux |  |
| RCD revision | done | Linux |  |
| Handle | done | Linux |  |
| Location | done | Linux |  |
| Use | done | Linux |  |
| Error correction | done | Linux |  |
| Maximum capacity | done | Linux |  |
| Error information handle | done | Linux | 0xFFFE and 0xFFFF decoded |
| Number of memory devices | done | Linux |  |
| 32-bit memory error Handle | done | Linux |  |
| Type | done | Linux | "Corrected singled-bit" typo not copied |
| Granularity | done | Linux |  |
| Operation | done | Linux |  |
| Vendor syndrome | done | Linux |  |
| Array error address | done | Linux |  |
| Device error address | done | Linux |  |
| Error resolution | done | Linux |  |
| 64-bit memory error Handle | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Type | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Granularity | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Operation | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Vendor syndrome | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Array error address | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Device error address | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Error resolution | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Handle | done | Linux |  |
| Starting address | done | Linux |  |
| Ending address | done | Linux | Last byte of the last KB, SI is 1 KB short, range size added |
| Array handle | done | Linux |  |
| Partition width | done | Linux |  |
| Handle | done | Linux |  |
| Starting address | done | Linux |  |
| Ending address | done | Linux | Same |
| Device handle | done | Linux |  |
| Array mapped address handle | done | Linux |  |
| Partition row position | done | Linux |  |
| Interleave position | done | Linux |  |
| Interleave data depth | done | Linux |  |
| Handle | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Error detection method | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Error correction capabilities | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Supported interleave | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Maximum module size | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Supported speeds | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Supported types | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Module voltage | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Handles | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Enabled error correction capabilities | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Handle | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Socket designation | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Bank connections | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Current speed | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Memory type | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Installed size | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Enabled size | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Error status | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Handle | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Type | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Maximum load | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |
| Channel #N handle and load | skipped |  | Only the SMBIOS RAM records, types 16 to 20, are shown |

### ACPI memory topology (borderline)

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Memory (Node: N, Base: 0x...) | skipped |  | Firmware table |
| System Locality Information Table (SLIT) | skipped |  | Firmware table |
| PCI Memory-Mapped Configuration (MCFG) | skipped |  | Firmware table |

### CPU cache sizes (borderline)

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| L1 cache | skipped |  | CPU caches |
| L2 cache | skipped |  | CPU caches |
| L3 cache | skipped |  | CPU caches |
| Cache record | skipped |  | CPU caches |

### Cache latency (borderline)

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Cache Latency window | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Pointer chase core | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Buffer allocation | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Thread pinning | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| TSC calibration | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Cache topology, CPUID | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Cache topology, NT | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| DRAM plateau detection | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Level latency estimate | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Tab Overview | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Graph L1 | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Graph L2 | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Graph L3 | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Graph DRAM | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Graph hover tooltip | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Graph menu One or Two graphs per row | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Details panel info labels | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Details panel value labels | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Status line | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Tab Table | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Size column (KB, cycles) | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| 8 column | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| 16 column | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| 32 column | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| 64 column | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| 128 column | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| 256 column | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| 512 column | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Table sort | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Table copy | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| OK button | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |
| Persistence | skipped |  | Not built, borderline : a CPU cache latency benchmark, not RAM use |

### Debug console (borderline)

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Debug console : mem address [numberOfBytes] | skipped |  | Debug console about SI's own memory |
| Debug console : enableleakdetect | skipped |  | Debug console about SI's own memory |
| Debug console : leakdetect [all] | skipped |  | Debug console about SI's own memory |
| Debug console : dumpobj and dumpautopool | skipped |  | Debug console about SI's own memory |
| Debug console : object tracking commands (stats, objects, objtrace and the like) | skipped |  | Debug console about SI's own memory |

## 2. Process memory list

### Data source

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Process snapshot | done | Linux | NtQuerySystemInformation class 5, 670 to 710 rows here. Numbers match exactly on System, Memory Compression |
| Process information layout (`SYSTEM_PROCESS_INFORMATION`) | done |  | Walk bounded by NextEntryOffset and buffer size |
| VM counters block copy | done |  | Read field by field |
| Per tick field refresh | done | Linux | Sampler job follows memRefreshMs and memPaused |
| Process information extension | done |  | Own SPIX struct with `static_assert`, bounds checked |
| Extended VM counters query (`VM_COUNTERS_EX2`) | approx | Linux | Fallback for shared commit when the extension is too short, never needed on this Windows, so never ran |
| PID reuse check | done | Linux | Key is pid + start, CreateTime rechecked after every open |
| Provider thread and UI queue | done |  | Three sampler jobs : procs, procs-info, procs-ws |
| Query handle per process | approx | elevated, Linux | Limited handle opened per pass, not kept, so no handles pin exited processes |
| Stripped handle detection | done | elevated | NtQueryObject GrantedAccess on a full query handle, tooltip explains it |
| Deferred process query | done |  | Procs-info job, statics once (Linux rereads them every 3 s, exec and setuid change them) |
| Removed process state | approx | Linux | Row and history go at the next tick, an open process window says the process exited |
| Latest snapshot pointer | approx |  | A snapshot object holds the parsed list |

### Sampling, deltas, history

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Delta bookkeeping | done | Linux | Per tick, no full delta on the first tick, null through MCP on that tick |
| Private bytes history | done | Linux | Float MB ring per ProcRef, freed on exit |
| Working set history (commented out) | done | Linux | WS ring next to it |
| Ring buffer semantics | done |  | 60 to 300 samples from memHistN, oldest first for readers |
| Sample time mapping | done |  | One shared time ring, unixSec per sample in ProcHist |
| Max usage bookkeeping | approx |  | The Memory history keeps the biggest process per sample |
| Elapsed time | done |  | ProcList.dtMs, growth highlight is per second from it |
| UI tick invalidation | done |  | Page rebuilds only when seq or the view changed, about 1 rebuild per tick over 17661 frames |

### Value formatting

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Size text | approx |  | 1024 based units, not SI's 1000 |
| Grouped count text | done |  | util::count |
| Signed delta text | done |  | util::delta, blank at zero |

### Columns

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Private bytes | done | Linux | PagefileUsage |
| Peak private bytes | done | Linux |  |
| Working set | done | Linux | Default column, sort key by default |
| Peak working set | done | Linux |  |
| Private WS | done | Linux | Exact against `Win32_PerfRawData` WorkingSetPrivate |
| Shared WS | done | elevated, Linux | Walk on the procs-ws job only while the column is on or a tool asked |
| Shareable WS | done | elevated, Linux | Same walk |
| Virtual size | done | Linux |  |
| Peak virtual size | done | Linux |  |
| Minimum working set | done | elevated | ProcessQuotaLimits on the limited handle |
| Maximum working set | done | elevated | Same query |
| Private bytes delta | done | Linux |  |
| Page faults | done | Linux | Exact against `Win32_Process` PageFaults, 64 bit total |
| Page faults delta | done | Linux | Has a total too |
| Page priority | done |  | Fetched on the worker, set then read back |
| Paged pool | done |  | Linux N/A |
| Peak paged pool | done |  | Matches `Win32_Process` |
| Non-paged pool | done |  | Linux N/A |
| Peak non-paged pool | done |  | Matches `Win32_Process` |
| Shared commit | done | Linux | From the extension |
| Private bytes history | done | Linux | Sparkline scaled to the row's own history, a deliberate change from SI |
| Job Object ID | done | Linux | Blank when not in a job |
| Subprocesses | done | Linux | Built although first judged not worth porting |
| Bits and Architecture | done | elevated, Linux | Built although first judged not worth porting |
| Thread local storage | skipped |  | Not memory |
| Default column set | done |  | Name, PID, Working set, Private WS, Private bytes, Private delta, Faults delta, Hard delta, History |
| Hard faults column | done | Linux | HardFaultCount |
| Hard faults delta column | done | Linux |  |

### Totals and aggregation

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Include usage of collapsed processes | done | Linux | Option procAggregate, off by default, not for peaks, Shared, Shareable, Shared commit |
| Column header totals | approx | Linux | A totals row under the header, option procTotals. Sum of what the filter shows |
| Blank versus zero cells | approx | Linux | Unknown prints N/A, a real zero size prints 0 B, zero deltas stay blank |

### Tree and sort

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Process tree build | done | Linux | Parent must be strictly older, which also rules out cycles |
| Orphan handling | done | Linux | A dead or reused parent makes the row a root |
| Collapse all | done |  |  |
| Expand all | done |  |  |
| Sorted view flattens tree | approx |  | Flat is its own view, Tree keeps its structure and sorts siblings, SI flattens on sort |
| Column header sort | done |  | Tri-state, numbers descending first |
| Stable sort tie break | done |  | Start then pid |
| Private delta sort | done |  | Zero rows last both ways |
| History column sort | done |  | Sorts by private bytes |
| Sort child processes | approx |  | Always on in Tree, no toggle |
| Sort root processes | approx |  | Always on in Tree, no toggle |
| Scroll to new processes | skipped |  | List convenience |

### Column management

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Size column to fit | done |  | ImGui table menu, right click a header |
| Size all columns to fit | done |  | Same menu |
| Hide column | done |  | Same menu |
| Choose columns | done |  | Same menu lists every column |
| Reset sort | approx |  | Third click on the header clears the sort, the menu has no such item |
| Column layout persistence | done |  | ui.ini, kept over a restart |
| Save column set | skipped |  | Memory columns preset instead |
| Load column set | skipped |  | Memory columns preset instead |
| Organize column sets | skipped |  | Memory columns preset instead |

### Filter, search, grouping

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Filter engine | done |  | Predicate at rebuild, children of a hidden parent move up a level |
| Search box | done |  | ui::search |
| Search matching modes | approx |  | Case-insensitive substring only, no regex or fuzzy |
| Search fields | approx | Linux | Name, pid, user, path |
| Hide processes from other users | done | elevated, Linux | Token user on Windows |
| Hide signed processes | skipped |  | No signature checks |
| Hide system processes | approx | elevated, Linux | System rows, service accounts, images under the Windows folder, session 0 rows FRS can't open |
| Process grouping by name | done | Linux | Group view and group:true in `list_processes` |
| Group membership rules | approx |  | Name only, case-insensitive, no SID, ancestry or window rule |
| Middle click select | done |  | Middle click, same name in Flat, Ctrl keeps the selection |

### Export

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Copy rows | done |  | Copy row and Ctrl+C with visible columns in display order |
| Save process list | done | Linux | Export CSV, editable path, plain bytes, `CREATE_NEW` then asks before replacing |

### Highlight and tooltips

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| New process highlight | done |  | Built although first judged not worth porting |
| Removed process highlight | skipped |  | List convenience |
| Job processes highlight | skipped |  | Needs the significant job test, which needs the driver |
| Highlight precedence | skipped |  | Only two highlights exist here, growth wins over new |
| Process tooltip memory content | done |  | Row tooltip with memory detail, new work over SI |
| Tooltip flag Process is in a job | done | Linux | Process is in a job from JobObjectId, needs no driver |
| Tooltip flag deep freeze | skipped |  | List convenience, not memory |

### Process menu

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Properties | done |  | Open details, double click, Enter, all through procwin::open |
| Pseudo process menu rule | done | Linux | Trim and page priority disabled on pseudo and system rows, and refused again inside reduceWs and setPagePrio |
| Multi select menu rule | done |  | Multi select keeps Reduce working set and Copy, Open details and Page priority go disabled |
| Terminate | skipped |  | Process control (end or freeze a process), not RAM, not built |
| Terminate tree | skipped |  | Process control (end or freeze a process), not RAM, not built |
| Process context menu | approx |  | Open details, Reduce working set, Page priority, Copy row, Copy pid. Dumps, heaps and the rest are process window tabs |
| Menu enable rules | done |  | None selected opens no menu, pseudo and system rules, multi select rules |
| Same items in process properties | approx |  | Reduce working set and page priority are also on the process window Memory tab |
| Efficiency mode | skipped |  | No memory effect |

### Pseudo and special processes

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| System Idle Process | done |  | Real pid 0 row, forced name, pseudo, target() refuses it |
| Interrupts | skipped |  | No memory values |
| DPCs | skipped |  | No memory values |
| System | done | elevated | Row shown from the snapshot, trim and page priority refused |
| Memory Compression | done | elevated, Linux | By Classification, 2.5 GB row shown unelevated, trim refused, tooltip |
| Registry | done | elevated | By Classification, tooltip, refused |
| Secure System | done |  | By Classification, refused |
| Classification test | done | Linux | Classification, flags reach `list_processes` |

## 3. Process memory details

### Statistics

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Statistics value columns | done | Linux | Tab Memory card Statistics : Property, Value, Min, Max, Difference, Average, Median, ring of 512 |
| Memory group | done | Linux | 24 rows in SI's order (SI's 22 plus the two commit limit rows it comments out) |
| Statistics update | done | Linux | Samples once per procs tick while Memory or Limits is drawn |
| Statistics handle | done | elevated | `PROCESS_QUERY_INFORMATION` on the worker |
| Private bytes | done | Linux |  |
| Private bytes delta | done | Linux | Signed, formatted with a sign |
| Peak private bytes | done | Linux | From the snapshot, not compared on its own |
| Virtual size | done | Linux |  |
| Peak virtual size | done | Linux |  |
| Page faults | done | Linux | Matches `Win32_Process` PageFaults |
| Page faults delta | done | Linux |  |
| Hard faults | done | Linux | From the snapshot |
| Hard faults delta | done | Linux | Same source, tip calls it the measure of what a trim cost |
| Working set | done | Linux |  |
| Peak working set | done | Linux |  |
| Private WS | done | Linux | Page walk ShareCount 0, falls back to the snapshot value. `get_process` says which one it was (`private_from`) |
| Shareable WS | done | Linux | Page walk, N/A when it fails, not 0 |
| Shared WS | done | Linux | Page walk, N/A when it fails, not 0 |
| Paged pool bytes | done |  | Linux N/A |
| Peak paged pool bytes | done |  | From the snapshot |
| Nonpaged pool bytes | done |  | Linux N/A |
| Peak nonpaged pool bytes | done |  | From the snapshot |
| Shared commit | done | Linux | SI bug not copied (never assigned there), from SharedCommitCharge in the snapshot only |
| Private commit | done | Linux | ProcessJobMemoryInformation |
| Peak private commit | done | Linux | Same, equal to PeakJobMemoryUsed through a job handle when both were read together (3035136) |
| Private and total commit limit | done | Linux | Shown in Statistics and Limits, not in SI's UI. Measured |
| Page priority | done |  | Name, Min, Max and Difference empty like SI. Linux N/A |

### Graphs

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Private bytes graph | done | Linux | Private bytes from procs::history, ui::graph |
| Private bytes graph scale | done | Linux | From the peak by default, raised to the largest visible sample |
| Private bytes Y axis label | done | Linux | Size label top left with a backing so the line does not cross it |
| Private bytes overlay text | done | Linux | Current value right of the caption |
| Private bytes tooltip | done | Linux | Value and sample time, drawn from the ring |
| Graph refresh rules | done | Linux | Keeps the last ring and row after exit |
| Graph layout | approx | Linux | Two graphs side by side, stacked when narrow |

### Working set counters

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Working set counters | done | Linux | MemoryWorkingSetInformation on the worker, total, private, shareable |
| Extended page attributes | done | Linux | MemoryWorkingSetExInformation in batches of 256 : locked, highest priority |
| Private, shareable, shared WS of a protected process | impossible | elevated | Private WS comes from the snapshot, Shareable and Shared show N/A with the reason |

### Working set and priority actions

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Reduce working set | done | Linux | Button and `reduce_working_set` through procs::reduceWs |
| Page priority submenu | done |  | Segmented control in Actions through procs::setPagePrio, confirmed by kernel32. Windows only |
| Page priority Normal | done |  | Set through the tool, kernel32 reads 5. The control runs the same call. Windows only |
| Page priority Below normal | done |  | kernel32 reads 4. Windows only |
| Page priority Medium | done |  | kernel32 reads 3. Windows only |
| Page priority Low | done |  | Set through the tool, kernel32 reads 2. Windows only |
| Page priority Very low | done |  | The tool accepted it on the process, kernel32 was read on a thread only (1). Windows only |
| Page priority radio check | done |  | The selected segment follows the live value, other values print "Now : Lowest" beside it. Windows only |
| Continue on error | done |  | No modal per process. `reduce_working_set` returns one result per target with its error and goes on, missing |
| System process refused | done | Linux | On Windows : System and pseudo refused before any OS call, threads of them too |
| Process working set control | skipped |  | Undocumented memory command with no SI menu, not built |
| Per range page priority | skipped |  | No caller in SI |
| Low memory priority worker technique | approx | Linux | Only the process details worker thread runs at low page priority and below normal |
| Reduce working set of one process | impossible | elevated | Opening a protected target fails and is reported |

### Page priority rules

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Save for &lt;process&gt; | done | Linux | Save for &lt;name&gt; tick in Actions and `page_priority_rules`, a second click clears, saves the current priority |
| Save for this command line | done | Linux | Tick disabled with no command line, exact match, wins over a name |
| Save for &lt;process&gt; (IFEO) | done | elevated | Value only under PerfOptions, Debugger, IoPriority and a subkey all survived a remove. Windows only |
| Default page priority dialog | done | elevated | Modal with five radios, preselects the saved value, Save, Remove, Cancel. Windows only |
| Configure page priority for executable | approx | elevated | No file picker : the rule form and `page_priority_rules` take a program name. Windows only |
| Saved rule tick marks | done | Linux | The three ticks follow rules and the IFEO value |
| Save items in the mini info list | done | Linux | The Live view row menu has the same save items. pagerules.ini holds it, IFEO greyed unelevated |
| Reapply saved page priority | done | Linux | A sampler job each tick, empty rules return at once, new spawn got its priority within a tick |
| Picking a priority updates the saved rule | done | Linux | The Processes menu goes through the same call |
| Rule storage | done | Linux | pagerules.ini through plat::readFile and writeFile, read back to catch a failed write |
| Page priority column | done | Linux | Table column shows the name, not SI's raw number |
| Delete rule | done | Linux | Delete per row and action remove |
| Copy rule rows | done | Linux | Copy per row |

### Thread page priority

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Page priority submenu | done |  | Right click menu on a thread row with the current value checked, shown and used, plus tid on `set_page_priority` |
| Normal | done |  | kernel32 reads 5 after the tool set it back |
| Below normal | done |  | Same call and menu |
| Medium | done |  | Same call and menu |
| Low | done |  | Same call and menu |
| Very low | done |  | Picked in the menu, kernel32 reads 1 for that thread only |
| Page priority | done |  | Table in fold Threads, read on the worker every 2 s only while the fold is open |

### Limits (job and quota)

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Set working set quota helper | done | elevated, Linux | Min and max MB with Apply and an ask in Limits, UI only no MCP tool for it |
| Job membership detection | done | Linux | NtIsProcessInJob with the exact status compare, snapshot job id as fallback when the open fails |
| Significant job test | skipped |  | Needs the driver |
| Job tab gate | approx | Linux | No gate : Limits is always there and says why when a job can't be read |
| User mode job lookup | done | elevated | Walks the named jobs in \BaseNamedObjects and the session's |
| Job memory limit | done | Linux | Read from the job. Without a job handle it still comes from the process and the row says so |
| Process memory limit | done | Linux | Read from the job, N/A with the reason without a job handle |
| Working set minimum | done | elevated, Linux | A job with a real value can't be made unelevated, SetInformationJobObject returns 1314 |
| Working set maximum | done | elevated, Linux | Same |
| Job page faults | done | Linux | TotalPageFaultCount of the job, nothing independent to compare it with |
| Peak process usage | done |  | From the job, nothing independent to compare it with |
| Peak job usage | done | Linux | Equal to peak private commit of the process view when both were read together (3035136) |
| Terminate job | skipped |  | Process control (end or freeze a process), not RAM, not built |
| Processes in job | skipped |  | Active and total counts are shown, not the list |
| Open the job of a process | approx | elevated, Linux | Works without the driver : handle table scan for job handles, `NtIsProcessInJob`, nested jobs innermost first. Only jobs FRS can open |
| Job Statistics page | done | Linux | Memory rows and process counts in the job card |
| Job tab | approx | Linux | Name, how it was found, memory limits |

### Thread stack memory (borderline)

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Stack usage | done | Linux | Column Stack "committed of reserved (pct)" from NtTib and AllocationBase, WOW64 branch by reading TEB32 |
| Kernel stack usage | skipped |  | Needs the driver |
| Shadow stack capture | skipped |  | Judged not worth porting |

### Thread wait reasons (borderline)

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Wait reason FreePage | done | Linux | Memory wait column and a count, Waiting state with this reason. No thread was in this wait during testing |
| Wait reason PageIn | done | Linux | Same, never seen in testing |
| Wait reason PoolAllocation | done | Linux | Same, never seen in testing |
| Wait reason WrFreePage | done | Linux | Same, never seen in testing |
| Wait reason WrPageIn | done | Linux | Same, never seen in testing |
| Wait reason WrPoolAllocation | done | Linux | Same, never seen in testing |
| Wait reason WrVirtualMemory | done | Linux | Same, never seen in testing |
| Wait reason WrPageOut | done | Linux | Same, never seen in testing |
| Wait reason WrPhysicalFault | done | Linux | Same, never seen in testing |
| Wait reason WrMdlCache | done |  | Same, never seen in testing. Windows only |

### Memory protection flags (borderline)

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| DEP | skipped |  | Security setting, not memory use |
| ASLR | skipped |  | Security setting, not memory use |
| CF Guard | skipped |  | Security setting, not memory use |
| ASLR | skipped |  | Security setting, not memory use |
| CF Guard | skipped |  | Security setting, not memory use |
| Restricted page combining | done | Linux | Page combining row in Limits and `page_combining_restricted` |
| DEP row | skipped |  | Security setting, not memory use |
| ASLR row | skipped |  | Security setting, not memory use |
| Dynamic code row | skipped |  | Security setting, not memory use |

## 4. Virtual memory

The Regions, Strings, Dump, Heaps and Diagnostics process window tabs. On Linux locks, CFG, GDI and Flush heaps say not available, heaps show only the [heap] mapping, dumps go through gcore.

### Regions : list engine

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Memory tab | done | Linux | Process window tab Regions order 30, PowerShell |
| Region enumeration | done | Linux | NtQueryVirtualMemory walk on a worker |
| Open fallback | done | elevated, Linux | Falls back to a limited handle, protected and limited targets still list base, size, type and protection |
| First page attributes | done | Linux | WSEx per region, the heap segment scan skips what is not resident, bad, guard or no access |
| Region type flags query | done | Linux | Region type column showed MappedImage, Private, MappedPageFile, MappedDataFile |
| Background refresh | done |  | task::run plus busy flag per process, UI draws latest |
| Status text in list | done |  | Loading, Unable to query memory information, no regions, filters hide them all |
| Allocation base grouping | done | Linux | Parents collapsed by default |
| Single region promotion | done |  |  |
| Parent row totals | done |  | Sums all children even hidden ones, Protection from AllocationProtect of the first, Original protection |
| Tri-state sort | done |  | Tri-state header, unknown sorts last both ways on every numeric column plus Signing level and Priority |
| Free range split | done | Linux | Free (Unusable) rows, free + non free covers 0 to 0x7fffffff0000 |
| Address to region lookup | done |  | RegionList::at with `upper_bound` |
| Sub-region split | done |  | PEB and TEB split out of one VAD on Windows 11, ported from PhpSplitMemoryItem |
| Plugin extension points | skipped |  | No plugins |

### Regions : columns

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Base address | done | Linux |  |
| Type | done | Linux | Private : Commit style |
| Size | done | Linux | Sort on raw bytes |
| Protection | done | Linux | Linux : approx from rwxps, WC for private file writes, +S shared |
| Use | done | Linux | See the classification rows |
| Total WS | done | Linux | N/A when the walk fails, blank for zero |
| Private WS | done | Linux |  |
| Shareable WS | done | Linux | Off by default for width |
| Shared WS | done | Linux |  |
| Locked WS | done | Linux | Off by default for width |
| Committed | done | Linux | Off by default like SI |
| Private | done | Linux | Off by default like SI. Region level, process private bytes also charge copy on write |
| Signing level | done |  | Windows, Microsoft |
| Original protection | done | Linux | Linux : approx, mr mw me flags |
| Original pages | done | Linux | Float math, SI's integer division bug not copied |
| Modified pages | done | Linux | Linux : approx, Anonymous on a file mapping |
| Region type | done | Linux | Linux : raw VmFlags |
| Priority | done |  | Priority 0 shows Lowest (0), SI's blank not copied |

### Regions : value formatters

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Protection short codes | done | Linux | NA R RW WC X RX RWX WCX +G +NC +WCM |
| State strings | done | Linux |  |
| Type strings | done | Linux |  |
| Signing level strings | done |  |  |
| Memory priority strings | done |  |  |
| Region type flag strings | done | Linux | Linux : VmFlags text |

### Regions : classification

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Classification pipeline | done | Linux | Thread data from class 57 |
| Mapped file name | done | Linux | Device paths mapped to drive letters and UNC |
| `USER_SHARED_DATA` label | done | Linux | Linux : approx [vvar] [vdso] [vsyscall] |
| `HYPERVISOR_SHARED_DATA` label | done |  |  |
| PEB | done |  | Split region |
| PEB 32-bit | done |  | The reserved root and tail no longer say PEB 32-bit |
| TEB (thread N) | done | Linux | Split region, the reserved tail no longer inherits a TEB label |
| Stack (thread N) | done | Linux | Linux : approx, [stack] and task/TID/syscall stack pointer |
| Stack 32-bit (thread N) | done |  |  |
| Heap (ID n) | done | Linux | Class text from the header |
| Heap 32-bit (ID n) | done |  |  |
| Heap Segment (ID n) | done |  | Resident pages only |
| Heap Segment 32-bit | done |  | Coded next to the 64 bit scan, no 32 bit process with a second segment was at hand so it did not fire |
| Heap tagging via debug info | skipped |  | Debug API that starts a thread inside the target and can deadlock it |
| ApiSetMap | done |  |  |
| CSR shared memory | done |  |  |
| CodePage data | done |  | PEB offset right but AnsiCodePageData is null on this Windows, so the tag did not fire |
| GDI shared handle table | done |  |  |
| Shim data | done |  |  |
| Process activation context data | done |  |  |
| System activation context data | done |  |  |
| Activation context data | done |  |  |
| WER registration data | done |  |  |
| Silo shared data | done |  | Field was null in every process here, so the tag did not fire |
| Telemetry coverage map | done |  | Field was null here, so the tag did not fire |
| `USER_PROCESS_PARAMETERS` label | done | Linux | As in SI it loses to the heap tag when it sits in a heap |
| `LEAP_SECOND_DATA` label | done |  |  |
| Desktop heap | done |  |  |
| CFG Bitmap | done |  |  |
| CFG Bitmap 32-bit | done |  |  |
| Shadow stack (thread N) | approx | Linux | Windows : dead in SI too, nothing sets it |
| Custom plugin text | skipped |  | Plugin interface |

### Regions : context menu (protection, free, decommit, empty, save)

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Read/Write memory | done | Linux | Opens the editor |
| Change protection | done | Linux | Linux : clean message, item greyed with why |
| Empty working set | done | Linux | No confirm like SI since nothing is lost |
| Free | done | Linux | Linux : clean message |
| Decommit | done | Linux | Asks first, the pages go back to reserved. The process crashes if it still uses them, the prompt says so |
| Save | done | Linux | Zeros for unreadable pages so offsets stay true, refused above 8 GB of address space |
| Copy | done | Linux |  |
| Copy "column" | done | Linux | Same code path as Copy |
| Menu enable rules | done |  | Read/Write also greyed on free and on protected targets |
| Confirm dialog engine | done |  | ui::askDraw, Cancel is one click away |
| Free prompt | done |  | SI text |
| Decommit prompt | done |  | SI text |
| Unmap prompt | done |  | SI text |

### Regions : options menu

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Hide free pages | done | Linux | Default on |
| Hide reserved pages | done | Linux | Default on |
| Hide guard pages | done | Linux | Linux : approx, ---p right below a stack |
| Highlight private pages | done | Linux |  |
| Highlight system pages | done | Linux | Linux : approx, Image type |
| Highlight CFG pages | done |  |  |
| Highlight executable pages | done | Linux | A parent tints when allocated executable like SI, or when a region in it is, SI misses that one |
| Highlight modified/COW pages | done | Linux | A parent sums its regions for the tint, its cell stays blank |
| Zero pad addresses | done | Linux | Column refits |
| Read/Write address | done | Linux | Bad address keeps the dialog open with SI's message |
| Heaps | done |  | Jumps to the Heaps tab |
| Modified | done |  | Jumps to Diagnostics which opens on Modified pages |
| Strings | done |  | Jumps to the Strings tab |
| CFG bitmap | approx |  | Jumps to the Diagnostics tab, one more click opens its CFG sub tab |
| Save | done | Linux | Flat all rows, CSV in plain bytes with BOM, else tabs as shown, never overwrites without asking |

### Regions : controls, search, keys

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Options button | done |  |  |
| Refresh button | done |  | Plus optional auto refresh |
| F5 | done |  |  |
| Enter | done |  |  |
| Ctrl+C | done |  |  |
| Ctrl+K | approx |  | Ctrl+F, Ctrl+K is the app wide palette and fires first |
| Double click | done |  |  |
| Search box | done |  | Address finds its region, base, type, state, use |
| Search regex mode | done |  | The same regex engine as the Strings tab, no backtracking and no recursion |
| Search case sensitive mode | done |  | Not tried in testing |
| Size column to fit | done |  | ImGui table menu |
| Size all columns to fit | done |  | ImGui table menu |
| Hide column | done |  | ImGui table menu |
| Choose columns | done |  | ImGui table menu lists all columns |
| Reset sort | approx |  | Options &gt; Reset sort, the table header menu has no slot for it |
| Persisted layout | done |  | One table id for all process windows, sort survived a restart in ui.ini, options in cfg |

### Hex editor

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Memory editor window | approx | Linux | No snapshot of the region : paged 64 KiB blocks on a worker, unreadable pages show ??, so it opens where SI refuses |
| 1 GB limit | approx |  | No limit needed, the editor pages through the region. Save refuses above 8 GB |
| One editor per region | done |  | Keyed pid, start, base, size |
| Window title | done | Linux |  |
| Re-read | done |  | Asks first when unwritten edits would go, SI drops them silently |
| Write | done | Linux | Only the changed bytes, not the whole region (SI hazard not copied) |
| Go to | done |  | Also takes an address in the region, bad input keeps the dialog |
| Save | done | Linux | Includes unwritten edits, default name with real end address, overwrite asks |
| Close | done |  | Asks first when edits would go |
| 4 bytes per row | done |  | Not looked at in testing |
| 8 bytes per row | done |  | Not looked at in testing |
| 16 bytes per row | done |  |  |
| 32 bytes per row | done |  | Not looked at in testing |
| 64 bytes per row | done |  |  |
| 128 bytes per row | done |  | Not looked at in testing |
| 256 bytes per row | done |  | Not looked at in testing |
| Window placement | skipped |  | Placement of an SI dialog, FRS keeps everything in one window |
| Open with selection | done |  | Opens the hex editor at the selection |
| Edit a section handle | done | Linux | Maps the section into FRS on a worker, read only when it can't be written |
| Hex control layout | done | Linux | Offset, hex, ascii columns, offset header |
| Hex nibble typing | done | Linux | Uppercase accepted too, SI ignores it |
| ASCII typing | done | Linux |  |
| Tab | done |  |  |
| Arrow keys | done |  | Shift extends |
| Page Up and Page Down | done |  |  |
| Home | done |  | Ctrl for region start |
| End | done |  | Ctrl for region end |
| Mouse select and click | done |  | Click picks pane and nibble, drag selects and autoscrolls |
| Wheel and scrollbar | done |  | Wheel and a 64 bit custom scrollbar |
| Ctrl+C in hex pane | done |  | Text only |
| Ctrl+C in ASCII pane | done |  | Text only |
| Ctrl+V | done |  | Hex pairs in the hex pane, else raw text, skips read only pages and says so |
| Ctrl+X | skipped |  | No-op in SI |
| Ctrl+Z | done |  | New, not in SI |
| Extended printable mode | skipped |  | No caller in SI |

### Change protection

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Memory Protection dialog | done | Linux | Validated value so SI's uninitialised path can't happen, whole region |
| Possible values list | done |  | Dropdown plus Guard, No cache, Write combine, full list in a tooltip |
| New value field | done |  | Takes RW or 0x04, bad input stays with the error |

### Strings : legacy dialog and scanner

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| String Search dialog | skipped |  | SI's older strings dialog |
| Minimum length | skipped |  | SI's older strings dialog |
| Detect Unicode | skipped |  | SI's older strings dialog |
| Extended Unicode | skipped |  | SI's older strings dialog |
| Private | skipped |  | SI's older strings dialog |
| Image | skipped |  | SI's older strings dialog |
| Mapped | skipped |  | SI's older strings dialog |
| OK | skipped |  | SI's older strings dialog |
| Cancel | skipped |  | SI's older strings dialog |
| Search progress | skipped |  | SI's older strings dialog |
| Search cancel | skipped |  | SI's older strings dialog |
| String scanner | done | Linux | Port of the PhpSearchStrings state machine and the UTF-8 pass, kept state across chunks |
| Region filter for scan | done | Linux | `MEM_COMMIT` without NOACCESS or GUARD, type mask. 4 MiB chunks (SI 32 MiB) |
| Bulk region enumeration | done | Linux | NtPssCaptureVaSpaceBulk with 256 entries a batch, falls back to the NtQueryVirtualMemory walk |

### Strings : results window

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Results window | skipped |  | SI's older strings dialog |
| Result count label | skipped |  | SI's older strings dialog |
| Address | skipped |  | SI's older strings dialog |
| Base Address | skipped |  | SI's older strings dialog |
| Length | skipped |  | SI's older strings dialog |
| Result | skipped |  | SI's older strings dialog |
| Contains | skipped |  | SI's older strings dialog |
| Contains (case-insensitive) | skipped |  | SI's older strings dialog |
| Regex | skipped |  | SI's older strings dialog |
| Regex (case-insensitive) | skipped |  | SI's older strings dialog |
| Filter button | skipped |  | SI's older strings dialog |
| Save | skipped |  | SI's older strings dialog |
| Copy button | skipped |  | SI's older strings dialog |
| Close | skipped |  | SI's older strings dialog |
| Double click | skipped |  | SI's older strings dialog |
| Read/Write memory | skipped |  | SI's older strings dialog |
| Copy | skipped |  | SI's older strings dialog |
| Copy "column" | skipped |  | SI's older strings dialog |
| Ctrl+C | skipped |  | SI's older strings dialog |
| Persisted layout | skipped |  | SI's older strings dialog |

### Strings : tree dialog

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Strings window | done | Linux | Process window tab Strings, order 40 |
| Parallel scan | approx | Linux | Workers pull the next region from a shared counter instead of fixed slices, 1 to 64 threads. MCP uses one worker |
| Batched UI updates | approx | Linux | Per worker batches flushed under a lock, the UI pulls every 200 ms, no arenas |
| Back-off | approx | Linux | Pulls every 200, 250 from 150000 rows, 400 from 500000 like SI. Filtering and sorting are sliced or run on a task |
| Clone when scanning self | approx | Linux | Scans FRS itself directly and says so. A clone of FRS stayed behind as a zombie process, so no clone |
| # | done | Linux |  |
| Base address | done | Linux |  |
| Address | done | Linux |  |
| Type | done | Linux | ANSI, Unicode, and UTF-8 as its own word (SI shows ANSI for it) |
| Length | done | Linux | True character count, not capped by the text cap |
| String | done | Linux | Through util::safe, bidi override dropped and checked |
| Protection | done | Linux | Hidden by default as in SI, enable it from the header menu |
| Memory type | done | Linux | Hidden by default as SI. Header was clipped at px(80), now px(100) |
| ANSI | done | Linux | Display filter, no rescan |
| Unicode | done | Linux | Display filter, no rescan |
| Extended character set | done | Linux | Rescan when the menu closes |
| Private | done | Linux | Default on as SI, rescan when the menu closes |
| Image | done | Linux |  |
| Mapped | done | Linux | Nothing ticked says so instead of finishing silently |
| Minimum length | done | Linux | 1 to 100000 in the UI (MCP 4 to 256), default 10 and saved in cfg as SI. Rescan when the menu closes |
| Thread count (auto) | done | Linux | 0 auto, 1 to 64, not saved and no rescan as SI |
| Zero pad addresses | done | Linux | Columns measured again from the mono font |
| Refresh F5 | done | Linux | Stops a running scan and waits for it first |
| Filter button | done | Linux | Deep copy of the visible rows, so closing the parent or scanning again can't touch it |
| Search box | done | Linux | Contains ignoring case for A to Z only (the Case tooltip says so, non ASCII is not folded), Case, Regex, saved |
| Status label | done | Linux | Searching... N strings bar, N strings, Loading strings..., There are no strings to display |
| Read/Write memory | done | Linux | Opens the hex editor at the hit, disabled unless one row is selected. 35 bytes selected at the right offset |
| Copy | done | Linux | Tab separated visible columns. The real clipboard was never touched |
| Copy "column" | done | Linux |  |
| Double click | done | Linux | Same as Read/Write memory, row is always valid so SI's null risk is gone |
| Ctrl+C | done | Linux | Cut at 64 MB with a toast |
| Ctrl+A | done | Linux |  |
| F5 | done | Linux | Not while the process is gone |
| Column header menu | approx | Linux | ImGui table menu : size to fit, size all, reset order, column list. # can't be hidden |
| Persisted settings | skipped |  | Placement of an SI dialog |

### Modified pages

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Modified pages | done | elevated | Sub tab Modified pages under tab Diagnostics, non modal, starts by itself because it only reads |
| Tamper scan | done | elevated, Linux | Region walk, MemoryImageInformation, MemoryWorkingSetExInformation one entry per page |
| Index | done | Linux |  |
| File | done | Linux |  |
| ImageBase | done | Linux |  |
| Offset | done | Linux |  |
| Address | done | Linux |  |
| Symbol | approx | Linux | Nearest export read from the file on disk, file+offset when none is within 64 KB, no PDBs and no dbghelp |
| Status label | approx |  | No symbol load to report, so the status line gives pages, images, resident, skipped, failed, cut short |
| Refresh | done | Linux |  |
| Close | done | Linux | The process window's close, tabs stay in that window |
| Copy | done | Linux | Row selection with Ctrl and Shift, menu and Ctrl+C. Does not copy SI's PhFree of uninitialised items |
| Copy "column" | done | Linux |  |
| Persisted layout | skipped |  | Placement of an SI dialog |

### CFG bitmap

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| CFG bitmap | done | Linux | Sub tab CFG, starts by itself because it only reads |
| Bitmap decode | done | elevated | LdrSystemDllInitBlock by its Size, committed regions of the reservation, 64 KB reads |
| Image target table | done | elevated | Own bounds checked PE reader over the file on disk, PE32 and PE32+ |
| Summary text | done |  | Same text, adds readable size |
| Status label | done |  | "N of M entries shown" plus a count per type |
| # | done |  |  |
| Bitmap | done |  |  |
| Type | done |  | Image CFG, Modified/allowed, All valid (no CFG), Export suppressed, plus Not mapped here |
| Address | done |  |  |
| End | done |  |  |
| Value | done |  |  |
| Symbol | approx |  | Nearest export or file+offset, on visible rows only, no PDBs |
| Allocation base | done |  | Off by default in the table |
| Region base | done |  | Off by default in the table |
| Region size | done |  | Off by default in the table |
| Memory type | done |  | Off by default in the table |
| State | done |  | Off by default in the table |
| Protection | done |  | Off by default in the table |
| Mapped file | done |  | Resolved once per region that has an entry |
| Show image CFG targets | done |  | Also holds export suppressed entries in memory mapped here, default off |
| Show modified/allowed targets | done |  | Default on |
| Show all-valid (no CFG) ranges | done |  | Default off. A range in free memory from a DLL loaded elsewhere is Not mapped here |
| Options button | done |  |  |
| Refresh | done |  |  |
| Close | done |  | The process window's close |
| Copy | done |  |  |
| Copy "column" | done |  |  |
| CFG target check helper | done |  | SI has no UI for it |
| Persisted layout | done |  | For columns and sort through the shared table helper |

### Heaps

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Heaps | done | elevated, Linux | Tab Heaps, runs only on the button with a visible warning, query on a worker |
| Heap query | done | elevated | RtlQueryProcessDebugInformation summary, entries, non invasive, buffer doubling on no memory |
| Keep-alive during query | done | elevated | Execution required request, errors ignored as in SI. Never run against a frozen packaged app |
| Reflection option | done |  | Checkbox Query on a clone in the Heaps tab bar, default off, used by the tab and `heap_diag` |
| WOW64 heaps | impossible |  | Needs a 32-bit helper and FRS is one 64-bit exe. Says not available, starts no thread |
| # | done | Linux |  |
| Address | done | Linux | Linux approx ([heap] mapping start) |
| Used | done | Linux | Same formula as SI : busy blocks with their headers and rounding |
| Committed | done | Linux | Linux approx (smaps Rss of [heap]) |
| Entries | done | Linux | Linux N/A |
| Flags | done | Linux | Reads No coalesce on free |
| Class | done | Linux | A segment heap has no class in its flags so a non default one is N/A, SI shows Process Heap there |
| Type | done | Linux | Signature at 0x10 and front end at 0x1a2 verified on 26200 |
| Bold default heap | done |  | ProcessHeap field of the debug buffer |
| Sizes in bytes | done | Linux | Remembered through cfg key diagSizesBytes |
| Refresh | done | Linux | Query heaps is the refresh |
| Close | done | Linux | The process window's close |
| Copy Ctrl+C | done | Linux |  |
| Copy "column" | done | Linux |  |
| Persisted layout | skipped |  | Placement of an SI dialog |
| Destroy heap (disabled code) | skipped |  | Dead in SI, a crash button |
| Heap counters (dead resource) | skipped |  | Dead code in SI |
| Flush heaps | done | elevated, Linux | Asks first |
| Heap list of a protected process | impossible |  | Needs the driver. A protected target reports access denied, no real protected process was tried |

### Locks (borderline)

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Locks | done | elevated, Linux | Sub tab Locks, button only, warning shown |
| Lock query | done | elevated | Same care as heaps, buffer doubling, layout check |
| # | done |  |  |
| Type | done |  |  |
| Address | done |  |  |
| Filename | done |  |  |
| Thread | approx |  | Thread id number, SI's client id name needs its process, which it passes as null anyway |
| Lock count | done |  | Blank when free as in SI, raw value otherwise |
| Contention count | done |  | Off by default in the table |
| Entry count | done |  |  |
| Recursion count | done |  | 3 on a lock the target entered 3 times |
| Waiting shared count | done |  | Off by default in the table |
| Waiting exclusive count | done |  | Off by default in the table |
| Refresh | done |  | Query locks is the refresh |
| Close | done |  | The process window's close |
| Copy Ctrl+C | done |  |  |
| Copy "column" | done |  |  |
| Persisted layout | done |  | For columns and sort through the shared table helper, window placement is procwin's |

### GDI handles

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| GDI Handles | done | Linux | Sub tab GDI, starts by itself because it only reads |
| GDI table walk | done |  | PEB table copied through a read call so a bad page can't take FRS down. 10 and 14 objects match |
| Handle inspection | done |  | Own objects answer, foreign ones mostly stay blank (only fonts answered), that is Windows |
| Type | done |  | Types the table does not know are counted and reported, not hidden silently |
| Handle | done |  | Matches the handles the target printed |
| Object | done |  | Shows what the table holds, on this build an encoded value and not a kernel pointer |
| Information | done |  |  |
| Bitmap and DIB info | done |  | DIB too |
| Brush info | done |  | Solid and hatch |
| ExtPen info | done |  | A bigger one does not |
| Font info | done |  |  |
| Palette info | done |  |  |
| Pen info | done |  |  |
| Region info | done |  |  |
| DC info | done |  | Foreign DCs stay blank instead of SI's row of zeros |
| Refresh | done |  |  |
| Close | done |  | The process window's close |
| Copy | done |  |  |
| Copy "column" | done |  |  |
| Column sort | done |  | All columns through the table header |

### Dump

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Create dump file | approx | Linux | Dump tab instead of a submenu. Linux : core file through gcore |
| Minimal | done | Linux | Flags as SI. Header and streams parsed in Python, thread, module, thread info there, no Memory64. Linux : core file through gcore |
| Limited | done | Linux | The flags SI meant. Linux : core file through gcore |
| Normal | done | Linux | Flags as SI, handle stream and marker from private memory found in the file. Linux : core file through gcore |
| Full | done | Linux | Flags as SI, shows the estimate and free disk. Linux : core file through gcore |
| Custom | done | Linux | Tab shows the common flags, MCP preset custom takes the same names (schema enum). Linux : core file through gcore |
| Normal | done |  | Value 0, ticked when nothing else is, as SI. Linux : none, gcore has no flags |
| With data segments | done |  | Linux : none, gcore has no flags |
| With full memory | done |  | Linux : none, gcore has no flags |
| With handle data | done |  | Linux : none, gcore has no flags |
| Filter memory | skipped |  | Rare dump flag, no checkbox |
| Scan memory | skipped |  | Rare dump flag, no checkbox |
| With unloaded modules | done |  | Linux : none, gcore has no flags |
| With indirectly referenced memory | skipped |  | Rare dump flag, no checkbox |
| Filter module paths | skipped |  | Rare dump flag, no checkbox |
| With process and thread data | skipped |  | Rare dump flag, no checkbox |
| With private read-write memory | done |  | Linux : none, gcore has no flags |
| Without optional data | skipped |  | Rare dump flag, no checkbox |
| With full memory info | done |  | Linux : none, gcore has no flags |
| With thread info | done |  | Linux : none, gcore has no flags |
| With code segments | skipped |  | Rare dump flag, no checkbox |
| Without auxiliary state | skipped |  | Rare dump flag, no checkbox |
| With full auxiliary state | skipped |  | Rare dump flag, no checkbox |
| With private write-copy memory | skipped |  | Rare dump flag, no checkbox |
| Ignore inaccessible memory | done |  | Linux : none, gcore has no flags |
| With token information | skipped |  | Rare dump flag, no checkbox |
| With module headers | skipped |  | Rare dump flag, no checkbox |
| With filter triage data | skipped |  | Rare dump flag, no checkbox |
| With AVX state context registers | skipped |  | Rare dump flag, no checkbox |
| With Intel processor trace data | skipped |  | Rare dump flag, no checkbox |
| Scan inaccessible partial pages | skipped |  | Rare dump flag, no checkbox |
| Filter write-combined memory | skipped |  | Rare dump flag, no checkbox |
| Save | done | Linux | Button Save |
| Cancel | approx | Linux | Cancel stops a running dump |
| Dump file name dialog | approx | Linux | Editable path field, no file dialog. Never overwrites, a taken name gets `_2` and up |
| Process open chain | done | elevated | SI's open ladder without its full access rung |
| Dump writer | done | Linux | Dbghelp from System32 at runtime on its own thread, `CREATE_NEW` so never overwrites, serialized by a mutex. Linux : core file through gcore |
| Dump progress | done | Linux | SI's bands and messages. Linux : core file through gcore |
| Dump cancel | done | Linux | Dbghelp only asks about cancel between regions, so the caller polls stop and walks away |
| Dump error page | approx | Linux | A red line in the card, not a page |
| Use process snapshot | done | Linux | Checkbox and MCP snapshot, marker found in the file, also on a DX11 target with nothing left behind. Linux : core file through gcore |
| Kernel minidump sidecar | skipped |  | Live kernel dump not built, borderline for a RAM tool |
| WOW64 dump through helper | skipped |  | No 32-bit helper, 32-bit targets get a 64-bit dump |
| Packaged app task | approx | Linux | GetPackageFullName then BeginTaskByHandle, best effort, a failure goes in the note. No real Store app was tried. Linux : core file through gcore |
| Read failures ignored | done | Linux | Status `S_OK`. Target has guard and no access pages, dumps complete. Linux : core file through gcore |
| Process dump of a protected process | done | elevated | Says protected process, a dump of it needs the signed driver. Windows only |

### Not in SI, new work

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Byte or pattern memory search | done | Linux | Bytes mode in the Strings tab and `search_memory` |
| Regex over raw memory | done | Linux | Regex mode in the Strings tab, own NFA, a match is 4 KB at most, overlap handled |

### Mapped views and sections

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Unload on mapped module | done |  | Unmap on a mapped or image allocation base |
| Section Type row | done | elevated, Linux | Sections view, Commit File Image Reserve Unknown |
| Section File row | done | elevated, Linux | One page mapped briefly with AllocationType 0 (`MEM_MAPPED` is refused on 26200), nothing read |
| Section Size row | done | Linux | MaximumSize |
| Best name for unnamed Section | done | Linux | Object name, else file, else type and size |
| View column | approx |  | Mapped by list : Image or Mapped from a user mode walk |
| Start column | approx |  | Same list |
| End column | approx |  | Same list |
| Size column | approx |  | Same list |
| Find mapped files and images | done | elevated, Linux | Find box in the Sections view, walks every openable process, counts what it could not read |
| Views of a section across processes | impossible |  | No user mode path to the views of a section. The Mapped by list finds file backed views by path, pagefile backed ones can't be followed |
| KphQueryVirtualMemory classes | skipped |  | Needs the driver |
| Section details for a handle held by a protected process | approx | elevated | Opening the owner fails and the view shows the protected message, no section details |
| Go to process | approx |  | On the Mapped by list, double click or menu |
| Copy | approx |  | On the Mapped by list |
| Mapped files | done | Linux | Mapped rows with their path in Regions |
| Mapped images | approx | Linux | Image rows in Regions |
| Size | done | Linux | Parent row size, 64 bit, SI's ULONG cut not copied |
| Limited enumeration via working set | skipped |  | Module identity or library only, no RAM value |
| Enclave base address | skipped |  | Module identity or library only, no RAM value |
| Enclave size | skipped |  | Module identity or library only, no RAM value |
| Hide mapped | done |  | Options &gt; Hide mapped views |
| Highlight mapped modules | done |  | Options &gt; Highlight mapped views |
| Mapped modules color | approx |  | No color picker, theme rule |
| SectionObjects text and section sync events | skipped |  | Needs the driver |

### Protected targets

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Read memory contents of a protected process | impossible |  | No way in without the driver. FRS detects PP and PPL targets and says so, menu items are greyed |
| Region list of a protected process | done |  | Same PPL process : base, size, type, protection, mapped file names and signing level, N/A for WS |

### Image coherency (borderline)

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Kernel module memory reads | impossible |  | Kernel image reads need the driver |
| Shared original scan | skipped |  | Image coherency not built, borderline : a tamper check, not RAM use |
| Image coherency | skipped |  | Image coherency not built, borderline : a tamper check, not RAM use |
| Coherency enable | skipped |  | Image coherency not built, borderline : a tamper check, not RAM use |
| Scan level | skipped |  | Image coherency not built, borderline : a tamper check, not RAM use |
| Byte compare scan | skipped |  | Image coherency not built, borderline : a tamper check, not RAM use |
| Coherency skip regions | skipped |  | Image coherency not built, borderline : a tamper check, not RAM use |
| Quick scan | skipped |  | Image coherency not built, borderline : a tamper check, not RAM use |
| Normal scan | skipped |  | Image coherency not built, borderline : a tamper check, not RAM use |
| Full scan | skipped |  | Image coherency not built, borderline : a tamper check, not RAM use |
| Managed image path | skipped |  | Image coherency not built, borderline : a tamper check, not RAM use |
| Kernel module coherency | impossible |  | Kernel image reads need the driver, and image coherency is not built either |

### Helpers with no UI

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Offer virtual memory | skipped |  | Phlib helper with no UI |
| Discard virtual memory | skipped |  | Phlib helper with no UI |
| Prefetch virtual memory | skipped |  | Phlib helper with no UI |

## 5. Kernel memory

### Live kernel dump (borderline)

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Create live dump | skipped |  | Live kernel dump not built, borderline for a RAM tool |
| Create live kernel dump file | skipped |  | Live kernel dump not built, borderline for a RAM tool |
| Minimal | skipped |  | Live kernel dump not built, borderline for a RAM tool |
| Normal | skipped |  | Live kernel dump not built, borderline for a RAM tool |
| Full | skipped |  | Live kernel dump not built, borderline for a RAM tool |
| Custom | skipped |  | Live kernel dump not built, borderline for a RAM tool |
| Use dump storage stack | skipped |  | Live kernel dump not built, borderline for a RAM tool |
| Compress memory pages | skipped |  | Live kernel dump not built, borderline for a RAM tool |
| Capture user pages | skipped |  | Live kernel dump not built, borderline for a RAM tool |
| Capture Hypervisor pages | skipped |  | Live kernel dump not built, borderline for a RAM tool |
| Include nonessential Hypervisor pages | skipped |  | Live kernel dump not built, borderline for a RAM tool |
| Only kernel thread stacks | skipped |  | Live kernel dump not built, borderline for a RAM tool |
| Save | skipped |  | Live kernel dump not built, borderline for a RAM tool |
| Cancel | skipped |  | Live kernel dump not built, borderline for a RAM tool |
| Live dump writer | skipped |  | Live kernel dump not built, borderline for a RAM tool |
| Live dump file name | skipped |  | Live kernel dump not built, borderline for a RAM tool |
| Live dump progress | skipped |  | Live kernel dump not built, borderline for a RAM tool |
| Live dump cancel | skipped |  | Live kernel dump not built, borderline for a RAM tool |
| Completion pages | skipped |  | Live kernel dump not built, borderline for a RAM tool |

### Page files

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Pagefiles window | approx | Linux | Sub tab Page files on page Kernel, not a window |
| Page file query, extended | done | Linux | Class 144, sizes match `Win32_PageFileUsage` to the MB |
| Page file query, legacy | done |  | As the fallback when class 144 fails |
| Page file list walk | done | Linux | Bounds checked, uses the return length on a zeroed buffer |
| File name column | done | Linux | \??\ stripped, column as wide as the longest path with a horizontal scroll |
| Usage column | done | Linux | Page size from GetSystemInfo, matches CurrentUsage |
| Peak usage column | done | Linux | Matches PeakUsage |
| Total column | done | Linux | Matches AllocatedBaseSize |
| Minimum column | done | Linux | Windows only |
| Maximum column | done | Linux | Windows only |
| Refresh button | done | Linux | Button reads through a task, and the tick updates it too |
| Close button | approx |  | A page has no close, leave through the sidebar |
| Copy menu item | done | Linux | Right click |
| Copy "column" menu item | done | Linux | Copy cell, greyed out when there is no text |
| No column sort | approx | Linux | FRS does sort, the note says it is cheap |
| Persistence | skipped |  | Placement of an SI dialog, FRS has no such dialog |

### Pool tags : window

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Pool Table window | approx | Linux | Sub tab Pool tags, not a window |
| Pool tag query | done | Linux | Class 22, x64 layout asserted |
| Row model and deltas | done | Linux | 12 counters and deltas, a tag's first sample is N/A and null |
| Counter wrap behaviour | done | Linux | Current is (LONG)(allocs - frees), alloc and free deltas are modular 32 bit |
| Live update | done | Linux | Sampler job runs only while the page is drawn or for 15 s after a tool call |
| Own UI thread | skipped |  | Not needed with ImGui |
| Search box | done | Linux | Tag, driver, description |
| Ctrl+K focus | approx | Linux | Ctrl+F, Ctrl+K is the app palette |
| Enable column remaining space resize checkbox | skipped |  | Cosmetic |
| Close button | approx |  | A page has no close, leave through the sidebar |
| Show allocations menu item | done | Linux | On Windows, right click and double click open Big pools on that tag and read the list again on a task |
| Copy menu item | done | Linux | Right click and a real Ctrl+C. Idx 4241 against 2120 rows, no read past the rows, 2120 lines copied |
| Menu copy cell | done | Linux | Greyed out when the cell has no text |
| Header right click menu | approx | Linux | ImGui's own table menu, columns, size to fit, reset order |
| Sort any column | done | Linux | Every column, deltas signed, numeric, tag tiebreak, unknown last |
| Persistence | skipped |  | Placement of an SI dialog, FRS has no such dialog |
| Blank zero cells | done | Linux | Zero cells blank, unknown N/A |

### Pool tags : columns

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Tag Name column | done | Linux | High bit masked and reported as protected, padding trimmed, other bytes print as a dot |
| Driver column | done | Linux | From pooltag.txt, column hidden when no file is loaded |
| Description column | done | Linux | From pooltag.txt, full text, cut with a tooltip. Hover can't stay on with posted messages |
| Paged allocations (total) column | done | Linux | Hidden by default, All columns shows it |
| Paged frees (total) column | done | Linux | Hidden by default |
| Paged current (total) column | done | Linux | Signed |
| Paged bytes (total) column | done | Linux | Named Paged bytes |
| Non-paged allocations (total) column | done | Linux | Linux approx, Objects, `num_objs` |
| Non-paged frees (total) column | done | Linux | Linux hidden, MCP null |
| Non-paged current (total) column | done | Linux | Signed |
| Non-paged bytes (total) column | done | Linux | Default sort |
| Allocations (total) column | done | Linux | 130 wide, the summed total fits up to 99,999,999,999,999 (Segoe UI digit advance at 15 px) |
| Frees (total) column | done | Linux | 130 wide like Allocs |
| Current (total) column | done | Linux | Linux hidden, same number as Active, MCP has it |
| Bytes (total) column | done | Linux | On by default |
| Paged allocations (delta) column | done | Linux | N/A on first sample |
| Paged frees (delta) column | done | Linux | Linux hidden, MCP null |
| Paged current (delta) column | done | Linux | Linux hidden, MCP null |
| Paged bytes (delta) column | done | Linux | On by default |
| Non-paged allocations (delta) column | done | Linux | Linux approx, change in `num_objs`, signed |
| Non-paged frees (delta) column | done | Linux | Linux hidden, MCP null |
| Non-paged current (delta) column | done | Linux | Linux approx, change in `active_objs` |
| Non-paged bytes (delta) column | done | Linux | On by default, sortable, the highlight watches total bytes |
| Allocations (delta) column | done | Linux | Linux hidden, MCP has it |
| Frees (delta) column | done | Linux | Linux hidden, MCP null |
| Current (delta) column | done | Linux | Linux hidden, MCP has it |
| Bytes (delta) column | done | Linux | Linux hidden, MCP has it |

### Pool tags : tag database

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Tag file lookup | approx |  | pooltag.txt next to the exe only |
| Bundled tag file | skipped |  | Microsoft's file, FRS does not ship it |
| Tag file parser | done |  | Own parser in `kmem_tags.cpp`, LF or CRLF, BOM, only the first two " - " split, no header search |
| Tag lookup | done |  | First entry of a duplicate stays |
| Short tag padding bug | done |  |  |
| Wildcard tags unmatched | done |  | ? patterns after the exact miss, the most literal one wins |
| Database teardown | approx |  | Loaded once on first use, stays until exit |

### Big pools

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Large Allocations window | approx | Linux | Sub tab Big pools with List and By tag, not a modal window |
| Big pool query | done | Linux | Class 66, 112k rows here, only on Refresh, on first view of the tab, or a tool call with refresh |
| Address column | done | elevated, Linux | NonPaged bit masked off |
| Size column | done | Linux | Numeric sort |
| NonPaged column | done | Linux | Bit survives blanking |
| Refresh button | done | Linux | Runs on a task, a failure shows as the empty state with its reason and what to do |
| Close button | approx |  | A page has no close, the tag chip goes back to all rows |
| Copy and Ctrl+C menu item | done | Linux | Right click and Ctrl+C. Selection is dropped when the filter, the tag chip or the tag changes |
| Copy "column" menu item | done | Linux | Greyed out when the cell has no text or the click was past the last column |
| Persistence | skipped |  | Placement of an SI dialog, FRS has no such dialog |
| Per tag big pool roll up | done | Linux | By tag view and `group_by_tag` with totals |

### Pool classes with no SI caller

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Pool tag table with 64 bit counters | done |  | Class 255 is tried first and read when it answers |
| Session pool tags | skipped |  | No caller in SI |
| Session big pool | skipped |  | No caller in SI |
| Pool limits | skipped |  | No caller in SI |
| Pool zeroing support | skipped |  | No caller in SI |
| Paged and non-paged block lists | skipped |  | No caller in SI |

### Object pool charges (borderline)

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Paged size column | approx | elevated | FRS has no handles tab : process window tab Object charges, one row per handle |
| Nonpaged size column | approx | elevated | Same Object charges tab. Handles that can't be duplicated show N/A |
| Paged row | approx | elevated | Same number in the tab, no handle properties dialog |
| Row labelled Virtual size | approx | elevated | Same number in the Object charges tab, labelled Non-paged, not SI's wrong Virtual size |
| Object basic info with count correction | skipped |  | Object plumbing |
| Memory used | skipped |  | Token memory, not RAM use |
| Memory available | skipped |  | Token memory, not RAM use |

### Borderline kernel items

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Paging I/O events | skipped |  | Needs the driver |

### Object Manager (unreachable)

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Object Manager window | skipped |  | Unreachable in SI |
| Section Target text | skipped |  | Unreachable in SI |
| Memory partition objects | skipped |  | Unreachable in SI |
| Paged | skipped |  | Unreachable in SI |
| Non-paged | skipped |  | Unreachable in SI |
| Pool Type | skipped |  | Unreachable in SI |
| Default Paged Charge | skipped |  | Unreachable in SI |
| Default NP Charge | skipped |  | Unreachable in SI |
| Heap size | skipped |  | Unreachable in SI |
| Heap Size column | skipped |  | Unreachable in SI |

## 6. Working set watch

### Window, data, columns, menu

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| WS Watch window | approx | Linux | Process window tab WS Watch, not a dialog. Refused for system and pseudo processes |
| Open target | done | elevated, Linux | OpenProcess then a start time check |
| Enable button | done | Linux | NtSetInformationProcess 42 behind a danger confirm that says it can't be undone |
| Fault drain | done | Linux | One running total per target so tab and MCP never split it, grows the buffer on too small up to 8 times |
| Already enabled probe | done | Linux | On tab open and on MCP status |
| Fault aggregation | done |  | By faulting pc, cap 100000 rows |
| Instruction column | approx | elevated, Linux | module+0xoffset from the mapped file name, no symbols. Raw address outside an image |
| Filename column | done | elevated, Linux | Win32 path through the device map, full path in a tooltip |
| Count column | done | Linux | Default sort descending, numeric |
| Explanation text | done | Linux | Warning kept and extended |
| Label "WS watch is enabled." | done | Linux |  |
| Label "Page faults:" | done | Linux | Totals beside it, 0 when on with no faults, N/A when off, refused or unread |
| Close button | approx |  | No Close button, the process window's X. Tracking stays, the text says so |
| Open file location menu item | done | Linux | Explorer /select on Windows, xdg-open on Linux. Never clicked in testing, it opens a window on the desktop |
| Copy and Ctrl+C menu item | done | Linux | Copy tried, Ctrl+C not |
| Copy "column" menu item | done | Linux | Right click picks the column |
| Window title | approx |  | Window title comes from procwin, heading in the tab reads WS Watch : name (pid) |
| Persistence | skipped |  | Placement of an SI dialog, FRS keeps everything in one window |

## 7. GPU and .NET memory

### GPU : data source

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Shared GPU memory is system RAM | done | Linux | Stated on the page, the tab, the tool description and in every `gpu_memory` result |
| Feature gates | done |  | Windows 10 1803 floor, options gpuMonitor, gpuPerfCounters, gpuAdapterStats all change without a restart |
| Adapter discovery | approx | Linux | D3DKMTEnumAdapters2 every 10 s. Compute only adapters are not listed |
| GPU perf counter query | approx | Linux | PDH with English counter names, not the Perf* API |
| GPU counter id naming trap | done |  | Local Usage is dedicated resident, Non Local Usage is shared resident |
| GPU counter block parse | approx |  | PDH replaces the block parser |
| D3DKMT system segment query | done | Linux | A failed query is N/A not 0, a counter failure falls back to here |
| D3DKMT per process usage | done | elevated, Linux | The tab says why, rings get N/A. A refused query was not forced here |
| D3DKMT per process committed | done | elevated, Linux | Same D3DKMT path as per process usage |
| D3DKMT per process system memory | done | elevated | Same D3DKMT path, the column says System memory allocated on this path |
| Memory limits | done | Linux | Per adapter and summed, GETSEGMENTSIZE, segment limits only if that fails |
| System dedicated total | done | Linux | Adapter counter |
| System shared total | done | Linux | Sum of per process Non Local Usage like SI, the adapter's own Shared Usage shown beside it because they differ |
| Per process counter lookup | done | Linux | pid map per tick, adapter match on the full LUID |
| GPU process filter | approx | Linux | Perf path needs none |
| Memory history buffers | done | Linux | System rings and per process rings, length from memHistN |
| Per process min, max, diff | done | Linux | Zero samples ignored, also for the committed pair |
| Detailed process GPU memory, not surfaced | done | elevated, Linux | Surfaced on the tab and in `gpu_memory` with pid, read for that one process only, not a status |
| Interface GPU adapter queries | skipped |  | Plugin interface |
| Interface process GPU | skipped |  | Plugin interface |

### GPU : system views

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Option Enable GPU monitoring | done | Linux | Off shows a reason on the page and in `gpu_memory`, no restart needed |
| System summary text | approx | Linux | Cards on the GPU page, used over limit for both kinds, no tile since FRS has no system info view |
| Graph Dedicated memory | done | Linux | Scaled to the limit |
| Graph Shared memory | done | Linux | Scaled to total RAM, so it reads flat when use is small and that is the point |
| Graph tooltips | done | Linux | Value and time on hover |
| Panel Dedicated memory Current | done | Linux |  |
| Panel Dedicated memory Limit | done | Linux |  |
| Panel Shared memory Current | done | Linux |  |
| Panel Shared memory Limit | done | Linux | Summed over adapters and capped at total RAM |
| Details row Total Memory | done | Linux | Registry value through `KMTQAITYPE_QUERYREGISTRY`, 15.9 GB on the NVIDIA, N/A on Intel where the value is absent |
| Details row Reserved Memory | done |  | N/A when dedicated is 0, SI's fallback to the shared limit gives odd numbers on iGPUs and is not copied |
| Details row Memory Frequency | skipped |  | VRAM detail |
| Details row Memory Bandwidth | skipped |  | VRAM detail |
| Mini info GPU Memory section | done | Linux | Live view section GPU memory : used over limit for dedicated and shared, top 15 of each |

### GPU : per process views

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| GPU dedicated bytes (resident) column | done | Linux | Column GPU dedicated in the Processes list, hidden by default |
| GPU shared bytes (resident) column | done | Linux | Column GPU shared in the Processes list, hidden by default |
| GPU dedicated bytes (committed) column | done | Linux | Column GPU dedicated committed in the Processes list, hidden by default |
| GPU shared bytes (committed) column | done | Linux | Column GPU shared committed in the Processes list, hidden by default |
| Statistics row Dedicated memory | done | Linux | Process GPU tab, current min max diff |
| Statistics row Shared memory | done | Linux | Same |
| Statistics row Commit memory | done | Linux | Same, label follows the data path |
| Statistics row Total memory | approx |  | Dedicated plus shared resident only, SI's sum counts committed on top and double counts |
| GPU tab | done | Linux | Order 90, no GPU load graph |
| GPU tab graph Dedicated memory | approx | Linux | Autoscaled with a 16 MB floor rather than to the limit, a process is a flat line against 16 GB. Current value above it |
| GPU tab graph Shared memory | approx | Linux | Same |
| GPU tab graph Commit memory | done | Linux | Autoscale with a floor, Windows only |
| Option Enable per-process GPU adapter statistics | done | elevated | Option off by default, queries only the process whose tab is open or that `gpu_memory` names |
| Statistics row Virtual memory | done | elevated | Tab and `gpu_memory` |
| Statistics row Evicted bytes | done | elevated, Linux | Tab and `gpu_memory` |
| Statistics row DMA buffer size | skipped |  | Command buffers |
| Statistics row DMA allocation list | skipped |  | Command buffers |
| Statistics row DMA patch list | skipped |  | Command buffers |

### NPU (borderline)

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| NPU dedicated bytes (resident) column | skipped |  | Not built, borderline : NPU memory was left out |
| NPU shared bytes (resident) column | skipped |  | Not built, borderline : NPU memory was left out |
| NPU dedicated bytes (committed) column | skipped |  | Not built, borderline : NPU memory was left out |
| NPU shared bytes (committed) column | skipped |  | Not built, borderline : NPU memory was left out |
| NPU memory query | skipped |  | Not built, borderline : NPU memory was left out |
| NPU Dedicated memory row | skipped |  | Not built, borderline : NPU memory was left out |
| NPU Shared memory row | skipped |  | Not built, borderline : NPU memory was left out |
| NPU Commit memory row | skipped |  | Not built, borderline : NPU memory was left out |
| NPU Total memory row | skipped |  | Not built, borderline : NPU memory was left out |
| NPU summary text | skipped |  | Not built, borderline : NPU memory was left out |
| NPU dedicated memory graph | skipped |  | Not built, borderline : NPU memory was left out |
| NPU shared memory graph | skipped |  | Not built, borderline : NPU memory was left out |
| NPU panel values | skipped |  | Not built, borderline : NPU memory was left out |
| Total Memory row | skipped |  | Not built, borderline : NPU memory was left out |
| Reserved Memory row | skipped |  | Not built, borderline : NPU memory was left out |
| Memory Frequency row | skipped |  | Not built, borderline : NPU memory was left out |
| Memory Bandwidth row | skipped |  | Not built, borderline : NPU memory was left out |
| Dedicated memory graph | skipped |  | Not built, borderline : NPU memory was left out |
| Shared memory graph | skipped |  | Not built, borderline : NPU memory was left out |
| Commit memory graph | skipped |  | Not built, borderline : NPU memory was left out |

### .NET : access path and tab

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| GC counters need no debugging stack | done |  | Read only section map, nothing injected, no handle needed for a normal target |
| .NET Framework only | done |  | A CoreCLR process (dotnet fsi) gets `is_dotnet` true, clr core, null numbers and a reason |
| CLR detection | approx |  | Opens the v4 then v2 block, a section check, a pipe listing for CoreCLR. The Store app retry did not run |
| Open CLR v4 block | done | elevated | Through NT calls with the namespace handle as root |
| Open CLR v2 block | done |  | Built although first judged not worth porting |
| CLR block layout | done |  | Offsets printed from SI's headers and asserted, version checked, stored pid must match or no numbers show |
| WOW64 thunk | done |  | Committed, all heaps, Gen 1, Gen 2, sizes and object counts equal the Windows .NET CLR Memory counters |
| .NET performance tab | done |  | Order 100, counter and value, grouped, cards, stack bar |
| Live update | done |  | Sampler once a second while the tab shows, mapped and unmapped each read, no handle growth over 25 s |
| Copy and Ctrl+C menu item | done |  |  |
| Copy "column" menu item | done |  |  |
| Persistence | skipped |  | Placement of an SI dialog, FRS keeps everything in one window |

### .NET : counters

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| # Gen 0 Collections counter | done |  | Read seconds apart from the Windows counter on a target that keeps collecting, so it only tracks it |
| # Gen 1 Collections counter | done |  |  |
| # Gen 2 Collections counter | done |  |  |
| Promoted Memory from Gen 0 counter | done |  | Equals Windows counter |
| Promoted Memory from Gen 1 counter | done |  | Equals Windows counter |
| Promoted Finalization-Memory from Gen 0 counter | done |  |  |
| Process ID counter | skipped |  | Not shown |
| Gen 0 Heap Size counter | done |  | Labeled Gen 0 budget (not usage), tooltip and tool note say so, left out of every total |
| Gen 1 Heap Size counter | done |  | Equals Windows counter |
| Gen 2 Heap Size counter | done |  | Equals Windows counter |
| Large Object Heap Size counter | done |  | Equals Windows counter |
| Finalization Survivors counter | done |  | Equals Windows counter |
| # GC Handles counter | done |  | Equals Windows counter |
| # Induced GC counter | done |  | Same as Gen 0 count : tracks the Windows counter, no equal read |
| % Time in GC counter | skipped |  | A time value |
| # Bytes in all Heaps counter | done |  | Gen 1 + Gen 2 + LOH, equals the Windows counter when both read the same GC, which they do not always |
| # Total Committed Bytes counter | done |  | Equals Windows counter |
| # Total Reserved Bytes counter | done |  | Equals Windows counter |
| # of Pinned Objects counter | done |  | Equals Windows counter |
| # of Sink Blocks in use counter | done |  | Equals Windows counter |
| Total Bytes Allocated (since start) counter | done |  |  |
| Total Bytes Allocated for Large Objects (since start) counter | done |  |  |
| Gen 0 Promoted Bytes / sec counter | done |  | Divided by the interval, tip says it is the change of a last GC value |
| Gen 1 Promoted Bytes / sec counter | done |  | Same |
| Promoted Finalization-Memory / sec counter | done |  | Same |
| Allocated Bytes / sec counter | done |  | Divided by the interval, 5.2 MB/s on a target allocating 1 MB per 200 ms |
| Bytes in Loader Heap counter | done |  | Equals Windows counter |

## 8. Tray and status surfaces

There is no tray on Linux : tray icons, close to tray, start hidden and the taskbar items exist only on Windows. The status bar, header graphs and the Live view page are on both.

### Shared rendering and formatting

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Plugin statistics snapshot | skipped |  | Plugin interface, FRS has no plugins |
| Percent formatting | done | Linux | Decimals from option uiPctPrec, status bar, tray tips, Live view, tables and bar tooltips |
| Colour storage format | approx |  | Colours are ImVec4 in ui::c, settings hold a palette index, no hex COLORREF |
| Derived line and fill colours | done | Linux | Line = colour, fill = half brightness |
| Graph palette | approx |  | One dark palette through ui::graph, tray and overlay always black or transparent |
| Graph rasteriser | done | Linux | Own rewrite, step 2 px, newest right, gaps for unknown samples |

### Tray icons

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Commit charge history icon | done | elevated | No tray on Linux |
| Physical memory history icon | done | elevated | No tray on Linux |
| Commit usage (text) icon | done | elevated | Guards limit 0. No tray on Linux |
| Physical usage (text) icon | done | elevated | SI's string leak not copied. No tray on Linux |
| Commit tooltip | done | elevated | Own series first plus the other memory lines. No tray on Linux |
| Physical tooltip | done | elevated | Same. No tray on Linux |
| Native tooltip gating | done | elevated | `NIF_SHOWTIP` always since there is no popup. No tray on Linux |
| Icon registry | done |  | kIcons in `tray_win.cpp`. No tray on Linux |
| View &gt; Tray icons | approx |  | No View menu, the check list is the Settings group Tray and taskbar. No tray on Linux |
| Options &gt; Tray Icon list | done |  | Settings group Tray and taskbar. No tray on Linux |
| Enabled icons persistence | done |  | One key per icon. No tray on Linux |
| Icon bitmap pipeline | done |  | DIB to HICON at `SM_CXSMICON`, 24 px at 150 %, GDI and USER counts flat over a soak and over thread restarts. No tray on Linux |
| Tray text drawing | done |  | Tahoma scaled to the icon, alpha pass after GDI. No tray on Linux |
| Icon update thread | done |  | Own thread, bounded join at exit, detaches if Explorer hangs. No tray on Linux |
| Skip first provider runs | approx |  | Icon waits for the first sample instead of counting runs. No tray on Linux |
| Explorer restart recovery | done |  | `NIM_MODIFY` failing re-adds. No tray on Linux |
| Add with version 4 | done | elevated | `NIM_ADD` then `NIM_SETVERSION` 4, shell confirmed through `Shell_NotifyIconGetRect`. No tray on Linux |
| Remove on exit | done |  | Shell reports the icons gone after exit. No tray on Linux |
| Transparent icons | done |  | Option trayTransparent. No tray on Linux |
| Lazy start | skipped |  | Tray host option, no memory effect |
| Persist tray position | skipped |  | Tray host option, no memory effect |
| Reset layout | skipped |  | Tray host option, no memory effect |
| Single click icons | done |  | Option traySingleClick, posted callbacks. No tray on Linux |
| Icon click toggles visibility | done |  | Option trayToggle, posted callbacks. No tray on Linux |
| Hover opens memory section | approx |  | Hover shows the native tip with memory lines, the section opens on click, no OS popup. No tray on Linux |
| Single icon opens Graphs | approx |  | A click always opens the icon's own list, better for a RAM cleaner. No tray on Linux |
| Single click opens memory section | done | elevated | Double click time + 140 ms like SI, posted callbacks. No tray on Linux |
| Double click | done | elevated | Posted callbacks. No tray on Linux |
| Right click menu | done | elevated | SetForegroundWindow on a real hidden window. No tray on Linux |
| Show/Hide System Informer | done |  | Show FRS / Hide FRS. No tray on Linux |
| System information | done |  | Item Memory opens the Memory page. No tray on Linux |
| Settings submenu | skipped |  | Tray host option, no memory effect |
| Processes submenu | skipped |  | CPU sorted process menu in SI, a RAM variant would be new work |
| Exit | done |  | Exits also when close to tray is on and while hidden. No tray on Linux |
| GPU memory history icon | done | elevated | gpumem::history ded over dedLimit. No tray on Linux |
| GPU memory usage (text) icon | done | elevated | Same. No tray on Linux |

### Mini information window

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Commit charge section | done | Linux | Section Commit charge |
| Physical memory section | done | Linux | Section Physical memory |
| Commit section label | done | Linux |  |
| Physical section label | done | Linux |  |
| Commit row usage text | done | Linux |  |
| Physical row usage text | done | Linux | Help text says shared pages count more than once |
| Commit sort | done | Linux | Two stage sort |
| Physical sort | done | Linux | Two stage sort, same fallback |
| Group row layout | approx | Linux | No icon or file description : name, pid or process count, usage lines and a usage bar |
| Terminated marker | done | Linux |  |
| Commit charge graph row | done | Linux |  |
| Physical memory graph row | done | Linux |  |
| Max Y label on memory rows | done | Linux | Option graphMaxText, on a box in ui::c.bg like the title and value so the line can't strike through them |
| Row fade | done | Linux | Gradient rect |
| Row frame and title | done | Linux | ui::graph frame plus title |
| Row height fill | done | Linux | Two rows split the height |
| Commit row tooltip | done | Linux | Current values plus hovered sample, its time and biggest process |
| Physical row tooltip | done | Linux | Same |
| Commit row click | done | Linux | Switches to the Commit charge section |
| Physical row click | done | Linux | Switches to the Physical memory section |
| Dead Memory graph row kind | skipped |  | Dead code in SI |
| Mini information window host | approx | Linux | A sidebar page named Live view, not a popup |
| Enable tray information window | approx |  | No popup to turn off, icons always use native tips |
| Pin count visibility | skipped |  | Live view is a page, not a popup |
| Popup placement | skipped |  | Live view is a page, not a popup |
| Hover keeps window open | skipped |  | Live view is a page, not a popup |
| Section menu Graphs | done | Linux | Tab bar |
| Section menu Commit charge | done | Linux |  |
| Section menu Physical memory | done | Linux |  |
| Pin button | skipped |  | Live view is a page, not a popup |
| Options button | approx | Linux | Sample button and Auto update sit in the tab row, no cog menu |
| Opacity 10% | skipped |  | Popup opacity, cosmetic |
| Opacity 20% | skipped |  | Popup opacity, cosmetic |
| Opacity 30% | skipped |  | Popup opacity, cosmetic |
| Opacity 40% | skipped |  | Popup opacity, cosmetic |
| Opacity 50% | skipped |  | Popup opacity, cosmetic |
| Opacity 60% | skipped |  | Popup opacity, cosmetic |
| Opacity 70% | skipped |  | Popup opacity, cosmetic |
| Opacity 80% | skipped |  | Popup opacity, cosmetic |
| Opacity 90% | skipped |  | Popup opacity, cosmetic |
| Opaque | skipped |  | Popup opacity, cosmetic |
| Refresh | done | Linux | Refresh icon button that takes a sample now, the in-page reload icon used everywhere (no "Refresh" label, so it is not read as the header's RAM bar Refresh), and F5 |
| Refresh automatically | done | Linux | Checkbox Auto update, the name every per-view auto toggle uses. It freezes this view only and is not Auto-Refresh |
| Position and size | skipped |  | Placement of an SI dialog, FRS keeps everything in one window |
| Update flow | done | Linux | Rebuilds only the visible list and only on a new tick |
| Selection across ticks | done | Linux | Selection is a ProcRef |
| Suspend updates during menus | done | Linux | List does not rebuild while a popup is open, popup id is the pid, actions copy their refs |
| Delete key | skipped |  | Process control (end or freeze a process), not RAM, not built |
| Enter key | done | Linux | Opens details |
| Double click row | done | Linux | Opens details |
| Terminate | skipped |  | Process control (end or freeze a process), not RAM, not built |
| Properties | done | Linux | Open details |
| Go to process / Go to processes | done | Linux | Show in Processes calls procs::select and opens the page |
| Missing memory actions | done | elevated, Linux | Reduce working set on the group through procs::reduceWs |

### Status bar

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Physical memory item | done | Linux | Off by default since the RAM bar shows it, on from the right click menu |
| Free memory item | done | Linux | Labeled Available with a tip that says what it is, matched Get-CimInstance within sampling noise |
| Commit charge item | done | Linux |  |
| Selected process WS item | done | Linux | Sums the Processes page selection through procs::selected, N/A without one |
| Selected process private bytes item | done | Linux | Same |
| Interval status item | done | Linux | Label Sampling, menu name Sampling status, prints the real interval, or Paused |
| Default items | approx | Linux | Available, Commit, top process, processes, MCP and cleaning state |
| Status bar persistence | done | Linux | One key per item, no list to validate |
| Double click Physical memory | done | Linux | Every memory item goes to Memory, others to their own page or process |
| Right click Customize | approx | Linux | Right click menu with the items checked in place, instead of a Customize dialog |
| Customize Status Bar dialog | skipped |  | Customize dialog chrome |
| Available status bar entries list | skipped |  | Customize dialog chrome |
| Current status bar entries list | skipped |  | Customize dialog chrome |
| Add &gt; | skipped |  | Customize dialog chrome |
| &lt; Remove | skipped |  | Customize dialog chrome |
| Move up | skipped |  | Customize dialog chrome |
| Move down | skipped |  | Customize dialog chrome |
| Drag reorder | skipped |  | Customize dialog chrome |
| Reset | skipped |  | Customize dialog chrome |
| Close | skipped |  | Customize dialog chrome |
| Part width handling | done | Linux | It stops the jitter : parts only grow, reset every 10 s |
| Update gating | done | Linux | Nothing draws while hidden or minimized, data keeps flowing |
| Refresh on interval change | skipped |  | Immediate mode makes it moot |
| Enable status bar | done | Linux | Option sbOn off : the panel goes away and the page takes its room |

### Toolbar graphs

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Physical memory history graph | done | Linux | Header sparkline, scaled by total |
| Commit charge history graph | done | Linux | Scaled by limit |
| Physical graph tooltip | done | Linux | Hovered sample and time |
| Commit graph tooltip | done | Linux | Same |
| Physical graph double click | done | Linux | Opens Memory |
| Commit graph double click | done | Linux | Opens Memory |
| Physical graph right click | skipped |  | Rebar toolbar plumbing, FRS has no such toolbar |
| Commit graph right click | skipped |  | Rebar toolbar plumbing, FRS has no such toolbar |
| Toolbar right click menu | skipped |  | Rebar toolbar plumbing, FRS has no such toolbar |
| View &gt; Toolbar submenu | skipped |  | Rebar toolbar plumbing, FRS has no such toolbar |
| Toolbar graph persistence | done | Linux | Options hdrPhys and hdrCommit |
| Graph band size | skipped |  | Rebar is pure Win32, source says skip |
| Graph tick | done | Linux | Immediate mode |
| Initialization delay | skipped |  | Rebar toolbar plumbing, FRS has no such toolbar |
| Dead toolbar graph code | skipped |  | Dead code in SI |
| Enable toolbar | skipped |  | Rebar toolbar plumbing, FRS has no such toolbar |

### Taskbar overlay

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Select the graph to show on the taskbar | done |  | Option taskOverlay. No tray on Linux |
| Physical memory taskbar overlay | done | elevated | SetOverlayIcon `S_OK`, never looked at, see top. No tray on Linux |
| Commit charge taskbar overlay | done | elevated | Same. No tray on Linux |
| Overlay thread | done |  | Shares the tray thread, COM apartment on that thread. No tray on Linux |
| Clear overlay | done |  | None clears overlay and progress and releases the object, handles flat over 10 on and off rounds. No tray on Linux |
| Overlay bitmap | done |  | Shares the tray rasteriser. No tray on Linux |

## 9. Options

### Memory tab and dump settings

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Heap memory tagging | skipped |  | Left off on purpose, it changes how heaps work |

### Refresh interval

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Refresh interval Fast (0.5s) | done | Linux | Palette command Sampling interval : Fast (0.5 s) and status menu Sampling interval write memRefreshMs |
| Refresh interval Normal (1s) | done | Linux | Same |
| Refresh interval Below normal (2s) | done | Linux | Same |
| Refresh interval Slow (5s) | done | Linux | Same |
| Refresh interval Very slow (10s) | done | Linux | Same |
| Refresh | done | Linux | sampler::kick wakes every ms = 0 job for one tick, also while paused. Palette Sample now (F5) |
| Refresh automatically | done | Linux | Option Pause sampling, also Pause sampling and Resume sampling in the Ctrl+K palette |
| Zero interval guard | done |  | memRefreshMs clamps to 100..10000 |
| Boost after start | done |  | Each sampler job ticks at once on start |

### Graphs

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Show commit charge instead of physical memory in summary view | approx | Linux | Option sumCommit, on by default like SI, puts Commit first in the header and the Live view |
| Private bytes colour | approx |  | Option colCommit, a list of palette colours, free colour ruled out |
| Physical memory colour | approx |  | Option colPhys, same |
| Graphs page | approx |  | Settings group Graphs and numbers |
| Show text | approx |  | Option graphText, drives the Live view graphs, the process graph has its own option |
| Use old colors (black background) | approx |  | No switch : FRS is dark only |
| Colour list availability | skipped |  | SI bug, not copied, colours always apply |
| Change colour | approx |  | List of palette colours, free colour ruled out |
| Reset colour | approx |  | Choice Default |
| Colour row tooltip | approx |  | Each colour option has its own hint, SI's wrong index not copied |

### General

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Max. size unit | approx |  | Option uiMaxUnit, status bar, tray and Live view only, the hint says so |
| Max precision | done |  | Option uiPctPrec |
| Graph history length | done |  | memHistN |
| Automatic | done | Linux | Option graphHistAuto off by default, 4480 px screen gave 2240 samples, clamped to memHistN's 60..3600 |
| Enable fixed graph scaling (experimental) | done |  | Option graphMaxScale for Live view and header |
| Max Y label | done |  | Option graphMaxText |
| Enable AVX extensions (experimental) | skipped |  | Same results as the scalar path, source says skip |
| Enable new memory strings dialog | skipped |  | Only chooses or serves the legacy strings UI |
| Enable kernel-mode driver | skipped |  | There is no driver to turn on |
| Enable warnings | skipped |  | FRS always asks before destructive actions, there is no switch to turn safety prompts off, on purpose |
| Enable monospace fonts | approx |  | Addresses are always in a monospace font, no switch |
| Icon processes | skipped |  | CPU sorted process menu in SI, a RAM variant would be new work |
| Hide when closed | done | elevated | Option trayClose off by default, only hides while an icon is up, without one close exits. No tray on Linux |
| Hide when minimized | done | elevated | Option trayMin off by default, same guard. No tray on Linux |
| Start hidden | approx | elevated | Option trayStartHidden hides once an icon is up, the window shows for about a second first. No tray on Linux |

### Highlighting

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Memory highlight thresholds | done | Linux | New : hlMem, hlWarn, hlBad colour physical and commit numbers in the status bar and Live view |
| Private pages | approx | Linux | Option regHlPrivate, fixed colour |
| System pages | approx | Linux | Option regHlSystem, fixed colour |
| CFG pages | approx | Linux | Option regHlCfg, fixed colour. Linux has no CFG |
| Execute pages | approx | Linux | Option regHlExec, fixed colour |
| Modified pages (no row) | approx | Linux | Option regHlModified, fixed colour |
| Memory group checkboxes do nothing | skipped |  | Checkboxes do nothing in SI |
| Highlight priority | done |  | `tab_regions.cpp` rowTint, execute, CFG, system, modified, mapped, private |
| Group view | skipped |  | SI options page chrome, no memory effect |
| Change colour | skipped |  | Palette choices instead of a free picker |
| Reset | skipped |  | Palette choices instead of a free picker, nothing to reset |
| Enable all | skipped |  | SI options page chrome, no memory effect |
| Disable all | skipped |  | SI options page chrome, no memory effect |
| Highlighting duration | skipped |  | SI options page chrome, no memory effect |
| New objects colour | skipped |  | SI options page chrome, no memory effect |
| Removed objects colour | skipped |  | SI options page chrome, no memory effect |

### Stored settings

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Stored setting MemoryListFlags | done |  | One key per bit (regHideFree, regHideReserved, regHideGuard, regZeroPad, regHl*) |
| Highlight bits default | done |  | All highlight defaults off in vmem and in the options here |
| Stored setting HideFreeRegions | skipped |  | Orphan key, nothing reads it, source says do not port |
| Stored setting MemEditBytesPerRow | done |  | Key hexBytesPerRow |
| Stored setting MemEditGotoChoices | skipped |  | Niche, left out |
| Stored setting MemFilterChoices | skipped |  | Only chooses or serves the legacy strings UI |
| Stored setting MemoryReadWriteAddressChoices | skipped |  | Niche, left out |
| Stored setting NoPurgeProcessRecords | skipped |  | Makes SI itself grow, keep FRS records bounded |
| ToolStatus plugin setting Config | skipped |  | ToolStatus plugin setting, FRS has no plugin |
| ToolStatus plugin setting ToolbarShowSystemInfoGraph | skipped |  | ToolStatus plugin setting, FRS has no plugin |
| ToolStatus plugin setting DelayConfig | skipped |  | ToolStatus plugin setting, FRS has no plugin |
| ToolStatus plugin setting TaskbarDisplayStyle | skipped |  | ToolStatus plugin setting, FRS has no plugin |

## 10. MCP

These rows compare the FRS MCP server to System Informer's own MCP plugin, AgentTools. Names in backticks are AgentTools tool and field names, FRS keeps its own and the note names the FRS tool and field. The rows carry no Not run marks : every tool ran on Windows in an unelevated build, none ran elevated, none ran on Linux.

### Tools : system memory

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Memory details tool (`get_memory_details`) | done |  | `memory_details` : physical, commit, `paged_pool`, `nonpaged_pool`, paging, `page_lists`, `file_cache`, compression |
| Page size (`page_size`) | done |  | `memory_details` `page_size` |
| Physical total (`physical_total_bytes`) | done |  | `memory_details` `physical.total_bytes`, plus `installed_bytes` |
| Physical available (`physical_available_bytes`) | done |  | `memory_details` `physical.available_bytes` |
| Resident available (`resident_available_bytes`) | done |  | `memory_details` `extra_bytes`."Resident available", null on Linux |
| Cache resident (`cache_resident_bytes`) | approx |  | `memory_details` `physical.cache_ws_bytes` (ResidentSystemCachePage) and `file_cache.current_bytes` |
| Commit total (`commit.total_bytes`) | done |  | `memory_details` `commit.current_bytes` |
| Commit limit (`commit.limit_bytes`) | done |  | `memory_details` `commit.limit_bytes` |
| Commit peak (`commit.peak_bytes`) | done |  | `memory_details` `commit.peak_bytes` with `peak_scope` (since boot, or since FRS started on Linux) |
| Lists zeroed (`lists.zeroed_bytes`) | done |  | `memory_details` `page_lists.zeroed_bytes` |
| Lists free (`lists.free_bytes`) | done |  | `memory_details` `page_lists.free_bytes` |
| Lists modified (`lists.modified_bytes`) | done |  | `memory_details` `page_lists.modified_bytes` |
| Lists modified no write (`lists.modified_no_write_bytes`) | done |  | `memory_details` `page_lists.modified_no_write_bytes` |
| Lists modified page file (`lists.modified_page_file_bytes`) | done |  | `memory_details` `page_lists.modified_pagefile_bytes` |
| Lists bad (`lists.bad_bytes`) | done |  | `memory_details` `page_lists.bad_bytes` |
| Lists standby (`lists.standby_bytes`) | done |  | `memory_details` `page_lists.standby_bytes` |
| Lists repurposed pages (`lists.repurposed_pages`) | approx |  | `memory_details` `page_lists.repurposed_total_bytes`, in bytes instead of pages |
| Lists by priority (`lists.by_priority[]`) | approx |  | `memory_details` `page_lists.standby_by_priority_bytes` and `repurposed_by_priority_bytes`, two arrays of 8 indexed by priority instead of one array of objects |
| Pools paged (`pools.paged_bytes`) | done |  | `memory_details` `paged_pool.virtual_size_bytes` (usage) and `working_set_bytes` |
| Pools paged available (`pools.paged_available_bytes`) | done |  | `memory_details` `extra_bytes`."Available paged pool" |
| Pools paged resident (`pools.paged_resident_bytes`) | done |  | `memory_details` `paged_pool.working_set_bytes` |
| Pools non paged (`pools.non_paged_bytes`) | done |  | `memory_details` `nonpaged_pool.usage_bytes` |
| Pools paged allocs (`pools.paged_allocs`) | approx |  | `memory_details` `paged_pool.allocs_per_sec`, a rate between two samples instead of the running count |
| Pools paged frees (`pools.paged_frees`) | approx |  | `memory_details` `paged_pool.frees_per_sec`, a rate |
| Pools non paged allocs (`pools.non_paged_allocs`) | approx |  | `memory_details` `nonpaged_pool.allocs_per_sec`, a rate |
| Pools non paged frees (`pools.non_paged_frees`) | approx |  | `memory_details` `nonpaged_pool.frees_per_sec`, a rate |
| File cache current (`file_cache.current_bytes`) | done |  | `memory_details` `file_cache.current_bytes` |
| File cache peak (`file_cache.peak_bytes`) | done |  | `memory_details` `file_cache.peak_bytes` |
| File cache minimum working set (`file_cache.minimum_working_set_bytes`) | done |  | `memory_details` `file_cache.min_bytes` |
| File cache maximum working set (`file_cache.maximum_working_set_bytes`) | done |  | `memory_details` `file_cache.max_bytes` |
| File cache current including transition (`file_cache.current_including_transition_bytes`) | done |  | `memory_details` `file_cache.current_including_transition_bytes`, null on Linux |
| File cache page fault count (`file_cache.page_fault_count`) | done |  | `memory_details` `extra_counts`."File cache page faults" |
| Compression store pid (`compression_store.pid`) | done |  | `memory_details` compression.pid, null when none |
| Compression store working set (`compression_store.working_set_bytes`) | done |  | `memory_details` `compression.store_ws_bytes` |
| Compression store total data compressed (`compression_store.total_data_compressed_bytes`) | done |  | `memory_details` `compression.total_data_bytes` |
| Compression store total compressed size (`compression_store.total_compressed_size_bytes`) | done |  | `memory_details` `compression.total_compressed_bytes`, plus ratio and `saved_bytes` |
| Compression store total unique data compressed (`compression_store.total_unique_data_compressed_bytes`) | done |  | `memory_details` `compression.unique_data_bytes` |
| Page faults total (`page_faults.total`) | approx |  | `memory_details` `paging.faults_per_sec`, a rate instead of the running total |
| Page faults transition (`page_faults.transition`) | done |  | `memory_details` `extra_counts`."Transition faults" (Windows, Linux has its own vmstat keys) |
| Page faults cache transition (`page_faults.cache_transition`) | done |  | `memory_details` `extra_counts`."Cache transition faults" |
| Page faults demand zero (`page_faults.demand_zero`) | done |  | `memory_details` `extra_counts`."Demand zero faults" |
| Page faults copy on write (`page_faults.copy_on_write`) | done |  | `memory_details` `extra_counts`."Copy on write faults" |
| Page file info tool (`get_pagefile_info`) | done |  | `page_files` : path, `size_bytes`, `used_bytes`, `peak_bytes`, `min_bytes`, `max_bytes`, `peak_scope`, totals |
| Pool tag list tool (`list_pool_tags`) | done |  | `pool_tags` |
| Pool tag sort keys (`sort_by`) | done |  | `pool_tags` `sort_by` takes any counter or delta key, tag, driver or description, order asc or desc |
| Tag | done |  | `pool_tags` rows tag |
| Tag value (`tag_value`) | done |  | `pool_tags` rows `tag_value` hex string, null on Linux |
| Protected | done |  | `pool_tags` rows protected |
| Paged allocs (`paged_allocs`) | done |  | `pool_tags` rows `paged_allocs` |
| Paged frees (`paged_frees`) | done |  | `pool_tags` rows `paged_frees` |
| Paged current (`paged_current`) | done |  | `pool_tags` rows `paged_current` |
| Paged (`paged_bytes`) | done |  | `pool_tags` rows `paged_bytes` |
| Nonpaged allocs (`nonpaged_allocs`) | done |  | `pool_tags` rows `nonpaged_allocs` |
| Nonpaged frees (`nonpaged_frees`) | done |  | `pool_tags` rows `nonpaged_frees` |
| Nonpaged current (`nonpaged_current`) | done |  | `pool_tags` rows `nonpaged_current` |
| Nonpaged (`nonpaged_bytes`) | done |  | `pool_tags` rows `nonpaged_bytes` |
| Total (`total_bytes`) | done |  | `pool_tags` rows bytes (paged plus nonpaged), plus every `*_delta` key |
| Big pool blocks per tag (`big_allocations`) | approx |  | `big_pools` `group_by_tag` blocks per tag, a second call instead of a `pool_tags` column |
| Big pool bytes per tag (`big_bytes`) | approx |  | `big_pools` `group_by_tag` bytes per tag |
| Pool tag totals (`tag_count`, `paged_bytes_total` and the like) | approx |  | `pool_tags` `total_count` and `matching_totals` (after the filter, not before), `big_pools` `total_blocks`, `total_bytes`, nonpaged and paged totals |
| System info, memory fields (`get_system_info`) | approx |  | No `get_system_info` : `ram_status` and `memory_details` carry physical total and available, commit, limit, peak, `page_size`, `app_status` carries os and elevated |
| Capabilities object | approx |  | No capabilities object : each tool says itself when something is unavailable and why |
| System history, memory series (`get_system_history`) | done |  | `memory_history` : commit and physical per sample with min, max, avg over the ring and `interval_ms` |

### Tools : process memory counters

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Process list, memory columns (`list_processes`) | done |  | `list_processes` : every memory column, filter, `sort_by`, descending, group, `ws_counters`, paging |
| Private (`private_bytes`) | done |  | `list_processes` `private_bytes` |
| Working set (`working_set_bytes`) | done |  | `list_processes` `working_set_bytes` |
| Process details, memory fields (`get_process`) | approx |  | `get_process` : one process in full, no batch of 64 (`list_processes` covers many, `reduce_working_set` takes pids) |
| Private (`private_bytes`) | done |  | `get_process` `statistics.private_bytes` |
| Peak private (`peak_private_bytes`) | done |  | `get_process` `statistics.peak_private_bytes` (Linux : highest FRS saw) |
| Working set (`working_set_bytes`) | done |  | `get_process` `statistics.working_set_bytes` |
| Peak working set (`peak_working_set_bytes`) | done |  | `get_process` `statistics.peak_working_set_bytes` |
| Virtual size (`virtual_size`) | done |  | `get_process` `statistics.virtual_bytes` |
| Working set private (`working_set_private_bytes`) | done |  | `get_process` `statistics.private_working_set_bytes` and `working_set.private_bytes` with `private_from` |
| Shared commit (`shared_commit_bytes`) | done |  | `get_process` `statistics.shared_commit_bytes` and `commit.shared_commit_bytes` |
| Private delta (`private_bytes_delta`) | done |  | `list_processes` and `get_process` `private_delta_bytes`, per tick, `deltas_cover_ms` says the span |
| Page faults (`page_faults`) | done |  | `list_processes` and `get_process` `page_faults` |
| Page faults delta (`page_faults_delta`) | done |  | `page_faults_delta` |
| Hard fault count (`hard_fault_count`) | done |  | `hard_faults` |
| Hard faults delta (`hard_faults_delta`) | done |  | `hard_faults_delta` |
| Update interval ms (`update_interval_ms`) | done |  | `interval_ms` on `list_processes`, `rank_processes`, `process_history`, `get_process` (plus `deltas_cover_ms`) |
| Access denied (`access_denied`) | done |  | `get_process` `access_denied`, `limited_access`, `access_error`. `list_processes` `handle_denied`, `ws_denied` |
| Image coherency (`image_coherency`) | skipped |  | Tamper detection, not RAM, heavy scan, off by default in SI |
| Statistics quota paged pool (`statistics.quota.paged_pool_bytes`) | done |  | `get_process` `statistics.paged_pool_bytes` |
| Statistics quota peak paged pool (`statistics.quota.peak_paged_pool_bytes`) | done |  | `get_process` `statistics.peak_paged_pool_bytes` |
| Statistics quota non paged pool (`statistics.quota.non_paged_pool_bytes`) | done |  | `get_process` `statistics.nonpaged_pool_bytes` |
| Statistics quota peak non paged pool (`statistics.quota.peak_non_paged_pool_bytes`) | done |  | `get_process` `statistics.peak_nonpaged_pool_bytes` |
| Statistics quota page file (`statistics.quota.page_file_bytes`) | approx |  | `get_process` `statistics.private_bytes`, PagefileUsage is that same number, no second key |
| Statistics quota peak page file (`statistics.quota.peak_page_file_bytes`) | approx |  | `get_process` `statistics.peak_private_bytes`, PeakPagefileUsage is that same number |
| Working set split (`statistics.working_set`) | done |  | `get_process` `working_set` {`total_bytes`, `private_bytes`, `shareable_bytes`, `shared_bytes`, `locked_bytes`, ...} |
| Statistics page priority (`statistics.page_priority`) | done |  | `get_process` `page_priority` and `page_priority_name`, null when unknown and on Linux |
| Statistics page faults (`statistics.page_faults`) | done |  | `get_process` `statistics.page_faults` |
| Statistics peak virtual size (`statistics.peak_virtual_size`) | done |  | `get_process` `statistics.peak_virtual_bytes` |
| GDI and USER handle counts (`statistics.gui_resources`) | approx |  | `process_diag` view gdi : `os_gdi_objects` and `os_gdi_peak` plus the object rows, no USER handle counts |
| DEP state (`statistics.dep`) | skipped |  | Judged not worth porting |
| Process history, memory series (`get_process_history`) | done |  | `process_history` : `private_bytes` and `working_set_bytes` rings with last, min, max, avg, `window_seconds` |
| Process ranking tool (`rank_processes`) | done |  | `rank_processes`, every row carries both sizes and both growths |
| Rank by private bytes growth | done |  | `rank_processes` `rank_by` `private_growth` |
| Rank by private bytes | done |  | `rank_processes` `rank_by` `private_bytes` |
| Private (`private_bytes`) | done |  | `rank_processes` `private_bytes` |
| Private growth (`private_bytes_growth`) | done |  | `rank_processes` `private_growth_bytes`, signed, null under 2 samples |
| Sample count (`sample_count`) | done |  | `rank_processes` `sample_count` and `covered_seconds` |
| Thread page priority (`page_priority`) | done |  | `get_process` `include_threads` `thread_list[].page_priority`, `set_page_priority` with tid |
| Memory wait reasons of threads (`wait_reason`) | done |  | `get_process` `thread_list[].memory_wait` and `threads_waiting_on_memory` |
| Saved page priority (`saved_page_priority`) | approx |  | `get_process` `page_priority_rule` and `ifeo_page_priority`, `page_priority_rules` tool (FRS rules and IFEO, not SI's notes file) |

### Tools : process memory regions

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Process memory regions tool (`get_process_memory_regions`) | done |  | `list_regions` |
| Region sort keys (`sort_by`) | approx |  | `list_regions` `sort_by` `base_address`, `size_bytes`, `ws_total_bytes`, `ws_private_bytes`, `committed_bytes`, `private_bytes` |
| Include free (`include_free`) | done |  | `list_regions` `include_free` |
| Allocations only (`allocations_only`) | done |  | `list_regions` `allocations_only` |
| Summary only (`summary_only`) | done |  | `list_regions` `summary_only`, regions null and paging fields kept |
| Summary committed (`summary.committed_bytes`) | done |  | `list_regions` `summary.committed_bytes` |
| Summary private (`summary.private_bytes`) | done |  | `list_regions` `summary.private_bytes`, null when any region is unknown |
| Summary working set (`summary.working_set_bytes`) | done |  | `list_regions` `summary.working_set_bytes`, null when the walk failed |
| Summary private working set (`summary.private_working_set_bytes`) | done |  | `list_regions` `summary.private_working_set_bytes` |
| Summary committed regions (`summary.committed_regions`) | done |  | `list_regions` `summary.committed_regions` |
| Summary reserved regions (`summary.reserved_regions`) | done |  | `list_regions` `summary.reserved_regions` |
| Summary free regions (`summary.free_regions`) | done |  | `list_regions` `summary.free_regions` |
| Summary image regions (`summary.image_regions`) | done |  | `list_regions` `summary.image_regions` |
| Summary private regions (`summary.private_regions`) | done |  | `list_regions` `summary.private_regions`, plus `mapped_regions` |
| Summary executable private regions (`summary.executable_private_regions`) | done |  | `list_regions` `summary.executable_private_regions` |
| Summary executable private (`summary.executable_private_bytes`) | done |  | `list_regions` `summary.executable_private_bytes` |
| Base address (`base_address`) | done |  | `list_regions` `base_address`, hex string |
| Allocation base (`allocation_base`) | done |  | `list_regions` `allocation_base`, null when none |
| Size | done |  | `list_regions` `size_bytes` |
| State | done |  | `list_regions` state commit, reserve or free |
| Type | done |  | `list_regions` type private, mapped, image or null |
| Protection | done |  | `list_regions` protection |
| Allocation protection (`allocation_protection`) | done |  | `list_regions` `allocation_protection` |
| Use | done |  | `list_regions` use |
| Committed (`committed_bytes`) | done |  | `list_regions` `committed_bytes` |
| Private (`private_bytes`) | done |  | `list_regions` `private_bytes` |
| Region type (`region_type`) | done |  | `list_regions` `region_type` token |
| Region flags (`flags`) | approx |  | `list_regions` `region_flags` as one text, not a token array |
| Page priority (`page_priority`) | done |  | `list_regions` `page_priority` |
| Working set total (`working_set.total_bytes`) | done |  | `list_regions` `ws_total_bytes` (flat keys, not a `working_set` object) |
| Working set private (`working_set.private_bytes`) | done |  | `list_regions` `ws_private_bytes` |
| Working set shared (`working_set.shared_bytes`) | done |  | `list_regions` `ws_shared_bytes` |
| Working set shareable (`working_set.shareable_bytes`) | done |  | `list_regions` `ws_shareable_bytes` |
| Working set locked (`working_set.locked_bytes`) | done |  | `list_regions` `ws_locked_bytes` |
| Mapped file (`mapped_file`) | done |  | `list_regions` `mapped_file` |
| Signing level (`signing_level`) | skipped |  | Judged not worth porting. `list_regions` still carries `signing_level` |
| Thread id (`thread_id`) | done |  | `list_regions` `thread_id` |
| Heap of a region (`heap`) | done |  | `list_regions` `heap_id` and `heap_class` |
| Pool charge of a handle (`paged_pool_charge`, `non_paged_pool_charge`) | skipped |  | FRS has no handle tool |
| Section handle count (`counts_by_type`) | skipped |  | Enumerates every handle, only for shared memory hunts |
| Section details (`section`) | skipped |  | FRS has no handle tool |

### Tools : memory contents

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Read memory tool (`read_process_memory`) | done |  | `read_memory` : hex, refused above 64 KiB, needs `start_time`, Danger off by default, ?? on unreadable pages |
| Hex and ASCII fields of a memory read | approx |  | `read_memory` returns hex only, no ASCII field on purpose |
| Memory search tool (`search_process_memory`) | done |  | `search_memory` : hex pattern, addresses only, deadline, partial |
| String search tool (`search_process_strings`) | done |  | `search_strings` |
| String | done |  | `search_strings` string, clipped at 256 chars with clipped true |
| Length | done |  | `search_strings` length |
| Encoding | done |  | `search_strings` encoding ansi, utf16, utf8 |
| Address | done |  | `search_strings` address hex |
| Region base (`region_base`) | done |  | `search_strings` `region_base` |
| Region type (`region_type`) | done |  | `search_strings` `region_type` |
| Protection | done |  | `search_strings` protection |
| String scan statistics (`regions_scanned` and the like) | done |  | `search_strings` complete, partial, reason, `next_address`, `scanned_bytes`, `unreadable_bytes`, `regions_scanned` |
| Image coherency tool (`get_process_image_coherency`) | skipped |  | Tamper detection, not RAM, heavy scan, off by default in SI |
| Modified image pages tool (`get_image_page_modifications`) | done |  | `process_diag` view `modified_pages` : file, `image_base`, offset, address, symbol, resident |

### Tools : working set and memory priority actions

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Empty one working set tool (`empty_process_working_set`) | done |  | `reduce_working_set` : one pid or up to 256, `before_bytes` and `after_bytes` per pid, system and pseudo refused |
| Honest action text | done |  | `reduce_working_set` description and note : pages move to standby and fault back, private bytes does not change |
| Set page priority tool (`set_process_page_priority`) | done |  | `set_page_priority` returns `previous_priority` and the new one (Windows, Linux refuses) |
| Page priority names (`page_priority_name`) | approx |  | `page_priority_name` labels Lowest, Very low, Low, Medium, Below normal, Normal, not `snake_case` tokens |
| No min or max working set tool | skipped |  | SI has no such tool either. `get_process` quota reads min and max working set |

### Tools : dumps

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Minidump tool (`create_process_minidump`) | done |  | `create_dump` : minidump presets, raw region file, gcore on Linux, never overwrites, real path and size back |

### Tools : sections and job limits

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Section mappings tool (`get_section_mappings`) | skipped |  | Needs the driver |
| Section mappings by path | skipped |  | Needs the driver |
| Section mappings by pid and address | skipped |  | Needs the driver |
| Section mappings by pid and handle | skipped |  | Needs the driver |
| Is in job (`is_in_job`) | done |  | `get_process` `job.in_job`, null when unknown |
| Job open | approx |  | The driverless lookup (handle table scan, named jobs), `get_process` `job.jobs[].found_through`, job.note says why when no job could be opened |
| Limits process memory limit (`limits.process_memory_limit`) | approx |  | `get_process` `job.jobs[].process_memory_limit` {set,bytes} when FRS could open the job, else note says why |
| Limits job memory limit (`limits.job_memory_limit`) | approx |  | `get_process` `job.jobs[].job_memory_limit`, same condition |
| Limits minimum working set (`limits.minimum_working_set`) | approx |  | `get_process` `job.jobs[].working_set_minimum`, same condition |
| Limits maximum working set (`limits.maximum_working_set`) | approx |  | `get_process` `job.jobs[].working_set_maximum`, same condition |
| Limits peak process memory used (`limits.peak_process_memory_used`) | approx |  | `get_process` `job.jobs[].peak_process_memory_bytes`, same condition |
| Limits peak job memory used (`limits.peak_job_memory_used`) | approx |  | `get_process` `job.jobs[].peak_job_memory_bytes`, same condition |
| Memory limit flags of a job (`limits.flags`) | approx |  | Each limit carries set true or false instead of a flag token array |
| Accounting total page fault count (`accounting.total_page_fault_count`) | approx |  | `get_process` `job.jobs[].total_page_faults`, same condition |

### Tools : GPU memory (borderline)

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| GPU usage, memory fields (`get_gpu_usage`) | done |  | `gpu_memory` adapters[] dedicated, shared and their limits, plus total |
| Process GPU stats, memory fields (`get_process_gpu_stats`) | done |  | `gpu_memory` processes[] and process (with pid) : dedicated, shared, the committed pair, commit |
| GPU adapters, memory fields (`list_gpu_adapters`) | skipped |  | Judged not worth porting |

### Borderline process actions

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Terminate a process (`terminate_process`) | skipped |  | Process control, frees RAM only by ending a process, not built |
| Freeze and thaw a process (`freeze_process`, `thaw_process`) | skipped |  | Process control, frees RAM only by freezing a process, not built |
| Process environment (`get_process_environment`) | skipped |  | Judged not worth porting |
| Process mitigations (`get_process_mitigations`) | skipped |  | Judged not worth porting |
| Mapped files in the module list (`include_mapped_files`) | skipped |  | The region tool already covers it |
| Processes that map a file (`mapped_users`) | skipped |  | FRS has no such tool, `list_regions` `mapped_file` answers it one process at a time |
| Section handles of all processes (`find_handles`) | skipped |  | Enumerates every handle, only for shared memory hunts |
| .NET memory stream check (`is_memory_stream`) | skipped |  | Judged not worth porting |

### Gaps : what upstream's MCP does not do

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| No system memory list command | done |  | Clean (baseline) and `memory_command` (every SI Empty command plus all) |
| No write, free, decommit or protect tool | done |  | `write_memory`, `protect_memory`, `free_memory` as Danger tools, off by default |
| Pool limits not reported | impossible |  | Kernel globals, need the driver. `paged_pool.limit_bytes` stays null, `nonpaged_pool.limit_bytes` is the documented formula |
| No heap tool | done |  | `heap_diag` view heaps and locks (Danger, remote thread) |
| No working set history | done |  | `process_history` `working_set_bytes` ring, `rank_processes` `working_set_growth`, `memory_history` physical |
| No physical page or RAMMap style view | skipped |  | SI has none, FRS has no such tool |

### Tier model

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Four tiers | approx |  | Three tiers : Read, Write, Danger. No egress tier, nothing leaves the machine |
| Action table row | approx |  | Each tool registers category, name, description, schema and tier, hints come from one table |
| Memory tool tiers | done |  | Memory reads Read, trims and page priority Write, contents, dumps |
| Self-check at load | approx |  | A schema that does not parse prints a warning at start, no tier or hint self-check |

### Consent and permission model

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Server off by default | done |  | mcpOn defaults to 0 |
| Server starts after first provider run | done |  | procs::target and list tools wait for the first snapshot (waitFirst), `rank_processes` reports `sample_count` |
| Per tool Access | done |  | A tool set to Off leaves tools/list, a call to it gets an error saying the user turned it off |
| Per tool Authorization | approx |  | One Off / Ask / On mode per tool, no delegate |
| Connection approval | approx |  | Bearer token on every request instead of a prompt per connection, FRS is stateless HTTP |
| Call flow | done |  | callTool : tool known, mode, Danger refused while LAN is on, Ask prompt, run, activity line |
| Confirm none path | done |  | Mode On |
| Grant held path | approx |  | Always allow in the prompt sets the tool On and it is saved. A Danger On while elevated lasts until FRS closes |
| Always ask path | done |  | Ask mode ImGui modal, the connection thread waits, the UI and other connections keep running. Timed out |
| Delegate path | skipped |  | Client elicitation path, FRS is plain JSON HTTP with no stream |
| Dialog content | done |  | Prompt shows tool, tier, "Client says" (User-Agent, 120 bytes) |
| Auto deny | done |  | No answer in 60 s and the call is refused |
| Dialog excluded from capture | skipped |  | Capture exclusion would hide the whole FRS window |
| Dialog placement | approx |  | Modal centred in the FRS window, the tray brings a hidden or minimised window back while a prompt waits (askPending) |
| Session drop-down | approx |  | Allow once, Always allow, Deny |
| Second confirm for a standing grant | approx |  | Always allow is its own button next to Allow once, no second confirm |
| Consent class ProcessMemory | approx |  | No class grant, every Danger tool has its own mode |
| Sensitive with no class | approx |  | Every tool has its own mode, the prompt shows the arguments with the pid |
| Revoke grants and Disconnect | approx |  | Set a tool back to Off or Ask, or press None, in the MCP tab |
| Consent through `input_required` | skipped |  | Client elicitation path, FRS is plain JSON HTTP with no stream |
| Elicitation by server request | skipped |  | Client elicitation path, FRS is plain JSON HTTP with no stream |
| Elicitation wording per tier | skipped |  | Client elicitation path, FRS is plain JSON HTTP with no stream |
| Other requests during a consent wait | skipped |  | Client elicitation path, FRS is plain JSON HTTP with no stream |
| Audit log | approx |  | MCP tab activity line per call (tool, ok, error, blocked, denied, ms) and every action logs "[mcp] &lt;label&gt; (pid, address) : OK\|failed (why)" in the FRS log |
| Warning when removing authorization | approx |  | The All preset asks before it turns Danger tools on, a single tool switched to On does not warn |

### Target resolution

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Process identity : pid plus a sequence number | approx |  | pid plus `start_time` (creation time as a decimal string), required on every Danger tool, optional elsewhere. A reused pid is refused |
| Live sequence check | done |  | The backends compare the creation time again after they open the process |
| Cache lookup | done |  | The pid is looked up in the latest snapshot |
| Pseudo process refused | done |  | Pseudo processes refused "has no memory of its own", system ones refused for trim, page priority, heaps |
| Least access per action | approx |  | Each backend opens the rights its own call needs, `list_processes` reads without a handle. Not audited tool by tool |
| Arguments judged first | done |  | Argument checks run before the process is opened, procs::target is a snapshot lookup only |
| Parameter folded into approval | approx |  | The Ask prompt shows every argument and the call runs with exactly that object, no identity hash |
| Headline, description, audit text | approx |  | One log form "[mcp] &lt;label&gt; (pid N, 0xaddr) : OK", the prompt shows the raw arguments, the tool name in the activity line |
| Target cleanup | done |  | ProcRef holds no handle, backends close theirs inside each call |
| Reads resolve their own target | done |  | Every tool resolves its own target inside the handler, nothing is held across a prompt |

### Schema conventions

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Output schema on every tool (`outputSchema`) | skipped |  | Would send every answer twice and add bytes to tools/list for every client |
| All four annotation hints explicit | done |  | readOnlyHint, destructiveHint, idempotentHint and openWorldHint on every tool |
| Hint presets | done |  | Hints follow the tier (Read : read only and idempotent, Write : plain, Danger : destructive), a few tools differ |
| Standard note macros | approx |  | The untrusted data sentence lives once in the server instructions, `search_strings` repeats it, no per tool macro |
| Addresses as hex strings | done |  | Addresses are hex strings in and out (util::hex, util::parseHex), a JSON number address is refused |
| Bytes everywhere | done |  | For the 28 new tools (integer bytes, `_bytes` suffix), the old tools keep `_mb` for baseline compatibility |
| Null is not zero | done |  | It used to read 0 |
| Keys stay present | done |  | Flags, address and `unreadable_bytes` (null for the other kind) |
| Tokens instead of labels | approx |  | State, type, `region_type`, encoding, view, status are tokens. `page_priority_name`, `region_flags` and heap `flags_text` are labels |
| Descriptions teach | done |  | Descriptions say what a number is not : working set includes shared pages, private bytes is commit |
| Batch by pids | approx |  | `reduce_working_set` takes up to 256 pids with one result per pid, a dead pid does not fail the call. `get_process` is one pid |
| Input schema only partly enforced | approx |  | Wrongly typed arguments are refused by name, unknown ones are ignored, a few booleans fall back to their default |
| Schema version field (`schema_version`) | skipped |  | Niche, serverInfo.version 2.0 is the only version a client gets |
| Tool list change notice (`listChanged`) | skipped |  | FRS declares false, correct for a no-stream server |

### Pagination and size limits

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Paging arguments (`limit`, `offset`) | done |  | Limit and offset, clamped per tool through mcp::paged or the same rules |
| Paging fields (`count`, `total_count`, `offset`, `limit`, `truncated`) | done |  | Count, `total_count`, offset, limit |
| Sort arguments (`sort_by`, `descending`) | done |  | The error lists them, descending or order, ties by pid or base |
| Second list in one result | done |  | `get_process` `thread_list` with `threads_count`, `threads_total`, `threads_offset`, `threads_limit`, `threads_truncated` |
| Paging arg type checks | done |  | Limit and offset must be numbers, descending must be true or false, `sort_by` a declared key |
| Cursor feed | approx |  | Scans resume through `next_address` and `start_address`. `get_log` keeps limit only, as in 1.2 |
| Read cap | done |  | `read_memory` and `write_memory` refuse above 65536 bytes |
| Search caps | done |  | Pattern above 256 bytes refused "Nothing was searched", `max_results` cap 1000, `max_seconds` cap 60, `max_bytes` |
| Strings caps | done |  | `min_length` 4 to 256, `max_results` cap 5000, `max_seconds` cap 60, `max_bytes` |
| Message size | done |  | Truncated true and `size_budget_cut` true |
| Request size | done |  | Http refuses a body over 4 MB with 413 |
| Bounded rings | done |  | Every store is bounded : memory history ring, process rings, 200 activity lines |

### Snapshots and history

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Snapshot id, time and paused flag (`snapshot_id`, `snapshot_time`, `updates_paused`) | approx |  | Sampled tools carry `age_ms` or `sample_age_ms`, seq or `sample_id`, paused, `interval_ms` |
| Delta since a snapshot (`since_snapshot_id`) | skipped |  | Extended, FRS has no such tool, `rank_processes` and `process_history` answer the memory question |
| Delta completeness (`changes.complete`) | skipped |  | Goes with snapshot deltas, which are skipped |
| Change table | skipped |  | Goes with snapshot deltas, which are skipped |
| Update interval tracking | done |  | `interval_ms` on `memory_history`, `list_processes`, `rank_processes`, `process_history` |
| Series statistics | done |  | `process_history` last, min, max, avg per series, `memory_history` min, max |
| Shortest series bounds the walk | done |  | `process_history` walks one time ring shared by both series, window by seconds |
| Process create and exit events | skipped |  | Border, FRS has no event tool |
| Process exits | approx |  | `gpu_memory`, `dotnet_memory` and `ws_watch` answer about a minute after an exit with exited true, no exit list |

### Untrusted strings

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Standing warning | done |  | The server instructions end with the untrusted data sentence (kept), `search_strings` repeats it |
| Tool output is not sanitised | approx |  | Output strings go out as data (JSON escaped), `search_strings` clips each string at 256 chars with clipped, error texts pass target text through util::safe |
| Display sanitiser | done |  | util::safe drops control and bidi chars in the Ask prompt and the log lines that carry OS text |
| Client string clip | done |  | clientInfo name and version clipped to 256 bytes on a character boundary and through util::safe |
| Claimed versus checked | approx |  | The prompt labels the User-Agent "Client says", nothing the client claims reads as checked |
| Prompt template fill | skipped |  | FRS ships no MCP prompts, nothing splices arguments into instructions |

### Error model

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Tool error shape | approx |  | isError true with one plain sentence naming the problem and what to do, no machine readable code or ntstatus |
| Error codes | approx |  | JSON-RPC codes for protocol errors (-32700, -32601, -32602), tool errors are sentences without a code set |
| Recovery hints | approx |  | Error sentences say what to do. A denied error says when FRS runs without admin or root |
| Empty handler is an error | done |  | A handler that returns nothing becomes an error that says to check `get_log` before a retry |
| Partial success inside a result | done |  | `reduce_working_set` one result per pid, `memory_command` status per step, scans complete, partial and reason |

### Transport and authentication

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Named pipe plus stdio broker | skipped |  | Pipe broker design, FRS keeps Streamable HTTP |
| Pipe security | skipped |  | Pipe broker design, FRS keeps Streamable HTTP |
| Client authentication | skipped |  | Pipe broker design, FRS keeps Streamable HTTP |
| Connection flood limits | skipped |  | Pipe broker design, FRS keeps Streamable HTTP. FRS has its own : 16 connections, when full the oldest one still sending its headers makes room |
| One thread per connection | done |  | One thread per connection, 16 at once, 503 above |
| Cancellation | approx |  | Notifications get 202 and are dropped, long tools stop on mcp::alive and their own deadlines (scans 60 s, dump 300 s, remote heap thread 45 s) |
| Protocol versions | done |  | The four versions echoed, stateless so tools work without initialize |
| Broker status tool | skipped |  | Pipe broker design, FRS keeps Streamable HTTP |

### Options UI

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Status line | done |  | MCP tab RUNNING with the url, stopped, or [!] with the start error |
| Allow sandboxed clients | skipped |  | Pipe broker design, FRS keeps Streamable HTTP |
| Search tools | done |  | MCP tab Filter tools box |
| Tool | done |  | MCP tab tool name column, description in the tooltip |
| Access | approx |  | One Off / Ask / On control per tool covers access and authorization |
| Authorization | approx |  | The same control, Ask |
| Allowed | approx |  | On |
| Denied | approx |  | Off |
| Always ask | done |  | Ask |
| Delegate to client | skipped |  | Client elicitation path, FRS is plain JSON HTTP with no stream |
| Not required | approx |  | On |
| Reset to defaults, Reset all to defaults | approx |  | Presets All, Standard (the tier defaults), Read-only, None |
| Ctrl+A | skipped |  | Niche, the ImGui tools table has no multi select, the presets set every tool at once |
| Id | skipped |  | Connection table for pipe sessions, FRS serves stateless HTTP |
| User | skipped |  | Connection table for pipe sessions, FRS serves stateless HTTP |
| Client | skipped |  | Connection table for pipe sessions, FRS serves stateless HTTP |
| Launcher | skipped |  | Connection table for pipe sessions, FRS serves stateless HTTP |
| Connected | skipped |  | Connection table for pipe sessions, FRS serves stateless HTTP |
| Calls | skipped |  | Connection table for pipe sessions, FRS serves stateless HTTP |
| Session grants | skipped |  | Connection table for pipe sessions, FRS serves stateless HTTP |
| Client configuration | skipped |  | FRS MCP tab already builds the client config |

### Resources and prompts

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Resource `si://system/summary` | skipped |  | No MCP resources in FRS and none planned, `ram_status` and `memory_details` give the same answer |
| Resource `si://processes/tree` | skipped |  | No MCP resources in FRS and none planned, `list_processes` gives the same answer |
| Prompt `why_slow` | approx |  | No prompts/get : the server instructions say look before you clean and name the entry tools per job |
| Prompt `triage_process`, memory line | approx |  | The descriptions carry those sentences : working set includes shared pages, private bytes is commit, a trim does not lower it |

### Design decisions : copy

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Copy : setters return the previous value | done |  | Setters return the old value : `write_memory` the old bytes, `protect_memory` the old protection, `set_page_priority` the old priority |
| Copy : actions report before and after | done |  | Actions return before and after : clean, `memory_command`, `reduce_working_set`, `flush_heaps` |
| Copy : answer trends from a running sampler | done |  | `memory_history`, `rank_processes` and `process_history` read the sampler rings, no sleep in the call |
| Copy : descriptions that correct the common misreading | done |  | Descriptions and the server instructions : standby is reusable cache, trimmed pages fault back |
| Copy : scans say how far they got | done |  | Complete, partial, reason, `unreadable_bytes`, `regions_no_access`, `next_address` on both scans |
| Copy : fail closed on consent | done |  | Ask refuses after 60 s with nobody answering, Esc denies, no keyboard nav so Enter can't allow |
| Copy : never overwrite on dump | done |  | `create_dump` refuses an existing path "the file already exists, nothing was written" |

### Design decisions : avoid

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| Avoid : arguments outside the approval | done |  | The call runs with exactly the argument object the prompt shows |
| Avoid : silently truncating a search pattern | done |  | `pattern_hex` above 256 bytes refused, nothing searched |
| Avoid : a memory scan with no deadline | done |  | Both scans have `max_seconds` (default 30, cap 60) and check mcp::alive per chunk |
| Avoid : uncounted read failures | done |  | `unreadable_bytes`, `regions_unreadable`, `regions_partial`, `regions_no_access` counted |
| Avoid : sending every result twice | done |  | Text content only, no structuredContent |
| Avoid : no response size budget | done |  | 2 MB budget per answer, plus row caps per tool and `search_strings` clips each string |
| Avoid : zero on failed query | done |  | The new tools send null when unknown. `ram_status` keeps its baseline numbers |
| Avoid : schema text drifting from code | approx |  | Descriptions written by hand and checked against the code, nothing generates them |
| Avoid : unchecked conversion of client text | done |  | Client strings arrive as UTF-8 through Json, `sort_by` is compared with known keys |
| Avoid : returning target bytes as readable words by default | done |  | `read_memory` returns hex only |
| Avoid : a full page walk for one number | done |  | `reduce_working_set` before and after come from ProcessVmCounters, no page walk |
| Avoid : 460 KB of tool text on every client | done |  | 38 tools, tools/list 34,163 bytes with every tool on, limit 45 KB. Off tools drop out of the list |

### FRS specific consequences

| Feature | Status | Not run | Note |
| --- | --- | --- | --- |
| FRS serves one request at a time | done |  | Http serves each connection on its own thread (16 at once), a long scan does not block other calls |
| Memory content tools need an owner decision | done |  | Owner decision applied : Danger tier off by default, needs a deliberate turn on |
| Map to existing FRS tools | done |  | Every suggested addition exists : `rank_processes`, `memory_details`, `reduce_working_set` with identity |
| FRS JSON is double only | done |  | Addresses, `start_time` and flag masks travel as strings, sizes stay under 2^53 |
