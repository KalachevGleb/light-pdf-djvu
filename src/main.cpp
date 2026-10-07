// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <windows.h>

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "main_window.h"
#include "settings.h"
#include "theme.h"
#include "util.h"

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show_cmd) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> files;
  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    if (a == L"--register" || a == L"--unregister") {
      bool ok = lv::register_file_types(a == L"--register");
      LocalFree(argv);
      return ok ? 0 : 1;
    }
    files.push_back(a);
  }
  LocalFree(argv);
  // A file that is already open elsewhere just brings that window to the front.
  if (files.size() == 1 && lv::activate_existing_window(files[0])) return 0;

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);
  lv::theme_init();
  lv::settings().load();

  lv::MainWindow::register_class(inst);
  lv::MainWindow window;
  if (!window.create(inst, show_cmd)) return 1;
  // One window per document: extra files get their own processes.
  for (size_t i = 1; i < files.size(); ++i) lv::spawn_self(files[i]);
  if (!files.empty()) window.open(files[0]);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!TranslateAcceleratorW(window.hwnd(), window.accelerators(), &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  CoUninitialize();
  return (int)msg.wParam;
}
