# Third party notices

FRS is MIT licensed, see [LICENSE](LICENSE). It builds on the work below.

## Dear ImGui

[ocornut/imgui](https://github.com/ocornut/imgui), v1.92.8. `setup.bat` and `setup.sh` fetch it into `imgui/` and it is compiled into FRS. It draws the entire UI.

```
The MIT License (MIT)

Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## System Informer

[winsiderss/systeminformer](https://github.com/winsiderss/systeminformer). The memory features of FRS (page lists, process memory, regions, hex editor, strings, dumps, heaps, pool tags, working set watch, GPU and .NET memory and more) are ported or adapted from it. [docs/SYSTEM_INFORMER_COVERAGE.md](docs/SYSTEM_INFORMER_COVERAGE.md) lists every feature and what FRS does with it.

Memory struct layouts follow the phnt headers, and the D3DKMT and CLR layouts as bundled with System Informer :

- NT structs in the `*_win.cpp` files follow System Informer's phnt headers (`phnt/include`).
- The D3DKMT layouts in `src/gpumem_win.cpp` follow `d3dkmthk.h` as bundled with System Informer (`plugins/ExtendedTools/d3dkmt`). That header says : Copyright (c) Microsoft Corporation. Licensed under the MIT License.
- The CLR block layouts in `src/dotnetmem_win.cpp` follow the .NET runtime headers as bundled with System Informer (`plugins/DotNetTools/clr`). Those headers say : Licensed to the .NET Foundation under one or more agreements. The .NET Foundation licenses this file to you under the MIT license.

Only the layouts were written down by hand, with size checks. No header is shipped with FRS.

```
MIT License

Copyright (c) 2022 Winsider Seminars & Solutions, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Not included

`pooltag.txt`, the pool tag list FRS can read for driver names and descriptions, is Microsoft's and is not shipped with FRS. You place your own copy next to the exe, see the Kernel page in [README.md](README.md#kernel-page).

## Mem Reduct

Mem Reduct by Henry++ (GPL-3.0) : feature ideas, no code taken. [henrypp/memreduct](https://github.com/henrypp/memreduct) gave the ideas for the clean set, the timer, the hotkey, the tray menu, the command line switches and more. FRS rebuilt that behaviour in its own code : no code, strings or struct layouts of Mem Reduct are in FRS. [docs/MEMREDUCT_COVERAGE.md](docs/MEMREDUCT_COVERAGE.md) lists every feature and what FRS does with it.

## Privacy tweaks

The privacy tweak list credit to [hellzerg/optimizer](https://github.com/hellzerg/optimizer) (GPL-3.0) stands as written in the Credits section of [README.md](README.md#credits).
