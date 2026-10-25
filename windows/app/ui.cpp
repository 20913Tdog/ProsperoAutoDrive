// Prospero AutoDrive - windows of ProsperoAutoDrive.exe (P3.3)
//
// Copyright (C) 2026 Prospero AutoDrive contributors
// SPDX-License-Identifier: MIT

#include "ui.h"

#include "setup.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <thread>
#include <vector>

namespace prospero::ui {

namespace {

constexpr UINT kStatusMsg = WM_APP + 1;     // lParam: Status*, deleted by the window
constexpr UINT kTrayMsg = WM_APP + 2;       // from the tray icon
constexpr UINT kFoundMsg = WM_APP + 3;      // lParam: Here* or nullptr
constexpr wchar_t kTitle[] = L"Prospero AutoDrive";

std::wstring Wide(const std::string &s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string Narrow(const std::wstring &w) {
  std::string s;
  for (wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '?';
  return s;
}

int Px(int v) { return MulDiv(v, static_cast<int>(GetDpiForSystem()), 96); }

HFONT UiFont() {
  static HFONT f = nullptr;
  if (!f) {
    NONCLIENTMETRICSW m{};
    m.cbSize = sizeof m;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof m, &m, 0);
    f = CreateFontIndirectW(&m.lfMessageFont);
  }
  return f;
}

HWND Control(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h,
             int id) {
  HWND c = CreateWindowExW(cls == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text,
                           WS_CHILD | WS_VISIBLE | style, Px(x), Px(y), Px(w), Px(h), parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
  SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(UiFont()), TRUE);
  return c;
}

std::wstring Text(HWND parent, int id) {
  wchar_t buf[1024];
  GetDlgItemTextW(parent, id, buf, 1024);
  return buf;
}

// A window of `cls` with a modal loop; the window procedure ends it with
// PostQuitMessage. Tab moves between the controls.
void ModalLoop(HWND w) {
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(w, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
}

HWND MakeWindow(const wchar_t *cls, WNDPROC proc, const wchar_t *title, int w, int h, void *param) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.lpszClassName = cls;
  wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
  RegisterClassExW(&wc);
  RECT r{0, 0, Px(w), Px(h)};
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  AdjustWindowRect(&r, style, FALSE);
  int ww = r.right - r.left, wh = r.bottom - r.top;
  return CreateWindowExW(WS_EX_DLGMODALFRAME, cls, title, style, (GetSystemMetrics(SM_CXSCREEN) - ww) / 2,
                         (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh, nullptr, nullptr,
                         GetModuleHandleW(nullptr), param);
}

// ---------------------------------------------------------------- setup

enum { kPath = 100, kBrowse, kAutostart, kStartMenu, kInstall, kCancel };

std::wstring PickFolder(HWND owner, const std::wstring &start) {
  std::wstring out;
  IFileOpenDialog *d = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                              reinterpret_cast<void **>(&d)))) {
    return out;
  }
  DWORD opts = 0;
  d->GetOptions(&opts);
  d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  d->SetTitle(L"Folder for Prospero AutoDrive");
  std::wstring parent = start.substr(0, start.find_last_of(L'\\'));
  IShellItem *item = nullptr;
  if (SUCCEEDED(SHCreateItemFromParsingName(parent.c_str(), nullptr, IID_IShellItem,
                                            reinterpret_cast<void **>(&item)))) {
    d->SetFolder(item);
    item->Release();
  }
  if (SUCCEEDED(d->Show(owner)) && SUCCEEDED(d->GetResult(&item))) {
    PWSTR p = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) out = p;
    CoTaskMemFree(p);
    item->Release();
  }
  d->Release();
  // As usual for installers: a folder of its own inside the chosen one.
  if (!out.empty()) {
    std::wstring last = out.substr(out.find_last_of(L'\\') + 1);
    if (_wcsicmp(last.c_str(), L"ProsperoAutoDrive") != 0) {
      if (out.back() != L'\\') out += L'\\';
      out += L"ProsperoAutoDrive";
    }
  }
  return out;
}

struct SetupState {
  int rc = 1;
};

LRESULT CALLBACK SetupProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  auto *st = reinterpret_cast<SetupState *>(GetWindowLongPtrW(w, GWLP_USERDATA));
  switch (msg) {
    case WM_CREATE: {
      SetWindowLongPtrW(w, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams));
      setup::Installed old = setup::FindInstalled();
      std::wstring path = old.found ? old.dir : L"";
      if (path.empty()) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &p))) {
          path = std::wstring(p) + L"\\ProsperoAutoDrive";
        }
        CoTaskMemFree(p);
      }
      Control(w, L"STATIC",
              old.found ? L"Prospero AutoDrive is already installed. Update it in this folder:"
                        : L"Prospero AutoDrive connects the network drive automatically whenever the server is "
                          L"reachable.\nInstall to:",
              0, 16, 12, 448, 34, 0);
      Control(w, L"EDIT", path.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, 16, 52, 340, 24, kPath);
      HWND b = Control(w, L"BUTTON", L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, 364, 51, 100, 26, kBrowse);
      EnableWindow(b, !old.found);
      EnableWindow(GetDlgItem(w, kPath), !old.found);
      Control(w, L"BUTTON", L"Start with Windows", WS_TABSTOP | BS_AUTOCHECKBOX, 16, 88, 300, 22, kAutostart);
      Control(w, L"BUTTON", L"Add to the Start menu", WS_TABSTOP | BS_AUTOCHECKBOX, 16, 112, 300, 22, kStartMenu);
      CheckDlgButton(w, kAutostart, BST_CHECKED);
      CheckDlgButton(w, kStartMenu, BST_CHECKED);
      Control(w, L"STATIC",
              L"For a protected folder such as C:\\Program Files, Windows asks for administrator rights. "
              L"The program itself then runs without them.",
              0, 16, 142, 448, 34, 0);
      Control(w, L"BUTTON", old.found ? L"Update" : L"Install",
              WS_TABSTOP | BS_DEFPUSHBUTTON, 256, 188, 100, 28, kInstall);
      Control(w, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 364, 188, 100, 28, kCancel);
      return 0;
    }
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case kBrowse: {
          std::wstring p = PickFolder(w, Text(w, kPath));
          if (!p.empty()) SetDlgItemTextW(w, kPath, p.c_str());
          return 0;
        }
        case kInstall: {
          std::wstring path = Text(w, kPath);
          if (path.size() < 3 || path[1] != L':') {
            MessageBoxW(w, L"Please enter a full folder path, for example C:\\Program Files\\ProsperoAutoDrive.",
                        kTitle, MB_OK | MB_ICONWARNING);
            return 0;
          }
          EnableWindow(w, FALSE);
          int rc = setup::Install(path, IsDlgButtonChecked(w, kStartMenu) == BST_CHECKED,
                                  IsDlgButtonChecked(w, kAutostart) == BST_CHECKED, true, false);
          EnableWindow(w, TRUE);
          if (rc == 0) {
            st->rc = 0;
            MessageBoxW(w, L"Done. Prospero AutoDrive now runs in the notification area of the taskbar "
                           L"(bottom right, possibly behind the arrow).", kTitle, MB_OK | MB_ICONINFORMATION);
            DestroyWindow(w);
          }
          return 0;
        }
        case kCancel:
        case IDCANCEL:
          DestroyWindow(w);
          return 0;
      }
      break;
    case WM_CLOSE:
      DestroyWindow(w);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(w, msg, wp, lp);
}

// ---------------------------------------------------------------- server

enum { kIp = 200, kSearch, kOk, kAbort, kState };

struct AskState {
  Settings *s;
  bool ok = false;
  bool searching = false;
};

void StartSearch(HWND w, AskState *st) {
  if (st->searching) return;
  st->searching = true;
  SetDlgItemTextW(w, kState, L"Searching the network...");
  EnableWindow(GetDlgItem(w, kSearch), FALSE);
  unsigned short port = st->s->port;
  std::thread([w, port] {
    std::vector<in_addr> targets;
    for (auto &t : BroadcastTargets()) targets.push_back(t.first);
    Here *hit = nullptr;
    for (int round = 0; round < 3 && !hit; round++) {
      std::vector<Here> found;
      std::string err;
      if (!Discover(targets, port, 2000, &found, &err)) break;
      if (!found.empty()) hit = new Here(found.front());
    }
    if (!PostMessageW(w, kFoundMsg, 0, reinterpret_cast<LPARAM>(hit))) delete hit;
  }).detach();
}

LRESULT CALLBACK AskProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  auto *st = reinterpret_cast<AskState *>(GetWindowLongPtrW(w, GWLP_USERDATA));
  switch (msg) {
    case WM_CREATE: {
      st = reinterpret_cast<AskState *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
      SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
      Control(w, L"STATIC",
              L"Setup: please switch the server on. The program searches the network for it once; "
              L"if it is not found, please enter its IP address.",
              0, 16, 12, 400, 48, 0);
      Control(w, L"STATIC", L"IP address:", 0, 16, 70, 80, 22, 0);
      Control(w, L"EDIT", Wide(st->s->ip).c_str(), WS_TABSTOP | ES_AUTOHSCROLL, 100, 67, 180, 24, kIp);
      Control(w, L"BUTTON", L"Search again", WS_TABSTOP | BS_PUSHBUTTON, 290, 66, 126, 26, kSearch);
      Control(w, L"STATIC", L"", 0, 16, 102, 400, 22, kState);
      Control(w, L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 208, 136, 100, 28, kOk);
      Control(w, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 316, 136, 100, 28, kAbort);
      StartSearch(w, st);
      return 0;
    }
    case kFoundMsg: {
      Here *h = reinterpret_cast<Here *>(lp);
      st->searching = false;
      EnableWindow(GetDlgItem(w, kSearch), TRUE);
      if (h) {
        SetDlgItemTextW(w, kIp, Wide(h->ip).c_str());
        SetDlgItemTextW(w, kState, (L"Found: " + Wide(h->name) + L" at " + Wide(h->ip)).c_str());
        delete h;
      } else {
        SetDlgItemTextW(w, kState, L"Nothing found. Is the server on? Otherwise please enter its IP address.");
      }
      return 0;
    }
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case kSearch:
          StartSearch(w, st);
          return 0;
        case kOk: {
          std::string ip = Narrow(Text(w, kIp));
          in_addr a;
          if (inet_pton(AF_INET, ip.c_str(), &a) != 1) {
            MessageBoxW(w, L"That is not an IPv4 address (example: 192.168.1.20).", kTitle, MB_OK | MB_ICONWARNING);
            return 0;
          }
          st->s->ip = ip;
          st->ok = true;
          DestroyWindow(w);
          return 0;
        }
        case kAbort:
        case IDCANCEL:
          DestroyWindow(w);
          return 0;
      }
      break;
    case WM_CLOSE:
      DestroyWindow(w);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(w, msg, wp, lp);
}

// ---------------------------------------------------------------- tray

enum { kOpen = 300, kFind, kPairOther, kAdopt, kRepair, kStopServer, kAuto, kLog, kQuit, kInfo };

// A round icon with a "P" in it, drawn at run time.
HICON MakeIcon(COLORREF fill, int size) {
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = size;
  bi.bmiHeader.biHeight = -size;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void *bits = nullptr;
  HDC screen = GetDC(nullptr);
  HDC dc = CreateCompatibleDC(screen);
  HBITMAP color = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  HGDIOBJ old = SelectObject(dc, color);
  const COLORREF key = RGB(255, 0, 255);
  HBRUSH bg = CreateSolidBrush(key);
  RECT r{0, 0, size, size};
  FillRect(dc, &r, bg);
  DeleteObject(bg);
  HBRUSH b = CreateSolidBrush(fill);
  HPEN pen = CreatePen(PS_SOLID, 1, fill);
  SelectObject(dc, b);
  SelectObject(dc, pen);
  Ellipse(dc, 0, 0, size, size);
  LOGFONTW lf{};
  lf.lfHeight = -size * 3 / 4;
  lf.lfWeight = FW_BOLD;
  lstrcpyW(lf.lfFaceName, L"Segoe UI");
  HFONT f = CreateFontIndirectW(&lf);
  SelectObject(dc, f);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(255, 255, 255));
  DrawTextW(dc, L"P", 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  GdiFlush();
  auto *px = static_cast<DWORD *>(bits);
  for (int i = 0; i < size * size; i++) {
    DWORD c = px[i] & 0xFFFFFF;
    px[i] = c == 0xFF00FF ? 0 : (c | 0xFF000000);       // DIB is BGR: key stays 0xFF00FF
  }
  SelectObject(dc, old);
  DeleteObject(f);
  DeleteObject(b);
  DeleteObject(pen);
  DeleteDC(dc);
  std::vector<BYTE> zero(static_cast<size_t>((size + 15) / 16 * 2 * size));
  HBITMAP mask = CreateBitmap(size, size, 1, 1, zero.data());
  ICONINFO ii{};
  ii.fIcon = TRUE;
  ii.hbmColor = color;
  ii.hbmMask = mask;
  HICON icon = CreateIconIndirect(&ii);
  DeleteObject(color);
  DeleteObject(mask);
  return icon;
}

struct TrayState {
  Agent *agent;
  HANDLE stop, search, agent_thread;
  std::wstring log_file;
  Status st;
  HICON on = nullptr, off = nullptr;
  UINT taskbar_created = 0;
  bool added = false;
};

void Tip(NOTIFYICONDATAW *nid, const TrayState *t) {
  std::wstring tip;
  if (t->st.connected) {
    tip = Wide(t->st.name) + L" connected as " + Wide(t->st.letter);
  } else {
    tip = Wide(t->st.name) + L" not reachable";
  }
  lstrcpynW(nid->szTip, tip.c_str(), ARRAYSIZE(nid->szTip));
}

void UpdateIcon(HWND w, TrayState *t, bool add, const std::string &message = "", bool problem = false) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof nid;
  nid.hWnd = w;
  nid.uID = 1;
  nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
  nid.uCallbackMessage = kTrayMsg;
  nid.hIcon = t->st.connected ? t->on : t->off;
  Tip(&nid, t);
  if (!message.empty()) {
    nid.uFlags |= NIF_INFO;
    lstrcpynW(nid.szInfoTitle, kTitle, ARRAYSIZE(nid.szInfoTitle));
    lstrcpynW(nid.szInfo, Wide(message).c_str(), ARRAYSIZE(nid.szInfo));
    nid.dwInfoFlags = problem ? NIIF_WARNING : NIIF_INFO;
  }
  if (add) {
    t->added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
  } else {
    Shell_NotifyIconW(NIM_MODIFY, &nid);
  }
}

void ShowMenu(HWND w, TrayState *t) {
  HMENU m = CreatePopupMenu();
  std::wstring head;
  std::wstring link = t->st.link == "wired" ? L", wired" : t->st.link == "wifi" ? L", Wi-Fi" : L"";
  if (t->st.connected) {
    head = Wide(t->st.name) + L" connected as " + Wide(t->st.letter) + link;
  } else if (!t->st.last_seen.empty()) {
    head = Wide(t->st.name) + L" not reachable, last seen " + Wide(t->st.last_seen);
  } else {
    head = Wide(t->st.name) + L" not reachable";
  }
  AppendMenuW(m, MF_STRING | MF_GRAYED, kInfo, head.c_str());
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (t->st.connected ? 0 : MF_GRAYED), kOpen, L"Open drive");
  AppendMenuW(m, MF_STRING, kFind, (L"Search the network for " + Wide(t->st.name) + L" (30 s)").c_str());
  AppendMenuW(m, MF_STRING | (t->st.connected ? 0 : MF_GRAYED), kPairOther, L"Pair another PC");
  if (!t->st.other_server.empty()) AppendMenuW(m, MF_STRING, kAdopt, L"Pair with the new server...");
  if (t->st.pair_refused) AppendMenuW(m, MF_STRING, kRepair, L"Pair again");
  AppendMenuW(m, MF_STRING | (t->st.connected ? 0 : MF_GRAYED), kStopServer,
              (L"Shut down " + Wide(t->st.name) + L"...").c_str());
  AppendMenuW(m, MF_STRING | (setup::AutostartOn() ? MF_CHECKED : 0), kAuto, L"Start with Windows");
  AppendMenuW(m, MF_STRING, kLog, L"Show log");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, kQuit, L"Exit");
  POINT p;
  GetCursorPos(&p);
  SetForegroundWindow(w);
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, w, nullptr);
  PostMessageW(w, WM_NULL, 0, 0);
  DestroyMenu(m);
  switch (cmd) {
    case kOpen:
      ShellExecuteW(nullptr, L"open", (Wide(t->st.letter) + L"\\").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      break;
    case kFind:
      SetEvent(t->search);
      break;
    case kPairOther: {
      Agent *a = t->agent;
      std::thread([w, a] {
        auto *st = new Status;
        st->message = a->OpenPairing();
        st->name = "-";       // only a message, the status stays
        if (!PostMessageW(w, kStatusMsg, 1, reinterpret_cast<LPARAM>(st))) delete st;
      }).detach();
      break;
    }
    case kAdopt: {
      // Never silently: a foreign device at the same address would become
      // the drive the user writes to.
      std::wstring text =
          L"A different server than the paired one answers at the saved address "
          L"(server ID " + Wide(t->st.other_server) + L").\n\n"
          L"This is normal if the server was set up again or its settings were deleted. "
          L"Only confirm if it is your own device.\n\n"
          L"Pair with this server? The old pairing will be deleted.";
      if (MessageBoxW(w, text.c_str(), kTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
        t->agent->Adopt();
      }
      break;
    }
    case kRepair:
      t->agent->Retry();
      break;
    case kStopServer: {
      std::wstring text = L"Shut down " + Wide(t->st.name) +
                          L"?\n\nThe drive will be disconnected and files open on it will be closed. "
                          L"The server can only be started again on the device itself.";
      if (MessageBoxW(w, text.c_str(), kTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) break;
      Agent *a = t->agent;
      std::thread([w, a] {
        std::string msg = a->QuitServer();
        if (msg.empty()) return;
        auto *st = new Status;
        st->message = msg;
        st->name = "-";
        if (!PostMessageW(w, kStatusMsg, 1, reinterpret_cast<LPARAM>(st))) delete st;
      }).detach();
      break;
    }
    case kAuto:
      setup::SetAutostart(!setup::AutostartOn());
      break;
    case kLog:
      ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + t->log_file + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
      break;
    case kQuit:
      DestroyWindow(w);
      break;
  }
}

LRESULT CALLBACK TrayProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  auto *t = reinterpret_cast<TrayState *>(GetWindowLongPtrW(w, GWLP_USERDATA));
  if (t && msg == t->taskbar_created) {
    UpdateIcon(w, t, true);     // the Explorer restarted
    return 0;
  }
  switch (msg) {
    case WM_CREATE:
      t = reinterpret_cast<TrayState *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
      SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(t));
      UpdateIcon(w, t, true);
      return 0;
    case kStatusMsg: {
      auto *st = reinterpret_cast<Status *>(lp);
      if (wp == 0) {
        std::string message = st->message;
        bool problem = st->problem;
        t->st = *st;
        UpdateIcon(w, t, !t->added, message, problem);
      } else {
        UpdateIcon(w, t, !t->added, st->message, false);
      }
      delete st;
      return 0;
    }
    case kTrayMsg:
      switch (LOWORD(lp)) {
        case WM_CONTEXTMENU:
        case NIN_SELECT:
        case NIN_KEYSELECT:
          ShowMenu(w, t);
          break;
      }
      return 0;
    case WM_QUERYENDSESSION:
      return TRUE;
    case WM_ENDSESSION:
      if (wp) {
        // Logoff or shutdown: the drive goes first.
        SetEvent(t->stop);
        WaitForSingleObject(t->agent_thread, 5000);
      }
      return 0;
    case WM_DESTROY: {
      NOTIFYICONDATAW nid{};
      nid.cbSize = sizeof nid;
      nid.hWnd = w;
      nid.uID = 1;
      Shell_NotifyIconW(NIM_DELETE, &nid);
      PostQuitMessage(0);
      return 0;
    }
  }
  return DefWindowProcW(w, msg, wp, lp);
}

}  // namespace

bool AskServer(Settings *s) {
  AskState st;
  st.s = s;
  HWND w = MakeWindow(L"ProsperoAutoDriveAsk", AskProc, L"Set up Prospero AutoDrive", 432, 176, &st);
  if (!w) return false;
  ShowWindow(w, SW_SHOW);
  SetForegroundWindow(w);
  ModalLoop(w);
  return st.ok;
}

int RunTray(Agent *agent, HANDLE stop, HANDLE search, const std::function<HANDLE()> &start,
            const std::wstring &log_file) {
  TrayState t;
  t.agent = agent;
  t.stop = stop;
  t.search = search;
  t.log_file = log_file;
  t.st.name = "Server";
  int small = GetSystemMetrics(SM_CXSMICON);
  t.on = MakeIcon(RGB(0x2e, 0x7d, 0x32), small);
  t.off = MakeIcon(RGB(0x80, 0x80, 0x80), small);
  t.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = TrayProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"ProsperoAutoDriveTray";
  RegisterClassExW(&wc);
  HWND w = CreateWindowExW(0, wc.lpszClassName, kTitle, 0, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, &t);
  agent->on_status = [w](const Status &s) {
    auto *copy = new Status(s);
    if (!PostMessageW(w, kStatusMsg, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
  };
  HANDLE agent_thread = start();
  t.agent_thread = agent_thread;
  MSG msg;
  HANDLE hs[2] = {stop, agent_thread};
  for (;;) {
    DWORD r = MsgWaitForMultipleObjects(2, hs, FALSE, INFINITE, QS_ALLINPUT);
    if (r == WAIT_OBJECT_0 || r == WAIT_OBJECT_0 + 1) {
      if (IsWindow(w)) DestroyWindow(w);
    }
    bool quit = false;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        quit = true;
        break;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (quit) break;
  }
  agent->on_status = nullptr;
  SetEvent(stop);
  WaitForSingleObject(agent_thread, 15000);
  DestroyIcon(t.on);
  DestroyIcon(t.off);
  return 0;
}

}  // namespace prospero::ui

namespace prospero::setup {

int SetupDialog() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ui::SetupState st;
  HWND w = ui::MakeWindow(L"ProsperoAutoDriveSetup", ui::SetupProc, L"Install Prospero AutoDrive", 480, 228,
                          &st);
  if (!w) return 1;
  ShowWindow(w, SW_SHOW);
  SetForegroundWindow(w);
  ui::ModalLoop(w);
  CoUninitialize();
  return st.rc;
}

}  // namespace prospero::setup
