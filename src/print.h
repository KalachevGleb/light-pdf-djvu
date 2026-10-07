// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "document.h"

namespace lv {

// Prints `pages` on a printer DC as one job. `output` names a file for print-to-file drivers (may be
// null). `progress(i)` is called before page i. Returns false if the job failed.
bool print_pages(HDC dc, Document& doc, const std::vector<int>& pages, const std::wstring& title,
                 const wchar_t* output, const std::function<void(size_t)>& progress);

// Shows the print dialog and prints in a background thread. Progress is reported to `owner` as
// `progress_msg` with wParam = pages done, lParam = pages total (wParam == lParam when finished,
// lParam == 0 on failure).
void print_document(HWND owner, UINT progress_msg, std::shared_ptr<Document> doc, int current_page);

// Number of print jobs still running in this process.
int active_print_jobs();

}  // namespace lv
