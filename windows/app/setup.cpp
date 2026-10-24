// Prospero AutoDrive - setup and removal of ProsperoAutoDrive.exe (P3.3)
//
// Copyright (C) 2026 Prospero AutoDrive contributors
// SPDX-License-Identifier: MIT

#include "setup.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincred.h>

#include <vector>

#ifndef PROSPERO_VERSION
#define PROSPERO_VERSION "0.0.0"
#endif

namespace prospero::setup {

namespace {

constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ProsperoAutoDrive";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kOwnKey[] = L"Software\\ProsperoAutoDrive";
constexpr wchar_t kRunValue[] = L"ProsperoAutoDrive";
constexpr wchar_t kShortcut[] = L"Prospero AutoDrive.lnk";
constexpr wchar_t kTitle[] = L"Prospero AutoDrive";

std::wstring Version() {
  std::string v = PROSPERO_VERSION;
  return std::wstring(v.begin(), v.end());
}

std::wstring RegString(HKEY root, const wchar_t *key, const wchar_t *value) {
  wchar_t buf[1024];
  DWORD size = sizeof buf;
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
  return buf;
}

DWORD RegDword(HKEY root, const wchar_t *key, const wchar_t *value, DWORD def) {
  DWORD v = 0, size = sizeof v;
  if (RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return def;
  return v;
}

void SetString(HKEY k, const wchar_t *name, const std::wstring &v) {
  RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(v.c_str()),
                 static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
}

void SetDword(HKEY k, const wchar_t *name, DWORD v) {
  RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&v), sizeof v);
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  std::wstring out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
  CoTaskMemFree(p);
  return out;
}

std::wstring Trim(std::wstring dir) {
  while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
  return dir;
}

bool SameDir(const std::wstring &a, const std::wstring &b) {
  return _wcsicmp(Trim(a).c_str(), Trim(b).c_str()) == 0;
}

// Can this process create files in dir (creating it if needed)?
bool Writable(const std::wstring &dir) {
  if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
    return SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) == ERROR_SUCCESS;
  }
  std::wstring probe = dir + L"\\.prospero-write-test";
  HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  CloseHandle(h);
  return true;
}

// Runs this exe again with admin rights (UAC) and waits; returns its exit
// code, or -1 if the user said no.
int RunElevated(const std::wstring &args) {
  std::wstring self = SelfPath();
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof sei;
  sei.fMask = SEE_MASK_NOCLOSEPROCESS;
  sei.lpVerb = L"runas";
  sei.lpFile = self.c_str();
  sei.lpParameters = args.c_str();
  sei.nShow = SW_HIDE;
  if (!ShellExecuteExW(&sei) || !sei.hProcess) return -1;
  WaitForSingleObject(sei.hProcess, INFINITE);
  DWORD rc = 1;
  GetExitCodeProcess(sei.hProcess, &rc);
  CloseHandle(sei.hProcess);
  return static_cast<int>(rc);
}

// Starts the installed program as the logged-on user. From an elevated
// process it goes through the Explorer, so the program runs without admin
// rights; otherwise its drive would be invisible to the Explorer.
void Launch(const std::wstring &exe) {
  if (Elevated()) {
    ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"\"" + exe + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    return;
  }
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"\"" + exe + L"\"";
  if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

bool MakeShortcut(const std::wstring &lnk, const std::wstring &target) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IShellLinkW *link = nullptr;
  bool ok = false;
  if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                 reinterpret_cast<void **>(&link)))) {
    link->SetPath(target.c_str());
    link->SetDescription(L"Netzlaufwerk automatisch verbinden");
    IPersistFile *file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&file)))) {
      ok = SUCCEEDED(file->Save(lnk.c_str(), TRUE));
      file->Release();
    }
    link->Release();
  }
  CoUninitialize();
  return ok;
}

void Message(bool quiet, const std::wstring &text, UINT icon = MB_ICONINFORMATION) {
  if (!quiet) MessageBoxW(nullptr, text.c_str(), kTitle, MB_OK | icon);
}

// Deletes the exe and its folder once this process has ended: a running
// exe cannot delete itself. The folder only if it is empty then, so a
// folder chosen by hand with other files in it stays.
void DeleteAfterExit(const std::wstring &exe, const std::wstring &dir) {
  std::wstring cmd = L"cmd.exe /d /c \"for /l %i in (1,1,30) do (ping -n 2 127.0.0.1 >nul & del /f /q \"" +
                     exe + L"\" 2>nul & if not exist \"" + exe + L"\" (rmdir \"" + dir +
                     L"\" 2>nul & exit))\"";
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  if (CreateProcessW((std::wstring(sys) + L"\\cmd.exe").c_str(), cmd.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, sys, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

// What belongs to the current user: autostart, own key, credentials,
// settings and log.
void RemoveUserData() {
  HKEY k;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
    RegDeleteValueW(k, kRunValue);
    RegCloseKey(k);
  }
  RegDeleteTreeW(HKEY_CURRENT_USER, kOwnKey);
  DWORD n = 0;
  PCREDENTIALW *creds = nullptr;
  if (CredEnumerateW(L"ProsperoAutoDrive/*", 0, &n, &creds)) {
    for (DWORD i = 0; i < n; i++) CredDeleteW(creds[i]->TargetName, creds[i]->Type, 0);
    CredFree(creds);
  }
  std::wstring state = StateDir();
  for (const wchar_t *f : {L"settings.ini", L"log.txt", L"log.1.txt"}) DeleteFileW((state + L"\\" + f).c_str());
  RemoveDirectoryW(state.c_str());
}

}  // namespace

std::wstring SelfPath() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
  return std::wstring(buf, n);
}

std::wstring SelfDir() {
  std::wstring p = SelfPath();
  return p.substr(0, p.find_last_of(L'\\'));
}

std::wstring StateDir() {
  wchar_t base[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
  return (n && n < MAX_PATH ? std::wstring(base) : std::wstring(L".")) + L"\\ProsperoAutoDrive";
}

bool Elevated() {
  HANDLE token = nullptr;
  TOKEN_ELEVATION e{};
  DWORD size = 0;
  bool out = false;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    if (GetTokenInformation(token, TokenElevation, &e, sizeof e, &size)) out = e.TokenIsElevated != 0;
    CloseHandle(token);
  }
  return out;
}

Installed FindInstalled() {
  Installed in;
  std::wstring dir = RegString(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation");
  if (!dir.empty()) {
    in.found = true;
    in.dir = dir;
    return in;
  }
  dir = RegString(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallLocation");
  if (!dir.empty()) {
    in.found = true;
    in.machine = true;
    in.dir = dir;
  }
  return in;
}

bool StopRunning(DWORD wait_ms) {
  HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\ProsperoAutoDrive.Instance");
  if (!running) return true;
  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, L"Local\\ProsperoAutoDrive.Stop");
  SetEvent(stop);
  DWORD w = WaitForSingleObject(running, wait_ms);
  CloseHandle(running);
  CloseHandle(stop);
  return w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
}

bool AutostartOn() {
  return !RegString(HKEY_CURRENT_USER, kRunKey, kRunValue).empty();
}

void SetAutostart(bool on) {
  HKEY k;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) !=
      ERROR_SUCCESS) {
    return;
  }
  if (on) {
    SetString(k, kRunValue, L"\"" + SelfPath() + L"\"");
  } else {
    RegDeleteValueW(k, kRunValue);
  }
  RegCloseKey(k);
}

// The installer cannot write the user's autostart when it runs elevated
// (the admin may be another account), so it stores the choice and every
// user's first start applies it.
void ApplyAutostartChoice() {
  if (RegDword(HKEY_CURRENT_USER, kOwnKey, L"AutostartApplied", 0)) return;
  Installed in = FindInstalled();
  if (!in.found || !SameDir(in.dir, SelfDir())) return;
  DWORD want = RegDword(in.machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, kOwnKey, L"Autostart", 1);
  SetAutostart(want != 0);
  HKEY k;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kOwnKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) ==
      ERROR_SUCCESS) {
    SetDword(k, L"AutostartApplied", 1);
    RegCloseKey(k);
  }
}

int InstallFiles(const std::wstring &dir_in, bool machine, bool startmenu, bool autostart) {
  std::wstring dir = Trim(dir_in);
  if (!Writable(dir)) return 5;
  std::wstring exe = dir + L"\\" + kExeName;
  if (_wcsicmp(exe.c_str(), SelfPath().c_str()) != 0 && !CopyFileW(SelfPath().c_str(), exe.c_str(), FALSE)) {
    return 3;
  }
  HKEY root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
  HKEY k;
  if (RegCreateKeyExW(root, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) {
    return 4;
  }
  SetString(k, L"DisplayName", L"Prospero AutoDrive");
  SetString(k, L"DisplayVersion", Version());
  SetString(k, L"Publisher", L"Prospero AutoDrive contributors");
  SetString(k, L"InstallLocation", dir);
  SetString(k, L"DisplayIcon", exe);
  SetString(k, L"UninstallString", L"\"" + exe + L"\" --uninstall");
  SetString(k, L"QuietUninstallString", L"\"" + exe + L"\" --uninstall --quiet");
  SetDword(k, L"NoModify", 1);
  SetDword(k, L"NoRepair", 1);
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &fa)) SetDword(k, L"EstimatedSize", fa.nFileSizeLow / 1024);
  RegCloseKey(k);
  if (RegCreateKeyExW(root, kOwnKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
    SetDword(k, L"Autostart", autostart ? 1 : 0);
    SetDword(k, L"StartMenu", startmenu ? 1 : 0);
    RegCloseKey(k);
  }
  std::wstring menu = KnownFolder(machine ? FOLDERID_CommonPrograms : FOLDERID_Programs);
  if (!menu.empty()) {
    std::wstring lnk = menu + L"\\" + kShortcut;
    if (startmenu) {
      MakeShortcut(lnk, exe);
    } else {
      DeleteFileW(lnk.c_str());
    }
  }
  return 0;
}

int Install(const std::wstring &dir_in, bool startmenu, bool autostart, bool launch, bool quiet) {
  std::wstring dir = Trim(dir_in);
  if (!StopRunning(15000)) {
    Message(quiet, L"Das laufende Programm reagiert nicht. Bitte ueber das Tray-Symbol beenden.", MB_ICONWARNING);
    return 6;
  }
  // An entry of an earlier installation elsewhere would point to the wrong
  // place; the user is told instead of silently moving it.
  Installed old = FindInstalled();
  if (old.found && !SameDir(old.dir, dir)) {
    Message(quiet, L"Prospero AutoDrive ist schon in\n" + old.dir +
                       L"\ninstalliert. Bitte erst dort ueber \"Installierte Apps\" entfernen.", MB_ICONWARNING);
    return 7;
  }
  // Ask for admin rights only for a folder this user cannot write to (and
  // keep an existing machine-wide installation machine-wide).
  bool machine = (old.found && old.machine) || !Writable(dir);
  int rc;
  if (machine && !Elevated()) {
    std::wstring args = L"--install-files \"" + dir + L"\" --machine" + (startmenu ? L"" : L" --no-startmenu") +
                        (autostart ? L"" : L" --no-autostart");
    rc = RunElevated(args);
    if (rc == -1) {
      Message(quiet, L"Ohne Administratorrechte laesst sich dieser Ordner nicht beschreiben. "
                     L"Bitte einen anderen Ordner waehlen.", MB_ICONWARNING);
      return 5;
    }
  } else {
    rc = InstallFiles(dir, machine, startmenu, autostart);
  }
  if (rc != 0) {
    Message(quiet, L"Installation fehlgeschlagen (Fehler " + std::to_wstring(rc) + L").", MB_ICONERROR);
    return rc;
  }
  if (launch) Launch(dir + L"\\" + kExeName);
  return 0;
}

int UninstallFiles() {
  Installed in = FindInstalled();
  if (!in.found) return 0;
  HKEY root = in.machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
  std::wstring menu = KnownFolder(in.machine ? FOLDERID_CommonPrograms : FOLDERID_Programs);
  if (!menu.empty()) DeleteFileW((menu + L"\\" + kShortcut).c_str());
  RegDeleteTreeW(root, kUninstallKey);
  RegDeleteTreeW(root, kOwnKey);
  DeleteAfterExit(Trim(in.dir) + L"\\" + kExeName, Trim(in.dir));
  return 0;
}

int Uninstall(bool quiet) {
  if (!StopRunning(15000)) {
    Message(quiet, L"Das laufende Programm reagiert nicht. Bitte ueber das Tray-Symbol beenden.", MB_ICONWARNING);
    return 6;
  }
  RemoveUserData();
  Installed in = FindInstalled();
  int rc = 0;
  if (in.found && in.machine && !Elevated()) {
    rc = RunElevated(L"--uninstall-files");
    if (rc == -1) {
      Message(quiet, L"Zum Entfernen werden Administratorrechte gebraucht.", MB_ICONWARNING);
      return 5;
    }
  } else {
    rc = UninstallFiles();
  }
  if (rc == 0) Message(quiet, L"Prospero AutoDrive wurde entfernt.");
  return rc;
}

}  // namespace prospero::setup
