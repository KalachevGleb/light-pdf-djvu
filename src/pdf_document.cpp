// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <windows.h>

#include <fpdf_doc.h>
#include <fpdf_formfill.h>
#include <fpdf_progressive.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_set>

#include "document.h"
#include "util.h"

namespace lv {

namespace {

void ensure_pdfium() {
  static bool initialized = [] {
    FPDF_LIBRARY_CONFIG cfg{};
    cfg.version = 2;
    FPDF_InitLibraryWithConfig(&cfg);
    return true;
  }();
  (void)initialized;
}

// Maps PDF user space to page points (top-left origin, /Rotate applied).
struct Affine {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  void apply(double x, double y, float& ox, float& oy) const {
    ox = (float)(a * x + c * y + e);
    oy = (float)(b * x + d * y + f);
  }
  RectF map_rect(double l, double t, double r, double btm) const {
    float xs[4], ys[4];
    apply(l, t, xs[0], ys[0]);
    apply(r, t, xs[1], ys[1]);
    apply(l, btm, xs[2], ys[2]);
    apply(r, btm, xs[3], ys[3]);
    RectF out{xs[0], ys[0], xs[0], ys[0]};
    for (int i = 1; i < 4; ++i) {
      out.x0 = std::min(out.x0, xs[i]);
      out.x1 = std::max(out.x1, xs[i]);
      out.y0 = std::min(out.y0, ys[i]);
      out.y1 = std::max(out.y1, ys[i]);
    }
    return out;
  }
};

struct Pause : IFSDK_PAUSE {
  const std::atomic<bool>* abort = nullptr;
};

FPDF_BOOL need_to_pause(IFSDK_PAUSE* p) {
  auto* a = static_cast<Pause*>(p)->abort;
  return a && a->load(std::memory_order_relaxed);
}

int get_block(void* param, unsigned long pos, unsigned char* buf, unsigned long size) {
  OVERLAPPED ov{};
  ov.Offset = pos;
  DWORD got = 0;
  if (!ReadFile((HANDLE)param, buf, size, &got, &ov)) return 0;
  return got == size;
}

// PDFium does not hint Type1/CFF fonts (most LaTeX output), so their stems come out as soft gray
// two-pixel lines. "Crisp text" compensates on screen: subpixel (ClearType) text where the system
// uses RGB ClearType, plus a gamma curve that restores stroke contrast.
std::atomic<bool> g_crisp_text{true};

bool system_uses_rgb_cleartype() {
  static const bool rgb = [] {
    BOOL smoothing = FALSE;
    UINT type = 0, orientation = 0;
    SystemParametersInfoW(SPI_GETFONTSMOOTHING, 0, &smoothing, 0);
    SystemParametersInfoW(SPI_GETFONTSMOOTHINGTYPE, 0, &type, 0);
    SystemParametersInfoW(SPI_GETFONTSMOOTHINGORIENTATION, 0, &orientation, 0);
    return smoothing && type == FE_FONTSMOOTHINGCLEARTYPE && orientation == FE_FONTSMOOTHINGORIENTATIONRGB;
  }();
  return rgb;
}

void apply_text_gamma(uint8_t* px, size_t pixels) {
  static const auto lut = [] {
    std::array<uint8_t, 256> t{};
    for (int v = 0; v < 256; ++v) t[v] = (uint8_t)std::lround(255.0 * std::pow(v / 255.0, 1.4));
    return t;
  }();
  for (size_t i = 0; i < pixels; ++i, px += 4) {
    px[0] = lut[px[0]];
    px[1] = lut[px[1]];
    px[2] = lut[px[2]];
  }
}

std::wstring clean_title(std::wstring s) {
  for (auto& c : s) {
    if (c < 0x20) c = L' ';
  }
  size_t b = s.find_first_not_of(L' ');
  size_t e = s.find_last_not_of(L' ');
  return b == std::wstring::npos ? std::wstring() : s.substr(b, e - b + 1);
}

class PdfDocument final : public Document {
 public:
  ~PdfDocument() override {
    std::lock_guard g(engine_mutex());
    for (auto& p : pages_) close_page(p.page);
    pages_.clear();
    if (form_) FPDFDOC_ExitFormFillEnvironment(form_);
    if (doc_) FPDF_CloseDocument(doc_);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
  }

  const wchar_t* kind() const override { return L"PDF"; }

  OpenResult open(const std::wstring& path, const std::string& password) {
    OpenResult r;
    path_ = path;
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
      r.error = L"Не удалось открыть файл.";
      return r;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file_, &size);
    if (size.QuadPart > 0xFFFFFFFFll) {
      r.error = L"PDF больше 4 ГБ не поддерживается.";
      return r;
    }
    std::lock_guard g(engine_mutex());
    ensure_pdfium();
    FPDF_FILEACCESS access{};
    access.m_FileLen = (unsigned long)size.QuadPart;
    access.m_GetBlock = get_block;
    access.m_Param = file_;
    doc_ = FPDF_LoadCustomDocument(&access, password.empty() ? nullptr : password.c_str());
    if (!doc_) {
      switch (FPDF_GetLastError()) {
        case FPDF_ERR_PASSWORD:
          r.need_password = true;
          r.error = L"Неверный пароль.";
          break;
        case FPDF_ERR_SECURITY:
          r.error = L"Неподдерживаемая схема защиты документа.";
          break;
        case FPDF_ERR_FORMAT:
          r.error = L"Файл повреждён или не является PDF.";
          break;
        default:
          r.error = L"Не удалось открыть PDF.";
      }
      return r;
    }
    int n = FPDF_GetPageCount(doc_);
    if (n <= 0) {
      r.error = L"В документе нет страниц.";
      return r;
    }
    sizes_.resize(n);
    for (int i = 0; i < n; ++i) {
      FS_SIZEF s{};
      if (FPDF_GetPageSizeByIndexF(doc_, i, &s) && s.width > 1 && s.height > 1) {
        sizes_[i] = {s.width, s.height};
      } else {
        sizes_[i] = i > 0 ? sizes_[i - 1] : SizeF{612, 792};
      }
    }
    if (FPDF_GetFormType(doc_) != FORMTYPE_NONE) {
      form_info_.version = 1;
      form_ = FPDFDOC_InitFormFillEnvironment(doc_, &form_info_);
    }
    init_caches();
    return r;
  }

  bool render(int page, int full_w, int full_h, int rot, int x, int y, int w, int h, uint8_t* out,
              const std::atomic<bool>* abort, bool printing) override {
    std::lock_guard g(engine_mutex());
    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRx, out, w * 4);
    if (!bmp) return false;
    FPDFBitmap_FillRect(bmp, 0, 0, w, h, 0xFFFFFFFF);
    bool ok = true;
    const bool crisp = !printing && g_crisp_text.load();
    if (CachedPage* cp = get_page(page)) {
      int flags = FPDF_ANNOT | (printing ? FPDF_PRINTING : 0);
      if (crisp && system_uses_rgb_cleartype()) flags |= FPDF_LCD_TEXT;
      Pause pause;
      pause.version = 1;
      pause.NeedToPauseNow = need_to_pause;
      pause.user = nullptr;
      pause.abort = abort;
      int st = FPDF_RenderPageBitmap_Start(bmp, cp->page, -x, -y, full_w, full_h, rot, flags, &pause);
      while (st == FPDF_RENDER_TOBECONTINUED && !(abort && abort->load())) {
        st = FPDF_RenderPage_Continue(cp->page, &pause);
      }
      FPDF_RenderPage_Close(cp->page);
      ok = !(abort && abort->load());
      if (ok && form_) FPDF_FFLDraw(form_, bmp, cp->page, -x, -y, full_w, full_h, rot, flags);
    }
    FPDFBitmap_Destroy(bmp);
    if (ok && crisp) apply_text_gamma(out, (size_t)w * h);
    return ok;
  }

  std::vector<OutlineItem> outline() override {
    std::lock_guard g(engine_mutex());
    std::vector<OutlineItem> out;
    std::unordered_set<FPDF_BOOKMARK> seen;
    int budget = 50000;
    walk_outline(nullptr, out, 0, seen, budget);
    return out;
  }

  Dest resolve_dest(const Dest& d) override {
    if (!d.raw || d.page < 0 || d.page >= page_count()) return d;
    std::lock_guard g(engine_mutex());
    Dest r = d;
    r.raw = false;
    if (CachedPage* cp = get_page(d.page)) {
      float px, py;
      cp->to_page.apply(d.raw_x, d.raw_y, px, py);
      r.y = std::clamp(py, 0.f, sizes_[d.page].h);
    }
    return r;
  }

 protected:
  PageText load_text(int page) override {
    PageText t;
    CachedPage* cp = get_page(page);
    if (!cp) return t;
    const Affine m = cp->to_page;
    FPDF_TEXTPAGE tp = FPDFText_LoadPage(cp->page);
    if (!tp) return t;
    int n = FPDFText_CountChars(tp);
    t.text.reserve(n);
    t.boxes.reserve(n);
    for (int i = 0; i < n; ++i) {
      unsigned int u = FPDFText_GetUnicode(tp, i);
      RectF box;
      FS_RECTF r;
      if (FPDFText_IsGenerated(tp, i) != 1 && FPDFText_GetLooseCharBox(tp, i, &r)) {
        box = m.map_rect(r.left, r.top, r.right, r.bottom);
      }
      if (u == 0) u = L' ';
      if (u == 2) u = 0xAD;  // PDFium marks a hyphen that breaks a word at the line end with U+0002
      if (u >= 0x10000 && u <= 0x10FFFF) {
        u -= 0x10000;
        t.text.push_back((wchar_t)(0xD800 + (u >> 10)));
        t.text.push_back((wchar_t)(0xDC00 + (u & 0x3FF)));
        t.boxes.push_back(box);
        t.boxes.push_back(box);
      } else {
        t.text.push_back((wchar_t)u);
        t.boxes.push_back(box);
      }
    }
    FPDFText_ClosePage(tp);
    return t;
  }

  std::vector<Link> load_links(int page) override {
    std::vector<Link> links;
    CachedPage* cp = get_page(page);
    if (!cp) return links;
    const Affine m = cp->to_page;
    int pos = 0;
    FPDF_LINK link = nullptr;
    while (FPDFLink_Enumerate(cp->page, &pos, &link)) {
      FS_RECTF r;
      if (!FPDFLink_GetAnnotRect(link, &r)) continue;
      Link l;
      l.rect = m.map_rect(r.left, r.top, r.right, r.bottom);
      FPDF_DEST dest = FPDFLink_GetDest(doc_, link);
      if (!dest) {
        if (FPDF_ACTION a = FPDFLink_GetAction(link)) {
          switch (FPDFAction_GetType(a)) {
            case PDFACTION_GOTO:
              dest = FPDFAction_GetDest(doc_, a);
              break;
            case PDFACTION_URI: {
              unsigned long len = FPDFAction_GetURIPath(doc_, a, nullptr, 0);
              if (len > 1) {
                std::string s(len, '\0');
                FPDFAction_GetURIPath(doc_, a, s.data(), len);
                s.resize(len - 1);
                l.uri = utf8_to_wide(s);
              }
              break;
            }
            case PDFACTION_LAUNCH:
            case PDFACTION_REMOTEGOTO: {
              unsigned long len = FPDFAction_GetFilePath(a, nullptr, 0);
              if (len > 1) {
                std::string s(len, '\0');
                FPDFAction_GetFilePath(a, s.data(), len);
                s.resize(len - 1);
                l.uri = utf8_to_wide(s);
              }
              break;
            }
          }
        }
      }
      if (dest) l.dest = make_dest(dest);
      if (l.dest.page >= 0 || !l.uri.empty()) links.push_back(std::move(l));
    }

    // URLs written as plain text.
    if (FPDF_TEXTPAGE tp = FPDFText_LoadPage(cp->page)) {
      if (FPDF_PAGELINK pl = FPDFLink_LoadWebLinks(tp)) {
        int count = FPDFLink_CountWebLinks(pl);
        for (int i = 0; i < count; ++i) {
          int len = FPDFLink_GetURL(pl, i, nullptr, 0);
          if (len <= 1) continue;
          std::wstring url(len, L'\0');
          FPDFLink_GetURL(pl, i, (unsigned short*)url.data(), len);
          url.resize(len - 1);
          int rects = FPDFLink_CountRects(pl, i);
          for (int k = 0; k < rects; ++k) {
            double l, t, r, b;
            if (!FPDFLink_GetRect(pl, i, k, &l, &t, &r, &b)) continue;
            Link wl;
            wl.rect = m.map_rect(l, t, r, b);
            wl.uri = url;
            links.push_back(std::move(wl));
          }
        }
        FPDFLink_CloseWebLinks(pl);
      }
      FPDFText_ClosePage(tp);
    }
    return links;
  }

 private:
  struct CachedPage {
    int index;
    FPDF_PAGE page;
    Affine to_page;
  };

  void close_page(FPDF_PAGE p) {
    if (form_) FORM_OnBeforeClosePage(p, form_);
    FPDF_ClosePage(p);
  }

  // Engine lock must be held. The returned pointer is valid until the next get_page() call.
  CachedPage* get_page(int index) {
    if (index < 0 || index >= page_count()) return nullptr;
    for (size_t i = 0; i < pages_.size(); ++i) {
      if (pages_[i].index == index) {
        if (i + 1 != pages_.size()) {
          CachedPage p = pages_[i];
          pages_.erase(pages_.begin() + i);
          pages_.push_back(p);
        }
        return &pages_.back();
      }
    }
    FPDF_PAGE p = FPDF_LoadPage(doc_, index);
    if (!p) return nullptr;
    if (form_) FORM_OnAfterLoadPage(p, form_);
    if (pages_.size() >= kMaxPages) {
      close_page(pages_.front().page);
      pages_.erase(pages_.begin());
    }
    pages_.push_back({index, p, compute_affine(p, sizes_[index])});
    return &pages_.back();
  }

  static Affine compute_affine(FPDF_PAGE p, SizeF sz) {
    // Probe the page display matrix at 16 px/pt to keep rounding errors small.
    const int S = 16;
    int sw = (int)(sz.w * S + 0.5f), sh = (int)(sz.h * S + 0.5f);
    int x0, y0, x1, y1, x2, y2;
    FPDF_PageToDevice(p, 0, 0, sw, sh, 0, 0.0, 0.0, &x0, &y0);
    FPDF_PageToDevice(p, 0, 0, sw, sh, 0, 1000.0, 0.0, &x1, &y1);
    FPDF_PageToDevice(p, 0, 0, sw, sh, 0, 0.0, 1000.0, &x2, &y2);
    Affine m;
    m.e = x0 / (double)S;
    m.f = y0 / (double)S;
    m.a = (x1 - x0) / (1000.0 * S);
    m.b = (y1 - y0) / (1000.0 * S);
    m.c = (x2 - x0) / (1000.0 * S);
    m.d = (y2 - y0) / (1000.0 * S);
    return m;
  }

  Dest make_dest(FPDF_DEST dest) {
    Dest d;
    d.page = FPDFDest_GetDestPageIndex(doc_, dest);
    FPDF_BOOL has_x = 0, has_y = 0, has_zoom = 0;
    FS_FLOAT x = 0, y = 0, zoom = 0;
    if (d.page >= 0 && FPDFDest_GetLocationInPage(dest, &has_x, &has_y, &has_zoom, &x, &y, &zoom) && has_y) {
      d.raw = true;
      d.raw_x = has_x ? x : 0;
      d.raw_y = y;
    }
    return d;
  }

  void walk_outline(FPDF_BOOKMARK parent, std::vector<OutlineItem>& out, int depth,
                    std::unordered_set<FPDF_BOOKMARK>& seen, int& budget) {
    if (depth > 64) return;
    for (FPDF_BOOKMARK b = FPDFBookmark_GetFirstChild(doc_, parent); b && budget > 0;
         b = FPDFBookmark_GetNextSibling(doc_, b)) {
      if (!seen.insert(b).second) break;  // malformed (cyclic) outline
      --budget;
      OutlineItem it;
      unsigned long len = FPDFBookmark_GetTitle(b, nullptr, 0);
      if (len > 2) {
        std::wstring s(len / 2, L'\0');
        FPDFBookmark_GetTitle(b, s.data(), len);
        s.resize(len / 2 - 1);
        it.title = clean_title(std::move(s));
      }
      FPDF_DEST dest = FPDFBookmark_GetDest(doc_, b);
      if (!dest) {
        FPDF_ACTION a = FPDFBookmark_GetAction(b);
        if (a && FPDFAction_GetType(a) == PDFACTION_GOTO) dest = FPDFAction_GetDest(doc_, a);
      }
      if (dest) it.dest = make_dest(dest);
      it.open = FPDFBookmark_GetCount(b) > 0;
      walk_outline(b, it.children, depth + 1, seen, budget);
      out.push_back(std::move(it));
    }
  }

  static constexpr size_t kMaxPages = 8;
  HANDLE file_ = INVALID_HANDLE_VALUE;
  FPDF_DOCUMENT doc_ = nullptr;
  FPDF_FORMHANDLE form_ = nullptr;
  FPDF_FORMFILLINFO form_info_{};
  std::vector<CachedPage> pages_;
};

}  // namespace

void set_crisp_text(bool on) { g_crisp_text = on; }

OpenResult open_pdf(const std::wstring& path, const std::string& password) {
  auto doc = std::make_unique<PdfDocument>();
  OpenResult r = doc->open(path, password);
  if (r.error.empty()) r.doc = std::move(doc);
  return r;
}

}  // namespace lv
