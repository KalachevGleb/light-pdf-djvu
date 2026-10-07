// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Engine smoke test (console).
//   lightview_smoke <file-or-folder> [--compare]
//       Opens every PDF/DjVu, renders the first page, extracts text/links/outline, prints timings.
//       --compare also checks DjVu page sizes against ddjvu's own (slow) page info.
//   lightview_smoke --print <file> <out.pdf> [first last]
//       Prints pages (1-based, inclusive) through "Microsoft Print to PDF" into out.pdf.
//   lightview_smoke --links <file> <page>
//       Lists the links of a page (1-based): rectangle in points and target.
#include <windows.h>

#include <ddjvuapi.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "document.h"
#include "print.h"
#include "util.h"

using namespace lv;
namespace fs = std::filesystem;

namespace {

double ms_since(LARGE_INTEGER t0) {
  LARGE_INTEGER f, t;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t);
  return (t.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
}

std::vector<SizeF> djvu_sizes_slow(const std::wstring& path) {
  std::vector<SizeF> out;
  ddjvu_context_t* ctx = ddjvu_context_create("smoke");
  ddjvu_document_t* doc = ddjvu_document_create_by_filename_utf8(ctx, wide_to_utf8(path).c_str(), 0);
  auto pump = [&] {
    ddjvu_message_wait(ctx);
    while (ddjvu_message_peek(ctx)) ddjvu_message_pop(ctx);
  };
  while (!ddjvu_document_decoding_done(doc)) pump();
  int n = ddjvu_document_get_pagenum(doc);
  for (int i = 0; i < n; ++i) {
    ddjvu_pageinfo_t pi{};
    while (ddjvu_document_get_pageinfo(doc, i, &pi) < DDJVU_JOB_OK) pump();
    int dpi = pi.dpi > 0 ? pi.dpi : 300;
    out.push_back({pi.width * 72.f / dpi, pi.height * 72.f / dpi});
  }
  ddjvu_document_release(doc);
  ddjvu_context_release(ctx);
  return out;
}

int print_test(const std::wstring& file, const std::wstring& out, int first, int last) {
  OpenResult r = open_document(file, "");
  if (!r.doc) {
    wprintf(L"cannot open: %s\n", r.error.c_str());
    return 1;
  }
  HDC dc = CreateDCW(L"WINSPOOL", L"Microsoft Print to PDF", nullptr, nullptr);
  if (!dc) {
    wprintf(L"no \"Microsoft Print to PDF\" printer\n");
    return 1;
  }
  std::vector<int> pages;
  for (int p = std::max(1, first); p <= std::min(last, r.doc->page_count()); ++p) pages.push_back(p - 1);
  LARGE_INTEGER t0;
  QueryPerformanceCounter(&t0);
  bool ok = print_pages(dc, *r.doc, pages, L"smoke", out.c_str(), nullptr);
  DeleteDC(dc);
  wprintf(L"printed %zu pages: %s, %.0f ms\n", pages.size(), ok ? L"ok" : L"FAILED", ms_since(t0));
  return ok ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc >= 4 && std::wstring(argv[1]) == L"--links") {
    OpenResult r = open_document(argv[2], "");
    if (!r.doc) return 1;
    const int page = _wtoi(argv[3]) - 1;
    for (const Link& l : *r.doc->links(page)) {
      Dest d = r.doc->resolve_dest(l.dest);
      wprintf(L"[%6.1f %6.1f %6.1f %6.1f] -> page %d y %.1f %s\n", l.rect.x0, l.rect.y0, l.rect.x1, l.rect.y1,
              d.page + 1, d.y, l.uri.c_str());
    }
    return 0;
  }
  if (argc >= 4 && std::wstring(argv[1]) == L"--print") {
    int first = argc >= 6 ? _wtoi(argv[4]) : 1, last = argc >= 6 ? _wtoi(argv[5]) : 3;
    return print_test(argv[2], full_path(argv[3]), first, last);
  }
  if (argc < 2) {
    wprintf(L"usage: lightview_smoke <file-or-folder> [--compare] | --print <file> <out.pdf> [first last]\n");
    return 1;
  }
  const bool compare = argc > 2 && std::wstring(argv[2]) == L"--compare";
  std::vector<fs::path> files;
  fs::path root = argv[1];
  if (fs::is_regular_file(root)) {
    files.push_back(root);
  } else {
    for (auto& e : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
      auto ext = e.path().extension().wstring();
      for (auto& c : ext) c = towlower(c);
      if (ext == L".pdf" || ext == L".djvu" || ext == L".djv") files.push_back(e.path());
    }
  }
  int failed = 0, mismatched = 0;
  double worst_open = 0;
  std::wstring worst_name;
  for (auto& f : files) {
    const std::wstring path = f.wstring();
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    OpenResult r = open_document(path, "");
    const double t_open = ms_since(t0);
    if (!r.doc) {
      ++failed;
      wprintf(L"FAIL  %s: %s%s\n", path.c_str(), r.error.c_str(), r.need_password ? L" (password)" : L"");
      continue;
    }
    Document& d = *r.doc;
    if (t_open > worst_open) {
      worst_open = t_open;
      worst_name = path;
    }
    QueryPerformanceCounter(&t0);
    SizeF s0 = d.page_size(0);
    int w = std::max(1, (int)(s0.w * 1.5f)), h = std::max(1, (int)(s0.h * 1.5f));
    std::vector<uint8_t> buf((size_t)w * h * 4);
    d.render(0, w, h, 0, 0, 0, w, h, buf.data(), nullptr);
    const double t_render = ms_since(t0);
    QueryPerformanceCounter(&t0);
    size_t chars = 0, links = 0;
    for (int p = 0; p < std::min(5, d.page_count()); ++p) {
      chars += d.text(p)->text.size();
      links += d.links(p)->size();
    }
    const double t_text = ms_since(t0);
    const size_t outline = d.outline().size();
    wprintf(L"%-5s %5d pages  open %7.1f ms  render %6.1f ms  text+links %6.1f ms (%zu chars, %zu links)  outline %zu  %s\n",
            d.kind(), d.page_count(), t_open, t_render, t_text, chars, links, outline, f.filename().c_str());
    if (compare && wcscmp(d.kind(), L"DjVu") == 0) {
      auto slow = djvu_sizes_slow(path);
      bool same = (int)slow.size() == d.page_count();
      for (int i = 0; same && i < d.page_count(); ++i) {
        same = fabsf(slow[i].w - d.page_size(i).w) < 0.01f && fabsf(slow[i].h - d.page_size(i).h) < 0.01f;
      }
      if (!same) {
        ++mismatched;
        wprintf(L"      MISMATCH with ddjvu page sizes\n");
      }
    }
    fflush(stdout);
  }
  wprintf(L"\n%zu files, %d failed to open, %d DjVu size mismatches; slowest open %.1f ms: %s\n", files.size(), failed,
          mismatched, worst_open, worst_name.c_str());
  return failed || mismatched ? 2 : 0;
}
