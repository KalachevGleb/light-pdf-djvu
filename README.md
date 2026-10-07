# LightView

**English** | [Русский](README.ru.md)

A small, fast PDF and DjVu viewer for Windows. It is a native Win32 C++ application with no
frameworks, background services, updater, telemetry or network access.

- **PDF**: rendered by PDFium (the engine used in Chrome), via the prebuilt
  [pdfium-binaries](https://github.com/bblanchon/pdfium-binaries). `pdfium.dll` is loaded only
  when a PDF is opened.
- **DjVu**: rendered by DjVuLibre, built from source and linked statically into `lightview.exe`.

The window is ready about 0.1 s after launch, even for a 640-page PDF or a 1,000-page DjVu book.
Pages are rendered as tiles on a background thread and cached, so scrolling stays smooth.

## Download

Prebuilt versions are on the [Releases](../../releases) page. Unpack `LightView-*-win-x64.zip`
anywhere and run `lightview.exe`. It needs Windows 10 or 11 (x64) and does not need installing.

## Features

- Continuous scrolling, two-page spreads (cover page shown alone), rotation.
- Zoom: fit width, fit page, actual size, Ctrl+wheel (zooms around the cursor).
- Outline (PDF and DjVu bookmarks) in a resizable side panel.
- Search in PDF text and in the DjVu text layer. It runs in the background, ignores case, treats
  Cyrillic `ё` as `е`, joins words hyphenated across lines and highlights every match.
- Text selection with the mouse, double-click to select a word, Ctrl+A / Ctrl+C.
- Links: jumps within the document and external URLs. LightView asks before opening local files or
  unusual URL schemes.
- Back and forward to the previous spot with the mouse side buttons (or Alt+← / Alt+→). A spot is
  remembered before following a link, an outline entry or a page number, before Home/End, and
  before a scroll-bar jump longer than half a screen. The exact position is restored, horizontal
  offset included.
- "Crisp PDF text" (on by default, toggled in the right-click menu). PDFium does not hint
  Type 1/CFF fonts, which most LaTeX output uses, so without help glyph stems look gray and soft.
  LightView turns on subpixel text when Windows uses ClearType and applies a gamma curve that
  restores stem contrast. Side effect: midtones in pictures get slightly darker. Printing is not
  affected.
- Printing: page ranges, copies, automatic rotation of landscape pages, shrink to fit.
- Night mode (inverted to dark gray tones) and full screen.
- Remembers page, zoom, rotation and spread mode for every file, and the window position.
- Reloads the file when it changes on disk (handy with LaTeX). The file is never locked.
- Opening a file that is already open brings its window to the front.
- Light and dark theme (follows Windows), Per-Monitor DPI v2.

## Keyboard

| Action | Keys |
| --- | --- |
| Open / print / close window | Ctrl+O / Ctrl+P / Ctrl+W |
| Reload | F5, Ctrl+R |
| Page through | PgUp/PgDn, Space/Shift+Space, arrows, Home/End |
| Go to page | Ctrl+G |
| Back / forward to the previous spot | mouse side buttons, Alt+← / Alt+→ |
| Outline | F9 |
| Zoom | Ctrl+wheel, Ctrl+plus / Ctrl+minus |
| Fit page / actual size / fit width | Ctrl+0 / Ctrl+1 / Ctrl+2 |
| Two-page spread | Ctrl+3 |
| Rotate | Ctrl+Shift+plus / Ctrl+Shift+minus |
| Night mode | Ctrl+I |
| Full screen | F11, Ctrl+L; Esc to leave |
| Find | Ctrl+F, then Enter/F3, backwards Shift+Enter/Shift+F3 |
| Copy / select all | Ctrl+C / Ctrl+A |
| Shortcut list | F1 |

Mouse: dragging over text selects it, dragging elsewhere scrolls the page (the middle button always
scrolls). The right-click menu holds every command and the recent files.

The user interface is in Russian.

## Building

You need Visual Studio 2022 with the "Desktop development with C++" workload, which includes MSVC,
CMake and Ninja. The first configure needs internet access: CMake downloads PDFium and DjVuLibre
into `build/*/_deps` and checks their SHA-256.

From an "x64 Native Tools Command Prompt for VS 2022" in the repository root:

```bat
cmake --preset release
cmake --build --preset release
cmake --install build/Release --prefix dist
```

`dist/` is then a portable folder with `lightview.exe`, `pdfium.dll` and the license texts. The CRT
is linked statically, so the Visual C++ Redistributable is not needed.

Debug build: `cmake --preset debug && cmake --build --preset debug`.

[GitHub Actions](.github/workflows/build.yml) builds every push and pull request and attaches the
zip as a run artifact. To publish a release, push a tag such as `v0.1.0`
(`git tag v0.1.0 && git push origin v0.1.0`). The workflow then attaches
`LightView-0.1.0-win-x64.zip` to a new GitHub release.

## Making it the default viewer

LightView installs nothing by itself. To add it to "Open with" for `.pdf`, `.djvu` and `.djv`, run
`lightview.exe --register` or use "Открывать PDF и DjVu в LightView…" in the right-click menu. This
writes only to `HKCU\Software\Classes`, so no administrator rights are needed. Then right-click a
file → "Open with" → "Choose another app" → LightView → "Always use this app". To undo, run
`lightview.exe --unregister`.

## Settings

`LightView.ini` is stored next to `lightview.exe` when that folder is writable (portable mode),
otherwise in `%APPDATA%\LightView`. It holds the window position, view options and the last 300 files.

## Engine smoke test

A console tool opens every PDF/DjVu in a folder, renders the first page, extracts text, links and
outline, and prints how long each step took:

```bat
cmake --build --preset release --target lightview_smoke
build\Release\bin\lightview_smoke.exe "D:\Books" --compare
build\Release\bin\lightview_smoke.exe --print book.djvu out.pdf 1 3
build\Release\bin\lightview_smoke.exe --links paper.pdf 1
```

`--compare` checks the DjVu page sizes read directly from the file (the fast path) against
DjVuLibre's own page info. `--print` prints through "Microsoft Print to PDF" without dialogs.
`--links` lists the links on a page.

## Source layout

| Path | Purpose |
| --- | --- |
| `src/document.*` | Common document interface, text and link caches, search normalization |
| `src/pdf_document.cpp` | PDFium: non-locking file access, abortable rendering, forms, text, links, outline |
| `src/djvu_document.cpp` | DjVuLibre: fast page-size scan, tile rendering, text layer, links, outline |
| `src/renderer.*` | Render thread, 1024×1024 tiles, LRU cache with a memory budget, cancellation |
| `src/canvas.*` | Document view: layout, scrolling, zoom, painting, selection, links, search, history |
| `src/toolbar.*` | Toolbar (Segoe MDL2 Assets icons) |
| `src/main_window.*` | Main window, outline panel, commands, menus, auto-reload, file type registration |
| `src/print.*` | Printing |
| `src/settings.*`, `src/theme.*`, `src/util.*` | Settings, theme, helpers |
| `cmake/*.cmake` | Fetching and building PDFium and DjVuLibre |
| `tests/smoke.cpp` | Engine smoke test |
| `tools/make-icon.ps1` | Generates `res/lightview.ico` |

## Limitations

- View only: no editing, annotations or form filling. Filled-in forms are displayed.
- No XFA forms (PDFium is built without XFA and JavaScript).
- DjVu pages with JPEG layers (`BGjp`/`FGjp`, very rare) are shown without those layers, because
  DjVuLibre is built without libjpeg.
- PDFs larger than 4 GB do not open.
- Printing is raster, at up to 300 dpi.
- Windows only for now. The document engines are portable; the user interface is Win32.

## License

Copyright © 2026 Kalachev Gleb. LightView is licensed under the
[GNU General Public License v3.0 or later](LICENSE).

Third-party components: DjVuLibre (GPL-2.0-or-later, used under GPL-3.0) and PDFium
(BSD-3-Clause) with its bundled libraries. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
`cmake --install` copies the full license texts to `dist/licenses`, and release archives include them.
