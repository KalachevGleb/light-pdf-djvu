// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace lv {

struct ToolbarHost {
  virtual void execute(int cmd) = 0;
  virtual void on_page_entered(const std::wstring& text) = 0;
  virtual void on_zoom_entered(const std::wstring& text) = 0;
  virtual void on_search(const std::wstring& text, bool forward) = 0;
  virtual void on_edit_escape() = 0;
};

class Toolbar {
 public:
  static void register_class(HINSTANCE inst);
  bool create(HWND parent, ToolbarHost* host, int dpi);
  HWND hwnd() const { return hwnd_; }
  int height() const;
  void set_dpi(int dpi);
  void set_page(int page, int count);  // page is 0-based; count 0 = no document
  void set_zoom(float percent);
  void set_checked(int cmd, bool on);
  void set_enabled(int cmd, bool on);
  void focus_page();
  void focus_search();
  std::wstring search_text() const;
  bool is_child_edit(HWND h) const { return h && (h == page_edit_ || h == zoom_edit_ || h == search_edit_); }

 private:
  enum Kind { Button, Separator, PageBox, ZoomBox, Spacer, SearchBox };
  struct Item {
    Kind kind;
    int cmd = 0;
    wchar_t glyph = 0;
    const wchar_t* tip = nullptr;
    bool checked = false;
    bool enabled = true;
    RECT rc{};
  };

  static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK edit_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
  LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
  void layout();
  void paint(HDC dc);
  void draw_fit_width(HDC dc, const RECT& rc, COLORREF color);
  void make_fonts();
  int s(int v) const { return MulDiv(v, dpi_, 96); }
  int hit(POINT pt) const;
  Item* item(int cmd);
  RECT edit_frame(Kind kind) const;

  HWND hwnd_ = nullptr;
  HWND tooltip_ = nullptr;
  HWND page_edit_ = nullptr, zoom_edit_ = nullptr, search_edit_ = nullptr;
  ToolbarHost* host_ = nullptr;
  int dpi_ = 96;
  HFONT ui_font_ = nullptr, icon_font_ = nullptr;
  HBRUSH edit_brush_ = nullptr;
  std::vector<Item> items_;
  int hot_ = -1, pressed_ = -1;
  bool tracking_ = false;
  std::wstring page_label_;
};

}  // namespace lv
