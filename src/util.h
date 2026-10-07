// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace lv {

std::wstring utf8_to_wide(std::string_view s);
std::string wide_to_utf8(std::wstring_view s);

std::wstring file_name_of(const std::wstring& path);
std::wstring dir_of(const std::wstring& path);
std::wstring exe_path();
std::wstring full_path(const std::wstring& path);
bool same_path(const std::wstring& a, const std::wstring& b);

std::wstring window_text(HWND hwnd);
std::wstring fmt(const wchar_t* format, ...);

// Size + last write time; used to detect that a file was rewritten.
struct FileStamp {
  uint64_t size = 0;
  uint64_t mtime = 0;
  bool valid = false;
  bool operator==(const FileStamp&) const = default;
};
FileStamp file_stamp(const std::wstring& path);

inline int dpi_scale(int v, int dpi) { return MulDiv(v, dpi, 96); }

// Launches another instance of this executable for `path`.
void spawn_self(const std::wstring& path);

}  // namespace lv
