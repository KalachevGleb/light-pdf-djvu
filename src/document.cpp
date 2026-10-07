// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "document.h"

#include <windows.h>

#include "util.h"

namespace lv {

namespace {

constexpr int kMaxCachedTexts = 300;

bool is_space(wchar_t c) {
  return c <= 0x20 || c == 0xA0 || (c >= 0x2000 && c <= 0x200B) || c == 0x202F || c == 0x3000;
}

void fold_case(std::wstring& s) {
  if (s.empty()) return;
  CharLowerBuffW(s.data(), (DWORD)s.size());
  for (auto& c : s) {
    if (c == L'\x451') c = L'\x435';  // ё -> е
  }
}

}  // namespace

std::mutex& engine_mutex() {
  static std::mutex mu;
  return mu;
}

void Document::init_caches() {
  text_cache_.assign(sizes_.size(), nullptr);
  text_used_.assign(sizes_.size(), 0);
  link_cache_.assign(sizes_.size(), nullptr);
}

std::shared_ptr<const PageText> Document::text(int page, bool wait) {
  if (page < 0 || page >= page_count()) return nullptr;
  {
    std::lock_guard g(cache_mu_);
    if (text_cache_[page]) {
      text_used_[page] = ++text_clock_;
      return text_cache_[page];
    }
  }
  std::unique_lock lk(engine_mutex(), std::defer_lock);
  if (wait) {
    lk.lock();
  } else if (!lk.try_lock()) {
    return nullptr;
  }
  {
    std::lock_guard g(cache_mu_);  // another thread may have loaded it meanwhile
    if (text_cache_[page]) return text_cache_[page];
  }
  auto t = std::make_shared<PageText>(load_text(page));
  lk.unlock();
  build_search_text(*t);

  std::lock_guard g(cache_mu_);
  if (!text_cache_[page]) {
    text_cache_[page] = t;
    text_used_[page] = ++text_clock_;
    if (++text_count_ > kMaxCachedTexts) {
      int victim = -1;
      for (int i = 0; i < (int)text_cache_.size(); ++i) {
        if (text_cache_[i] && i != page && (victim < 0 || text_used_[i] < text_used_[victim])) victim = i;
      }
      if (victim >= 0) {
        text_cache_[victim].reset();
        --text_count_;
      }
    }
  }
  return text_cache_[page];
}

std::shared_ptr<const std::vector<Link>> Document::links(int page, bool wait) {
  if (page < 0 || page >= page_count()) return nullptr;
  {
    std::lock_guard g(cache_mu_);
    if (link_cache_[page]) return link_cache_[page];
  }
  std::unique_lock lk(engine_mutex(), std::defer_lock);
  if (wait) {
    lk.lock();
  } else if (!lk.try_lock()) {
    return nullptr;
  }
  {
    std::lock_guard g(cache_mu_);
    if (link_cache_[page]) return link_cache_[page];
  }
  auto l = std::make_shared<std::vector<Link>>(load_links(page));
  lk.unlock();
  std::lock_guard g(cache_mu_);
  if (!link_cache_[page]) link_cache_[page] = l;
  return link_cache_[page];
}

void build_search_text(PageText& t) {
  const std::wstring& s = t.text;
  const size_t n = s.size();
  t.norm.clear();
  t.norm_map.clear();
  t.norm.reserve(n);
  t.norm_map.reserve(n);
  bool pending_space = false;
  for (size_t i = 0; i < n; ++i) {
    wchar_t c = s[i];
    if (c == 0xAD) continue;  // soft hyphen
    // Join words hyphenated across a line break: "exam-\r\nple" -> "example".
    if ((c == L'-' || c == 0x2010) && i > 0 && IsCharAlphaW(s[i - 1])) {
      size_t j = i + 1;
      bool newline = false;
      while (j < n && is_space(s[j])) {
        if (s[j] == L'\n' || s[j] == L'\r') newline = true;
        ++j;
      }
      if (newline && j < n && IsCharAlphaW(s[j])) {
        i = j - 1;
        continue;
      }
    }
    if (is_space(c)) {
      pending_space = !t.norm.empty();
      continue;
    }
    if (pending_space) {
      t.norm.push_back(L' ');
      t.norm_map.push_back((int)i - 1);
      pending_space = false;
    }
    t.norm.push_back(c);
    t.norm_map.push_back((int)i);
  }
  fold_case(t.norm);
}

std::wstring normalize_query(const std::wstring& q) {
  std::wstring out;
  bool pending_space = false;
  for (wchar_t c : q) {
    if (is_space(c)) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) out.push_back(L' ');
    pending_space = false;
    out.push_back(c);
  }
  fold_case(out);
  return out;
}

OpenResult open_document(const std::wstring& path, const std::string& password) {
  OpenResult r;
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    r.error = L"Не удалось открыть файл.";
    return r;
  }
  char head[1024] = {};
  DWORD got = 0;
  ReadFile(f, head, sizeof(head), &got, nullptr);
  CloseHandle(f);

  std::string_view h(head, got);
  if (h.substr(0, 8) == "AT&TFORM") return open_djvu(path);
  if (h.find("%PDF") != std::string_view::npos) return open_pdf(path, password);

  std::wstring ext = path.substr(path.find_last_of(L'.') == std::wstring::npos ? path.size() : path.find_last_of(L'.'));
  CharLowerBuffW(ext.data(), (DWORD)ext.size());
  if (ext == L".djvu" || ext == L".djv") return open_djvu(path);
  if (ext == L".pdf") return open_pdf(path, password);
  r.error = L"Формат файла не распознан (поддерживаются PDF и DjVu).";
  return r;
}

}  // namespace lv
