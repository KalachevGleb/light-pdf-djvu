// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print.h"

#include <objbase.h>  // PRINTDLGEX needs the COM declarations

#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#include "util.h"

namespace lv {

namespace {

std::atomic<int> g_jobs{0};

constexpr int kMaxPrintDpi = 300;
constexpr int kBandHeight = 512;

bool print_page(HDC dc, Document& doc, int page) {
  const int dev_w = GetDeviceCaps(dc, HORZRES), dev_h = GetDeviceCaps(dc, VERTRES);
  const int dpi_x = GetDeviceCaps(dc, LOGPIXELSX);
  SizeF sz = doc.page_size(page);
  // Turn landscape pages on portrait paper (and vice versa).
  const int rot = ((sz.w > sz.h) != (dev_w > dev_h)) ? 1 : 0;
  const float pw = rot ? sz.h : sz.w, ph = rot ? sz.w : sz.h;
  // Device pixels per point: actual size, shrunk to fit the printable area if needed.
  float scale = std::min({dpi_x / 72.f, (float)dev_w / pw, (float)dev_h / ph});
  const int dst_w = std::max(1, (int)(pw * scale)), dst_h = std::max(1, (int)(ph * scale));
  const int dst_x = (dev_w - dst_w) / 2, dst_y = (dev_h - dst_h) / 2;
  // Render at no more than kMaxPrintDpi and let the driver scale up.
  const float render_scale = std::min(scale, kMaxPrintDpi / 72.f);
  const int full_w = std::max(1, (int)(pw * render_scale)), full_h = std::max(1, (int)(ph * render_scale));

  if (StartPage(dc) <= 0) return false;
  std::vector<uint8_t> buf((size_t)full_w * kBandHeight * 4);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  SetStretchBltMode(dc, HALFTONE);
  SetBrushOrgEx(dc, 0, 0, nullptr);
  for (int y = 0; y < full_h; y += kBandHeight) {
    const int h = std::min(kBandHeight, full_h - y);
    doc.render(page, full_w, full_h, rot, 0, y, full_w, h, buf.data(), nullptr, true);
    bi.bmiHeader.biWidth = full_w;
    bi.bmiHeader.biHeight = -h;
    const int y0 = dst_y + (int)((int64_t)y * dst_h / full_h);
    const int y1 = dst_y + (int)((int64_t)(y + h) * dst_h / full_h);
    StretchDIBits(dc, dst_x, y0, dst_w, y1 - y0, 0, 0, full_w, h, buf.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
  }
  return EndPage(dc) > 0;
}

}  // namespace

int active_print_jobs() { return g_jobs.load(); }

bool print_pages(HDC dc, Document& doc, const std::vector<int>& pages, const std::wstring& title,
                 const wchar_t* output, const std::function<void(size_t)>& progress) {
  DOCINFOW di{sizeof(di)};
  di.lpszDocName = title.c_str();
  di.lpszOutput = output;
  bool ok = StartDocW(dc, &di) > 0;
  for (size_t i = 0; ok && i < pages.size(); ++i) {
    if (progress) progress(i);
    ok = print_page(dc, doc, pages[i]);
  }
  if (ok) EndDoc(dc);
  else AbortDoc(dc);
  return ok;
}

void print_document(HWND owner, UINT progress_msg, std::shared_ptr<Document> doc, int current_page) {
  if (!doc) return;
  const int n = doc->page_count();
  PRINTPAGERANGE ranges[32] = {};
  PRINTDLGEXW pd{sizeof(pd)};
  pd.hwndOwner = owner;
  pd.Flags = PD_RETURNDC | PD_NOSELECTION | PD_USEDEVMODECOPIESANDCOLLATE;
  pd.nMaxPageRanges = (DWORD)std::size(ranges);
  pd.lpPageRanges = ranges;
  pd.nMinPage = 1;
  pd.nMaxPage = (DWORD)n;
  pd.nCopies = 1;
  pd.nStartPage = START_PAGE_GENERAL;
  HRESULT hr = PrintDlgExW(&pd);
  auto cleanup = [&] {
    if (pd.hDevMode) GlobalFree(pd.hDevMode);
    if (pd.hDevNames) GlobalFree(pd.hDevNames);
  };
  if (FAILED(hr) || pd.dwResultAction != PD_RESULT_PRINT || !pd.hDC) {
    if (pd.hDC) DeleteDC(pd.hDC);
    cleanup();
    return;
  }

  std::vector<int> pages;
  if (pd.Flags & PD_PAGENUMS) {
    for (DWORD i = 0; i < pd.nPageRanges; ++i) {
      int a = std::clamp((int)ranges[i].nFromPage, 1, n), b = std::clamp((int)ranges[i].nToPage, 1, n);
      for (int p = a; p <= b; ++p) pages.push_back(p - 1);
    }
  } else if (pd.Flags & PD_CURRENTPAGE) {
    pages.push_back(current_page);
  } else {
    for (int p = 0; p < n; ++p) pages.push_back(p);
  }
  // The driver did not take over copies: repeat the pages ourselves.
  std::vector<int> job;
  for (DWORD c = 0; c < std::max<DWORD>(1, pd.nCopies); ++c) job.insert(job.end(), pages.begin(), pages.end());
  HDC dc = pd.hDC;
  cleanup();
  if (job.empty()) {
    DeleteDC(dc);
    return;
  }

  ++g_jobs;
  std::thread([owner, progress_msg, doc, dc, job] {
    bool ok = print_pages(dc, *doc, job, file_name_of(doc->path()), nullptr,
                          [&](size_t i) { PostMessageW(owner, progress_msg, i, job.size()); });
    DeleteDC(dc);
    --g_jobs;
    PostMessageW(owner, progress_msg, job.size(), ok ? job.size() : 0);
  }).detach();
}

}  // namespace lv
