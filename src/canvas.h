// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "document.h"
#include "renderer.h"
#include "settings.h"

namespace lv {

struct CanvasHost {
  virtual void on_view_changed() = 0;
  virtual void on_context_menu(POINT screen) = 0;
  virtual void on_canvas_escape() = 0;
};

// The document view: lays pages out in a continuous column (or two-page spreads), draws cached
// tiles, and handles scrolling, zoom, text selection, links and search.
class Canvas {
 public:
  static void register_class(HINSTANCE inst);
  bool create(HWND parent, CanvasHost* host, int dpi);
  ~Canvas();
  HWND hwnd() const { return hwnd_; }

  void set_document(std::shared_ptr<Document> doc, const ViewState* view, bool reload);
  const std::shared_ptr<Document>& doc() const { return doc_; }
  ViewState view_state() const;
  void set_dpi(int dpi);

  int current_page() const;
  float zoom_percent() const { return scale_ * 72.f / dpi_ * 100.f; }
  ZoomMode zoom_mode() const { return mode_; }
  bool facing() const { return facing_; }
  bool invert() const { return invert_; }
  bool has_selection() const { return has_sel_; }

  void go_to_page(int page, bool history = true);
  void go_to_dest(const Dest& d);
  void page_step(int dir);
  void first_page();
  void last_page();
  void back();
  void forward();
  void zoom_step(int dir);
  void set_zoom_mode(ZoomMode m);
  void set_zoom_percent(float pct);
  void rotate(int dir);
  void set_facing(bool on);
  void set_invert(bool on);
  // Re-renders everything (rendering options changed); current tiles stay as placeholders.
  void rerender();
  void copy();
  void select_all();
  void find(const std::wstring& query, bool forward);
  void show_toast(const std::wstring& text, int ms = 1800);

 private:
  struct PageBox {
    int x = 0, y = 0, w = 0, h = 0;
  };
  struct Row {
    int y = 0, h = 0, first = 0, count = 1;
  };
  struct Anchor {
    int page = -1;
    float fx = 0, fy = 0;
    int cx = 0, cy = 0;
  };
  struct TextPos {
    int page = -1, idx = -1;
  };
  // A reading position: the page at the top of the view and the view's top-left corner relative to
  // that page, in points (independent of zoom).
  struct Pos {
    int page = 0;
    float y = 0;
    float x = 0;
  };
  enum class Drag { None, Pan, Select, Link };
  enum class Cursor { Arrow, Hand, IBeam, Move };

  static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

  int s(int v) const { return MulDiv(v, dpi_, 96); }
  int off_x() const { return doc_w_ < view_w_ ? (view_w_ - doc_w_) / 2 : 0; }
  int off_y() const { return doc_h_ < view_h_ ? (view_h_ - doc_h_) / 2 : 0; }
  SizeF rotated_size(int page) const;
  void compute_scale();
  void layout();
  void clamp_scroll();
  void update_scrollbars();
  void scroll_to(int x, int y);
  void scroll_by(int dx, int dy) { scroll_to(scroll_x_ + dx, scroll_y_ + dy); }
  void changed();  // repaint + notify host
  int row_at(int doc_y) const;
  Anchor anchor_at(int cx, int cy) const;
  void restore_anchor(const Anchor& a);
  void apply_zoom(ZoomMode m, float zoom, POINT anchor);
  // Applies a layout-changing setting, keeping the current page in view and centering horizontally.
  void relayout_at_page(const std::function<void()>& change);
  Pos position() const;
  void go_to_pos(const Pos& p);
  // Remembers a spot for back(): before following a link, an outline entry, an explicit page jump or
  // a long scroll bar move.
  void push_history(const Pos& p);
  void push_history() { push_history(position()); }

  void rotate_pt(int page, float x, float y, float& u, float& v) const;
  void unrotate_pt(int page, float u, float v, float& x, float& y) const;
  RECT to_client(int page, const RectF& r) const;
  bool hit_page(int cx, int cy, bool nearest, int& page, float& x, float& y) const;
  static int char_at(const PageText& t, float x, float y, bool nearest);
  void range_rects(int page, const PageText& t, int a, int b, std::vector<RECT>& out) const;
  std::wstring selected_text();
  void select_word(int page, int idx);
  void update_hover(POINT pt);
  void update_selection_drag(POINT pt);
  const Link* link_at(int page, float x, float y, bool wait);
  void follow_link(const Link& l);

  void stop_search();
  const std::vector<int>& page_matches(int page, const PageText& t);
  void scroll_to_match();

  void ensure_buffer();
  void paint(HDC hdc);
  void draw_page(int page, const RECT& rc, std::vector<TileRequest>& reqs, std::vector<std::shared_ptr<const Tile>>& tmp);
  void add_tile_requests(int page, const RECT& rc, const RECT& area, std::vector<TileRequest>& reqs);
  void fill_px(RECT r, COLORREF c);
  void blend_px(RECT r, COLORREF c, int alpha);
  void invert_px(RECT r);
  void draw_bubble(const std::wstring& text, bool bottom_left);
  void make_fonts();

  HWND hwnd_ = nullptr;
  CanvasHost* host_ = nullptr;
  std::unique_ptr<Renderer> renderer_;
  std::shared_ptr<Document> doc_;
  int dpi_ = 96;

  ZoomMode mode_ = ZoomMode::FitWidth;
  float zoom_ = 1;
  int rot_ = 0;
  bool facing_ = false;
  bool invert_ = false;
  float scale_ = 1;  // device pixels per point
  float med_w_ = 612, med_h_ = 792;

  std::vector<PageBox> pages_;
  std::vector<Row> rows_;
  std::vector<int> row_of_page_;
  int doc_w_ = 0, doc_h_ = 0;
  int view_w_ = 0, view_h_ = 0;
  int scroll_x_ = 0, scroll_y_ = 0;
  float wheel_rest_ = 0, hwheel_rest_ = 0;

  HDC mem_dc_ = nullptr;
  HBITMAP mem_bmp_ = nullptr;
  HGDIOBJ mem_old_ = nullptr;
  uint32_t* bits_ = nullptr;
  int buf_w_ = 0, buf_h_ = 0;
  HFONT ui_font_ = nullptr, hint_font_ = nullptr;

  Drag drag_ = Drag::None;
  POINT drag_start_{};
  int drag_sx_ = 0, drag_sy_ = 0;
  bool drag_moved_ = false;
  Link drag_link_;
  Cursor cursor_ = Cursor::Arrow;
  std::wstring hover_text_;

  TextPos sel_a_, sel_b_;
  bool has_sel_ = false;

  std::wstring query_;
  int match_page_ = -1, match_pos_ = -1;
  std::unordered_map<int, std::vector<int>> match_cache_;
  std::thread search_thread_;
  std::atomic<bool> search_cancel_{false};
  uint32_t search_id_ = 0;
  bool searching_ = false;

  std::vector<Pos> back_, fwd_;
  bool sb_tracking_ = false;  // a scroll bar interaction is in progress
  Pos sb_start_;
  int sb_start_x_ = 0, sb_start_y_ = 0;
  std::wstring toast_;
};

}  // namespace lv
