// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "util.h"

#include <cstdarg>
#include <vector>

namespace lv {

std::wstring utf8_to_wide(std::string_view s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring out(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
  return out;
}

std::string wide_to_utf8(std::wstring_view s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring file_name_of(const std::wstring& path) {
  size_t p = path.find_last_of(L"\\/");
  return p == std::wstring::npos ? path : path.substr(p + 1);
}

std::wstring dir_of(const std::wstring& path) {
  size_t p = path.find_last_of(L"\\/");
  return p == std::wstring::npos ? std::wstring() : path.substr(0, p);
}

std::wstring exe_path() {
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    if (n < buf.size()) {
      buf.resize(n);
      return buf;
    }
    buf.resize(buf.size() * 2);
  }
}

std::wstring full_path(const std::wstring& path) {
  DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (!n) return path;
  std::wstring out(n, L'\0');
  n = GetFullPathNameW(path.c_str(), n, out.data(), nullptr);
  out.resize(n);
  return out;
}

bool same_path(const std::wstring& a, const std::wstring& b) {
  return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

std::wstring window_text(HWND hwnd) {
  int n = GetWindowTextLengthW(hwnd);
  std::wstring s(n + 1, L'\0');
  n = GetWindowTextW(hwnd, s.data(), n + 1);
  s.resize(n);
  return s;
}

std::wstring fmt(const wchar_t* format, ...) {
  va_list ap;
  va_start(ap, format);
  int n = _vscwprintf(format, ap);
  va_end(ap);
  std::wstring s(n, L'\0');
  va_start(ap, format);
  _vsnwprintf_s(s.data(), n + 1, _TRUNCATE, format, ap);
  va_end(ap);
  return s;
}

FileStamp file_stamp(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA fa;
  FileStamp st;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return st;
  st.size = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
  st.mtime = ((uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
  st.valid = true;
  return st;
}

void spawn_self(const std::wstring& path) {
  std::wstring cmd = L"\"" + exe_path() + L"\" \"" + path + L"\"";
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

}  // namespace lv
