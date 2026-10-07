// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "toolbar.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>

#include "commands.h"
#include "theme.h"
#include "util.h"

namespace lv {

namespace {

constexpr wchar_t kClass[] = L"LightView.Toolbar";
constexpr int kGlyphFitWidth = 1;  // drawn with GDI, no suitable font glyph
constexpr UINT kMsgRefresh = WM_APP + 10;

void fill(HDC dc, const RECT& rc, COLORREF c) {
  SetDCBrushColor(dc, c);
  FillRect(dc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
}

void frame(HDC dc, const RECT& rc, COLORREF c) {
  SetDCBrushColor(dc, c);
  FrameRect(dc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
}

}  // namespace

void Toolbar::register_class(HINSTANCE inst) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);
}

bool Toolbar::create(HWND parent, ToolbarHost* host, int dpi) {
  host_ = host;
  dpi_ = dpi;
  items_ = {
      {Button, CMD_TOC, 0xE8A4, L"Оглавление (F9)"},
      {Button, CMD_OPEN, 0xED25, L"Открыть (Ctrl+O)"},
      {Button, CMD_PRINT, 0xE749, L"Печать (Ctrl+P)"},
      {Separator},
      {Button, CMD_PREV_PAGE, 0xE70E, L"Предыдущая страница (PgUp)"},
      {PageBox},
      {Button, CMD_NEXT_PAGE, 0xE70D, L"Следующая страница (PgDn)"},
      {Separator},
      {Button, CMD_ZOOM_OUT, 0xE71F, L"Уменьшить (Ctrl+−)"},
      {ZoomBox},
      {Button, CMD_ZOOM_IN, 0xE8A3, L"Увеличить (Ctrl++)"},
      {Button, CMD_FIT_WIDTH, kGlyphFitWidth, L"По ширине (Ctrl+2)"},
      {Button, CMD_FIT_PAGE, 0xE9A6, L"Страница целиком (Ctrl+0)"},
      {Separator},
      {Button, CMD_ROTATE_RIGHT, 0xE7AD, L"Повернуть по часовой (Ctrl+Shift++)"},
      {Button, CMD_FACING, 0xE89A, L"Разворот: две страницы (Ctrl+3)"},
      {Button, CMD_INVERT, 0xE708, L"Ночной режим (Ctrl+I)"},
      {Button, CMD_FULLSCREEN, 0xE740, L"Полный экран (F11)"},
      {Spacer},
      {SearchBox},
      {Button, CMD_FIND_PREV, 0xE70E, L"Найти предыдущее (Shift+F3)"},
      {Button, CMD_FIND_NEXT, 0xE70D, L"Найти далее (F3)"},
  };

  HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
  hwnd_ = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, nullptr,
                          inst, this);
  if (!hwnd_) return false;

  const DWORD es = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL;
  page_edit_ = CreateWindowExW(0, WC_EDITW, L"", es | ES_CENTER, 0, 0, 0, 0, hwnd_, (HMENU)1, inst, nullptr);
  zoom_edit_ = CreateWindowExW(0, WC_EDITW, L"", es | ES_CENTER, 0, 0, 0, 0, hwnd_, (HMENU)2, inst, nullptr);
  search_edit_ = CreateWindowExW(0, WC_EDITW, L"", es, 0, 0, 0, 0, hwnd_, (HMENU)3, inst, nullptr);
  for (HWND e : {page_edit_, zoom_edit_, search_edit_}) SetWindowSubclass(e, edit_proc, 1, (DWORD_PTR)this);
  SendMessageW(search_edit_, EM_SETCUEBANNER, TRUE, (LPARAM)L"Поиск (Ctrl+F)");
  SendMessageW(page_edit_, EM_SETLIMITTEXT, 7, 0);
  SendMessageW(zoom_edit_, EM_SETLIMITTEXT, 7, 0);
  edit_brush_ = CreateSolidBrush(theme().edit_bg);

  tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0,
                             0, 0, hwnd_, nullptr, inst, nullptr);
  for (auto& it : items_) {
    if (it.kind != Button) continue;
    TTTOOLINFOW ti{sizeof(ti)};
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = hwnd_;
    ti.uId = it.cmd;
    ti.lpszText = (LPWSTR)it.tip;
    SendMessageW(tooltip_, TTM_ADDTOOLW, 0, (LPARAM)&ti);
  }
  make_fonts();
  set_page(0, 0);
  return true;
}

int Toolbar::height() const { return s(40); }

void Toolbar::make_fonts() {
  if (ui_font_) DeleteObject(ui_font_);
  if (icon_font_) DeleteObject(icon_font_);
  NONCLIENTMETRICSW ncm{sizeof(ncm)};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_);
  ui_font_ = CreateFontIndirectW(&ncm.lfMessageFont);
  icon_font_ = CreateFontW(-s(16), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");
  for (HWND e : {page_edit_, zoom_edit_, search_edit_}) {
    SendMessageW(e, WM_SETFONT, (WPARAM)ui_font_, FALSE);
    SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(s(3), s(3)));
  }
  SendMessageW(tooltip_, WM_SETFONT, (WPARAM)ui_font_, FALSE);
}

void Toolbar::set_dpi(int dpi) {
  dpi_ = dpi;
  make_fonts();
  layout();
}

void Toolbar::set_page(int page, int count) {
  std::wstring label = count > 0 ? fmt(L"/ %d", count) : L"/ –";
  if (label != page_label_) {
    page_label_ = label;
    layout();
  }
  std::wstring text = count > 0 ? fmt(L"%d", page + 1) : L"";
  if (GetFocus() != page_edit_ && window_text(page_edit_) != text) SetWindowTextW(page_edit_, text.c_str());
}

void Toolbar::set_zoom(float percent) {
  if (GetFocus() == zoom_edit_) return;
  std::wstring t = percent > 0 ? fmt(L"%d%%", (int)(percent + 0.5f)) : L"";
  if (window_text(zoom_edit_) != t) SetWindowTextW(zoom_edit_, t.c_str());
}

Toolbar::Item* Toolbar::item(int cmd) {
  for (auto& it : items_) {
    if (it.kind == Button && it.cmd == cmd) return &it;
  }
  return nullptr;
}

void Toolbar::set_checked(int cmd, bool on) {
  if (Item* it = item(cmd); it && it->checked != on) {
    it->checked = on;
    InvalidateRect(hwnd_, &it->rc, FALSE);
  }
}

void Toolbar::set_enabled(int cmd, bool on) {
  if (Item* it = item(cmd); it && it->enabled != on) {
    it->enabled = on;
    InvalidateRect(hwnd_, &it->rc, FALSE);
  }
}

void Toolbar::focus_page() {
  SetFocus(page_edit_);
  SendMessageW(page_edit_, EM_SETSEL, 0, -1);
}

void Toolbar::focus_search() {
  SetFocus(search_edit_);
  SendMessageW(search_edit_, EM_SETSEL, 0, -1);
}

std::wstring Toolbar::search_text() const { return window_text(search_edit_); }

RECT Toolbar::edit_frame(Kind kind) const {
  for (auto& it : items_) {
    if (it.kind == kind) {
      RECT r = it.rc;
      int h = s(26);
      r.top = (height() - h) / 2;
      r.bottom = r.top + h;
      if (kind == PageBox) r.right = r.left + s(48);
      return r;
    }
  }
  return {};
}

void Toolbar::layout() {
  if (!hwnd_) return;
  RECT cr;
  GetClientRect(hwnd_, &cr);
  const int width = cr.right;
  const int bh = s(32), top = (height() - bh) / 2;

  HDC dc = GetDC(hwnd_);
  HGDIOBJ old = SelectObject(dc, ui_font_);
  SIZE label{};
  GetTextExtentPoint32W(dc, page_label_.c_str(), (int)page_label_.size(), &label);
  SelectObject(dc, old);
  ReleaseDC(hwnd_, dc);

  auto item_width = [&](const Item& it) {
    switch (it.kind) {
      case Button: return bh;
      case Separator: return s(13);
      case PageBox: return s(48) + s(6) + (int)label.cx + s(4);
      case ZoomBox: return s(58);
      default: return 0;
    }
  };
  int fixed = s(6);
  for (auto& it : items_) fixed += item_width(it) + s(2);
  int search_w = std::clamp(width - fixed - s(8), 0, s(240));
  if (search_w < s(110)) search_w = 0;

  int x = s(6);
  for (auto& it : items_) {
    int w = item_width(it);
    if (it.kind == Spacer) w = std::max(0, width - fixed - search_w - s(8));
    if (it.kind == SearchBox) w = search_w;
    it.rc = {x, top, x + w, top + bh};
    x += w + (w ? s(2) : 0);
    if (it.kind == Button && tooltip_) {
      TTTOOLINFOW ti{sizeof(ti)};
      ti.hwnd = hwnd_;
      ti.uId = it.cmd;
      ti.rect = it.rc;
      SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    }
  }
  // Search prev/next buttons are useless without the box.
  for (auto& it : items_) {
    if (search_w == 0 && (it.cmd == CMD_FIND_PREV || it.cmd == CMD_FIND_NEXT)) it.rc = {};
  }

  HDC tdc = GetDC(hwnd_);
  old = SelectObject(tdc, ui_font_);
  TEXTMETRICW tm;
  GetTextMetricsW(tdc, &tm);
  SelectObject(tdc, old);
  ReleaseDC(hwnd_, tdc);
  const int eh = tm.tmHeight + s(2);
  auto place = [&](HWND e, RECT f, bool visible) {
    if (!visible || f.right - f.left < s(20)) {
      ShowWindow(e, SW_HIDE);
      return;
    }
    int y = f.top + (f.bottom - f.top - eh) / 2;
    SetWindowPos(e, nullptr, f.left + s(2), y, f.right - f.left - s(4), eh, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  };
  place(page_edit_, edit_frame(PageBox), true);
  place(zoom_edit_, edit_frame(ZoomBox), true);
  place(search_edit_, edit_frame(SearchBox), search_w > 0);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

int Toolbar::hit(POINT pt) const {
  for (int i = 0; i < (int)items_.size(); ++i) {
    if (items_[i].kind == Button && PtInRect(&items_[i].rc, pt)) return i;
  }
  return -1;
}

void Toolbar::draw_fit_width(HDC dc, const RECT& rc, COLORREF color) {
  // |<->| : two bars and a double-headed arrow between them.
  int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
  int half = s(8), bar = s(6), head = s(3);
  int pw = std::max(1, s(1));
  HPEN pen = CreatePen(PS_SOLID, pw, color);
  HGDIOBJ old = SelectObject(dc, pen);
  MoveToEx(dc, cx - half, cy - bar, nullptr);
  LineTo(dc, cx - half, cy + bar + 1);
  MoveToEx(dc, cx + half, cy - bar, nullptr);
  LineTo(dc, cx + half, cy + bar + 1);
  MoveToEx(dc, cx - half + s(3), cy, nullptr);
  LineTo(dc, cx + half - s(2), cy);
  MoveToEx(dc, cx - half + s(3) + head, cy - head, nullptr);
  LineTo(dc, cx - half + s(3), cy);
  LineTo(dc, cx - half + s(3) + head + 1, cy + head + 1);
  MoveToEx(dc, cx + half - s(3) - head, cy - head, nullptr);
  LineTo(dc, cx + half - s(3), cy);
  LineTo(dc, cx + half - s(3) - head - 1, cy + head + 1);
  SelectObject(dc, old);
  DeleteObject(pen);
}

void Toolbar::paint(HDC target) {
  RECT cr;
  GetClientRect(hwnd_, &cr);
  HDC dc = CreateCompatibleDC(target);
  HBITMAP bmp = CreateCompatibleBitmap(target, std::max(1L, cr.right), std::max(1L, cr.bottom));
  HGDIOBJ old_bmp = SelectObject(dc, bmp);
  const Theme& t = theme();
  fill(dc, cr, t.bar_bg);
  RECT line = {0, cr.bottom - 1, cr.right, cr.bottom};
  fill(dc, line, t.bar_border);
  SetBkMode(dc, TRANSPARENT);

  for (int i = 0; i < (int)items_.size(); ++i) {
    const Item& it = items_[i];
    if (IsRectEmpty(&it.rc)) continue;
    switch (it.kind) {
      case Button: {
        COLORREF bg = 0;
        bool draw_bg = true;
        if (it.enabled && i == pressed_ && i == hot_) bg = t.bar_pressed;
        else if (it.enabled && i == hot_) bg = t.bar_hover;
        else if (it.checked) bg = t.bar_checked;
        else draw_bg = false;
        if (draw_bg) {
          SetDCBrushColor(dc, bg);
          SetDCPenColor(dc, bg);
          HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
          HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
          RoundRect(dc, it.rc.left, it.rc.top, it.rc.right, it.rc.bottom, s(6), s(6));
          SelectObject(dc, ob);
          SelectObject(dc, op);
        }
        COLORREF fg = it.enabled ? t.bar_fg : t.bar_fg_disabled;
        if (it.glyph == kGlyphFitWidth) {
          draw_fit_width(dc, it.rc, fg);
        } else {
          SelectObject(dc, icon_font_);
          SetTextColor(dc, fg);
          RECT r = it.rc;
          DrawTextW(dc, &it.glyph, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        break;
      }
      case Separator: {
        int x = (it.rc.left + it.rc.right) / 2;
        RECT r = {x, it.rc.top + s(6), x + 1, it.rc.bottom - s(6)};
        fill(dc, r, t.bar_border);
        break;
      }
      case PageBox:
      case ZoomBox:
      case SearchBox: {
        RECT f = edit_frame(it.kind);
        fill(dc, f, t.edit_bg);
        frame(dc, f, t.edit_border);
        if (it.kind == PageBox) {
          SelectObject(dc, ui_font_);
          SetTextColor(dc, t.bar_fg);
          RECT r = {f.right + s(6), it.rc.top, it.rc.right, it.rc.bottom};
          DrawTextW(dc, page_label_.c_str(), (int)page_label_.size(), &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        break;
      }
      default:
        break;
    }
  }
  BitBlt(target, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
  SelectObject(dc, old_bmp);
  DeleteObject(bmp);
  DeleteDC(dc);
}

LRESULT CALLBACK Toolbar::edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
  auto* self = (Toolbar*)ref;
  switch (msg) {
    case WM_KEYDOWN:
      if (wp == VK_RETURN) {
        std::wstring text = window_text(hwnd);
        if (hwnd == self->page_edit_) self->host_->on_page_entered(text);
        else if (hwnd == self->zoom_edit_) self->host_->on_zoom_entered(text);
        else self->host_->on_search(text, GetKeyState(VK_SHIFT) >= 0);
        return 0;
      }
      if (wp == VK_ESCAPE) {
        self->host_->on_edit_escape();
        return 0;
      }
      break;
    case WM_CHAR:
      if (wp == L'\r' || wp == 27) return 0;  // no beep
      if (wp == 1 && GetKeyState(VK_CONTROL) < 0) {  // Ctrl+A selects all text
        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        return 0;
      }
      break;
    case WM_SETFOCUS:
      if (hwnd != self->search_edit_) PostMessageW(hwnd, EM_SETSEL, 0, -1);
      break;
    case WM_KILLFOCUS:
      // Restore the displayed value if the user abandoned editing (once the focus has moved).
      if (hwnd == self->page_edit_ || hwnd == self->zoom_edit_) PostMessageW(self->hwnd_, kMsgRefresh, 0, 0);
      break;
  }
  return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK Toolbar::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  Toolbar* self;
  if (msg == WM_NCCREATE) {
    self = (Toolbar*)((CREATESTRUCTW*)lp)->lpCreateParams;
    self->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
  } else {
    self = (Toolbar*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  }
  return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Toolbar::handle(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SIZE:
      layout();
      return 0;
    case kMsgRefresh:
      host_->execute(CMD_NONE);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd_, &ps);
      paint(dc);
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_CTLCOLOREDIT: {
      HDC dc = (HDC)wp;
      SetTextColor(dc, theme().edit_fg);
      SetBkColor(dc, theme().edit_bg);
      return (LRESULT)edit_brush_;
    }
    case WM_MOUSEMOVE: {
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      int h = hit(pt);
      if (h != hot_) {
        if (hot_ >= 0) InvalidateRect(hwnd_, &items_[hot_].rc, FALSE);
        hot_ = h;
        if (hot_ >= 0) InvalidateRect(hwnd_, &items_[hot_].rc, FALSE);
      }
      if (!tracking_) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tme);
        tracking_ = true;
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      tracking_ = false;
      if (hot_ >= 0) InvalidateRect(hwnd_, &items_[hot_].rc, FALSE);
      hot_ = -1;
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      pressed_ = hit(pt);
      if (pressed_ >= 0) {
        SetCapture(hwnd_);
        InvalidateRect(hwnd_, &items_[pressed_].rc, FALSE);
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      int p = pressed_;
      pressed_ = -1;
      if (GetCapture() == hwnd_) ReleaseCapture();
      if (p >= 0) {
        InvalidateRect(hwnd_, &items_[p].rc, FALSE);
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (hit(pt) == p && items_[p].enabled) host_->execute(items_[p].cmd);
      }
      return 0;
    }
    case WM_DESTROY:
      if (ui_font_) DeleteObject(ui_font_);
      if (icon_font_) DeleteObject(icon_font_);
      if (edit_brush_) DeleteObject(edit_brush_);
      ui_font_ = icon_font_ = nullptr;
      edit_brush_ = nullptr;
      return 0;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace lv
