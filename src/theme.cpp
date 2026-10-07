// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "theme.h"

#include <dwmapi.h>
#include <uxtheme.h>

namespace lv {

namespace {

Theme g_theme;

bool system_uses_dark() {
  DWORD v = 1, size = sizeof(v);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                   L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) {
    return false;
  }
  return v == 0;
}

}  // namespace

const Theme& theme() { return g_theme; }

void theme_init() {
  Theme& t = g_theme;
  t.dark = system_uses_dark();
  if (t.dark) {
    t.bar_bg = RGB(0x2B, 0x2B, 0x2B);
    t.bar_fg = RGB(0xE8, 0xE8, 0xE8);
    t.bar_fg_disabled = RGB(0x70, 0x70, 0x70);
    t.bar_hover = RGB(0x3D, 0x3D, 0x3D);
    t.bar_pressed = RGB(0x4A, 0x4A, 0x4A);
    t.bar_checked = RGB(0x1F, 0x4E, 0x79);
    t.bar_border = RGB(0x1A, 0x1A, 0x1A);
    t.edit_bg = RGB(0x1F, 0x1F, 0x1F);
    t.edit_fg = RGB(0xE8, 0xE8, 0xE8);
    t.edit_border = RGB(0x55, 0x55, 0x55);
    t.canvas_bg = RGB(0x1E, 0x1E, 0x1E);
    t.page_border = RGB(0x0A, 0x0A, 0x0A);
    t.hint_fg = RGB(0x9A, 0x9A, 0x9A);
    t.side_bg = RGB(0x25, 0x25, 0x25);
    t.side_fg = RGB(0xE0, 0xE0, 0xE0);
    t.splitter = RGB(0x1A, 0x1A, 0x1A);
    t.toast_bg = RGB(0x3C, 0x3C, 0x3C);
    t.toast_fg = RGB(0xF2, 0xF2, 0xF2);
  } else {
    t.bar_bg = RGB(0xF5, 0xF5, 0xF5);
    t.bar_fg = RGB(0x1E, 0x1E, 0x1E);
    t.bar_fg_disabled = RGB(0xA8, 0xA8, 0xA8);
    t.bar_hover = RGB(0xE2, 0xE2, 0xE2);
    t.bar_pressed = RGB(0xD0, 0xD0, 0xD0);
    t.bar_checked = RGB(0xCC, 0xE4, 0xF7);
    t.bar_border = RGB(0xD9, 0xD9, 0xD9);
    t.edit_bg = RGB(0xFF, 0xFF, 0xFF);
    t.edit_fg = RGB(0x1E, 0x1E, 0x1E);
    t.edit_border = RGB(0xBC, 0xBC, 0xBC);
    t.canvas_bg = RGB(0xD6, 0xD6, 0xD6);
    t.page_border = RGB(0xA6, 0xA6, 0xA6);
    t.hint_fg = RGB(0x60, 0x60, 0x60);
    t.side_bg = RGB(0xFB, 0xFB, 0xFB);
    t.side_fg = RGB(0x1E, 0x1E, 0x1E);
    t.splitter = RGB(0xD9, 0xD9, 0xD9);
    t.toast_bg = RGB(0x33, 0x33, 0x33);
    t.toast_fg = RGB(0xF2, 0xF2, 0xF2);
  }
}

void apply_dark_title(HWND hwnd) {
  BOOL dark = g_theme.dark;
  // DWMWA_USE_IMMERSIVE_DARK_MODE is 20 since Windows 10 20H1 and 19 before that.
  if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)))) DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
}

void apply_dark_controls(HWND hwnd) {
  SetWindowTheme(hwnd, g_theme.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

}  // namespace lv
