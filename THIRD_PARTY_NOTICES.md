# Third-party software

LightView itself is licensed under the GNU General Public License v3.0 or later (see `LICENSE`).
It uses the following third-party components. None of their source code is stored in this
repository: CMake downloads pinned versions (verified by SHA-256) at configure time.

## DjVuLibre

- Used for: DjVu decoding (`libdjvu`, statically linked into `lightview.exe`)
- Source: <https://sourceforge.net/projects/djvu/>, mirror <https://github.com/barak/djvulibre>,
  commit `03e6abd7295c74f795ed70a7118294b25351e0f4`
- License: GNU General Public License, version 2 or (at your option) any later version.
  LightView uses it under GPL-3.0-or-later.
- Copyright (c) 2002 Leon Bottou and Yann Le Cun, (c) 2001 AT&T, (c) 1999-2001 LizardTech, Inc.

Built without `NEED_JPEG_DECODER`, so there is no libjpeg dependency.

## PDFium

- Used for: PDF rendering, text and links (`pdfium.dll`, loaded on demand)
- Binaries: <https://github.com/bblanchon/pdfium-binaries>, release `chromium/8086` (PDFium 157.0.8086.0),
  built from <https://pdfium.googlesource.com/pdfium/>
- License: BSD 3-Clause, Copyright 2014 The PDFium Authors

`pdfium.dll` also contains the following libraries, each under its own license:

| Component | License |
| --- | --- |
| Abseil | Apache-2.0 |
| Anti-Grain Geometry 2.3 | AGG license (BSD-style) |
| Dragonbox | Apache-2.0 with LLVM exception, or Boost Software License 1.0 |
| fast_float | MIT (also Apache-2.0 / BSL-1.0) |
| FreeType | FreeType License (FTL) or GPL-2.0 |
| HarfBuzz | Old MIT |
| ICU | Unicode License V3 |
| Little CMS | MIT |
| libjpeg-turbo | IJG License, BSD 3-Clause, zlib |
| OpenJPEG | BSD 2-Clause |
| libpng | libpng License |
| LLVM libc | Apache-2.0 with LLVM exception |
| simdutf | MIT (also Apache-2.0) |
| zlib | zlib License |

Release archives contain the complete license texts in `licenses/`, installed by `cmake --install`.

## Icons

Toolbar icons are glyphs of the *Segoe MDL2 Assets* font that ships with Windows. The font is
used at run time and is not redistributed. The application icon (`res/lightview.ico`) is generated
by `tools/make-icon.ps1` and is part of LightView.
