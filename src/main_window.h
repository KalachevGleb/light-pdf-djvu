// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <windows.h>

#include <commctrl.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "canvas.h"
#include "document.h"
#include "toolbar.h"
#include "util.h"

namespace lv {

// Posts `msg` to `notify` whenever something changes in a directory.
class DirWatcher {
 public:
  ~DirWatcher() { stop(); }
  void start(HWND notify, UINT msg, const std::wstring& dir);
  void stop();

 private:
  std::thread thread_;
  HANDLE stop_event_ = nullptr;
};

class MainWindow : public CanvasHost, public ToolbarHost {
 public:
  static void register_class(HINSTANCE inst);
  bool create(HINSTANCE inst, int show_cmd);
  HWND hwnd() const { return hwnd_; }
  bool open(const std::wstring& path);
  HACCEL accelerators() const { return accel_; }

  // CanvasHost
  void on_view_changed() override;
  void on_context_menu(POINT screen) override;
  void on_canvas_escape() override;
  // ToolbarHost
  void execute(int cmd) override;
  void on_page_entered(const std::wstring& text) override;
  void on_zoom_entered(const std::wstring& text) override;
  void on_search(const std::wstring& text, bool forward) override;
  void on_edit_escape() override;

 private:
  static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
  int s(int v) const { return MulDiv(v, dpi_, 96); }
  void layout();
  void update_ui();
  void update_title();
  void load_outline();
  void insert_outline(const std::vector<OutlineItem>& items, HTREEITEM parent, int depth, size_t total);
  void navigate_outline(HTREEITEM item);
  bool sidebar_visible() const { return show_sidebar_ && has_outline_ && !fullscreen_; }
  RECT splitter_rect() const;
  void open_dialog();
  void reload(bool quiet);
  void remember_position();
  void save_settings();
  void toggle_fullscreen();
  void show_help();
  void register_app();
  bool ask_password(const std::wstring& name, bool retry, std::wstring& out);
  void apply_tree_theme();

  HWND hwnd_ = nullptr;
  HWND tree_ = nullptr;
  HINSTANCE inst_ = nullptr;
  HACCEL accel_ = nullptr;
  HFONT tree_font_ = nullptr;
  int dpi_ = 96;
  Toolbar toolbar_;
  Canvas canvas_;

  std::shared_ptr<Document> doc_;
  std::wstring path_;
  std::string password_;
  FileStamp stamp_, pending_stamp_;
  int reload_retries_ = 0;
  DirWatcher watcher_;

  std::vector<Dest> outline_dests_;
  bool has_outline_ = false;
  bool show_sidebar_ = true;
  int sidebar_w_ = 260;  // at 96 dpi
  bool filling_tree_ = false;
  bool tree_clicked_ = false;
  bool dragging_splitter_ = false;

  bool fullscreen_ = false;
  WINDOWPLACEMENT saved_placement_{sizeof(WINDOWPLACEMENT)};
  LONG_PTR saved_style_ = 0;

  std::vector<std::wstring> recent_menu_;
};

// "Open with" registration for .pdf/.djvu/.djv in HKCU (no admin rights needed).
bool register_file_types(bool install);

// If another LightView window already shows `path`, brings it to the front and returns true.
bool activate_existing_window(const std::wstring& path);

}  // namespace lv
