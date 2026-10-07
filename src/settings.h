// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace lv {

enum class ZoomMode : int { FitWidth = 0, FitPage = 1, Custom = 2 };

// Reading position and view options, remembered per file.
struct ViewState {
  int page = 0;
  float page_y = 0;  // points from the page top that sit at the top of the viewport
  ZoomMode mode = ZoomMode::FitWidth;
  float zoom = 1;    // custom zoom, 1 = actual size
  int rot = 0;       // view rotation, clockwise quarter turns
  bool facing = false;
};

struct FileEntry {
  std::wstring path;
  ViewState view;
  uint64_t time = 0;
};

struct Settings {
  bool have_window = false;
  RECT window{};
  bool maximized = false;
  bool sidebar = true;
  int sidebar_w = 260;  // at 96 dpi
  bool invert = false;
  bool crisp_text = true;
  std::vector<FileEntry> files;  // most recent first

  void load();
  // Re-reads the file (other windows may have written it), merges our changes and writes it back.
  void save(const FileEntry* current);
  const FileEntry* find(const std::wstring& path) const;
  void touch(const FileEntry& e);
  void forget(const std::wstring& path);
};

Settings& settings();

}  // namespace lv
