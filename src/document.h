// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lv {

struct SizeF {
  float w = 0, h = 0;
};

// Page-space rectangle in points (1/72"), origin at the top-left of the page as the document
// presents it (intrinsic page rotation applied, view rotation not applied).
struct RectF {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool empty() const { return x1 <= x0 || y1 <= y0; }
  bool contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

struct Dest {
  int page = -1;
  float y = -1;  // offset from the page top in points; < 0 means "top of the page"
  // PDF destinations keep user-space coordinates until Document::resolve_dest() maps them.
  bool raw = false;
  float raw_x = 0, raw_y = 0;
};

struct OutlineItem {
  std::wstring title;
  Dest dest;
  bool open = false;
  std::vector<OutlineItem> children;
};

struct Link {
  RectF rect;
  Dest dest;         // internal target, or
  std::wstring uri;  // external target
};

struct PageText {
  std::wstring text;         // lines end with "\r\n"
  std::vector<RectF> boxes;  // one per UTF-16 unit of `text`; empty for generated characters
  std::wstring norm;         // lowercased, whitespace-collapsed, de-hyphenated text for search
  std::vector<int> norm_map; // index in `norm` -> index in `text`
};

class Document {
 public:
  virtual ~Document() = default;
  virtual const wchar_t* kind() const = 0;

  int page_count() const { return (int)sizes_.size(); }
  SizeF page_size(int page) const { return sizes_[page]; }
  const std::wstring& path() const { return path_; }

  // Renders the (x, y, w, h) part of the page scaled to full_w x full_h device pixels and turned
  // by `rot` quarter turns clockwise. Output is BGRx, top-down, stride w*4.
  // Returns false if rendering was aborted.
  virtual bool render(int page, int full_w, int full_h, int rot, int x, int y, int w, int h,
                      uint8_t* out, const std::atomic<bool>* abort, bool printing = false) = 0;

  virtual std::vector<OutlineItem> outline() = 0;
  virtual Dest resolve_dest(const Dest& d) { return d; }

  // Cached. With wait=false returns null instead of blocking while the engine is busy.
  std::shared_ptr<const PageText> text(int page, bool wait = true);
  std::shared_ptr<const std::vector<Link>> links(int page, bool wait = true);

 protected:
  virtual PageText load_text(int page) = 0;         // engine lock is held
  virtual std::vector<Link> load_links(int page) = 0;  // engine lock is held
  void init_caches();

  std::wstring path_;
  std::vector<SizeF> sizes_;

 private:
  std::mutex cache_mu_;
  std::vector<std::shared_ptr<const PageText>> text_cache_;
  std::vector<uint64_t> text_used_;
  uint64_t text_clock_ = 0;
  int text_count_ = 0;
  std::vector<std::shared_ptr<const std::vector<Link>>> link_cache_;
};

// Both PDFium and DjVuLibre are driven from several threads (renderer, search, UI), so every
// call into an engine is serialized by this lock.
std::mutex& engine_mutex();

struct OpenResult {
  std::unique_ptr<Document> doc;
  std::wstring error;
  bool need_password = false;
};
OpenResult open_document(const std::wstring& path, const std::string& password);

// Engine-specific factories (pdf_document.cpp, djvu_document.cpp).
OpenResult open_pdf(const std::wstring& path, const std::string& password);
OpenResult open_djvu(const std::wstring& path);

// Screen rendering of PDF text: ClearType + contrast gamma (on) or plain grayscale (off).
// Printing always uses plain rendering.
void set_crisp_text(bool on);

void build_search_text(PageText& t);
std::wstring normalize_query(const std::wstring& q);

}  // namespace lv
