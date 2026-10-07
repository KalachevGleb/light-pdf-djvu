// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "canvas.h"

#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <climits>
#include <cmath>

#include "theme.h"
#include "util.h"

namespace lv {

namespace {

constexpr wchar_t kClass[] = L"LightView.Canvas";
constexpr UINT kMsgTile = WM_APP + 1;
constexpr UINT kMsgSearchDone = WM_APP + 2;
constexpr UINT kMsgSearchProgress = WM_APP + 3;
constexpr UINT_PTR kTimerToast = 1;
constexpr UINT_PTR kTimerAutoScroll = 2;
constexpr int kMargin = 10;     // around the document, at 96 dpi
constexpr int kGap = 10;        // between pages
constexpr int kLine = 48;       // arrow-key scroll step
constexpr int kWheelLine = 36;  // one wheel "line"
constexpr float kMinZoom = 0.05f, kMaxZoom = 64.f;
constexpr float kZoomLevels[] = {0.1f, 0.125f, 0.25f, 0.333f, 0.5f, 0.667f, 0.75f, 1.f,  1.25f, 1.5f, 2.f,
                                 3.f,  4.f,    6.f,   8.f,    12.f, 16.f,   24.f,  32.f, 48.f,  64.f};

inline uint32_t to_px(COLORREF c) { return ((uint32_t)GetRValue(c) << 16) | ((uint32_t)GetGValue(c) << 8) | GetBValue(c); }

bool text_before(int pa, int ia, int pb, int ib) { return pa < pb || (pa == pb && ia < ib); }

bool starts_with_ci(const std::wstring& s, const wchar_t* prefix) {
  size_t n = wcslen(prefix);
  return s.size() >= n && CompareStringOrdinal(s.c_str(), (int)n, prefix, (int)n, TRUE) == CSTR_EQUAL;
}

}  // namespace

void Canvas::register_class(HINSTANCE inst) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = inst;
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);
}

bool Canvas::create(HWND parent, CanvasHost* host, int dpi) {
  host_ = host;
  dpi_ = dpi;
  HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
  hwnd_ = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_CLIPSIBLINGS, 0, 0,
                          0, 0, parent, nullptr, inst, this);
  if (!hwnd_) return false;
  renderer_ = std::make_unique<Renderer>(hwnd_, kMsgTile);
  apply_dark_controls(hwnd_);
  make_fonts();
  update_scrollbars();
  return true;
}

Canvas::~Canvas() {
  stop_search();
  renderer_.reset();
  if (mem_dc_) {
    SelectObject(mem_dc_, mem_old_);
    DeleteDC(mem_dc_);
    DeleteObject(mem_bmp_);
  }
  if (ui_font_) DeleteObject(ui_font_);
  if (hint_font_) DeleteObject(hint_font_);
}

void Canvas::make_fonts() {
  if (ui_font_) DeleteObject(ui_font_);
  if (hint_font_) DeleteObject(hint_font_);
  NONCLIENTMETRICSW ncm{sizeof(ncm)};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_);
  ui_font_ = CreateFontIndirectW(&ncm.lfMessageFont);
  ncm.lfMessageFont.lfHeight = ncm.lfMessageFont.lfHeight * 3 / 2;
  hint_font_ = CreateFontIndirectW(&ncm.lfMessageFont);
}

void Canvas::set_dpi(int dpi) {
  Anchor a = anchor_at(view_w_ / 2, 0);
  dpi_ = dpi;
  make_fonts();
  layout();
  restore_anchor(a);
  update_scrollbars();
  changed();
}

// ---------------------------------------------------------------------------------------------
// Document and layout

void Canvas::set_document(std::shared_ptr<Document> doc, const ViewState* view, bool reload) {
  stop_search();
  match_page_ = -1;
  match_cache_.clear();
  if (!reload) {
    query_.clear();
    has_sel_ = false;
    back_.clear();
    fwd_.clear();
  } else if (has_sel_ && doc && std::max(sel_a_.page, sel_b_.page) >= doc->page_count()) {
    has_sel_ = false;
  }
  doc_ = std::move(doc);
  renderer_->set_document(doc_, reload);
  hover_text_.clear();

  if (doc_) {
    const int n = doc_->page_count();
    const int step = std::max(1, n / 2000);
    std::vector<float> ws, hs;
    for (int i = 0; i < n; i += step) {
      ws.push_back(doc_->page_size(i).w);
      hs.push_back(doc_->page_size(i).h);
    }
    std::nth_element(ws.begin(), ws.begin() + ws.size() / 2, ws.end());
    std::nth_element(hs.begin(), hs.begin() + hs.size() / 2, hs.end());
    med_w_ = ws[ws.size() / 2];
    med_h_ = hs[hs.size() / 2];
  }
  if (view) {
    mode_ = view->mode;
    zoom_ = std::clamp(view->zoom, kMinZoom, kMaxZoom);
    rot_ = view->rot & 3;
    facing_ = view->facing;
  } else if (!reload) {
    mode_ = ZoomMode::FitWidth;
    rot_ = 0;
    facing_ = false;
  }
  layout();
  // Horizontally centered: with a few wider pages (fold-outs) the usual ones stay fully visible.
  scroll_x_ = (doc_w_ - view_w_) / 2;
  scroll_y_ = 0;
  if (doc_ && view) {
    Pos p{std::clamp(view->page, 0, doc_->page_count() - 1), view->page_y};
    scroll_y_ = pages_[p.page].y + (int)lroundf(p.y * scale_);
  }
  clamp_scroll();
  update_scrollbars();
  changed();
}

SizeF Canvas::rotated_size(int page) const {
  SizeF sz = doc_->page_size(page);
  return (rot_ & 1) ? SizeF{sz.h, sz.w} : sz;
}

void Canvas::compute_scale() {
  if (mode_ != ZoomMode::Custom && view_w_ > 0 && view_h_ > 0) {
    float w = (rot_ & 1) ? med_h_ : med_w_;
    float h = (rot_ & 1) ? med_w_ : med_h_;
    float avail_w = (float)(view_w_ - 2 * s(kMargin) - (facing_ ? s(kGap) : 0));
    float sc = avail_w / (facing_ ? 2 * w : w);
    if (mode_ == ZoomMode::FitPage) sc = std::min(sc, (float)(view_h_ - 2 * s(kMargin)) / h);
    zoom_ = std::clamp(sc * 72.f / dpi_, kMinZoom, kMaxZoom);
  }
  scale_ = zoom_ * dpi_ / 72.f;
}

void Canvas::layout() {
  rows_.clear();
  pages_.clear();
  row_of_page_.clear();
  doc_w_ = doc_h_ = 0;
  if (!doc_) return;
  compute_scale();
  const int n = doc_->page_count(), margin = s(kMargin), gap = s(kGap);
  pages_.resize(n);
  row_of_page_.resize(n);
  int y = margin, max_w = 0;
  for (int i = 0; i < n;) {
    Row r;
    r.y = y;
    r.first = i;
    r.count = (facing_ && i > 0 && i + 1 < n) ? 2 : 1;  // the first page is a cover
    for (int k = 0; k < r.count; ++k) {
      SizeF sz = rotated_size(i + k);
      PageBox& b = pages_[i + k];
      b.w = std::max(1, (int)lroundf(sz.w * scale_));
      b.h = std::max(1, (int)lroundf(sz.h * scale_));
      r.h = std::max(r.h, b.h);
      row_of_page_[i + k] = (int)rows_.size();
    }
    int row_w = r.count == 2 ? 2 * std::max(pages_[i].w, pages_[i + 1].w) + gap : pages_[i].w;
    max_w = std::max(max_w, row_w);
    rows_.push_back(r);
    y += r.h + gap;
    i += r.count;
  }
  doc_h_ = y - gap + margin;
  doc_w_ = max_w + 2 * margin;
  const int center = doc_w_ / 2;
  for (const Row& r : rows_) {
    PageBox& a = pages_[r.first];
    if (r.count == 2) {
      PageBox& b = pages_[r.first + 1];
      a.x = center - gap / 2 - a.w;
      b.x = center + (gap - gap / 2);
      b.y = r.y + (r.h - b.h) / 2;
    } else {
      a.x = (doc_w_ - a.w) / 2;
    }
    a.y = r.y + (r.h - a.h) / 2;
  }
  clamp_scroll();
}

void Canvas::clamp_scroll() {
  scroll_x_ = std::clamp(scroll_x_, 0, std::max(0, doc_w_ - view_w_));
  scroll_y_ = std::clamp(scroll_y_, 0, std::max(0, doc_h_ - view_h_));
}

void Canvas::update_scrollbars() {
  static int depth = 0;  // showing/hiding the horizontal bar resizes the window re-entrantly
  if (!hwnd_ || depth > 2) return;
  ++depth;
  SCROLLINFO si{sizeof(si)};
  si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;
  si.nMin = 0;
  si.nMax = std::max(0, doc_h_ - 1);
  si.nPage = (UINT)std::max(0, view_h_);
  si.nPos = scroll_y_;
  SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
  const bool need_h = doc_ && doc_w_ > view_w_;
  si.fMask = SIF_ALL;
  si.nMax = need_h ? doc_w_ - 1 : 0;
  si.nPage = need_h ? (UINT)view_w_ : 1;
  si.nPos = need_h ? scroll_x_ : 0;
  SetScrollInfo(hwnd_, SB_HORZ, &si, TRUE);
  --depth;
}

void Canvas::changed() {
  InvalidateRect(hwnd_, nullptr, FALSE);
  if (host_) host_->on_view_changed();
}

void Canvas::scroll_to(int x, int y) {
  int ox = scroll_x_, oy = scroll_y_;
  scroll_x_ = x;
  scroll_y_ = y;
  clamp_scroll();
  if (ox == scroll_x_ && oy == scroll_y_) return;
  SCROLLINFO si{sizeof(si), SIF_POS};
  si.nPos = scroll_y_;
  SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
  si.nPos = scroll_x_;
  SetScrollInfo(hwnd_, SB_HORZ, &si, TRUE);
  changed();
}

int Canvas::row_at(int doc_y) const {
  auto it = std::upper_bound(rows_.begin(), rows_.end(), doc_y, [](int v, const Row& r) { return v < r.y; });
  return it == rows_.begin() ? 0 : (int)(it - rows_.begin()) - 1;
}

int Canvas::current_page() const {
  if (!doc_ || rows_.empty()) return 0;
  if (scroll_y_ > 0 && scroll_y_ >= doc_h_ - view_h_) return rows_.back().first;
  return rows_[row_at(scroll_y_ - off_y() + view_h_ / 3)].first;
}

Canvas::Anchor Canvas::anchor_at(int cx, int cy) const {
  Anchor a;
  a.cx = cx;
  a.cy = cy;
  if (!doc_ || rows_.empty()) return a;
  int dx = cx + scroll_x_ - off_x(), dy = cy + scroll_y_ - off_y();
  const Row& r = rows_[row_at(dy)];
  int p = r.first;
  if (r.count == 2 && dx >= pages_[r.first + 1].x) p = r.first + 1;
  const PageBox& b = pages_[p];
  a.page = p;
  a.fx = (float)(dx - b.x) / b.w;
  a.fy = (float)(dy - b.y) / b.h;
  return a;
}

void Canvas::restore_anchor(const Anchor& a) {
  if (a.page < 0 || a.page >= (int)pages_.size()) return;
  const PageBox& b = pages_[a.page];
  scroll_x_ = b.x + (int)lroundf(a.fx * b.w) - a.cx;
  scroll_y_ = b.y + (int)lroundf(a.fy * b.h) - a.cy;
  clamp_scroll();
}

ViewState Canvas::view_state() const {
  ViewState v;
  Pos p = position();
  v.page = p.page;
  v.page_y = p.y;
  v.mode = mode_;
  v.zoom = zoom_;
  v.rot = rot_;
  v.facing = facing_;
  return v;
}

Canvas::Pos Canvas::position() const {
  if (!doc_ || rows_.empty()) return {};
  int top = scroll_y_ - off_y();
  int p = rows_[row_at(top)].first;
  return {p, (top - pages_[p].y) / scale_, (scroll_x_ - off_x() - pages_[p].x) / scale_};
}

void Canvas::go_to_pos(const Pos& p) {
  if (!doc_) return;
  int page = std::clamp(p.page, 0, doc_->page_count() - 1);
  scroll_to(pages_[page].x + (int)lroundf(p.x * scale_) + off_x(), pages_[page].y + (int)lroundf(p.y * scale_) + off_y());
}

void Canvas::push_history(const Pos& p) {
  if (!doc_) return;
  fwd_.clear();
  if (!back_.empty()) {
    const Pos& last = back_.back();
    if (last.page == p.page && fabsf(last.y - p.y) < 2 && fabsf(last.x - p.x) < 2) return;  // same spot
  }
  back_.push_back(p);
  if (back_.size() > 100) back_.erase(back_.begin());
}

void Canvas::back() {
  if (back_.empty()) return;
  fwd_.push_back(position());
  Pos p = back_.back();
  back_.pop_back();
  go_to_pos(p);
}

void Canvas::forward() {
  if (fwd_.empty()) return;
  back_.push_back(position());
  Pos p = fwd_.back();
  fwd_.pop_back();
  go_to_pos(p);
}

// ---------------------------------------------------------------------------------------------
// Navigation and zoom

void Canvas::go_to_page(int page, bool history) {
  if (!doc_) return;
  page = std::clamp(page, 0, doc_->page_count() - 1);
  if (history) push_history();
  const Row& r = rows_[row_of_page_[page]];
  int y = (mode_ == ZoomMode::FitPage && r.h <= view_h_) ? r.y + r.h / 2 - view_h_ / 2 : r.y - s(kGap) / 2;
  scroll_to(scroll_x_, y);
}

void Canvas::go_to_dest(const Dest& dest) {
  if (!doc_) return;
  Dest d = doc_->resolve_dest(dest);
  if (d.page < 0 || d.page >= doc_->page_count()) return;
  push_history();
  if (d.y < 0) {
    go_to_page(d.page, false);
    return;
  }
  float u, v;
  rotate_pt(d.page, 0, d.y, u, v);
  scroll_to(scroll_x_, pages_[d.page].y + (int)(v * scale_) - s(kLine) / 2);
}

void Canvas::page_step(int dir) {
  if (!doc_) return;
  int r = row_of_page_[current_page()];
  int target;
  if (dir < 0) {
    // In the middle of a tall page "previous" first goes to its top.
    target = rows_[r].y < scroll_y_ - off_y() - s(kGap) ? rows_[r].first : rows_[std::max(0, r - 1)].first;
  } else {
    target = rows_[std::min(r + 1, (int)rows_.size() - 1)].first;
  }
  go_to_page(target, false);
}

void Canvas::first_page() {
  if (!doc_) return;
  push_history();
  scroll_to(scroll_x_, 0);
}

void Canvas::last_page() {
  if (!doc_) return;
  push_history();
  scroll_to(scroll_x_, INT_MAX / 2);
}

void Canvas::apply_zoom(ZoomMode m, float zoom, POINT pt) {
  if (!doc_) {
    mode_ = m;
    return;
  }
  Anchor a = anchor_at(pt.x, pt.y);
  mode_ = m;
  if (m == ZoomMode::Custom) zoom_ = std::clamp(zoom, kMinZoom, kMaxZoom);
  layout();
  restore_anchor(a);
  update_scrollbars();
  changed();
}

void Canvas::set_zoom_mode(ZoomMode m) {
  if (m == ZoomMode::FitPage && doc_) {
    relayout_at_page([&] { mode_ = m; });
    return;
  }
  apply_zoom(m, zoom_, {view_w_ / 2, 0});
  if (m == ZoomMode::FitWidth) scroll_to((doc_w_ - view_w_) / 2, scroll_y_);
}

void Canvas::relayout_at_page(const std::function<void()>& change) {
  int p = current_page();
  change();
  if (!doc_) return;
  layout();
  scroll_x_ = (doc_w_ - view_w_) / 2;
  clamp_scroll();
  update_scrollbars();
  go_to_page(p, false);
  changed();
}

void Canvas::set_zoom_percent(float pct) { apply_zoom(ZoomMode::Custom, pct / 100.f, {view_w_ / 2, view_h_ / 2}); }

void Canvas::zoom_step(int dir) {
  float z = zoom_;
  float next = z;
  if (dir > 0) {
    next = kZoomLevels[std::size(kZoomLevels) - 1];
    for (float l : kZoomLevels) {
      if (l > z * 1.01f) {
        next = l;
        break;
      }
    }
  } else {
    next = kZoomLevels[0];
    for (float l : kZoomLevels) {
      if (l < z * 0.99f) next = l;
    }
  }
  apply_zoom(ZoomMode::Custom, next, {view_w_ / 2, view_h_ / 2});
}

void Canvas::rotate(int dir) {
  relayout_at_page([&] { rot_ = (rot_ + dir + 4) & 3; });
}

void Canvas::set_facing(bool on) {
  if (facing_ != on) relayout_at_page([&] { facing_ = on; });
}

void Canvas::set_invert(bool on) {
  invert_ = on;
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void Canvas::rerender() {
  if (doc_) renderer_->set_document(doc_, true);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void Canvas::show_toast(const std::wstring& text, int ms) {
  toast_ = text;
  SetTimer(hwnd_, kTimerToast, ms, nullptr);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------------------------
// Coordinates, text and links

// Page space (points, document orientation) -> rotated page space (points, view orientation).
void Canvas::rotate_pt(int page, float x, float y, float& u, float& v) const {
  SizeF sz = doc_->page_size(page);
  switch (rot_) {
    case 1: u = sz.h - y; v = x; break;
    case 2: u = sz.w - x; v = sz.h - y; break;
    case 3: u = y; v = sz.w - x; break;
    default: u = x; v = y; break;
  }
}

void Canvas::unrotate_pt(int page, float u, float v, float& x, float& y) const {
  SizeF sz = doc_->page_size(page);
  switch (rot_) {
    case 1: x = v; y = sz.h - u; break;
    case 2: x = sz.w - u; y = sz.h - v; break;
    case 3: x = sz.w - v; y = u; break;
    default: x = u; y = v; break;
  }
}

RECT Canvas::to_client(int page, const RectF& r) const {
  float u0, v0, u1, v1;
  rotate_pt(page, r.x0, r.y0, u0, v0);
  rotate_pt(page, r.x1, r.y1, u1, v1);
  const PageBox& b = pages_[page];
  int bx = b.x - scroll_x_ + off_x(), by = b.y - scroll_y_ + off_y();
  return {bx + (int)floorf(std::min(u0, u1) * scale_), by + (int)floorf(std::min(v0, v1) * scale_),
          bx + (int)ceilf(std::max(u0, u1) * scale_), by + (int)ceilf(std::max(v0, v1) * scale_)};
}

bool Canvas::hit_page(int cx, int cy, bool nearest, int& page, float& x, float& y) const {
  if (!doc_ || rows_.empty()) return false;
  int dx = cx + scroll_x_ - off_x(), dy = cy + scroll_y_ - off_y();
  int ri = row_at(dy);
  if (nearest && ri + 1 < (int)rows_.size()) {
    const Row& r = rows_[ri];
    if (dy > r.y + r.h && dy - (r.y + r.h) > rows_[ri + 1].y - dy) ++ri;
  }
  const Row& r = rows_[ri];
  int best = -1, best_d = INT_MAX;
  for (int k = 0; k < r.count; ++k) {
    const PageBox& b = pages_[r.first + k];
    int ddx = dx < b.x ? b.x - dx : (dx >= b.x + b.w ? dx - (b.x + b.w - 1) : 0);
    int ddy = dy < b.y ? b.y - dy : (dy >= b.y + b.h ? dy - (b.y + b.h - 1) : 0);
    if (ddx + ddy < best_d) {
      best_d = ddx + ddy;
      best = r.first + k;
    }
  }
  if (best < 0 || (!nearest && best_d > 0)) return false;
  const PageBox& b = pages_[best];
  float u = (dx - b.x) / scale_, v = (dy - b.y) / scale_;
  if (nearest) {
    SizeF rs = rotated_size(best);
    u = std::clamp(u, 0.f, rs.w);
    v = std::clamp(v, 0.f, rs.h);
  }
  unrotate_pt(best, u, v, x, y);
  page = best;
  return true;
}

int Canvas::char_at(const PageText& t, float x, float y, bool nearest) {
  int best = -1;
  float best_d = FLT_MAX;
  for (int i = 0; i < (int)t.boxes.size(); ++i) {
    const RectF& b = t.boxes[i];
    if (b.empty()) continue;
    float dx = std::max({b.x0 - x, 0.f, x - b.x1});
    float dy = std::max({b.y0 - y, 0.f, y - b.y1});
    if (dx == 0 && dy == 0) return i;
    if (!nearest && (dx > 1.5f || dy > 1.5f)) continue;
    float d = dx * dx + 4 * dy * dy;  // prefer characters on the same line
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return best;
}

void Canvas::range_rects(int page, const PageText& t, int a, int b, std::vector<RECT>& out) const {
  a = std::max(a, 0);
  b = std::min(b, (int)t.boxes.size() - 1);
  RectF cur;
  bool have = false;
  for (int i = a; i <= b; ++i) {
    const RectF& x = t.boxes[i];
    if (x.empty()) continue;
    if (have) {
      float overlap = std::min(cur.y1, x.y1) - std::max(cur.y0, x.y0);
      float hmin = std::min(cur.y1 - cur.y0, x.y1 - x.y0);
      if (overlap > 0.5f * hmin && x.x0 >= cur.x0 - 1.f) {
        cur.x0 = std::min(cur.x0, x.x0);
        cur.x1 = std::max(cur.x1, x.x1);
        cur.y0 = std::min(cur.y0, x.y0);
        cur.y1 = std::max(cur.y1, x.y1);
        continue;
      }
      out.push_back(to_client(page, cur));
    }
    cur = x;
    have = true;
  }
  if (have) out.push_back(to_client(page, cur));
}

std::wstring Canvas::selected_text() {
  if (!has_sel_ || !doc_) return {};
  TextPos a = sel_a_, b = sel_b_;
  if (text_before(b.page, b.idx, a.page, a.idx)) std::swap(a, b);
  std::wstring out;
  for (int p = a.page; p <= b.page; ++p) {
    auto t = doc_->text(p);
    if (!t || t->text.empty()) continue;
    int n = (int)t->text.size();
    int from = p == a.page ? std::clamp(a.idx, 0, n - 1) : 0;
    int to = p == b.page ? std::clamp(b.idx, 0, n - 1) : n - 1;
    if (from > to) continue;
    if (!out.empty() && out.back() != L'\n') out += L"\r\n";
    for (int i = from; i <= to; ++i) {
      wchar_t c = t->text[i];
      if (c == 0xAD || (c < 0x20 && c != L'\r' && c != L'\n' && c != L'\t')) continue;  // soft hyphens, controls
      out.push_back(c);
    }
  }
  return out;
}

void Canvas::copy() {
  std::wstring text = selected_text();
  if (text.empty()) return;
  if (!OpenClipboard(hwnd_)) return;
  EmptyClipboard();
  size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    memcpy(GlobalLock(mem), text.c_str(), bytes);
    GlobalUnlock(mem);
    if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
  }
  CloseClipboard();
  show_toast(L"Скопировано", 1000);
}

void Canvas::select_all() {
  if (!doc_) return;
  sel_a_ = {0, 0};
  sel_b_ = {doc_->page_count() - 1, INT_MAX};
  has_sel_ = true;
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void Canvas::select_word(int page, int idx) {
  auto t = doc_->text(page);
  if (!t || idx < 0 || idx >= (int)t->text.size()) return;
  auto word = [&](int i) { return IsCharAlphaNumericW(t->text[i]) || t->text[i] == L'_'; };
  int a = idx, b = idx;
  if (word(idx)) {
    while (a > 0 && word(a - 1)) --a;
    while (b + 1 < (int)t->text.size() && word(b + 1)) ++b;
  }
  sel_a_ = {page, a};
  sel_b_ = {page, b};
  has_sel_ = true;
  InvalidateRect(hwnd_, nullptr, FALSE);
}

const Link* Canvas::link_at(int page, float x, float y, bool wait) {
  auto links = doc_->links(page, wait);
  if (!links) return nullptr;
  for (const Link& l : *links) {
    if (l.rect.contains(x, y)) return &l;  // the document's cache keeps the vector alive
  }
  return nullptr;
}

void Canvas::follow_link(const Link& l) {
  if (l.uri.empty()) {
    go_to_dest(l.dest);
    return;
  }
  std::wstring u = l.uri;
  if (starts_with_ci(u, L"www.")) u = L"http://" + u;
  bool web = starts_with_ci(u, L"http://") || starts_with_ci(u, L"https://") || starts_with_ci(u, L"mailto:") ||
             starts_with_ci(u, L"ftp://");
  if (!web) {
    // Local files and other schemes could launch programs: ask first.
    if (u.find(L':') == std::wstring::npos && !starts_with_ci(u, L"\\\\")) u = dir_of(doc_->path()) + L"\\" + u;
    std::wstring q = L"Документ ссылается на:\n" + u + L"\n\nОткрыть?";
    if (MessageBoxW(GetAncestor(hwnd_, GA_ROOT), q.c_str(), L"LightView", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
  }
  ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Canvas::update_hover(POINT pt) {
  Cursor c = Cursor::Arrow;
  std::wstring text;
  int page;
  float x, y;
  if (doc_ && hit_page(pt.x, pt.y, false, page, x, y)) {
    if (const Link* l = link_at(page, x, y, false)) {
      c = Cursor::Hand;
      text = l->uri.empty() ? fmt(L"Перейти на страницу %d", l->dest.page + 1) : l->uri;
    } else if (auto t = doc_->text(page, false); t && char_at(*t, x, y, false) >= 0) {
      c = Cursor::IBeam;
    }
  }
  if (c != cursor_) {
    cursor_ = c;
    SendMessageW(hwnd_, WM_SETCURSOR, (WPARAM)hwnd_, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
  }
  if (text != hover_text_) {
    hover_text_ = text;
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

void Canvas::update_selection_drag(POINT pt) {
  int page;
  float x, y;
  if (!hit_page(pt.x, pt.y, true, page, x, y)) return;
  auto t = doc_->text(page);
  if (!t || t->text.empty()) return;
  int i = char_at(*t, x, y, true);
  if (i >= 0 && (sel_b_.page != page || sel_b_.idx != i)) {
    sel_b_ = {page, i};
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

// ---------------------------------------------------------------------------------------------
// Search

void Canvas::stop_search() {
  search_cancel_ = true;
  if (search_thread_.joinable()) search_thread_.join();
  searching_ = false;
}

void Canvas::find(const std::wstring& raw, bool forward) {
  if (!doc_) return;
  std::wstring q = normalize_query(raw);
  if (q.empty()) {  // an emptied search box clears the highlights
    stop_search();
    query_.clear();
    match_page_ = -1;
    match_cache_.clear();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return;
  }
  int start_page, start_pos;
  if (q == query_ && match_page_ >= 0) {
    start_page = match_page_;
    start_pos = forward ? match_pos_ + 1 : match_pos_ - 1;
  } else {
    start_page = current_page();
    start_pos = forward ? 0 : INT_MAX;
  }
  if (q != query_) {
    query_ = q;
    match_cache_.clear();
    match_page_ = -1;
  }
  stop_search();
  search_cancel_ = false;
  searching_ = true;
  const uint32_t id = ++search_id_;
  std::shared_ptr<Document> doc = doc_;
  HWND hwnd = hwnd_;
  search_thread_ = std::thread([this, doc, q, start_page, start_pos, forward, id, hwnd] {
    const int n = doc->page_count();
    for (int k = 0; k <= n; ++k) {
      if (search_cancel_) return;
      int p = forward ? (start_page + k) % n : ((start_page - k) % n + n) % n;
      auto t = doc->text(p);
      if (!t) continue;
      const std::wstring& s = t->norm;
      size_t pos = std::wstring::npos;
      if (forward) {
        // k == n revisits the start page: only the part before the start position.
        size_t from = k == 0 ? (size_t)std::max(start_pos, 0) : 0;
        if (from <= s.size()) pos = s.find(q, from);
        if (k == n && pos != std::wstring::npos && (int)pos >= start_pos) pos = std::wstring::npos;
      } else {
        if (k > 0 || start_pos >= 0) pos = s.rfind(q, k == 0 ? (size_t)start_pos : std::wstring::npos);
        if (k == n && pos != std::wstring::npos && (int)pos <= start_pos) pos = std::wstring::npos;
      }
      if (pos != std::wstring::npos) {
        PostMessageW(hwnd, kMsgSearchDone, id, ((LPARAM)(p + 1) << 32) | (LPARAM)(uint32_t)pos);
        return;
      }
      if (k % 20 == 19) PostMessageW(hwnd, kMsgSearchProgress, id, p);
    }
    PostMessageW(hwnd, kMsgSearchDone, id, 0);
  });
}

const std::vector<int>& Canvas::page_matches(int page, const PageText& t) {
  auto it = match_cache_.find(page);
  if (it != match_cache_.end()) return it->second;
  std::vector<int> m;
  for (size_t pos = t.norm.find(query_); pos != std::wstring::npos; pos = t.norm.find(query_, pos + 1)) {
    m.push_back((int)pos);
    if (m.size() > 2000) break;
  }
  return match_cache_.emplace(page, std::move(m)).first->second;
}

void Canvas::scroll_to_match() {
  auto t = doc_->text(match_page_);
  if (!t || match_pos_ < 0 || match_pos_ >= (int)t->norm_map.size()) return;
  int last = std::min(match_pos_ + (int)query_.size() - 1, (int)t->norm_map.size() - 1);
  std::vector<RECT> rs;
  range_rects(match_page_, *t, t->norm_map[match_pos_], t->norm_map[last], rs);
  if (rs.empty()) {
    go_to_page(match_page_, false);
    return;
  }
  RECT u = rs[0];
  for (auto& r : rs) UnionRect(&u, &u, &r);
  int dx = 0, dy = 0;
  if (u.top < 0 || u.bottom > view_h_) dy = u.top - view_h_ / 3;
  if (u.left < 0 || u.right > view_w_) dx = (u.left + u.right) / 2 - view_w_ / 2;
  if (dx || dy) scroll_by(dx, dy);
}

// ---------------------------------------------------------------------------------------------
// Painting

void Canvas::ensure_buffer() {
  int w = std::max(1, view_w_), h = std::max(1, view_h_);
  if (mem_dc_ && buf_w_ == w && buf_h_ == h) return;
  if (mem_dc_) {
    SelectObject(mem_dc_, mem_old_);
    DeleteDC(mem_dc_);
    DeleteObject(mem_bmp_);
  }
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  mem_bmp_ = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)&bits_, nullptr, 0);
  mem_dc_ = CreateCompatibleDC(nullptr);
  mem_old_ = SelectObject(mem_dc_, mem_bmp_);
  buf_w_ = w;
  buf_h_ = h;
}

void Canvas::fill_px(RECT r, COLORREF c) {
  r.left = std::max(r.left, 0L);
  r.top = std::max(r.top, 0L);
  r.right = std::min(r.right, (LONG)buf_w_);
  r.bottom = std::min(r.bottom, (LONG)buf_h_);
  const uint32_t v = to_px(c);
  for (int y = r.top; y < r.bottom; ++y) std::fill_n(bits_ + (size_t)y * buf_w_ + r.left, std::max(0L, r.right - r.left), v);
}

void Canvas::blend_px(RECT r, COLORREF c, int alpha) {
  r.left = std::max(r.left, 0L);
  r.top = std::max(r.top, 0L);
  r.right = std::min(r.right, (LONG)buf_w_);
  r.bottom = std::min(r.bottom, (LONG)buf_h_);
  const uint32_t cr = GetRValue(c) * alpha, cg = GetGValue(c) * alpha, cb = GetBValue(c) * alpha;
  const uint32_t ia = 255 - alpha;
  for (int y = r.top; y < r.bottom; ++y) {
    uint32_t* p = bits_ + (size_t)y * buf_w_;
    for (int x = r.left; x < r.right; ++x) {
      uint32_t d = p[x];
      uint32_t rr = (((d >> 16) & 0xFF) * ia + cr) / 255;
      uint32_t gg = (((d >> 8) & 0xFF) * ia + cg) / 255;
      uint32_t bb = ((d & 0xFF) * ia + cb) / 255;
      p[x] = (rr << 16) | (gg << 8) | bb;
    }
  }
}

void Canvas::invert_px(RECT r) {
  // Night mode: white paper becomes dark gray, black ink becomes light gray.
  static const auto lut = [] {
    std::array<uint8_t, 256> t{};
    for (int v = 0; v < 256; ++v) t[v] = (uint8_t)(0x1C + (255 - v) * (0xD8 - 0x1C) / 255);
    return t;
  }();
  r.left = std::max(r.left, 0L);
  r.top = std::max(r.top, 0L);
  r.right = std::min(r.right, (LONG)buf_w_);
  r.bottom = std::min(r.bottom, (LONG)buf_h_);
  for (int y = r.top; y < r.bottom; ++y) {
    uint32_t* p = bits_ + (size_t)y * buf_w_;
    for (int x = r.left; x < r.right; ++x) {
      uint32_t d = p[x];
      p[x] = ((uint32_t)lut[(d >> 16) & 0xFF] << 16) | ((uint32_t)lut[(d >> 8) & 0xFF] << 8) | lut[d & 0xFF];
    }
  }
}

void Canvas::add_tile_requests(int page, const RECT& rc, const RECT& area, std::vector<TileRequest>& reqs) {
  RECT is;
  if (!IntersectRect(&is, &rc, &area)) return;
  const int T = Renderer::kTileSize;
  const int fw = rc.right - rc.left, fh = rc.bottom - rc.top;
  const int x0 = (is.left - rc.left) / T, x1 = (is.right - 1 - rc.left) / T;
  const int y0 = (is.top - rc.top) / T, y1 = (is.bottom - 1 - rc.top) / T;
  for (int ty = y0; ty <= y1; ++ty) {
    for (int tx = x0; tx <= x1; ++tx) {
      TileRequest r;
      r.key = {page, rot_, fw, tx, ty};
      r.full_h = fh;
      r.x = tx * T;
      r.y = ty * T;
      r.w = std::min(T, fw - r.x);
      r.h = std::min(T, fh - r.y);
      reqs.push_back(r);
    }
  }
}

void Canvas::draw_page(int page, const RECT& rc, std::vector<TileRequest>& reqs,
                       std::vector<std::shared_ptr<const Tile>>& tmp) {
  const RECT all = {0, 0, buf_w_, buf_h_};
  RECT vis;
  IntersectRect(&vis, &rc, &all);
  fill_px({rc.left - 1, rc.top - 1, rc.right + 1, rc.bottom + 1}, theme().page_border);
  fill_px(rc, RGB(255, 255, 255));
  const int fw = rc.right - rc.left;

  // Placeholders: tiles of this page at other zoom levels, scaled. Closest scale is drawn last.
  tmp.clear();
  renderer_->page_tiles(page, rot_, tmp);
  std::sort(tmp.begin(), tmp.end(), [&](auto& a, auto& b) { return abs(a->key.full_w - fw) > abs(b->key.full_w - fw); });
  bool clipped = false;
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  for (auto& t : tmp) {
    if (t->key.full_w == fw) continue;
    double k = (double)fw / t->key.full_w;
    RECT d = {rc.left + (LONG)floor(t->x * k), rc.top + (LONG)floor(t->y * k), rc.left + (LONG)ceil((t->x + t->w) * k),
              rc.top + (LONG)ceil((t->y + t->h) * k)};
    RECT is;
    if (!IntersectRect(&is, &d, &vis)) continue;
    if (!clipped) {
      SaveDC(mem_dc_);
      IntersectClipRect(mem_dc_, vis.left, vis.top, vis.right, vis.bottom);
      clipped = true;
    }
    SetStretchBltMode(mem_dc_, k < 1 ? HALFTONE : COLORONCOLOR);
    SetBrushOrgEx(mem_dc_, 0, 0, nullptr);
    bi.bmiHeader.biWidth = t->w;
    bi.bmiHeader.biHeight = -t->h;
    StretchDIBits(mem_dc_, d.left, d.top, d.right - d.left, d.bottom - d.top, 0, 0, t->w, t->h, t->px.data(), &bi,
                  DIB_RGB_COLORS, SRCCOPY);
  }
  if (clipped) {
    RestoreDC(mem_dc_, -1);
    GdiFlush();
  }

  // Exact tiles (possibly stale after a reload); request whatever is missing.
  std::vector<TileRequest> want;
  add_tile_requests(page, rc, vis, want);
  for (const TileRequest& r : want) {
    bool fresh = false;
    if (auto t = renderer_->find(r.key, &fresh)) {
      int dx = rc.left + t->x, dy = rc.top + t->y;
      int x0 = std::max(dx, (int)vis.left), x1 = std::min(dx + t->w, (int)vis.right);
      int y0 = std::max(dy, (int)vis.top), y1 = std::min(dy + t->h, (int)vis.bottom);
      for (int y = y0; y < y1; ++y) {
        memcpy(bits_ + (size_t)y * buf_w_ + x0, t->px.data() + ((size_t)(y - dy) * t->w + (x0 - dx)) * 4,
               (size_t)std::max(0, x1 - x0) * 4);
      }
    }
    if (!fresh) reqs.push_back(r);
  }
}

void Canvas::draw_bubble(const std::wstring& text, bool bottom_left) {
  const Theme& th = theme();
  HGDIOBJ old_font = SelectObject(mem_dc_, ui_font_);
  RECT tr = {0, 0, std::max(1, view_w_ - s(48)), 0};
  DrawTextW(mem_dc_, text.c_str(), (int)text.size(), &tr, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
  tr.right = std::min(tr.right, (LONG)std::max(1, view_w_ - s(48)));
  int w = tr.right + s(24), h = tr.bottom + s(12);
  int x = bottom_left ? s(8) : (view_w_ - w) / 2;
  int y = view_h_ - h - (bottom_left ? s(8) : s(24));
  SetDCBrushColor(mem_dc_, th.toast_bg);
  SetDCPenColor(mem_dc_, th.toast_bg);
  HGDIOBJ ob = SelectObject(mem_dc_, GetStockObject(DC_BRUSH));
  HGDIOBJ op = SelectObject(mem_dc_, GetStockObject(DC_PEN));
  RoundRect(mem_dc_, x, y, x + w, y + h, s(8), s(8));
  SelectObject(mem_dc_, ob);
  SelectObject(mem_dc_, op);
  SetBkMode(mem_dc_, TRANSPARENT);
  SetTextColor(mem_dc_, th.toast_fg);
  RECT r = {x + s(12), y, x + w - s(12), y + h};
  DrawTextW(mem_dc_, text.c_str(), (int)text.size(), &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
  SelectObject(mem_dc_, old_font);
}

void Canvas::paint(HDC hdc) {
  ensure_buffer();
  const Theme& th = theme();
  const RECT all = {0, 0, buf_w_, buf_h_};
  fill_px(all, th.canvas_bg);

  if (!doc_) {
    const wchar_t* hint = L"Откройте PDF или DjVu: Ctrl+O\nили перетащите файл в это окно";
    HGDIOBJ old = SelectObject(mem_dc_, hint_font_);
    SetBkMode(mem_dc_, TRANSPARENT);
    SetTextColor(mem_dc_, th.hint_fg);
    RECT m = all;
    DrawTextW(mem_dc_, hint, -1, &m, DT_CALCRECT | DT_CENTER | DT_NOPREFIX);
    RECT r = {0, (view_h_ - m.bottom) / 2, view_w_, (view_h_ + m.bottom) / 2};
    DrawTextW(mem_dc_, hint, -1, &r, DT_CENTER | DT_NOPREFIX);
    SelectObject(mem_dc_, old);
    if (!toast_.empty()) draw_bubble(toast_, false);
    BitBlt(hdc, 0, 0, view_w_, view_h_, mem_dc_, 0, 0, SRCCOPY);
    return;
  }

  std::vector<TileRequest> reqs, pre;
  std::vector<std::shared_ptr<const Tile>> tmp;
  std::vector<int> visible;
  const int ox = off_x() - scroll_x_, oy = off_y() - scroll_y_;
  auto page_rect = [&](int p) {
    const PageBox& b = pages_[p];
    return RECT{b.x + ox, b.y + oy, b.x + ox + b.w, b.y + oy + b.h};
  };
  const int top = -oy, bottom = top + view_h_;
  for (int r = row_at(top); r < (int)rows_.size() && rows_[r].y < bottom; ++r) {
    for (int k = 0; k < rows_[r].count; ++k) {
      int p = rows_[r].first + k;
      RECT rc = page_rect(p), is;
      if (!IntersectRect(&is, &rc, &all)) continue;
      visible.push_back(p);
      draw_page(p, rc, reqs, tmp);
    }
  }
  // Render what is closest to the middle of the window first.
  auto dist = [&](const TileRequest& t) {
    const PageBox& b = pages_[t.key.page];
    int cx = b.x + ox + t.x + t.w / 2 - view_w_ / 2;
    int cy = b.y + oy + t.y + t.h / 2 - view_h_ / 2;
    return (int64_t)cx * cx + (int64_t)cy * cy;
  };
  std::stable_sort(reqs.begin(), reqs.end(), [&](auto& a, auto& b) { return dist(a) < dist(b); });

  // Prefetch: a screen below and half a screen above.
  const RECT below = {0, view_h_, view_w_, 2 * view_h_};
  const RECT above = {0, -view_h_ / 2, view_w_, 0};
  for (int r = row_at(bottom); r < (int)rows_.size() && rows_[r].y < bottom + view_h_; ++r) {
    for (int k = 0; k < rows_[r].count; ++k) add_tile_requests(rows_[r].first + k, page_rect(rows_[r].first + k), below, pre);
  }
  for (int r = row_at(std::max(0, top - view_h_ / 2)); r < (int)rows_.size() && rows_[r].y < top; ++r) {
    for (int k = 0; k < rows_[r].count; ++k) add_tile_requests(rows_[r].first + k, page_rect(rows_[r].first + k), above, pre);
  }
  reqs.insert(reqs.end(), pre.begin(), pre.end());
  renderer_->request(std::move(reqs));

  if (invert_) {
    for (int p : visible) invert_px(page_rect(p));
  }

  std::vector<RECT> rects;
  if (!query_.empty()) {
    for (int p : visible) {
      auto t = doc_->text(p, false);
      if (!t) continue;
      for (int pos : page_matches(p, *t)) {
        int last = std::min(pos + (int)query_.size() - 1, (int)t->norm_map.size() - 1);
        rects.clear();
        range_rects(p, *t, t->norm_map[pos], t->norm_map[last], rects);
        bool current = p == match_page_ && pos == match_pos_;
        for (auto& r : rects) {
          RECT rr = {r.left - 1, r.top - 1, r.right + 1, r.bottom + 1};
          blend_px(rr, current ? RGB(255, 120, 0) : RGB(255, 210, 0), current ? 140 : 100);
        }
      }
    }
  }
  if (has_sel_) {
    TextPos a = sel_a_, b = sel_b_;
    if (text_before(b.page, b.idx, a.page, a.idx)) std::swap(a, b);
    for (int p : visible) {
      if (p < a.page || p > b.page) continue;
      auto t = doc_->text(p, false);
      if (!t || t->text.empty()) continue;
      rects.clear();
      range_rects(p, *t, p == a.page ? a.idx : 0, p == b.page ? b.idx : INT_MAX, rects);
      for (auto& r : rects) blend_px(r, RGB(0, 120, 215), 90);
    }
  }

  if (!hover_text_.empty()) draw_bubble(hover_text_, true);
  if (!toast_.empty()) draw_bubble(toast_, false);
  BitBlt(hdc, 0, 0, view_w_, view_h_, mem_dc_, 0, 0, SRCCOPY);
}

// ---------------------------------------------------------------------------------------------
// Window procedure

LRESULT CALLBACK Canvas::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  Canvas* self;
  if (msg == WM_NCCREATE) {
    self = (Canvas*)((CREATESTRUCTW*)lp)->lpCreateParams;
    self->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
  } else {
    self = (Canvas*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  }
  return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Canvas::handle(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd_, &ps);
      paint(dc);
      EndPaint(hwnd_, &ps);
      return 0;
    }

    case WM_SIZE: {
      int w = LOWORD(lp), h = HIWORD(lp);
      if (w == view_w_ && h == view_h_) return 0;
      const bool had_layout = !pages_.empty() && view_w_ > 0 && view_h_ > 0;
      Anchor a = anchor_at(view_w_ / 2, 0);
      a.cx = w / 2;
      view_w_ = w;
      view_h_ = h;
      if (doc_) {
        layout();
        if (had_layout) restore_anchor(a);
      }
      size_t budget = std::clamp<size_t>((size_t)w * h * 4 * 8, 96u << 20, 512u << 20);
      if (renderer_) renderer_->set_budget(budget);
      update_scrollbars();
      changed();
      return 0;
    }

    case kMsgTile:
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;

    case kMsgSearchDone: {
      if ((uint32_t)wp != search_id_) return 0;
      searching_ = false;
      int page = (int)((uint64_t)lp >> 32) - 1;
      int pos = (int)(uint32_t)(lp & 0xFFFFFFFF);
      if (page < 0) {
        match_page_ = -1;
        show_toast(L"Не найдено");
      } else {
        match_page_ = page;
        match_pos_ = pos;
        toast_.clear();
        scroll_to_match();
      }
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    }

    case kMsgSearchProgress:
      if ((uint32_t)wp == search_id_ && searching_) show_toast(fmt(L"Поиск… страница %d", (int)lp + 1), 1500);
      return 0;

    case WM_TIMER:
      if (wp == kTimerToast) {
        KillTimer(hwnd_, kTimerToast);
        toast_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
      } else if (wp == kTimerAutoScroll && drag_ == Drag::Select && GetAsyncKeyState(VK_LBUTTON) < 0) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        int dx = pt.x < 0 ? pt.x : (pt.x > view_w_ ? pt.x - view_w_ : 0);
        int dy = pt.y < 0 ? pt.y : (pt.y > view_h_ ? pt.y - view_h_ : 0);
        if (dx || dy) {
          scroll_by(dx, dy);
          update_selection_drag(pt);
        }
      }
      return 0;

    case WM_VSCROLL:
    case WM_HSCROLL: {
      const bool vert = msg == WM_VSCROLL;
      // One scroll bar interaction (press .. release) that moved the view by more than half a screen
      // is a jump worth returning to.
      if (LOWORD(wp) == SB_ENDSCROLL) {
        if (sb_tracking_ && (abs(scroll_y_ - sb_start_y_) > view_h_ / 2 || abs(scroll_x_ - sb_start_x_) > view_w_ / 2)) {
          push_history(sb_start_);
        }
        sb_tracking_ = false;
        return 0;
      }
      if (!sb_tracking_) {
        sb_tracking_ = true;
        sb_start_ = position();
        sb_start_x_ = scroll_x_;
        sb_start_y_ = scroll_y_;
      }
      SCROLLINFO si{sizeof(si), SIF_ALL};
      GetScrollInfo(hwnd_, vert ? SB_VERT : SB_HORZ, &si);
      int pos = vert ? scroll_y_ : scroll_x_;
      const int page = vert ? view_h_ : view_w_, line = s(kLine);
      switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= line; break;
        case SB_LINEDOWN: pos += line; break;
        case SB_PAGEUP: pos -= std::max(line, page - line); break;
        case SB_PAGEDOWN: pos += std::max(line, page - line); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = INT_MAX / 2; break;
        default: return 0;
      }
      if (vert) scroll_to(scroll_x_, pos);
      else scroll_to(pos, scroll_y_);
      return 0;
    }

    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
      const int delta = GET_WHEEL_DELTA_WPARAM(wp);
      const WORD keys = GET_KEYSTATE_WPARAM(wp);
      if (msg == WM_MOUSEWHEEL && (keys & MK_CONTROL)) {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        apply_zoom(ZoomMode::Custom, zoom_ * powf(1.2f, delta / 120.f), pt);
        return 0;
      }
      UINT lines = 3;
      SystemParametersInfoW(msg == WM_MOUSEWHEEL ? SPI_GETWHEELSCROLLLINES : SPI_GETWHEELSCROLLCHARS, 0, &lines, 0);
      const bool horizontal = msg == WM_MOUSEHWHEEL || (keys & MK_SHIFT);
      float step = lines == WHEEL_PAGESCROLL ? (float)(horizontal ? view_w_ : view_h_) : (float)(lines * s(kWheelLine));
      float& rest = horizontal ? hwheel_rest_ : wheel_rest_;
      float amount = (msg == WM_MOUSEHWHEEL ? 1 : -1) * delta / 120.f * step + rest;
      int d = (int)amount;
      rest = amount - d;
      if (horizontal) scroll_by(d, 0);
      else scroll_by(0, d);
      return 0;
    }

    case WM_KEYDOWN: {
      const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
      const int line = s(kLine), pg = std::max(line, view_h_ - line);
      switch (wp) {
        case VK_UP: scroll_by(0, -line); return 0;
        case VK_DOWN: scroll_by(0, line); return 0;
        case VK_LEFT:
          if (doc_w_ > view_w_) scroll_by(-line, 0);
          else page_step(-1);
          return 0;
        case VK_RIGHT:
          if (doc_w_ > view_w_) scroll_by(line, 0);
          else page_step(1);
          return 0;
        case VK_PRIOR:
          if (mode_ == ZoomMode::FitPage) page_step(-1);
          else scroll_by(0, -pg);
          return 0;
        case VK_NEXT:
          if (mode_ == ZoomMode::FitPage) page_step(1);
          else scroll_by(0, pg);
          return 0;
        case VK_SPACE:
          if (mode_ == ZoomMode::FitPage) page_step(shift ? -1 : 1);
          else scroll_by(0, shift ? -pg : pg);
          return 0;
        case VK_HOME: first_page(); return 0;
        case VK_END: last_page(); return 0;
        case 'C':
        case VK_INSERT:
          if (ctrl) copy();
          return 0;
        case 'A':
          if (ctrl) select_all();
          return 0;
        case VK_ESCAPE:
          if (searching_) {
            stop_search();
            toast_.clear();
            InvalidateRect(hwnd_, nullptr, FALSE);
          } else if (has_sel_ || match_page_ >= 0) {
            has_sel_ = false;
            match_page_ = -1;
            query_.clear();
            match_cache_.clear();
            InvalidateRect(hwnd_, nullptr, FALSE);
          } else if (host_) {
            host_->on_canvas_escape();
          }
          return 0;
      }
      break;
    }

    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN: {
      SetFocus(hwnd_);
      if (!doc_) return 0;
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      drag_start_ = pt;
      drag_sx_ = scroll_x_;
      drag_sy_ = scroll_y_;
      drag_moved_ = false;
      SetCapture(hwnd_);
      int page;
      float x, y;
      if (msg == WM_LBUTTONDOWN) {
        if ((wp & MK_SHIFT) && has_sel_ && hit_page(pt.x, pt.y, true, page, x, y)) {
          drag_ = Drag::Select;
          drag_moved_ = true;  // a shift-click keeps the extended selection
          update_selection_drag(pt);
          SetTimer(hwnd_, kTimerAutoScroll, 30, nullptr);
          return 0;
        }
        if (hit_page(pt.x, pt.y, false, page, x, y)) {
          if (const Link* l = link_at(page, x, y, true)) {
            drag_link_ = *l;
            drag_ = Drag::Link;
            return 0;
          }
          if (auto t = doc_->text(page)) {
            int i = char_at(*t, x, y, false);
            if (i >= 0) {
              sel_a_ = sel_b_ = {page, i};
              has_sel_ = true;
              drag_ = Drag::Select;
              SetTimer(hwnd_, kTimerAutoScroll, 30, nullptr);
              InvalidateRect(hwnd_, nullptr, FALSE);
              return 0;
            }
          }
        }
        if (has_sel_) {
          has_sel_ = false;
          InvalidateRect(hwnd_, nullptr, FALSE);
        }
      }
      drag_ = Drag::Pan;
      cursor_ = Cursor::Move;
      SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
      return 0;
    }

    case WM_LBUTTONDBLCLK: {
      if (!doc_) return 0;
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      int page;
      float x, y;
      if (hit_page(pt.x, pt.y, false, page, x, y)) {
        if (auto t = doc_->text(page)) {
          int i = char_at(*t, x, y, false);
          if (i >= 0) select_word(page, i);
        }
      }
      return 0;
    }

    case WM_MOUSEMOVE: {
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      if (drag_ != Drag::None) {
        if (abs(pt.x - drag_start_.x) + abs(pt.y - drag_start_.y) > s(3)) drag_moved_ = true;
        if (drag_ == Drag::Link && drag_moved_) {
          drag_ = Drag::Pan;
          cursor_ = Cursor::Move;
          SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
        }
        if (drag_ == Drag::Pan) scroll_to(drag_sx_ - (pt.x - drag_start_.x), drag_sy_ - (pt.y - drag_start_.y));
        else if (drag_ == Drag::Select) update_selection_drag(pt);
        return 0;
      }
      TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
      TrackMouseEvent(&tme);
      update_hover(pt);
      return 0;
    }

    case WM_MOUSELEAVE:
      if (!hover_text_.empty()) {
        hover_text_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return 0;

    case WM_LBUTTONUP:
    case WM_MBUTTONUP: {
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      Drag d = drag_;
      drag_ = Drag::None;
      KillTimer(hwnd_, kTimerAutoScroll);
      if (GetCapture() == hwnd_) ReleaseCapture();
      if (d == Drag::Link && !drag_moved_) follow_link(drag_link_);
      if (d == Drag::Select && !drag_moved_) {
        has_sel_ = false;  // a plain click is not a selection
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      cursor_ = Cursor::Arrow;
      if (doc_) update_hover(pt);
      return 0;
    }

    case WM_CAPTURECHANGED:
      if ((HWND)lp != hwnd_ && drag_ != Drag::None) {
        drag_ = Drag::None;
        KillTimer(hwnd_, kTimerAutoScroll);
      }
      return 0;

    case WM_XBUTTONDOWN:
      return TRUE;  // handled on release; DefWindowProc would also emit WM_APPCOMMAND
    case WM_XBUTTONUP:
      if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) back();
      else forward();
      return TRUE;

    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        LPCWSTR id = IDC_ARROW;
        switch (cursor_) {
          case Cursor::Hand: id = IDC_HAND; break;
          case Cursor::IBeam: id = IDC_IBEAM; break;
          case Cursor::Move: id = IDC_SIZEALL; break;
          default: break;
        }
        SetCursor(LoadCursor(nullptr, id));
        return TRUE;
      }
      break;

    case WM_CONTEXTMENU: {
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      if (pt.x == -1 && pt.y == -1) {
        pt = {view_w_ / 2, view_h_ / 2};
        ClientToScreen(hwnd_, &pt);
      }
      if (host_) host_->on_context_menu(pt);
      return 0;
    }

    case WM_DESTROY:
      stop_search();
      renderer_.reset();
      return 0;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace lv
