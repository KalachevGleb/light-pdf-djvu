// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <windows.h>

#include <ddjvuapi.h>
#include <miniexp.h>

#include <algorithm>
#include <cstring>

#include "document.h"
#include "util.h"

namespace lv {

namespace {

ddjvu_context_t* context() {
  static ddjvu_context_t* ctx = [] {
    ddjvu_context_t* c = ddjvu_context_create("LightView");
    ddjvu_cache_set_size(c, 32u << 20);
    return c;
  }();
  return ctx;
}

// Drains the message queue; with wait=true first blocks until a message arrives.
void pump(bool wait) {
  ddjvu_context_t* ctx = context();
  if (wait) ddjvu_message_wait(ctx);
  while (ddjvu_message_peek(ctx)) ddjvu_message_pop(ctx);
}

struct PageInfo {
  int w0 = 0, h0 = 0;  // unrotated size in pixels
  int dpi = 300;
  int rot = 0;         // initial rotation, counter-clockwise quarter turns
};

// Unrotated page pixels (origin bottom-left) -> page points (origin top-left, initial rotation applied).
struct TextXform {
  PageInfo pi;
  float s = 1;
  void map(float x, float yb, float& ox, float& oy) const {
    float xt = x, yt = pi.h0 - yb;
    switch (pi.rot) {
      case 1: ox = yt; oy = pi.w0 - xt; break;
      case 2: ox = pi.w0 - xt; oy = pi.h0 - yt; break;
      case 3: ox = pi.h0 - yt; oy = xt; break;
      default: ox = xt; oy = yt; break;
    }
    ox *= s;
    oy *= s;
  }
  RectF map_rect(float x0, float y0, float x1, float y1) const {
    float ax, ay, bx, by;
    map(x0, y0, ax, ay);
    map(x1, y1, bx, by);
    return {std::min(ax, bx), std::min(ay, by), std::max(ax, bx), std::max(ay, by)};
  }
};

// Fast path for page sizes: ddjvu needs ~2 ms per page to report them (a decoding job per page),
// so read the INFO chunks of bundled and single-page documents directly. Returns false for anything
// unusual (indirect documents, unexpected layout) and the caller falls back to ddjvu.
bool read_page_infos(const std::wstring& path, int expected, std::vector<PageInfo>& out) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  // Returns the number of bytes read (short near the end of the file).
  auto read = [&](uint64_t off, void* buf, DWORD n) -> DWORD {
    OVERLAPPED ov{};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    return ReadFile(h, buf, n, &got, &ov) ? got : 0;
  };
  auto be32 = [](const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; };
  // `form` holds `got` bytes: "FORM" <size> <type>, then the first chunk ("INFO" <size> <data>).
  auto parse_page = [&](const uint8_t* form, DWORD got, PageInfo& pi) {
    if (got < 25 || memcmp(form + 12, "INFO", 4) != 0) return false;
    const uint32_t n = std::min<uint32_t>(be32(form + 16), 10);
    if (n < 5 || got < 20 + n) return false;
    const uint8_t* d = form + 20;
    pi.w0 = d[0] << 8 | d[1];
    pi.h0 = d[2] << 8 | d[3];
    pi.dpi = (n >= 8 && d[7] != 0xFF) ? (d[7] << 8 | d[6]) : 300;
    if (pi.dpi < 25 || pi.dpi > 6000) pi.dpi = 300;
    switch (n >= 10 ? d[9] & 7 : 0) {  // same mapping as DjVuInfo::decode
      case 6: pi.rot = 1; break;
      case 2: pi.rot = 2; break;
      case 5: pi.rot = 3; break;
      default: pi.rot = 0; break;
    }
    return pi.w0 > 0 && pi.h0 > 0;
  };

  bool ok = false;
  uint8_t head[40] = {};
  out.clear();
  const DWORD head_len = read(0, head, sizeof(head));
  if (head_len >= 16 && memcmp(head, "AT&TFORM", 8) == 0) {
    if (memcmp(head + 12, "DJVU", 4) == 0) {
      PageInfo pi;
      ok = parse_page(head + 4, head_len - 4, pi);
      out.push_back(pi);
    } else if (head_len >= 27 && memcmp(head + 12, "DJVM", 4) == 0 && memcmp(head + 16, "DIRM", 4) == 0 &&
               (head[24] & 0x80)) {
      // Bundled directory: version byte, file count, then one offset per component in page order.
      const int files = head[25] << 8 | head[26];
      std::vector<uint8_t> offsets((size_t)files * 4);
      ok = files > 0 && read(27, offsets.data(), (DWORD)offsets.size()) == offsets.size();
      for (int i = 0; ok && i < files; ++i) {
        uint8_t form[32];
        const DWORD got = read(be32(&offsets[(size_t)i * 4]), form, sizeof(form));
        ok = got >= 12 && memcmp(form, "FORM", 4) == 0;
        if (ok && memcmp(form + 8, "DJVU", 4) == 0) {
          PageInfo pi;
          ok = parse_page(form, got, pi);
          out.push_back(pi);
        }
      }
    }
  }
  CloseHandle(h);
  return ok && (int)out.size() == expected;
}

bool is_symbol(miniexp_t e, const char* name) {
  return miniexp_symbolp(e) && strcmp(miniexp_to_name(e), name) == 0;
}

void end_line(PageText& t) {
  if (!t.text.empty() && t.text.back() == L' ') {
    t.text.pop_back();
    t.boxes.pop_back();
  }
  if (!t.text.empty() && t.text.back() != L'\n') {
    t.text += L"\r\n";
    t.boxes.push_back({});
    t.boxes.push_back({});
  }
}

void walk_text(miniexp_t node, PageText& t, const TextXform& xf, int depth) {
  if (!miniexp_consp(node) || depth > 32) return;
  miniexp_t type = miniexp_car(node);
  if (!miniexp_symbolp(type)) return;
  int c[4];
  miniexp_t p = miniexp_cdr(node);
  for (int k = 0; k < 4; ++k) {
    if (!miniexp_consp(p) || !miniexp_numberp(miniexp_car(p))) return;
    c[k] = miniexp_to_int(miniexp_car(p));
    p = miniexp_cdr(p);
  }
  const char* name = miniexp_to_name(type);
  bool is_word = strcmp(name, "word") == 0;
  bool is_char = strcmp(name, "char") == 0;
  if (miniexp_consp(p) && miniexp_stringp(miniexp_car(p))) {
    std::wstring s = utf8_to_wide(miniexp_to_str(miniexp_car(p)));
    // The text layer only has word boxes: split them evenly along the (unrotated) text direction.
    int n = (int)s.size();
    for (int i = 0; i < n; ++i) {
      float x0 = c[0] + (c[2] - c[0]) * (float)i / n;
      float x1 = c[0] + (c[2] - c[0]) * (float)(i + 1) / n;
      t.text.push_back(s[i]);
      t.boxes.push_back(xf.map_rect(x0, (float)c[1], x1, (float)c[3]));
    }
    if (is_word) {
      t.text.push_back(L' ');
      t.boxes.push_back({});
    } else if (!is_char) {
      end_line(t);
    }
    return;
  }
  for (; miniexp_consp(p); p = miniexp_cdr(p)) walk_text(miniexp_car(p), t, xf, depth + 1);
  if (!is_word && !is_char) end_line(t);
}

class DjvuDocument final : public Document {
 public:
  ~DjvuDocument() override {
    std::lock_guard g(engine_mutex());
    for (auto& p : pages_) ddjvu_page_release(p.second);
    if (fmt_) ddjvu_format_release(fmt_);
    if (doc_) ddjvu_document_release(doc_);
    pump(false);
  }

  const wchar_t* kind() const override { return L"DjVu"; }

  OpenResult open(const std::wstring& path) {
    OpenResult r;
    path_ = path;
    std::lock_guard g(engine_mutex());
    doc_ = ddjvu_document_create_by_filename_utf8(context(), wide_to_utf8(path).c_str(), TRUE);
    if (!doc_) {
      r.error = L"Не удалось открыть DjVu.";
      return r;
    }
    while (!ddjvu_document_decoding_done(doc_)) pump(true);
    if (ddjvu_document_decoding_error(doc_)) {
      r.error = L"Файл DjVu повреждён или не поддерживается.";
      return r;
    }
    int n = ddjvu_document_get_pagenum(doc_);
    if (n <= 0) {
      r.error = L"В документе нет страниц.";
      return r;
    }
    sizes_.resize(n);
    const bool fast = read_page_infos(path, n, info_);
    info_.resize(n);
    for (int i = 0; i < n; ++i) {
      PageInfo& inf = info_[i];
      if (!fast) {
        ddjvu_pageinfo_t pi{};
        ddjvu_status_t st;
        while ((st = ddjvu_document_get_pageinfo(doc_, i, &pi)) < DDJVU_JOB_OK) pump(true);
        if (st == DDJVU_JOB_OK && pi.width > 0 && pi.height > 0) {
          inf.rot = pi.rotation & 3;
          inf.dpi = pi.dpi > 0 ? pi.dpi : 300;
          // pageinfo sizes already include the initial rotation.
          inf.w0 = (inf.rot & 1) ? pi.height : pi.width;
          inf.h0 = (inf.rot & 1) ? pi.width : pi.height;
        } else if (i > 0) {
          inf = info_[i - 1];
        } else {
          inf = {2550, 3300, 300, 0};
        }
      }
      float s = 72.f / inf.dpi;
      sizes_[i] = (inf.rot & 1) ? SizeF{inf.h0 * s, inf.w0 * s} : SizeF{inf.w0 * s, inf.h0 * s};
    }
    static unsigned int masks[4] = {0xff0000, 0xff00, 0xff, 0xff000000};
    fmt_ = ddjvu_format_create(DDJVU_FORMAT_RGBMASK32, 4, masks);
    ddjvu_format_set_row_order(fmt_, 1);
    ddjvu_format_set_y_direction(fmt_, 1);
    init_caches();
    return r;
  }

  bool render(int page, int full_w, int full_h, int rot, int x, int y, int w, int h, uint8_t* out,
              const std::atomic<bool>* abort, bool /*printing*/) override {
    std::lock_guard g(engine_mutex());
    if (abort && abort->load()) return false;
    ddjvu_page_t* pg = get_page(page);
    bool drawn = false;
    if (pg) {
      int ccw = (info_[page].rot + 4 - (rot & 3)) & 3;
      ddjvu_page_set_rotation(pg, (ddjvu_page_rotation_t)ccw);
      ddjvu_rect_t prect{0, 0, (unsigned)full_w, (unsigned)full_h};
      ddjvu_rect_t rrect{x, y, (unsigned)w, (unsigned)h};
      drawn = ddjvu_page_render(pg, DDJVU_RENDER_COLOR, &prect, &rrect, fmt_, (unsigned long)w * 4, (char*)out) != 0;
    }
    if (!drawn) memset(out, 0xFF, (size_t)w * h * 4);
    return true;
  }

  std::vector<OutlineItem> outline() override {
    std::lock_guard g(engine_mutex());
    std::vector<OutlineItem> out;
    miniexp_t r;
    while ((r = ddjvu_document_get_outline(doc_)) == miniexp_dummy) pump(true);
    if (miniexp_consp(r) && is_symbol(miniexp_car(r), "bookmarks")) parse_outline(miniexp_cdr(r), out, 0);
    ddjvu_miniexp_release(doc_, r);
    return out;
  }

 protected:
  PageText load_text(int page) override {
    PageText t;
    miniexp_t r;
    while ((r = ddjvu_document_get_pagetext(doc_, page, "word")) == miniexp_dummy) pump(true);
    TextXform xf{info_[page], 72.f / info_[page].dpi};
    walk_text(r, t, xf, 0);
    ddjvu_miniexp_release(doc_, r);
    while (!t.text.empty() && (t.text.back() == L'\n' || t.text.back() == L'\r' || t.text.back() == L' ')) {
      t.text.pop_back();
      t.boxes.pop_back();
    }
    return t;
  }

  std::vector<Link> load_links(int page) override {
    std::vector<Link> links;
    miniexp_t r;
    while ((r = ddjvu_document_get_pageanno(doc_, page)) == miniexp_dummy) pump(true);
    // Map areas use rotated page pixels with the origin at the bottom-left.
    const PageInfo& pi = info_[page];
    const float s = 72.f / pi.dpi;
    const float page_h = sizes_[page].h;
    for (miniexp_t l = r; miniexp_consp(l); l = miniexp_cdr(l)) {
      miniexp_t a = miniexp_car(l);
      if (!miniexp_consp(a) || !is_symbol(miniexp_car(a), "maparea")) continue;
      miniexp_t url = miniexp_nth(1, a);
      std::string href;
      if (miniexp_stringp(url)) {
        href = miniexp_to_str(url);
      } else if (miniexp_consp(url) && is_symbol(miniexp_car(url), "url") && miniexp_stringp(miniexp_nth(1, url))) {
        href = miniexp_to_str(miniexp_nth(1, url));
      }
      if (href.empty()) continue;
      miniexp_t area = miniexp_nth(3, a);
      if (!miniexp_consp(area)) continue;
      std::vector<int> v;
      for (miniexp_t q = miniexp_cdr(area); miniexp_consp(q) && miniexp_numberp(miniexp_car(q)); q = miniexp_cdr(q)) {
        v.push_back(miniexp_to_int(miniexp_car(q)));
      }
      float x0, y0, x1, y1;
      if (is_symbol(miniexp_car(area), "poly") && v.size() >= 4) {
        x0 = x1 = (float)v[0];
        y0 = y1 = (float)v[1];
        for (size_t k = 0; k + 1 < v.size(); k += 2) {
          x0 = std::min(x0, (float)v[k]);
          x1 = std::max(x1, (float)v[k]);
          y0 = std::min(y0, (float)v[k + 1]);
          y1 = std::max(y1, (float)v[k + 1]);
        }
      } else if (v.size() >= 4) {  // rect, oval, text: x y w h
        x0 = (float)v[0];
        y0 = (float)v[1];
        x1 = x0 + v[2];
        y1 = y0 + v[3];
      } else {
        continue;
      }
      Link link;
      link.rect = {x0 * s, page_h - y1 * s, x1 * s, page_h - y0 * s};
      if (href[0] == '#') {
        link.dest.page = resolve_url(href.c_str() + 1, page);
        if (link.dest.page < 0) continue;
      } else {
        link.uri = utf8_to_wide(href);
      }
      links.push_back(std::move(link));
    }
    ddjvu_miniexp_release(doc_, r);
    return links;
  }

 private:
  // Engine lock must be held.
  ddjvu_page_t* get_page(int index) {
    for (size_t i = 0; i < pages_.size(); ++i) {
      if (pages_[i].first == index) {
        auto p = pages_[i];
        pages_.erase(pages_.begin() + i);
        pages_.push_back(p);
        return p.second;
      }
    }
    ddjvu_page_t* pg = ddjvu_page_create_by_pageno(doc_, index);
    if (!pg) return nullptr;
    while (!ddjvu_page_decoding_done(pg)) pump(true);
    if (ddjvu_page_decoding_error(pg)) {
      ddjvu_page_release(pg);
      return nullptr;
    }
    if (pages_.size() >= kMaxPages) {
      ddjvu_page_release(pages_.front().second);
      pages_.erase(pages_.begin());
    }
    pages_.push_back({index, pg});
    return pg;
  }

  // "#12" -> page 11, "#+1"/"#-1" -> relative, otherwise a page/file id.
  int resolve_url(const char* s, int current) {
    if ((s[0] == '+' || s[0] == '-') && isdigit((unsigned char)s[1])) {
      int p = current + atoi(s);
      return p >= 0 && p < page_count() ? p : -1;
    }
    bool digits = *s != 0;
    for (const char* q = s; *q; ++q) digits = digits && isdigit((unsigned char)*q);
    if (digits) {
      int p = atoi(s) - 1;
      return p >= 0 && p < page_count() ? p : -1;
    }
    return ddjvu_document_search_pageno(doc_, s);
  }

  void parse_outline(miniexp_t list, std::vector<OutlineItem>& out, int depth) {
    if (depth > 64) return;
    for (; miniexp_consp(list); list = miniexp_cdr(list)) {
      miniexp_t item = miniexp_car(list);
      if (!miniexp_consp(item) || !miniexp_stringp(miniexp_car(item))) continue;
      OutlineItem it;
      it.title = utf8_to_wide(miniexp_to_str(miniexp_car(item)));
      miniexp_t url = miniexp_cadr(item);
      if (miniexp_stringp(url)) {
        const char* u = miniexp_to_str(url);
        if (u[0] == '#') it.dest.page = resolve_url(u + 1, 0);
      }
      parse_outline(miniexp_cddr(item), it.children, depth + 1);
      out.push_back(std::move(it));
    }
  }

  static constexpr size_t kMaxPages = 3;
  ddjvu_document_t* doc_ = nullptr;
  ddjvu_format_t* fmt_ = nullptr;
  std::vector<PageInfo> info_;
  std::vector<std::pair<int, ddjvu_page_t*>> pages_;
};

}  // namespace

OpenResult open_djvu(const std::wstring& path) {
  auto doc = std::make_unique<DjvuDocument>();
  OpenResult r = doc->open(path);
  if (r.error.empty()) r.doc = std::move(doc);
  return r;
}

}  // namespace lv
