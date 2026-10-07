// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <windows.h>

namespace lv {

struct Theme {
  bool dark = false;
  COLORREF bar_bg, bar_fg, bar_fg_disabled, bar_hover, bar_pressed, bar_checked, bar_border;
  COLORREF edit_bg, edit_fg, edit_border;
  COLORREF canvas_bg, page_border, hint_fg;
  COLORREF side_bg, side_fg, splitter;
  COLORREF toast_bg, toast_fg;
};

// Follows the Windows "app mode" (light/dark) setting.
const Theme& theme();
void theme_init();

// Dark title bar and dark scrollbars/tree on Windows 10 1809+; no-op elsewhere.
void apply_dark_title(HWND hwnd);
void apply_dark_controls(HWND hwnd);

}  // namespace lv
