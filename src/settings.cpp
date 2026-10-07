// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "settings.h"

#include <shlobj.h>

#include <algorithm>
#include <cwchar>

#include "util.h"

namespace lv {

namespace {

constexpr size_t kMaxFiles = 300;

// Portable mode: next to the executable when that folder is writable, otherwise %APPDATA%\LightView.
const std::wstring& settings_path() {
  static std::wstring path = [] {
    std::wstring portable = dir_of(exe_path()) + L"\\LightView.ini";
    HANDLE h = CreateFileW(portable.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      CloseHandle(h);
      return portable;
    }
    PWSTR appdata = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) dir = appdata;
    CoTaskMemFree(appdata);
    dir += L"\\LightView";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\LightView.ini";
  }();
  return path;
}

std::wstring read_all(const std::wstring& path) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return {};
  LARGE_INTEGER size{};
  GetFileSizeEx(h, &size);
  std::string data((size_t)std::min<LONGLONG>(size.QuadPart, 16 << 20), '\0');
  DWORD got = 0;
  ReadFile(h, data.data(), (DWORD)data.size(), &got, nullptr);
  CloseHandle(h);
  data.resize(got);
  return utf8_to_wide(data);
}

std::vector<std::wstring> split(const std::wstring& s, wchar_t sep) {
  std::vector<std::wstring> out;
  size_t b = 0;
  for (;;) {
    size_t e = s.find(sep, b);
    out.push_back(s.substr(b, e == std::wstring::npos ? std::wstring::npos : e - b));
    if (e == std::wstring::npos) return out;
    b = e + 1;
  }
}

void parse(const std::wstring& text, Settings& s) {
  for (const std::wstring& raw : split(text, L'\n')) {
    std::wstring line = raw;
    if (!line.empty() && line.back() == L'\r') line.pop_back();
    auto f = split(line, L'\t');
    if (f.empty()) continue;
    if (f[0] == L"window" && f.size() >= 6) {
      s.have_window = true;
      s.window = {_wtoi(f[1].c_str()), _wtoi(f[2].c_str()), _wtoi(f[3].c_str()), _wtoi(f[4].c_str())};
      s.maximized = _wtoi(f[5].c_str()) != 0;
    } else if (f[0] == L"sidebar" && f.size() >= 3) {
      s.sidebar = _wtoi(f[1].c_str()) != 0;
      s.sidebar_w = std::clamp(_wtoi(f[2].c_str()), 120, 800);
    } else if (f[0] == L"invert" && f.size() >= 2) {
      s.invert = _wtoi(f[1].c_str()) != 0;
    } else if (f[0] == L"crisp" && f.size() >= 2) {
      s.crisp_text = _wtoi(f[1].c_str()) != 0;
    } else if (f[0] == L"file" && f.size() >= 9 && !f[8].empty()) {
      FileEntry e;
      e.time = _wcstoui64(f[1].c_str(), nullptr, 10);
      e.view.page = std::max(0, _wtoi(f[2].c_str()));
      e.view.page_y = (float)_wtof(f[3].c_str());
      e.view.mode = (ZoomMode)std::clamp(_wtoi(f[4].c_str()), 0, 2);
      e.view.zoom = std::clamp((float)_wtof(f[5].c_str()), 0.05f, 64.f);
      e.view.rot = _wtoi(f[6].c_str()) & 3;
      e.view.facing = _wtoi(f[7].c_str()) != 0;
      e.path = f[8];
      s.files.push_back(std::move(e));
    }
  }
}

}  // namespace

Settings& settings() {
  static Settings s;
  return s;
}

void Settings::load() {
  *this = Settings();
  parse(read_all(settings_path()), *this);
  std::stable_sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.time > b.time; });
}

const FileEntry* Settings::find(const std::wstring& path) const {
  for (auto& e : files) {
    if (same_path(e.path, path)) return &e;
  }
  return nullptr;
}

void Settings::touch(const FileEntry& e) {
  forget(e.path);
  files.insert(files.begin(), e);
}

void Settings::forget(const std::wstring& path) {
  files.erase(std::remove_if(files.begin(), files.end(), [&](auto& f) { return same_path(f.path, path); }),
              files.end());
}

void Settings::save(const FileEntry* current) {
  if (current) touch(*current);

  Settings disk;
  parse(read_all(settings_path()), disk);
  for (auto& d : disk.files) {
    auto it = std::find_if(files.begin(), files.end(), [&](auto& f) { return same_path(f.path, d.path); });
    if (it == files.end()) {
      files.push_back(d);
    } else if (d.time > it->time) {
      *it = d;
    }
  }
  std::stable_sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.time > b.time; });
  if (files.size() > kMaxFiles) files.resize(kMaxFiles);

  std::wstring out = L"lightview-settings\t1\n";
  if (have_window) {
    out += fmt(L"window\t%d\t%d\t%d\t%d\t%d\n", window.left, window.top, window.right, window.bottom, maximized ? 1 : 0);
  }
  out += fmt(L"sidebar\t%d\t%d\n", sidebar ? 1 : 0, sidebar_w);
  out += fmt(L"invert\t%d\n", invert ? 1 : 0);
  out += fmt(L"crisp\t%d\n", crisp_text ? 1 : 0);
  for (auto& f : files) {
    out += fmt(L"file\t%llu\t%d\t%.2f\t%d\t%.4f\t%d\t%d\t", (unsigned long long)f.time, f.view.page, f.view.page_y,
               (int)f.view.mode, f.view.zoom, f.view.rot, f.view.facing ? 1 : 0);
    out += f.path;
    out += L"\n";
  }
  std::string data = wide_to_utf8(out);
  std::wstring tmp = settings_path() + L".tmp";
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr);
  CloseHandle(h);
  if (ok && written == data.size()) {
    MoveFileExW(tmp.c_str(), settings_path().c_str(), MOVEFILE_REPLACE_EXISTING);
  } else {
    DeleteFileW(tmp.c_str());
  }
}

}  // namespace lv
