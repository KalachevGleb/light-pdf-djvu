// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "main_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>

#include "commands.h"
#include "print.h"
#include "resource.h"
#include "settings.h"
#include "theme.h"

namespace lv {

namespace {

constexpr wchar_t kClass[] = L"LightView.Main";
constexpr UINT kMsgFileChanged = WM_APP + 20;
constexpr UINT kMsgPrint = WM_APP + 21;
constexpr UINT_PTR kTimerReload = 10;
constexpr int kSplitter = 4;
constexpr ULONG_PTR kCopyDataIsOpen = 0x4C564F50;  // 'LVOP': "do you show this file?"

uint64_t now_stamp() {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

struct PasswordDialog {
  std::wstring prompt;
  std::wstring result;
};

INT_PTR CALLBACK password_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  auto* data = (PasswordDialog*)GetWindowLongPtrW(dlg, DWLP_USER);
  switch (msg) {
    case WM_INITDIALOG:
      SetWindowLongPtrW(dlg, DWLP_USER, lp);
      SetDlgItemTextW(dlg, IDC_PROMPT, ((PasswordDialog*)lp)->prompt.c_str());
      return TRUE;
    case WM_COMMAND:
      if (LOWORD(wp) == IDOK) {
        data->result = window_text(GetDlgItem(dlg, IDC_PASSWORD));
        EndDialog(dlg, IDOK);
        return TRUE;
      }
      if (LOWORD(wp) == IDCANCEL) {
        EndDialog(dlg, IDCANCEL);
        return TRUE;
      }
      break;
  }
  return FALSE;
}

}  // namespace

// ---------------------------------------------------------------------------------------------

void DirWatcher::start(HWND notify, UINT msg, const std::wstring& dir) {
  stop();
  HANDLE change = FindFirstChangeNotificationW(
      dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE);
  if (change == INVALID_HANDLE_VALUE) return;
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE stop_ev = stop_event_;
  thread_ = std::thread([notify, msg, change, stop_ev] {
    HANDLE handles[2] = {stop_ev, change};
    while (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
      PostMessageW(notify, msg, 0, 0);
      if (!FindNextChangeNotification(change)) break;
    }
    FindCloseChangeNotification(change);
  });
}

void DirWatcher::stop() {
  if (!stop_event_) return;
  SetEvent(stop_event_);
  thread_.join();
  CloseHandle(stop_event_);
  stop_event_ = nullptr;
}

// ---------------------------------------------------------------------------------------------

void MainWindow::register_class(HINSTANCE inst) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                               GetSystemMetrics(SM_CYICON), 0);
  wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                 GetSystemMetrics(SM_CYSMICON), 0);
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);
  Toolbar::register_class(inst);
  Canvas::register_class(inst);
}

bool MainWindow::create(HINSTANCE inst, int show_cmd) {
  inst_ = inst;
  const Settings& st = settings();
  show_sidebar_ = st.sidebar;
  sidebar_w_ = st.sidebar_w;
  hwnd_ = CreateWindowExW(0, kClass, L"LightView", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                          CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, inst, this);
  if (!hwnd_) return false;
  canvas_.set_invert(st.invert);
  set_crisp_text(st.crisp_text);

  const bool normal_show = show_cmd == SW_SHOWNORMAL || show_cmd == SW_SHOWDEFAULT || show_cmd == SW_SHOW;
  if (st.have_window && MonitorFromRect(&st.window, MONITOR_DEFAULTTONULL)) {
    WINDOWPLACEMENT wp{sizeof(wp)};
    wp.rcNormalPosition = st.window;
    wp.showCmd = (st.maximized && normal_show) ? SW_SHOWMAXIMIZED : show_cmd;
    SetWindowPlacement(hwnd_, &wp);
  } else {
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = std::min(s(1100), (int)(work.right - work.left)), h = std::min(s(860), (int)(work.bottom - work.top));
    SetWindowPos(hwnd_, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2,
                 w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd_, show_cmd);
  }
  UpdateWindow(hwnd_);
  return true;
}

void MainWindow::apply_tree_theme() {
  const Theme& t = theme();
  apply_dark_controls(tree_);
  TreeView_SetBkColor(tree_, t.side_bg);
  TreeView_SetTextColor(tree_, t.side_fg);
  // TVS_LINESATROOT (expand buttons on top-level items) only works with TVS_HASLINES: hide the lines.
  TreeView_SetLineColor(tree_, t.side_bg);
  TreeView_SetExtendedStyle(tree_, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
  if (tree_font_) DeleteObject(tree_font_);
  NONCLIENTMETRICSW ncm{sizeof(ncm)};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_);
  tree_font_ = CreateFontIndirectW(&ncm.lfMessageFont);
  SendMessageW(tree_, WM_SETFONT, (WPARAM)tree_font_, TRUE);
  TreeView_SetItemHeight(tree_, std::abs(ncm.lfMessageFont.lfHeight) + s(10));
}

bool MainWindow::open(const std::wstring& path_in) {
  const std::wstring path = full_path(path_in);
  std::string password;
  OpenResult r;
  {
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    r = open_document(path, password);
    SetCursor(old);
  }
  while (!r.doc && r.need_password) {
    std::wstring pw;
    if (!ask_password(file_name_of(path), !password.empty(), pw)) return false;
    password = wide_to_utf8(pw);
    r = open_document(path, password);
  }
  if (!r.doc) {
    MessageBoxW(hwnd_, (path + L"\n\n" + r.error).c_str(), L"LightView", MB_ICONERROR);
    return false;
  }
  if (doc_) save_settings();
  path_ = path;
  password_ = password;
  doc_ = std::shared_ptr<Document>(r.doc.release());
  stamp_ = file_stamp(path_);
  KillTimer(hwnd_, kTimerReload);

  const FileEntry* fe = settings().find(path_);
  ViewState view = fe ? fe->view : ViewState{};
  canvas_.set_document(doc_, fe ? &view : nullptr, false);
  load_outline();
  watcher_.start(hwnd_, kMsgFileChanged, dir_of(path_));
  settings().touch({path_, canvas_.view_state(), now_stamp()});
  update_title();
  update_ui();
  SetFocus(canvas_.hwnd());
  return true;
}

void MainWindow::reload(bool quiet) {
  if (path_.empty()) return;
  OpenResult r = open_document(path_, password_);
  if (!r.doc) {
    if (!quiet) {
      MessageBoxW(hwnd_, (path_ + L"\n\n" + r.error).c_str(), L"LightView", MB_ICONERROR);
    } else if (++reload_retries_ < 10) {
      SetTimer(hwnd_, kTimerReload, 700, nullptr);  // the file may still be being written
    }
    return;
  }
  reload_retries_ = 0;
  ViewState view = canvas_.view_state();
  doc_ = std::shared_ptr<Document>(r.doc.release());
  stamp_ = file_stamp(path_);
  canvas_.set_document(doc_, &view, true);
  load_outline();
  update_ui();
}

bool MainWindow::ask_password(const std::wstring& name, bool retry, std::wstring& out) {
  PasswordDialog d;
  d.prompt = (retry ? L"Неверный пароль. " : L"") + std::wstring(L"Документ «") + name + L"» защищён паролем:";
  if (DialogBoxParamW(inst_, MAKEINTRESOURCEW(IDD_PASSWORD), hwnd_, password_proc, (LPARAM)&d) != IDOK) return false;
  out = d.result;
  return true;
}

void MainWindow::open_dialog() {
  std::wstring buf(32768, L'\0');
  std::wstring dir = dir_of(path_);
  OPENFILENAMEW ofn{sizeof(ofn)};
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFilter = L"PDF и DjVu\0*.pdf;*.djvu;*.djv\0PDF\0*.pdf\0DjVu\0*.djvu;*.djv\0Все файлы\0*.*\0";
  ofn.lpstrFile = buf.data();
  ofn.nMaxFile = (DWORD)buf.size();
  ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameW(&ofn)) return;
  // Multi-select returns "dir\0name1\0name2\0\0"; a single file is a full path.
  std::vector<std::wstring> files;
  const wchar_t* p = buf.c_str();
  std::wstring first = p;
  p += first.size() + 1;
  if (!*p) {
    files.push_back(first);
  } else {
    for (; *p; p += wcslen(p) + 1) files.push_back(first + L"\\" + p);
  }
  for (size_t i = 1; i < files.size(); ++i) spawn_self(files[i]);
  open(files[0]);
}

void MainWindow::load_outline() {
  filling_tree_ = true;
  SendMessageW(tree_, WM_SETREDRAW, FALSE, 0);
  TreeView_DeleteAllItems(tree_);
  outline_dests_.clear();
  std::vector<OutlineItem> items;
  if (doc_) items = doc_->outline();
  size_t total = 0;
  std::vector<const std::vector<OutlineItem>*> stack = {&items};
  while (!stack.empty()) {
    auto* v = stack.back();
    stack.pop_back();
    total += v->size();
    for (auto& it : *v) stack.push_back(&it.children);
  }
  insert_outline(items, TVI_ROOT, 0, total);
  SendMessageW(tree_, WM_SETREDRAW, TRUE, 0);
  if (HTREEITEM first = TreeView_GetRoot(tree_)) TreeView_EnsureVisible(tree_, first);
  filling_tree_ = false;
  has_outline_ = !items.empty();
  layout();
}

void MainWindow::insert_outline(const std::vector<OutlineItem>& items, HTREEITEM parent, int depth, size_t total) {
  for (const OutlineItem& it : items) {
    TVINSERTSTRUCTW is{};
    is.hParent = parent;
    is.hInsertAfter = TVI_LAST;
    is.item.mask = TVIF_TEXT | TVIF_PARAM;
    std::wstring title = it.title.empty() ? L"(без названия)" : it.title;
    is.item.pszText = title.data();
    is.item.lParam = (LPARAM)outline_dests_.size();
    outline_dests_.push_back(it.dest);
    HTREEITEM h = TreeView_InsertItem(tree_, &is);
    if (!it.children.empty()) {
      insert_outline(it.children, h, depth + 1, total);
      // Small outlines are shown fully expanded, big ones as the document asks.
      if (it.open || total <= 40 || (depth == 0 && total <= 300)) TreeView_Expand(tree_, h, TVE_EXPAND);
    }
  }
}

void MainWindow::navigate_outline(HTREEITEM item) {
  if (!item) return;
  TVITEMW it{};
  it.mask = TVIF_PARAM;
  it.hItem = item;
  if (!TreeView_GetItem(tree_, &it)) return;
  size_t i = (size_t)it.lParam;
  if (i < outline_dests_.size()) canvas_.go_to_dest(outline_dests_[i]);
}

RECT MainWindow::splitter_rect() const {
  if (!sidebar_visible()) return {};
  RECT rc;
  GetWindowRect(tree_, &rc);
  MapWindowPoints(nullptr, hwnd_, (POINT*)&rc, 2);
  return {rc.right, rc.top, rc.right + s(kSplitter), rc.bottom};
}

void MainWindow::layout() {
  if (!hwnd_ || !tree_) return;
  RECT rc;
  GetClientRect(hwnd_, &rc);
  int top = 0;
  if (fullscreen_) {
    ShowWindow(toolbar_.hwnd(), SW_HIDE);
  } else {
    top = toolbar_.height();
    SetWindowPos(toolbar_.hwnd(), nullptr, 0, 0, rc.right, top, SWP_NOZORDER | SWP_SHOWWINDOW);
  }
  int left = 0;
  if (sidebar_visible()) {
    int sw = std::clamp(s(sidebar_w_), s(120), std::max(s(120), (int)rc.right / 2));
    SetWindowPos(tree_, nullptr, 0, top, sw, rc.bottom - top, SWP_NOZORDER | SWP_SHOWWINDOW);
    left = sw + s(kSplitter);
  } else {
    ShowWindow(tree_, SW_HIDE);
  }
  SetWindowPos(canvas_.hwnd(), nullptr, left, top, rc.right - left, rc.bottom - top, SWP_NOZORDER);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::update_title() {
  std::wstring t = path_.empty() ? L"LightView" : file_name_of(path_) + L" — LightView";
  SetWindowTextW(hwnd_, t.c_str());
}

void MainWindow::update_ui() {
  const bool has = (bool)doc_;
  toolbar_.set_page(canvas_.current_page(), has ? doc_->page_count() : 0);
  toolbar_.set_zoom(has ? canvas_.zoom_percent() : 0);
  toolbar_.set_enabled(CMD_TOC, has_outline_);
  toolbar_.set_checked(CMD_TOC, sidebar_visible());
  toolbar_.set_checked(CMD_FIT_WIDTH, has && canvas_.zoom_mode() == ZoomMode::FitWidth);
  toolbar_.set_checked(CMD_FIT_PAGE, has && canvas_.zoom_mode() == ZoomMode::FitPage);
  toolbar_.set_checked(CMD_FACING, canvas_.facing());
  toolbar_.set_checked(CMD_INVERT, canvas_.invert());
  toolbar_.set_checked(CMD_FULLSCREEN, fullscreen_);
  for (int cmd : {CMD_PRINT, CMD_PREV_PAGE, CMD_NEXT_PAGE, CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_FIT_WIDTH, CMD_FIT_PAGE,
                  CMD_ROTATE_RIGHT, CMD_FACING, CMD_FIND_PREV, CMD_FIND_NEXT}) {
    toolbar_.set_enabled(cmd, has);
  }
}

void MainWindow::remember_position() {
  if (doc_ && !path_.empty()) settings().touch({path_, canvas_.view_state(), now_stamp()});
}

void MainWindow::save_settings() {
  remember_position();
  Settings& st = settings();
  WINDOWPLACEMENT wp{sizeof(wp)};
  if (fullscreen_) wp = saved_placement_;
  else GetWindowPlacement(hwnd_, &wp);
  st.have_window = true;
  st.window = wp.rcNormalPosition;
  st.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
  st.sidebar = show_sidebar_;
  st.sidebar_w = sidebar_w_;
  st.invert = canvas_.invert();
  st.save(nullptr);
}

void MainWindow::toggle_fullscreen() {
  if (!fullscreen_) {
    saved_placement_.length = sizeof(saved_placement_);
    GetWindowPlacement(hwnd_, &saved_placement_);
    saved_style_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
    fullscreen_ = true;
    SetWindowLongPtrW(hwnd_, GWL_STYLE, saved_style_ & ~WS_OVERLAPPEDWINDOW);
    SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  } else {
    fullscreen_ = false;
    SetWindowLongPtrW(hwnd_, GWL_STYLE, saved_style_);
    SetWindowPlacement(hwnd_, &saved_placement_);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  }
  layout();
  update_ui();
  SetFocus(canvas_.hwnd());
}

void MainWindow::show_help() {
  MessageBoxW(hwnd_,
              L"Открыть: Ctrl+O или перетащить файл в окно\n"
              L"Печать: Ctrl+P\n"
              L"Перезагрузить: F5 (изменённый файл перезагружается сам)\n"
              L"\n"
              L"Листать: PgUp/PgDn, Пробел, стрелки, Home/End\n"
              L"Перейти на страницу: Ctrl+G\n"
              L"Вернуться к прежнему месту и обратно: боковые кнопки мыши, Alt+←/Alt+→\n"
              L"(место запоминается перед переходом по ссылке, оглавлению, номеру страницы\n"
              L"и перед прыжком полосой прокрутки)\n"
              L"Оглавление: F9\n"
              L"\n"
              L"Масштаб: Ctrl+колесо мыши, Ctrl+плюс/минус\n"
              L"Страница целиком: Ctrl+0, реальный размер: Ctrl+1, по ширине: Ctrl+2\n"
              L"Разворот (две страницы): Ctrl+3\n"
              L"Поворот: Ctrl+Shift+плюс/минус\n"
              L"Ночной режим: Ctrl+I\n"
              L"Полный экран: F11, выход: Esc\n"
              L"\n"
              L"Поиск: Ctrl+F, далее Enter/F3, назад Shift+Enter/Shift+F3\n"
              L"Выделить текст: тянуть мышью по тексту, слово: двойной щелчок\n"
              L"Копировать: Ctrl+C, выделить всё: Ctrl+A\n"
              L"Прокрутка мышью: тянуть за пустое место или средней кнопкой",
              L"LightView — клавиши", MB_ICONINFORMATION);
}

void MainWindow::register_app() {
  if (!register_file_types(true)) {
    MessageBoxW(hwnd_, L"Не удалось записать настройки в реестр.", L"LightView", MB_ICONERROR);
    return;
  }
  MessageBoxW(hwnd_,
              L"LightView добавлен в список «Открыть с помощью» для PDF и DjVu.\n\n"
              L"Чтобы открывать такие файлы по двойному щелчку: щёлкните файл правой кнопкой → "
              L"«Открыть с помощью» → «Выбрать другое приложение» → LightView → "
              L"отметьте «Всегда использовать это приложение».",
              L"LightView", MB_ICONINFORMATION);
}

// ---------------------------------------------------------------------------------------------
// Commands

void MainWindow::execute(int cmd) {
  switch (cmd) {
    case CMD_NONE: break;
    case CMD_OPEN: open_dialog(); break;
    case CMD_PRINT:
      if (doc_) print_document(hwnd_, kMsgPrint, doc_, canvas_.current_page());
      break;
    case CMD_RELOAD: reload(false); break;
    case CMD_CLOSE: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    case CMD_SHOW_IN_FOLDER:
      if (!path_.empty()) {
        std::wstring args = L"/select,\"" + path_ + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
      }
      break;
    case CMD_TOC:
      if (has_outline_) {
        show_sidebar_ = !sidebar_visible();
        layout();
        if (show_sidebar_) SetFocus(tree_);
      }
      break;
    case CMD_PREV_PAGE: canvas_.page_step(-1); break;
    case CMD_NEXT_PAGE: canvas_.page_step(1); break;
    case CMD_FIRST_PAGE: canvas_.first_page(); break;
    case CMD_LAST_PAGE: canvas_.last_page(); break;
    case CMD_GOTO_PAGE:
      if (fullscreen_) toggle_fullscreen();
      toolbar_.focus_page();
      break;
    case CMD_BACK: canvas_.back(); break;
    case CMD_FORWARD: canvas_.forward(); break;
    case CMD_ZOOM_IN: canvas_.zoom_step(1); break;
    case CMD_ZOOM_OUT: canvas_.zoom_step(-1); break;
    case CMD_FIT_WIDTH: canvas_.set_zoom_mode(ZoomMode::FitWidth); break;
    case CMD_FIT_PAGE: canvas_.set_zoom_mode(ZoomMode::FitPage); break;
    case CMD_ACTUAL_SIZE: canvas_.set_zoom_percent(100); break;
    case CMD_ROTATE_LEFT: canvas_.rotate(-1); break;
    case CMD_ROTATE_RIGHT: canvas_.rotate(1); break;
    case CMD_FACING: canvas_.set_facing(!canvas_.facing()); break;
    case CMD_INVERT: canvas_.set_invert(!canvas_.invert()); break;
    case CMD_CRISP_TEXT:
      settings().crisp_text = !settings().crisp_text;
      set_crisp_text(settings().crisp_text);
      canvas_.rerender();
      break;
    case CMD_FULLSCREEN: toggle_fullscreen(); break;
    case CMD_FIND:
      if (fullscreen_) toggle_fullscreen();
      toolbar_.focus_search();
      break;
    case CMD_FIND_NEXT:
    case CMD_FIND_PREV: {
      std::wstring q = toolbar_.search_text();
      if (q.empty() || fullscreen_) execute(CMD_FIND);
      else canvas_.find(q, cmd == CMD_FIND_NEXT);
      break;
    }
    case CMD_COPY: canvas_.copy(); break;
    case CMD_SELECT_ALL: canvas_.select_all(); break;
    case CMD_HELP: show_help(); break;
    case CMD_REGISTER: register_app(); break;
    case CMD_ESCAPE:
      if (fullscreen_) toggle_fullscreen();
      break;
    default:
      if (cmd >= CMD_RECENT_FIRST && cmd <= CMD_RECENT_LAST) {
        size_t i = (size_t)(cmd - CMD_RECENT_FIRST);
        if (i < recent_menu_.size()) open(recent_menu_[i]);
      }
      break;
  }
  update_ui();
}

void MainWindow::on_view_changed() { update_ui(); }

void MainWindow::on_canvas_escape() { execute(CMD_ESCAPE); }

void MainWindow::on_page_entered(const std::wstring& text) {
  int n = _wtoi(text.c_str());
  if (doc_ && n >= 1 && n <= doc_->page_count()) canvas_.go_to_page(n - 1);
  SetFocus(canvas_.hwnd());
  update_ui();
}

void MainWindow::on_zoom_entered(const std::wstring& text) {
  float z = (float)_wtof(text.c_str());
  if (doc_ && z > 0) canvas_.set_zoom_percent(z);
  SetFocus(canvas_.hwnd());
  update_ui();
}

void MainWindow::on_search(const std::wstring& text, bool forward) { canvas_.find(text, forward); }

void MainWindow::on_edit_escape() {
  SetFocus(canvas_.hwnd());
  update_ui();
}

void MainWindow::on_context_menu(POINT pt) {
  HMENU menu = CreatePopupMenu();
  HMENU recent = CreatePopupMenu();
  recent_menu_.clear();
  for (const FileEntry& f : settings().files) {
    if (recent_menu_.size() >= (size_t)(CMD_RECENT_LAST - CMD_RECENT_FIRST + 1)) break;
    if (same_path(f.path, path_)) continue;
    std::wstring label = file_name_of(f.path);
    for (size_t p = label.find(L'&'); p != std::wstring::npos; p = label.find(L'&', p + 2)) label.insert(p, 1, L'&');
    AppendMenuW(recent, MF_STRING, CMD_RECENT_FIRST + recent_menu_.size(), label.c_str());
    recent_menu_.push_back(f.path);
  }
  const bool has = (bool)doc_;
  const UINT on = MF_STRING, dis = MF_STRING | MF_GRAYED;
  auto check = [](bool c) { return c ? MF_CHECKED : MF_UNCHECKED; };
  AppendMenuW(menu, on, CMD_OPEN, L"Открыть…\tCtrl+O");
  AppendMenuW(menu, MF_POPUP | (recent_menu_.empty() ? MF_GRAYED : 0), (UINT_PTR)recent, L"Недавние");
  AppendMenuW(menu, has ? on : dis, CMD_PRINT, L"Печать…\tCtrl+P");
  AppendMenuW(menu, has ? on : dis, CMD_RELOAD, L"Перезагрузить\tF5");
  AppendMenuW(menu, has ? on : dis, CMD_SHOW_IN_FOLDER, L"Показать в папке");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, canvas_.has_selection() ? on : dis, CMD_COPY, L"Копировать\tCtrl+C");
  AppendMenuW(menu, has ? on : dis, CMD_SELECT_ALL, L"Выделить всё\tCtrl+A");
  AppendMenuW(menu, has ? on : dis, CMD_FIND, L"Найти…\tCtrl+F");
  AppendMenuW(menu, has ? on : dis, CMD_GOTO_PAGE, L"Перейти на страницу…\tCtrl+G");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, (has ? on : dis) | check(has && canvas_.zoom_mode() == ZoomMode::FitWidth), CMD_FIT_WIDTH, L"По ширине\tCtrl+2");
  AppendMenuW(menu, (has ? on : dis) | check(has && canvas_.zoom_mode() == ZoomMode::FitPage), CMD_FIT_PAGE, L"Страница целиком\tCtrl+0");
  AppendMenuW(menu, has ? on : dis, CMD_ACTUAL_SIZE, L"Реальный размер\tCtrl+1");
  AppendMenuW(menu, (has ? on : dis) | check(canvas_.facing()), CMD_FACING, L"Разворот\tCtrl+3");
  AppendMenuW(menu, has ? on : dis, CMD_ROTATE_RIGHT, L"Повернуть по часовой\tCtrl+Shift++");
  AppendMenuW(menu, has ? on : dis, CMD_ROTATE_LEFT, L"Повернуть против часовой\tCtrl+Shift+−");
  AppendMenuW(menu, on | check(canvas_.invert()), CMD_INVERT, L"Ночной режим\tCtrl+I");
  AppendMenuW(menu, on | check(settings().crisp_text), CMD_CRISP_TEXT, L"Чёткий текст в PDF");
  AppendMenuW(menu, on | check(fullscreen_), CMD_FULLSCREEN, L"Полный экран\tF11");
  AppendMenuW(menu, (has_outline_ ? on : dis) | check(sidebar_visible()), CMD_TOC, L"Оглавление\tF9");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, on, CMD_REGISTER, L"Открывать PDF и DjVu в LightView…");
  AppendMenuW(menu, on, CMD_HELP, L"Клавиши\tF1");
  int cmd = TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, hwnd_, nullptr);
  DestroyMenu(menu);  // also destroys the submenu
  if (cmd) execute(cmd);
}

// ---------------------------------------------------------------------------------------------

LRESULT CALLBACK MainWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  MainWindow* self;
  if (msg == WM_NCCREATE) {
    self = (MainWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
    self->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
  } else {
    self = (MainWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  }
  return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      dpi_ = (int)GetDpiForWindow(hwnd_);
      apply_dark_title(hwnd_);
      toolbar_.create(hwnd_, this, dpi_);
      tree_ = CreateWindowExW(0, WC_TREEVIEWW, L"",
                              WS_CHILD | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                                  TVS_SHOWSELALWAYS | TVS_TRACKSELECT | TVS_NOHSCROLL,
                              0, 0, 0, 0, hwnd_, (HMENU)10, inst_, nullptr);
      apply_tree_theme();
      canvas_.create(hwnd_, this, dpi_);
      DragAcceptFiles(hwnd_, TRUE);
      ACCEL acc[] = {
          {FVIRTKEY | FCONTROL, 'O', CMD_OPEN},
          {FVIRTKEY | FCONTROL, 'P', CMD_PRINT},
          {FVIRTKEY | FCONTROL, 'W', CMD_CLOSE},
          {FVIRTKEY | FCONTROL, 'R', CMD_RELOAD},
          {FVIRTKEY, VK_F5, CMD_RELOAD},
          {FVIRTKEY | FCONTROL, 'F', CMD_FIND},
          {FVIRTKEY, VK_F3, CMD_FIND_NEXT},
          {FVIRTKEY | FSHIFT, VK_F3, CMD_FIND_PREV},
          {FVIRTKEY | FCONTROL, 'G', CMD_GOTO_PAGE},
          {FVIRTKEY | FCONTROL, VK_OEM_PLUS, CMD_ZOOM_IN},
          {FVIRTKEY | FCONTROL, VK_ADD, CMD_ZOOM_IN},
          {FVIRTKEY | FCONTROL, VK_OEM_MINUS, CMD_ZOOM_OUT},
          {FVIRTKEY | FCONTROL, VK_SUBTRACT, CMD_ZOOM_OUT},
          {FVIRTKEY | FCONTROL, '0', CMD_FIT_PAGE},
          {FVIRTKEY | FCONTROL, VK_NUMPAD0, CMD_FIT_PAGE},
          {FVIRTKEY | FCONTROL, '1', CMD_ACTUAL_SIZE},
          {FVIRTKEY | FCONTROL, VK_NUMPAD1, CMD_ACTUAL_SIZE},
          {FVIRTKEY | FCONTROL, '2', CMD_FIT_WIDTH},
          {FVIRTKEY | FCONTROL, VK_NUMPAD2, CMD_FIT_WIDTH},
          {FVIRTKEY | FCONTROL, '3', CMD_FACING},
          {FVIRTKEY | FCONTROL, VK_NUMPAD3, CMD_FACING},
          {FVIRTKEY | FCONTROL | FSHIFT, VK_OEM_PLUS, CMD_ROTATE_RIGHT},
          {FVIRTKEY | FCONTROL | FSHIFT, VK_ADD, CMD_ROTATE_RIGHT},
          {FVIRTKEY | FCONTROL | FSHIFT, VK_OEM_MINUS, CMD_ROTATE_LEFT},
          {FVIRTKEY | FCONTROL | FSHIFT, VK_SUBTRACT, CMD_ROTATE_LEFT},
          {FVIRTKEY | FCONTROL, 'I', CMD_INVERT},
          {FVIRTKEY, VK_F11, CMD_FULLSCREEN},
          {FVIRTKEY | FCONTROL, 'L', CMD_FULLSCREEN},
          {FVIRTKEY, VK_F9, CMD_TOC},
          {FVIRTKEY | FALT, VK_LEFT, CMD_BACK},
          {FVIRTKEY | FALT, VK_RIGHT, CMD_FORWARD},
          {FVIRTKEY | FCONTROL, VK_HOME, CMD_FIRST_PAGE},
          {FVIRTKEY | FCONTROL, VK_END, CMD_LAST_PAGE},
          {FVIRTKEY, VK_F1, CMD_HELP},
      };
      accel_ = CreateAcceleratorTableW(acc, (int)std::size(acc));
      update_ui();
      return 0;
    }

    case WM_SIZE:
      layout();
      return 0;

    case WM_GETMINMAXINFO: {
      auto* mm = (MINMAXINFO*)lp;
      mm->ptMinTrackSize = {s(420), s(300)};
      return 0;
    }

    case WM_DPICHANGED: {
      dpi_ = HIWORD(wp);
      toolbar_.set_dpi(dpi_);
      canvas_.set_dpi(dpi_);
      apply_tree_theme();
      const RECT* r = (const RECT*)lp;
      SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      layout();
      return 0;
    }

    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd_, &ps);
      SetDCBrushColor(dc, theme().splitter);
      FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(DC_BRUSH));
      EndPaint(hwnd_, &ps);
      return 0;
    }

    case WM_SETCURSOR:
      if ((HWND)wp == hwnd_ && LOWORD(lp) == HTCLIENT && sidebar_visible()) {
        SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
        return TRUE;
      }
      break;

    case WM_LBUTTONDOWN:
      if (sidebar_visible()) {
        dragging_splitter_ = true;
        SetCapture(hwnd_);
      }
      return 0;

    case WM_MOUSEMOVE:
      if (dragging_splitter_) {
        int x = GET_X_LPARAM(lp);
        sidebar_w_ = std::clamp(MulDiv(x, 96, dpi_), 120, 800);
        layout();
        UpdateWindow(hwnd_);
      }
      return 0;

    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
      if (dragging_splitter_) {
        dragging_splitter_ = false;
        if (msg == WM_LBUTTONUP) ReleaseCapture();
      }
      return 0;

    case WM_COMMAND:
      if (lp == 0) execute(LOWORD(wp));
      return 0;

    case WM_NOTIFY: {
      auto* nm = (NMHDR*)lp;
      if (nm->hwndFrom != tree_) break;
      switch (nm->code) {
        case TVN_SELCHANGEDW: {
          auto* tv = (NMTREEVIEWW*)lp;
          if (!filling_tree_ && (tv->action == TVC_BYMOUSE || tv->action == TVC_BYKEYBOARD)) {
            if (tv->action == TVC_BYMOUSE) tree_clicked_ = true;
            navigate_outline(tv->itemNew.hItem);
          }
          break;
        }
        case NM_CLICK: {
          // A click on the already selected item does not change the selection: navigate anyway.
          TVHITTESTINFO ht{};
          GetCursorPos(&ht.pt);
          ScreenToClient(tree_, &ht.pt);
          HTREEITEM h = TreeView_HitTest(tree_, &ht);
          if (h && (ht.flags & TVHT_ONITEM) && !tree_clicked_ && h == TreeView_GetSelection(tree_)) navigate_outline(h);
          tree_clicked_ = false;
          break;
        }
        case TVN_KEYDOWN:
          if (((NMTVKEYDOWN*)lp)->wVKey == VK_RETURN) navigate_outline(TreeView_GetSelection(tree_));
          if (((NMTVKEYDOWN*)lp)->wVKey == VK_ESCAPE) SetFocus(canvas_.hwnd());
          break;
      }
      return 0;
    }

    case WM_DROPFILES: {
      HDROP drop = (HDROP)wp;
      UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      std::vector<std::wstring> files;
      for (UINT i = 0; i < n; ++i) {
        UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring f(len + 1, L'\0');
        DragQueryFileW(drop, i, f.data(), len + 1);
        f.resize(len);
        files.push_back(f);
      }
      DragFinish(drop);
      SetForegroundWindow(hwnd_);
      for (size_t i = 1; i < files.size(); ++i) spawn_self(files[i]);
      if (!files.empty()) open(files[0]);
      return 0;
    }

    case WM_APPCOMMAND:
      // Mouse "back"/"forward" buttons (and keyboard media keys) when they arrive as app commands.
      switch (GET_APPCOMMAND_LPARAM(lp)) {
        case APPCOMMAND_BROWSER_BACKWARD: execute(CMD_BACK); return TRUE;
        case APPCOMMAND_BROWSER_FORWARD: execute(CMD_FORWARD); return TRUE;
      }
      break;

    case WM_COPYDATA: {
      auto* cds = (COPYDATASTRUCT*)lp;
      if (cds->dwData != kCopyDataIsOpen || path_.empty()) return FALSE;
      std::wstring other((const wchar_t*)cds->lpData, cds->cbData / sizeof(wchar_t));
      if (!same_path(other, path_)) return FALSE;
      if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
      return TRUE;
    }

    case WM_ACTIVATE:
      if (LOWORD(wp) != WA_INACTIVE) {
        HWND f = GetFocus();
        if (!f || !IsChild(hwnd_, f)) SetFocus(canvas_.hwnd());
      }
      return 0;

    case WM_SETFOCUS:
      SetFocus(canvas_.hwnd());
      return 0;

    case kMsgFileChanged: {
      if (path_.empty()) return 0;
      FileStamp now = file_stamp(path_);
      if (!now.valid || now == stamp_) return 0;
      pending_stamp_ = now;
      reload_retries_ = 0;
      SetTimer(hwnd_, kTimerReload, 300, nullptr);
      return 0;
    }

    case WM_TIMER:
      if (wp == kTimerReload) {
        KillTimer(hwnd_, kTimerReload);
        FileStamp now = file_stamp(path_);
        if (!now.valid) {
          if (++reload_retries_ < 20) SetTimer(hwnd_, kTimerReload, 500, nullptr);
        } else if (!(now == pending_stamp_)) {
          pending_stamp_ = now;  // still being written
          SetTimer(hwnd_, kTimerReload, 300, nullptr);
        } else if (!(now == stamp_)) {
          reload(true);
        }
      }
      return 0;

    case kMsgPrint:
      if (lp == 0) canvas_.show_toast(L"Ошибка печати", 3000);
      else if (wp >= (WPARAM)lp) canvas_.show_toast(L"Отправлено на печать", 2500);
      else canvas_.show_toast(fmt(L"Печать: страница %d из %d", (int)wp + 1, (int)lp), 5000);
      return 0;

    case WM_CLOSE:
      if (active_print_jobs() > 0 &&
          MessageBoxW(hwnd_, L"Документ ещё печатается. Прервать печать и закрыть?", L"LightView",
                      MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return 0;
      }
      save_settings();
      DestroyWindow(hwnd_);
      return 0;

    case WM_DESTROY:
      watcher_.stop();
      if (accel_) DestroyAcceleratorTable(accel_);
      accel_ = nullptr;
      if (tree_font_) DeleteObject(tree_font_);
      tree_font_ = nullptr;
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------

bool activate_existing_window(const std::wstring& path_in) {
  struct Search {
    std::wstring path;
    HWND found = nullptr;
  } search{full_path(path_in)};
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* s = (Search*)lp;
        wchar_t cls[64];
        if (!GetClassNameW(hwnd, cls, 64) || wcscmp(cls, kClass) != 0) return TRUE;
        COPYDATASTRUCT cds{kCopyDataIsOpen, (DWORD)(s->path.size() * sizeof(wchar_t)), (void*)s->path.data()};
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(hwnd, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 2000, &result) && result) {
          s->found = hwnd;
          return FALSE;
        }
        return TRUE;
      },
      (LPARAM)&search);
  if (!search.found) return false;
  SetForegroundWindow(search.found);  // allowed: this process was just started by the user
  return true;
}

bool register_file_types(bool install) {
  const std::wstring exe = exe_path();
  const std::wstring classes = L"Software\\Classes\\";
  const std::wstring progid = L"LightView.Document";
  const std::wstring app = classes + L"Applications\\" + file_name_of(exe);
  const wchar_t* exts[] = {L".pdf", L".djvu", L".djv"};
  auto set = [](const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(),
                           (DWORD)((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
  };
  bool ok = true;
  if (install) {
    const std::wstring command = L"\"" + exe + L"\" \"%1\"";
    ok &= set(classes + progid, nullptr, L"Документ PDF/DjVu");
    ok &= set(classes + progid + L"\\DefaultIcon", nullptr, L"\"" + exe + L"\",0");
    ok &= set(classes + progid + L"\\shell\\open\\command", nullptr, command);
    ok &= set(app, L"FriendlyAppName", L"LightView");
    ok &= set(app + L"\\shell\\open\\command", nullptr, command);
    for (const wchar_t* e : exts) {
      ok &= set(app + L"\\SupportedTypes", e, L"");
      ok &= set(classes + e + L"\\OpenWithProgids", progid.c_str(), L"");
    }
  } else {
    RegDeleteTreeW(HKEY_CURRENT_USER, (classes + progid).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, app.c_str());
    for (const wchar_t* e : exts) RegDeleteKeyValueW(HKEY_CURRENT_USER, (classes + e + L"\\OpenWithProgids").c_str(), progid.c_str());
  }
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return ok;
}

}  // namespace lv
