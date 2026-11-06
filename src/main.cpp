// シンプル WinRoll ver 1.3.3
// (c) 2026 KINA  https://github.com/KinaPPP
// Inspired by WinRoll (c) 2003-2004 Wil Palma. This is a new, separate program.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shellscalingapi.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cwchar>
#include <atomic>
#ifndef ICON_SMALL2
#define ICON_SMALL2 2
#endif

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const wchar_t* APP_NAME      = L"シンプル WinRoll";
static const wchar_t* APP_VERSION   = L"1.3.3";
static const wchar_t* MAIN_CLASS    = L"SimpleWinRoll.KINA.Main";
static const wchar_t* BAR_CLASS     = L"SimpleWinRoll.KINA.Bar";
static const wchar_t* PANEL_CLASS   = L"SimpleWinRoll.KINA.Panel";
static const wchar_t* SPLASH_CLASS  = L"SimpleWinRoll.KINA.Splash";
static const wchar_t* NOTICE_CLASS  = L"SimpleWinRoll.KINA.Notice";
static const wchar_t* MUTEX_NAME    = L"Local\\SimpleWinRoll.KINA.Instance";
static const wchar_t* RUN_KEY       = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_VALUE     = L"SimpleWinRoll";
static const wchar_t* GITHUB_URL    = L"https://github.com/KinaPPP";

enum {
    WM_APP_ROLL       = WM_APP + 1,   // wParam = HWND to roll
    WM_APP_TRAY       = WM_APP + 2,
    WM_APP_REACTIVATE = WM_APP + 3,   // a second copy was launched
    WM_APP_ACTION     = WM_APP + 4,   // wParam = Action, lParam = HWND
    WM_APP_TRAYWIN    = WM_APP + 5,   // a window minimized to the tray (wParam = icon id)
};
enum {
    ID_TRAY = 1,
    IDM_SETTINGS = 100, IDM_ABOUT, IDM_RESTOREALL, IDM_EXIT,
    IDM_TW_RESTORE = 300, IDM_TW_CLOSE,
    IDM_TRAYED_BASE = 1000,          // + index (tray "menu" mode)
};
enum Action { A_ROLL = 1, A_ROLL_ALL, A_TRANSLUCENT, A_BACK, A_TOPMOST, A_TRAY, A_MIN_ALL, A_MAX_ALL, A_CLOSE_ALL };
static const UINT_PTR MAIN_TIMER = 2;
static const BYTE TRANSLUCENT_ALPHA = 204;   // 80%
static const UINT_PTR BAR_TIMER = 1;

// Palette (KINA default)
#define C_BG       RGB(0xe8,0xe8,0xe8)
#define C_PANEL    RGB(0xf7,0xf7,0xf7)
#define C_CARD     RGB(0xff,0xff,0xff)
#define C_BORDER   RGB(0xd0,0xd0,0xd0)
#define C_TEXT     RGB(0x1a,0x1a,0x1a)
#define C_MUTED    RGB(0x88,0x88,0x88)
#define C_SUB      RGB(0x55,0x55,0x55)
#define C_BLUE     RGB(0x4a,0x8c,0xe0)
#define C_BLUE_HV  RGB(0x3a,0x7b,0xd5)
#define C_BLUE_ACT RGB(0x2d,0x6a,0xbf)
#define C_HOVER    RGB(0xec,0xec,0xec)

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
struct Settings {
    int  sizePct   = 150;   // 100,125,150,175,200
    bool followDpi = false;
    bool autoStart = false;
    bool hideIcon  = false;
    bool noMiddle  = false;   // don't use the middle mouse button (Shift+Right still works)
    bool trayAsMenu = false;  // tray-minimized windows: false = own icon, true = listed in our menu
    bool showSplash = true;   // splash screen on every start (auto-start included)
};

struct Rolled {
    HWND target = nullptr;
    HWND bar = nullptr;
    DWORD pid = 0;
    RECT winRect{};        // GetWindowRect of target when rolled
    POINT barOrigin{};     // where the bar was first placed
    std::wstring appName;
    std::wstring title;
    std::wstring aumid;    // taskbar group ID copied from the target
    HICON iconSmall = nullptr;
    HICON iconBig = nullptr;
    HFONT fontBold = nullptr, fontNormal = nullptr, fontSmall = nullptr;
    double fontScale = 0;
};

static HINSTANCE g_inst;
static HWND g_main;
static HHOOK g_mouseHook;              // owned by the hook thread
static DWORD g_hookThreadId;
static HANDLE g_hookThread;
static std::atomic<bool> g_noMiddleFlag{false};   // copy of g_set.noMiddle for the hook thread
static Settings g_set;
static std::vector<Rolled*> g_rolled;
static bool g_trayShown = false;
static bool g_forceIcon = false;          // shown because user re-launched
static UINT g_msgTaskbarCreated;
static std::wstring g_dataDir;
static HWND g_settingsWnd, g_aboutWnd;
static ULONG_PTR g_gdipToken;
static HICON g_appIconSmall, g_appIconBig;

struct Trayed {                 // a window minimized to the tray
    HWND target = nullptr;
    DWORD pid = 0;
    UINT id = 0;
    bool hasIcon = false;
    HICON icon = nullptr;
    std::wstring title;
};
static std::vector<Trayed*> g_trayed;
static UINT g_nextTrayId = 1000;

struct Translucent {            // a window we made translucent
    HWND h = nullptr;
    DWORD pid = 0;
    bool addedLayered = false;
    COLORREF key = 0;
    DWORD flags = 0;
};
static std::vector<Translucent> g_trans;

static void UpdateTrayIcon();

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------
static std::wstring DataPath(const wchar_t* name) { return g_dataDir + L"\\" + name; }

static void InitDataDir() {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) {
        g_dataDir = std::wstring(p) + L"\\simple-winroll";
        CoTaskMemFree(p);
    } else {
        wchar_t buf[MAX_PATH]; GetModuleFileNameW(nullptr, buf, MAX_PATH);
        std::wstring s(buf); g_dataDir = s.substr(0, s.find_last_of(L'\\'));
    }
    CreateDirectoryW(g_dataDir.c_str(), nullptr);
}

static int ClampSize(int v) {
    static const int ok[] = {100,125,150,175,200};
    for (int x : ok) if (x == v) return v;
    return 150;
}

static void LoadSettings() {
    std::wstring ini = DataPath(L"settings.ini");
    g_set.sizePct   = ClampSize(GetPrivateProfileIntW(L"display", L"size", 150, ini.c_str()));
    g_set.followDpi = GetPrivateProfileIntW(L"display", L"followDpi", 0, ini.c_str()) != 0;
    g_set.autoStart = GetPrivateProfileIntW(L"startup", L"autoStart", 0, ini.c_str()) != 0;
    g_set.hideIcon  = GetPrivateProfileIntW(L"startup", L"hideIcon", 0, ini.c_str()) != 0;
    g_set.noMiddle  = GetPrivateProfileIntW(L"mouse", L"noMiddle", 0, ini.c_str()) != 0;
    g_noMiddleFlag = g_set.noMiddle;
    g_set.trayAsMenu = GetPrivateProfileIntW(L"tray", L"asMenu", 0, ini.c_str()) != 0;
    g_set.showSplash = GetPrivateProfileIntW(L"startup", L"splash", 1, ini.c_str()) != 0;
}

static void SaveSettings() {
    std::wstring ini = DataPath(L"settings.ini");
    wchar_t b[16];
    swprintf(b, 16, L"%d", g_set.sizePct);
    WritePrivateProfileStringW(L"display", L"size", b, ini.c_str());
    WritePrivateProfileStringW(L"display", L"followDpi", g_set.followDpi ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"startup", L"autoStart", g_set.autoStart ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"startup", L"hideIcon", g_set.hideIcon ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"mouse", L"noMiddle", g_set.noMiddle ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"tray", L"asMenu", g_set.trayAsMenu ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"startup", L"splash", g_set.showSplash ? L"1" : L"0", ini.c_str());
}

static void ApplyAutoStart() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (g_set.autoStart) {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(k, RUN_VALUE, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, RUN_VALUE);
    }
    RegCloseKey(k);
}

static double ScaleForWindow(HWND h) {
    if (g_set.followDpi) {
        UINT dpi = h ? GetDpiForWindow(h) : GetDpiForSystem();
        if (!dpi) dpi = 96;
        return dpi / 96.0;
    }
    return g_set.sizePct / 100.0;
}

// Bars always use the original size, following only Windows' own display scale.
static double BarScale(HWND h) {
    UINT dpi = h ? GetDpiForWindow(h) : GetDpiForSystem();
    return (dpi ? dpi : 96) / 96.0;
}

// Taskbar grouping: read / write a window's AppUserModelID.
static std::wstring GetWindowAumid(HWND h) {
    std::wstring out;
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(&ps))) && ps) {
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) out = pv.pwszVal;
        PropVariantClear(&pv);
        ps->Release();
    }
    return out;
}

static void SetWindowAumid(HWND h, const wchar_t* id) {
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(&ps))) && ps) {
        PROPVARIANT pv; PropVariantInit(&pv);
        if (id && *id) InitPropVariantFromString(id, &pv);   // else VT_EMPTY clears it
        ps->SetValue(PKEY_AppUserModel_ID, pv);
        ps->Commit();
        PropVariantClear(&pv);
        ps->Release();
    }
}

// Bar colors follow Windows: Settings > Personalization > Colors
// ("Show accent color on title bars and window borders").
struct BarColors { COLORREF bg, text, sub, pillBg, pillBorder, border; };

static COLORREF Blend(COLORREF a, COLORREF b, double t) {   // t = amount of a
    return RGB((int)(GetRValue(a) * t + GetRValue(b) * (1 - t)),
               (int)(GetGValue(a) * t + GetGValue(b) * (1 - t)),
               (int)(GetBValue(a) * t + GetBValue(b) * (1 - t)));
}

static bool ReadDwmDword(const wchar_t* name, DWORD* out) {
    DWORD sz = sizeof(DWORD);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", name,
                        RRF_RT_REG_DWORD, nullptr, out, &sz) == ERROR_SUCCESS;
}

static BarColors GetBarColors(bool active) {
    BarColors c;
    DWORD prevalence = 0;
    ReadDwmDword(L"ColorPrevalence", &prevalence);
    if (prevalence == 1) {
        DWORD accent = 0; COLORREF bg;
        if (ReadDwmDword(L"AccentColor", &accent)) {
            bg = accent & 0x00FFFFFF;                       // stored as 0xAABBGGRR = COLORREF
        } else {
            DWORD cz = 0; BOOL opaque = FALSE;
            DwmGetColorizationColor(&cz, &opaque);          // 0xAARRGGBB
            bg = RGB((cz >> 16) & 0xFF, (cz >> 8) & 0xFF, cz & 0xFF);
        }
        if (!active) {
            // Inactive bars are paler, like inactive title bars.
            DWORD ina = 0;
            if (ReadDwmDword(L"AccentColorInactive", &ina)) bg = ina & 0x00FFFFFF;
            else bg = Blend(bg, RGB(255,255,255), 0.45);
        }
        double lum = 0.299 * GetRValue(bg) + 0.587 * GetGValue(bg) + 0.114 * GetBValue(bg);
        COLORREF fg = lum > 150 ? RGB(0,0,0) : RGB(255,255,255);
        c.bg = bg; c.text = fg; c.sub = Blend(fg, bg, 0.78);
        c.pillBg = Blend(fg, bg, 0.12); c.pillBorder = Blend(fg, bg, 0.35); c.border = bg;
    } else {
        c.bg = C_CARD; c.text = active ? C_TEXT : C_MUTED; c.sub = active ? C_SUB : C_MUTED;
        c.pillBg = C_PANEL; c.pillBorder = C_BORDER; c.border = C_BORDER;
    }
    return c;
}

static HFONT MakeFont(double px, bool bold) {
    int h = -(int)(px + 0.5);
    HFONT f = CreateFontW(h, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH, L"Yu Gothic UI");
    return f;
}

static bool IsOurWindow(HWND h) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

static bool ClassIs(HWND h, const wchar_t* name) {
    wchar_t c[128] = {0}; GetClassNameW(h, c, 128);
    return wcscmp(c, name) == 0;
}

static RECT VisibleRect(HWND h) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) || r.right <= r.left)
        GetWindowRect(h, &r);
    return r;
}

// App name from the executable's FileDescription.
static std::wstring ExeDescription(DWORD pid, std::wstring* exeBaseOut) {
    std::wstring result;
    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!ph) return result;
    wchar_t path[MAX_PATH * 2]; DWORD len = MAX_PATH * 2;
    if (QueryFullProcessImageNameW(ph, 0, path, &len)) {
        std::wstring p(path);
        std::wstring base = p.substr(p.find_last_of(L'\\') + 1);
        if (exeBaseOut) *exeBaseOut = base;
        DWORD dummy = 0, sz = GetFileVersionInfoSizeW(path, &dummy);
        if (sz) {
            std::vector<BYTE> buf(sz);
            if (GetFileVersionInfoW(path, 0, sz, buf.data())) {
                struct LangCp { WORD lang, cp; } *tr = nullptr; UINT trLen = 0;
                std::vector<std::wstring> keys;
                if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &trLen) && trLen >= sizeof(LangCp)) {
                    for (UINT i = 0; i < trLen / sizeof(LangCp); i++) {
                        wchar_t k[64]; swprintf(k, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr[i].lang, tr[i].cp);
                        keys.push_back(k);
                    }
                }
                keys.push_back(L"\\StringFileInfo\\041104b0\\FileDescription");
                keys.push_back(L"\\StringFileInfo\\040904b0\\FileDescription");
                keys.push_back(L"\\StringFileInfo\\040904e4\\FileDescription");
                for (auto& k : keys) {
                    wchar_t* v = nullptr; UINT vl = 0;
                    if (VerQueryValueW(buf.data(), k.c_str(), (void**)&v, &vl) && v && vl > 1 && v[0]) {
                        result.assign(v, wcsnlen(v, vl));   // never read past the returned length
                        break;
                    }
                }
            }
        }
        if (result.empty()) {
            result = base;
            size_t dot = result.find_last_of(L'.');
            if (dot != std::wstring::npos) result = result.substr(0, dot);
        }
    }
    CloseHandle(ph);
    // trim spaces
    while (!result.empty() && result.back() == L' ') result.pop_back();
    return result;
}

static HICON GetWindowIconCopy(HWND h, DWORD pid, bool big) {
    DWORD_PTR r = 0; HICON ic = nullptr;
    if (big) {
        if (SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 200, &r) && r) ic = (HICON)r;
        if (!ic) ic = (HICON)GetClassLongPtrW(h, GCLP_HICON);
    } else {
        if (SendMessageTimeoutW(h, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 200, &r) && r) ic = (HICON)r;
        if (!ic && SendMessageTimeoutW(h, WM_GETICON, ICON_SMALL, 0, SMTO_ABORTIFHUNG, 200, &r) && r) ic = (HICON)r;
        if (!ic) ic = (HICON)GetClassLongPtrW(h, GCLP_HICONSM);
        if (!ic && SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 200, &r) && r) ic = (HICON)r;
        if (!ic) ic = (HICON)GetClassLongPtrW(h, GCLP_HICON);
    }
    if (ic) return CopyIcon(ic);
    // Fall back to the executable's own icon.
    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (ph) {
        wchar_t path[MAX_PATH * 2]; DWORD len = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(ph, 0, path, &len)) {
            HICON lg = nullptr, sm = nullptr;
            if (ExtractIconExW(path, 0, &lg, &sm, 1) > 0) {
                if (big) { ic = lg; if (sm) DestroyIcon(sm); }
                else     { ic = sm; if (lg) DestroyIcon(lg); }
            }
        }
        CloseHandle(ph);
    }
    if (ic) return ic;
    return CopyIcon(LoadIcon(nullptr, IDI_APPLICATION));
}

// ---------------------------------------------------------------------------
// GDI+ helpers
// ---------------------------------------------------------------------------
static Gdiplus::Color GC(COLORREF c) { return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }

static void RoundRectPath(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    if (r * 2 > w) r = w / 2; if (r * 2 > h) r = h / 2;
    float d = r * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

static void FillRound(Gdiplus::Graphics& g, RECT rc, float r, COLORREF fill, COLORREF border, float bw = 1.0f) {
    Gdiplus::GraphicsPath p;
    float half = bw / 2.0f;
    RoundRectPath(p, rc.left + half, rc.top + half, (float)(rc.right - rc.left) - bw, (float)(rc.bottom - rc.top) - bw, r);
    Gdiplus::SolidBrush b(GC(fill));
    g.FillPath(&b, &p);
    if (bw > 0) { Gdiplus::Pen pen(GC(border), bw); g.DrawPath(&pen, &p); }
}

static void DrawTextIn(HDC dc, const std::wstring& s, RECT rc, HFONT f, COLORREF col, UINT fmt) {
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, col); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), (int)s.size(), &rc, fmt | DT_NOPREFIX);
    SelectObject(dc, old);
}

// ---------------------------------------------------------------------------
// Rolled-window state file (for recovery after a crash)
// ---------------------------------------------------------------------------
// Lines: "R <hwnd> <pid>" rolled, "T <hwnd> <pid>" in tray, "A <hwnd> <pid> <addedLayered>" translucent
// Identifies this Windows session (the boot time, in minutes). Records from an earlier boot are ignored.
static unsigned long long BootStamp() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER now; now.LowPart = ft.dwLowDateTime; now.HighPart = ft.dwHighDateTime;
    unsigned long long bootMs = now.QuadPart / 10000ULL - GetTickCount64();
    return bootMs / 60000ULL;
}

static void SaveRolledState() {
    std::wstring path = DataPath(L"rolled.txt");
    if (g_rolled.empty() && g_trayed.empty() && g_trans.empty()) { DeleteFileW(path.c_str()); return; }
    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) return;
    fprintf(f, "B %llu 0 0\n", BootStamp());
    for (auto* r : g_rolled)
        fprintf(f, "R %llu %lu 0\n", (unsigned long long)(ULONG_PTR)r->target, (unsigned long)r->pid);
    for (auto* t : g_trayed)
        fprintf(f, "T %llu %lu 0\n", (unsigned long long)(ULONG_PTR)t->target, (unsigned long)t->pid);
    for (auto& a : g_trans)
        fprintf(f, "A %llu %lu %d\n", (unsigned long long)(ULONG_PTR)a.h, (unsigned long)a.pid, a.addedLayered ? 1 : 0);
    fclose(f);
}

static void RecoverFromPreviousRun() {
    std::wstring path = DataPath(L"rolled.txt");
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return;
    char line[128];
    unsigned long long boot = BootStamp();
    bool skip = false;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'B') {   // written in an earlier Windows session? then every handle in it is stale
            unsigned long long b = 0;
            if (sscanf(line, "B %llu", &b) == 1 && (b + 1 < boot || b > boot + 1)) skip = true;
            continue;
        }
        if (skip) break;
        char type = 'R'; unsigned long long hv = 0; unsigned long pid = 0; int added = 0;
        if (line[0] >= 'A' && line[0] <= 'Z') {
            if (sscanf(line, "%c %llu %lu %d", &type, &hv, &pid, &added) < 3) continue;
        } else if (sscanf(line, "%llu %lu", &hv, &pid) != 2) continue;   // ver 1.0-1.1 format
        HWND h = (HWND)(ULONG_PTR)hv;
        if (!IsWindow(h)) continue;
        DWORD realPid = 0; GetWindowThreadProcessId(h, &realPid);
        if (realPid != pid) continue;  // handle was reused by another program
        if (type == 'A') {
            // Only undo it if the window still has exactly the alpha we set.
            BYTE alpha = 255; DWORD fl = 0; COLORREF key = 0;
            if (!GetLayeredWindowAttributes(h, &key, &alpha, &fl) || !(fl & LWA_ALPHA) || alpha != TRANSLUCENT_ALPHA) continue;
            SetLayeredWindowAttributes(h, key, 255, fl);
            if (added) SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) & ~WS_EX_LAYERED);
        } else if (!IsWindowVisible(h)) {
            // Bring it back in front of everything (without taking focus) so it is clearly visible again.
            SetWindowPos(h, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
            ShowWindowAsync(h, SW_SHOWNA);
        }
    }
    fclose(f);
    DeleteFileW(path.c_str());
}

// Emergency restore: used on exit, shutdown and crash. Must be safe to call anytime.
static void RestoreAllHidden() {
    for (auto* r : g_rolled)
        if (r->target && IsWindow(r->target) && !IsWindowVisible(r->target))
            ShowWindowAsync(r->target, SW_SHOWNA);
    for (auto* t : g_trayed)
        if (t->target && IsWindow(t->target) && !IsWindowVisible(t->target))
            ShowWindowAsync(t->target, SW_SHOWNA);
    for (auto& a : g_trans)
        if (IsWindow(a.h)) {
            SetLayeredWindowAttributes(a.h, a.key, 255, a.flags | LWA_ALPHA);
            if (a.addedLayered) SetWindowLongPtrW(a.h, GWL_EXSTYLE, GetWindowLongPtrW(a.h, GWL_EXSTYLE) & ~WS_EX_LAYERED);
        }
}

// ---------------------------------------------------------------------------
// Roll / unroll
// ---------------------------------------------------------------------------
static Rolled* FindByTarget(HWND h) { for (auto* r : g_rolled) if (r->target == h) return r; return nullptr; }

static std::wstring DisplayTitle(const Rolled* r) {
    std::wstring t = r->title;
    const std::wstring& a = r->appName;
    if (!a.empty()) {
        if (t == a) return L"";
        const wchar_t* seps[] = { L" - ", L" — ", L" – " };
        for (auto s : seps) {
            std::wstring suf = std::wstring(s) + a;
            if (t.size() > suf.size() && t.compare(t.size() - suf.size(), suf.size(), suf) == 0)
                return t.substr(0, t.size() - suf.size());
        }
    }
    return t;
}

static std::wstring ReadTitle(HWND h) {
    wchar_t buf[512] = {0};
    // InternalGetWindowText does not send messages, so a hung app cannot block us.
    InternalGetWindowText(h, buf, 512);
    return buf;
}

static int BarHeightFor(double s) { return (int)(34 * s + 0.5); }

static void UpdateBarText(Rolled* r) {
    std::wstring t = DisplayTitle(r);
    std::wstring caption = r->appName.empty() ? t : (t.empty() ? r->appName : r->appName + L" — " + t);
    SetWindowTextW(r->bar, caption.c_str());
}

static void RemoveRolled(Rolled* r) {
    for (size_t i = 0; i < g_rolled.size(); i++) if (g_rolled[i] == r) { g_rolled.erase(g_rolled.begin() + i); break; }
    if (r->bar && IsWindow(r->bar)) {
        SetWindowLongPtrW(r->bar, GWLP_USERDATA, 0);
        if (!r->aumid.empty()) SetWindowAumid(r->bar, nullptr);
        DestroyWindow(r->bar);
    }
    if (r->iconSmall) DestroyIcon(r->iconSmall);
    if (r->iconBig) DestroyIcon(r->iconBig);
    if (r->fontBold) DeleteObject(r->fontBold);
    if (r->fontNormal) DeleteObject(r->fontNormal);
    if (r->fontSmall) DeleteObject(r->fontSmall);
    delete r;
    SaveRolledState();
}

static bool IsRollable(HWND h) {
    if (!h || !IsWindow(h) || !IsWindowVisible(h) || IsIconic(h)) return false;
    if (GetAncestor(h, GA_ROOT) != h) return false;
    if (IsOurWindow(h)) return false;
    if (GetWindowLongPtrW(h, GWL_STYLE) & WS_CHILD) return false;
    if (ClassIs(h, L"Shell_TrayWnd") || ClassIs(h, L"Shell_SecondaryTrayWnd") ||
        ClassIs(h, L"Progman") || ClassIs(h, L"WorkerW")) return false;
    return true;
}

static void ApplyBarAlpha(Rolled* r);

static void RollWindow(HWND h) {
    if (!IsRollable(h) || FindByTarget(h)) return;

    Rolled* r = new Rolled();
    r->target = h;
    GetWindowThreadProcessId(h, &r->pid);
    GetWindowRect(h, &r->winRect);
    RECT vis = VisibleRect(h);

    std::wstring exeBase;
    r->appName = ExeDescription(r->pid, &exeBase);
    r->title = ReadTitle(h);
    if (_wcsicmp(exeBase.c_str(), L"ApplicationFrameHost.exe") == 0) {  // Store apps
        r->appName = r->title; r->title.clear();
    }
    r->iconSmall = GetWindowIconCopy(h, r->pid, false);
    r->iconBig   = GetWindowIconCopy(h, r->pid, true);

    bool topmost = (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    HWND prev = GetWindow(h, GW_HWNDPREV);
    bool wasForeground = (GetForegroundWindow() == h);

    r->aumid = GetWindowAumid(h);
    double s = BarScale(h);
    int w = vis.right - vis.left;
    int bh = BarHeightFor(s);
    r->barOrigin = { vis.left, vis.top };

    r->bar = CreateWindowExW(WS_EX_APPWINDOW | (topmost ? WS_EX_TOPMOST : 0), BAR_CLASS, L"",
                             WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX,
                             vis.left, vis.top, w, bh, nullptr, nullptr, g_inst, r);
    if (!r->bar) { if (r->iconSmall) DestroyIcon(r->iconSmall); if (r->iconBig) DestroyIcon(r->iconBig); delete r; return; }
    g_rolled.push_back(r);
    ApplyBarAlpha(r);
    if (!r->aumid.empty()) SetWindowAumid(r->bar, r->aumid.c_str());  // group with the app on the taskbar

    SendMessageW(r->bar, WM_SETICON, ICON_SMALL, (LPARAM)r->iconSmall);
    SendMessageW(r->bar, WM_SETICON, ICON_BIG, (LPARAM)r->iconBig);
    UpdateBarText(r);

    // Take the target's place in the z-order.
    HWND after = (prev && prev != r->bar) ? prev : HWND_TOP;
    if (topmost && after == HWND_TOP) after = HWND_TOPMOST;
    SetWindowPos(r->bar, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    SaveRolledState();           // record BEFORE hiding, so a crash right now is recoverable
    ShowWindowAsync(h, SW_HIDE);
    if (wasForeground) SetForegroundWindow(r->bar);
}

static void UnrollWindow(Rolled* r, bool activate) {
    HWND t = r->target;
    if (IsWindow(t)) {
        RECT br; GetWindowRect(r->bar, &br);
        int dx = br.left - r->barOrigin.x, dy = br.top - r->barOrigin.y;
        if ((dx || dy) && !IsZoomed(t)) {
            SetWindowPos(t, nullptr, r->winRect.left + dx, r->winRect.top + dy, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        }
        // Put it where the bar is in the z-order (just behind whatever is in front of the bar), then show it.
        HWND front = GetWindow(r->bar, GW_HWNDPREV);
        bool barTop = (GetWindowLongPtrW(r->bar, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        HWND after = front ? front : (barTop ? HWND_TOPMOST : HWND_TOP);
        SetWindowPos(t, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        ShowWindowAsync(t, activate ? SW_SHOW : SW_SHOWNA);
        if (activate) SetForegroundWindow(t);
    }
    RemoveRolled(r);
}

static void UnrollAll() {
    while (!g_rolled.empty()) UnrollWindow(g_rolled.back(), false);
}

// ---------------------------------------------------------------------------
// Small notice shown near the mouse (e.g. why translucency did not work)
// ---------------------------------------------------------------------------
struct Notice { std::wstring l1, l2; bool openColors; DWORD start; double scale; HFONT fB, fN; };
static HWND g_notice;
static const UINT_PTR NOTICE_TIMER = 4;
static const DWORD NOTICE_MS = 4000;

static LRESULT CALLBACK NoticeProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Notice* n = (Notice*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: if (n) {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);
        double s = n->scale;
        HBRUSH bg = CreateSolidBrush(C_CARD); FillRect(hdc, &cr, bg); DeleteObject(bg);
        int is = (int)(16 * s + 0.5);
        DrawIconEx(hdc, (int)(14 * s), (int)(14 * s), g_appIconSmall, is, is, 0, nullptr, DI_NORMAL);
        RECT r1 = { (int)(40 * s), (int)(10 * s), cr.right - (int)(12 * s), (int)(32 * s) };
        DrawTextIn(hdc, n->l1, r1, n->fB, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT r2 = { (int)(40 * s), (int)(32 * s), cr.right - (int)(12 * s), (int)(54 * s) };
        DrawTextIn(hdc, n->l2, r2, n->fN, C_SUB, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        EndPaint(hwnd, &ps);
        return 0; }
        break;
    case WM_TIMER:
        if (wp == NOTICE_TIMER) DestroyWindow(hwnd);
        return 0;
    case WM_LBUTTONDOWN:
        if (n && n->openColors) ShellExecuteW(nullptr, L"open", L"ms-settings:colors", nullptr, nullptr, SW_SHOWNORMAL);
        DestroyWindow(hwnd);
        return 0;
    case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: DestroyWindow(hwnd); return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_DESTROY: KillTimer(hwnd, NOTICE_TIMER); if (g_notice == hwnd) g_notice = nullptr; return 0;
    case WM_NCDESTROY:
        if (n) { DeleteObject(n->fB); DeleteObject(n->fN); delete n; SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowNotice(const std::wstring& l1, const std::wstring& l2, bool openColors) {
    if (g_notice) DestroyWindow(g_notice);
    POINT cur; GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96; GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    double s = g_set.followDpi ? dx / 96.0 : g_set.sizePct / 100.0;
    Notice* n = new Notice();
    n->l1 = l1; n->l2 = l2; n->openColors = openColors; n->start = GetTickCount(); n->scale = s;
    n->fB = MakeFont(13 * s, true); n->fN = MakeFont(12 * s, false);
    // width to fit the longer line
    HDC dc = GetDC(nullptr);
    HGDIOBJ o = SelectObject(dc, n->fB); SIZE a; GetTextExtentPoint32W(dc, l1.c_str(), (int)l1.size(), &a);
    SelectObject(dc, n->fN); SIZE b; GetTextExtentPoint32W(dc, l2.c_str(), (int)l2.size(), &b);
    SelectObject(dc, o); ReleaseDC(nullptr, dc);
    int w = (a.cx > b.cx ? a.cx : b.cx) + (int)(56 * s), h = (int)(64 * s);
    int maxW = (mi.rcWork.right - mi.rcWork.left) - 20; if (w > maxW) w = maxW;
    int x = cur.x + (int)(12 * s), y = cur.y + (int)(18 * s);
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w - 4;
    if (y + h > mi.rcWork.bottom) y = cur.y - h - (int)(8 * s);
    if (x < mi.rcWork.left) x = mi.rcWork.left + 4;
    HWND hw = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, NOTICE_CLASS, APP_NAME, WS_POPUP,
                              x, y, w, h, nullptr, nullptr, g_inst, n);
    if (!hw) { DeleteObject(n->fB); DeleteObject(n->fN); delete n; MessageBeep(MB_OK); return; }
    DWORD pref = 3; DwmSetWindowAttribute(hw, 33, &pref, sizeof(pref));
    COLORREF bc = C_BORDER; DwmSetWindowAttribute(hw, 34, &bc, sizeof(bc));
    ShowWindow(hw, SW_SHOWNOACTIVATE);
    SetTimer(hw, NOTICE_TIMER, NOTICE_MS, nullptr);
    g_notice = hw;
    MessageBeep(MB_OK);
}

// ---------------------------------------------------------------------------
// Other modes: translucency, back / top, tray, all windows
// ---------------------------------------------------------------------------
static bool TransparencyEnabled() {   // Settings > Personalization > Colors > Transparency effects
    DWORD v = 1, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS) return true;
    return v != 0;
}

static int FindTrans(HWND h) { for (size_t i = 0; i < g_trans.size(); i++) if (g_trans[i].h == h) return (int)i; return -1; }

static void ApplyBarAlpha(Rolled* r) {
    if (!r || !r->bar) return;
    bool t = FindTrans(r->target) >= 0;
    LONG_PTR ex = GetWindowLongPtrW(r->bar, GWL_EXSTYLE);
    if (t) {
        if (!(ex & WS_EX_LAYERED)) SetWindowLongPtrW(r->bar, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        SetLayeredWindowAttributes(r->bar, 0, TRANSLUCENT_ALPHA, LWA_ALPHA);
    } else if (ex & WS_EX_LAYERED) {
        SetWindowLongPtrW(r->bar, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
        RedrawWindow(r->bar, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    }
}

static void RemoveTranslucent(int i) {
    Translucent a = g_trans[i];
    g_trans.erase(g_trans.begin() + i);
    if (IsWindow(a.h)) {
        SetLayeredWindowAttributes(a.h, a.key, 255, a.flags | LWA_ALPHA);
        if (a.addedLayered) SetWindowLongPtrW(a.h, GWL_EXSTYLE, GetWindowLongPtrW(a.h, GWL_EXSTYLE) & ~WS_EX_LAYERED);
    }
    ApplyBarAlpha(FindByTarget(a.h));
}

static void RestoreAllTranslucent() {
    while (!g_trans.empty()) RemoveTranslucent((int)g_trans.size() - 1);
    SaveRolledState();
}

static void ToggleTranslucent(HWND h) {
    int i = FindTrans(h);
    if (i >= 0) { RemoveTranslucent(i); SaveRolledState(); return; }
    if (!TransparencyEnabled()) {   // follow Windows' own setting
        ShowNotice(L"透明効果がオフのため、半透明にできません",
                   L"個人用設定 → 色 →「透明効果」をオンにしてください（クリックで設定を開く）", true);
        return;
    }
    Translucent a; a.h = h; GetWindowThreadProcessId(h, &a.pid);
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_LAYERED) {
        BYTE alpha = 255; DWORD fl = 0; COLORREF key = 0;
        // Windows drawn with UpdateLayeredWindow cannot be changed this way.
        if (!GetLayeredWindowAttributes(h, &key, &alpha, &fl) || ((fl & LWA_ALPHA) && alpha < 255)) {
            ShowNotice(L"このウィンドウは半透明にできません", L"アプリが独自の方法で透明度を扱っています", false);
            return;
        }
        a.key = key; a.flags = fl & LWA_COLORKEY;
    } else {
        SetWindowLongPtrW(h, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        a.addedLayered = true;
    }
    if (!SetLayeredWindowAttributes(h, a.key, TRANSLUCENT_ALPHA, a.flags | LWA_ALPHA)) {
        if (a.addedLayered) SetWindowLongPtrW(h, GWL_EXSTYLE, ex);
        ShowNotice(L"このウィンドウは半透明にできません", L"アプリ側が外からの変更を受け付けていません", false);
        return;
    }
    g_trans.push_back(a);
    ApplyBarAlpha(FindByTarget(h));
    SaveRolledState();
}

static void SendToBack(HWND h) {
    SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

static void ToggleTopmost(HWND h) {
    bool top = (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    SetWindowPos(h, top ? HWND_NOTOPMOST : HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

// --- tray ---
static Trayed* FindTrayedById(UINT id) { for (auto* t : g_trayed) if (t->id == id) return t; return nullptr; }

static std::wstring TrayLabel(const Trayed* t) {
    std::wstring s = t->title.empty() ? L"(タイトルなし)" : t->title;
    if (s.size() > 60) s = s.substr(0, 58) + L"…";
    return s;
}

static void AddTrayedIcon(Trayed* t) {
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_main; nid.uID = t->id;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAYWIN;
    nid.hIcon = t->icon;
    std::wstring tip = TrayLabel(t);
    wcsncpy(nid.szTip, tip.c_str(), 127); nid.szTip[127] = 0;
    t->hasIcon = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

static void RemoveTrayedIcon(Trayed* t) {
    if (!t->hasIcon) return;
    NOTIFYICONDATAW nid = { sizeof(nid) }; nid.hWnd = g_main; nid.uID = t->id;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    t->hasIcon = false;
}

static void RemoveTrayed(Trayed* t) {
    RemoveTrayedIcon(t);
    for (size_t i = 0; i < g_trayed.size(); i++) if (g_trayed[i] == t) { g_trayed.erase(g_trayed.begin() + i); break; }
    if (t->icon) DestroyIcon(t->icon);
    delete t;
    SaveRolledState();
    UpdateTrayIcon();
}

static void TrayWindow(HWND h) {
    if (!IsRollable(h)) return;
    for (auto* t : g_trayed) if (t->target == h) return;
    Trayed* t = new Trayed();
    t->target = h;
    GetWindowThreadProcessId(h, &t->pid);
    t->title = ReadTitle(h);
    t->icon = GetWindowIconCopy(h, t->pid, false);
    t->id = g_nextTrayId++;
    g_trayed.push_back(t);
    SaveRolledState();               // record BEFORE hiding
    ShowWindowAsync(h, SW_HIDE);
    if (g_set.trayAsMenu) UpdateTrayIcon();   // make sure our own icon is there to hold the menu
    else AddTrayedIcon(t);
}

static void RestoreTrayed(Trayed* t, bool activate) {
    HWND h = t->target;
    if (IsWindow(h)) {
        ShowWindowAsync(h, SW_SHOW);
        if (activate) SetForegroundWindow(h);
    }
    RemoveTrayed(t);
}

static void RestoreAllTrayed() { while (!g_trayed.empty()) RestoreTrayed(g_trayed.back(), false); }

static void ApplyTrayMode() {   // after the "as icon / as menu" setting changes
    for (auto* t : g_trayed) {
        if (g_set.trayAsMenu) RemoveTrayedIcon(t);
        else if (!t->hasIcon) AddTrayedIcon(t);
    }
    UpdateTrayIcon();
}

static void ShowTrayedMenu(Trayed* t) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, TrayLabel(t).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TW_RESTORE, L"元に戻す");
    AppendMenuW(m, MF_STRING, IDM_TW_CLOSE, L"閉じる");
    SetMenuDefaultItem(m, IDM_TW_RESTORE, FALSE);
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_main);
    UINT cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g_main, nullptr);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(m);
    UINT id = t->id;
    Trayed* still = FindTrayedById(id);
    if (!still) return;
    if (cmd == IDM_TW_RESTORE) RestoreTrayed(still, true);
    else if (cmd == IDM_TW_CLOSE) {
        HWND h = still->target;
        RestoreTrayed(still, true);
        if (IsWindow(h)) PostMessageW(h, WM_SYSCOMMAND, SC_CLOSE, 0);
    }
}

// --- all windows ---
static bool IsAppWindow(HWND h) {
    if (!IsRollable(h)) return false;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return false;
    if (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    return true;
}

static BOOL CALLBACK CollectProc(HWND h, LPARAM lp) {
    if (IsAppWindow(h)) ((std::vector<HWND>*)lp)->push_back(h);
    return TRUE;
}

static std::vector<HWND> AppWindows() {
    std::vector<HWND> v; EnumWindows(CollectProc, (LPARAM)&v); return v;
}

static void DoAction(int act, HWND h) {
    switch (act) {
    case A_ROLL: RollWindow(h); break;
    case A_ROLL_ALL: for (HWND w : AppWindows()) RollWindow(w); break;
    case A_TRANSLUCENT: if (IsWindow(h)) ToggleTranslucent(h); break;
    case A_BACK: if (IsWindow(h)) SendToBack(h); break;
    case A_TOPMOST: if (IsWindow(h)) ToggleTopmost(h); break;
    case A_TRAY: TrayWindow(h); break;
    case A_MIN_ALL:
        for (HWND w : AppWindows())
            if (GetWindowLongPtrW(w, GWL_STYLE) & WS_MINIMIZEBOX) ShowWindowAsync(w, SW_MINIMIZE);
        break;
    case A_MAX_ALL:
        for (HWND w : AppWindows())
            if (GetWindowLongPtrW(w, GWL_STYLE) & WS_MAXIMIZEBOX) ShowWindowAsync(w, SW_MAXIMIZE);
        break;
    case A_CLOSE_ALL: {
        static bool s_asking = false;      // don't open a second confirmation while one is shown
        if (s_asking) break;
        std::vector<HWND> ws = AppWindows();
        if (ws.empty()) break;
        wchar_t msg[256];
        swprintf(msg, 256, L"表示中のウィンドウ %d 個をすべて閉じますか？\n（保存していない内容があるアプリは、それぞれ確認が出ます）", (int)ws.size());
        SetForegroundWindow(g_main);
        s_asking = true;
        int ans = MessageBoxW(g_main, msg, APP_NAME, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND | MB_TOPMOST);
        s_asking = false;
        if (ans == IDYES)
            for (HWND w : ws) if (IsWindow(w)) PostMessageW(w, WM_SYSCOMMAND, SC_CLOSE, 0);
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// Bar window
// ---------------------------------------------------------------------------
static void EnsureBarFonts(Rolled* r, double s) {
    if (r->fontScale == s && r->fontBold) return;
    if (r->fontBold) DeleteObject(r->fontBold);
    if (r->fontNormal) DeleteObject(r->fontNormal);
    if (r->fontSmall) DeleteObject(r->fontSmall);
    r->fontBold = MakeFont(13 * s, true);
    r->fontNormal = MakeFont(13 * s, false);
    r->fontSmall = MakeFont(11 * s, false);
    r->fontScale = s;
}

static void PaintBar(HWND hwnd, Rolled* r) {
    PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    double s = BarScale(hwnd);
    EnsureBarFonts(r, s);

    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);

    BarColors col = GetBarColors(GetActiveWindow() == hwnd);
    DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &col.border, sizeof(col.border));
    HBRUSH bg = CreateSolidBrush(col.bg); FillRect(dc, &rc, bg); DeleteObject(bg);
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        // pill on the right
        std::wstring pill = L"巻き上げ中";
        HGDIOBJ of = SelectObject(dc, r->fontSmall);
        SIZE ts; GetTextExtentPoint32W(dc, pill.c_str(), (int)pill.size(), &ts);
        SelectObject(dc, of);
        int pw = ts.cx + (int)(16 * s), ph = (int)(20 * s);
        RECT pr = { W - (int)(8 * s) - pw, (H - ph) / 2, W - (int)(8 * s), (H - ph) / 2 + ph };
        bool showPill = W > pw + (int)(140 * s);
        if (showPill) {
            FillRound(g, pr, 6 * (float)s, col.pillBg, col.pillBorder);
        }
        g.Flush();
        if (showPill) DrawTextIn(dc, pill, pr, r->fontSmall, col.sub, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        int x = (int)(10 * s);
        int is = (int)(16 * s);
        if (r->iconSmall) DrawIconEx(dc, x, (H - is) / 2, r->iconSmall, is, is, 0, nullptr, DI_NORMAL);
        x += is + (int)(8 * s);
        int right = showPill ? pr.left - (int)(8 * s) : W - (int)(8 * s);

        std::wstring app = r->appName, tt = DisplayTitle(r);
        if (!app.empty() && x < right) {
            HGDIOBJ o = SelectObject(dc, r->fontBold);
            SIZE as; GetTextExtentPoint32W(dc, app.c_str(), (int)app.size(), &as);
            SelectObject(dc, o);
            int aw = as.cx; if (x + aw > right) aw = right - x;
            RECT ar = { x, 0, x + aw, H };
            DrawTextIn(dc, app, ar, r->fontBold, col.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            x += aw + (int)(8 * s);
        }
        if (!tt.empty() && x < right) {
            RECT tr = { x, 0, right, H };
            DrawTextIn(dc, tt, tr, r->fontNormal, col.sub, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    BitBlt(hdc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp); DeleteObject(bmp); DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

static void CloseTargetFromBar(Rolled* r) {
    HWND t = r->target;
    UnrollWindow(r, true);  // unroll first (same as the original WinRoll 1.3)
    if (IsWindow(t)) PostMessageW(t, WM_SYSCOMMAND, SC_CLOSE, 0);
}

static LRESULT CALLBACK BarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Rolled* r = (Rolled*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE: {
        auto cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        break;
    }
    case WM_CREATE: {
        DWORD pref = 3; // DWMWCP_ROUNDSMALL
        DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &pref, sizeof(pref));
        COLORREF bc = C_BORDER;
        DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &bc, sizeof(bc));
        SetTimer(hwnd, BAR_TIMER, 1000, nullptr);
        return 0;
    }
    case WM_NCHITTEST: return HTCAPTION;   // the whole bar can be dragged
    case WM_NCRBUTTONDOWN: return 0;
    case WM_NCRBUTTONUP:
        if (r) {
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0, alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            if (alt) UnrollAll();                         // Alt: unroll every bar
            else if (shift) ToggleTranslucent(r->target); // Shift+Right = middle click
            else UnrollWindow(r, true);
        }
        return 0;
    case WM_NCMBUTTONDOWN: return 0;
    case WM_NCMBUTTONUP: if (r && !g_set.noMiddle) ToggleTranslucent(r->target); return 0;
    case WM_CONTEXTMENU: return 0;
    case WM_NCLBUTTONDBLCLK: if (r) UnrollWindow(r, true); return 0;
    case WM_SYSCOMMAND:
        switch (wp & 0xFFF0) {
        case SC_MINIMIZE: case SC_RESTORE: case SC_MAXIMIZE:
            if (r) UnrollWindow(r, true);
            return 0;
        case SC_CLOSE:
            if (r) CloseTargetFromBar(r);
            return 0;
        case SC_MOVE: break;
        case SC_KEYMENU: return 0;
        }
        break;
    case WM_CLOSE: if (r) CloseTargetFromBar(r); return 0;
    case WM_TIMER:
        if (wp == BAR_TIMER && r) {
            if (!IsWindow(r->target)) { RemoveRolled(r); return 0; }       // app has exited
            if (IsWindowVisible(r->target)) { RemoveRolled(r); return 0; } // app showed itself
            std::wstring t = ReadTitle(r->target);
            if (t != r->title) { r->title = t; UpdateBarText(r); InvalidateRect(hwnd, nullptr, FALSE); }
        }
        return 0;
    case WM_DPICHANGED:
        if (r) {
            RECT* sug = (RECT*)lp;
            int bh = BarHeightFor(BarScale(hwnd));
            SetWindowPos(hwnd, nullptr, sug->left, sug->top, sug->right - sug->left, bh, SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_DWMCOLORIZATIONCOLORCHANGED: case WM_SETTINGCHANGE: case WM_SYSCOLORCHANGE:
    case WM_ACTIVATE:
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
        if (r) { PaintBar(hwnd, r); return 0; }
        break;
    case WM_DESTROY:
        KillTimer(hwnd, BAR_TIMER);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void RelayoutBars() {
    for (auto* r : g_rolled) {
        RECT br; GetWindowRect(r->bar, &br);
        int bh = BarHeightFor(BarScale(r->bar));
        SetWindowPos(r->bar, nullptr, 0, 0, br.right - br.left, bh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(r->bar, nullptr, FALSE);
    }
}

// ---------------------------------------------------------------------------
// Mouse hook: right-click on a title bar rolls the window
// ---------------------------------------------------------------------------
typedef BOOL (WINAPI *PhysToLogFn)(HWND, LPPOINT);
static PhysToLogFn g_physToLog;

static LRESULT HitTestAt(HWND h, POINT pt) {
    POINT p = pt;
    if (g_physToLog) g_physToLog(h, &p);
    DWORD_PTR ht = 0;
    if (!SendMessageTimeoutW(h, WM_NCHITTEST, 0, MAKELPARAM((short)p.x, (short)p.y),
                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &ht)) return HTNOWHERE;
    return (LRESULT)ht;
}

static bool IsInteresting(LRESULT ht) {
    return ht == HTCAPTION || ht == HTCLOSE || ht == HTMINBUTTON || ht == HTMAXBUTTON;
}

// Decide what a click means. Shift+Right counts as a middle click (as in the original WinRoll).
static int DecideAction(WPARAM btn, LRESULT ht, bool shift, bool alt) {
    bool middle = (btn == WM_MBUTTONDOWN) || (btn == WM_RBUTTONDOWN && shift);
    if (middle) {
        if (ht == HTCAPTION) return A_TRANSLUCENT;
        if (ht == HTCLOSE) return A_TOPMOST;
        if (ht == HTMINBUTTON) return A_TRAY;
        return 0;
    }
    if (btn == WM_RBUTTONDOWN) {
        if (ht == HTCAPTION) return alt ? A_ROLL_ALL : A_ROLL;
        if (ht == HTCLOSE && !alt) return A_BACK;
        return 0;
    }
    // left button: only with Shift or Alt
    if (shift && !alt && ht == HTCAPTION) return A_BACK;
    if (alt && !shift) {
        if (ht == HTMINBUTTON) return A_MIN_ALL;
        if (ht == HTMAXBUTTON) return A_MAX_ALL;
        if (ht == HTCLOSE) return A_CLOSE_ALL;
    }
    return 0;
}

static LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
    static WPARAM swallowUp = 0;   // the button-up message that belongs to a click we handled
    if (code == HC_ACTION) {
        auto* m = (MSLLHOOKSTRUCT*)lp;
        if (swallowUp && wp == swallowUp) { swallowUp = 0; return 1; }
        if (!(m->flags & LLMHF_INJECTED) && (wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN || wp == WM_LBUTTONDOWN)) {
            bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            bool alt   = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool test = false;
            if (!ctrl) {
                if (wp == WM_RBUTTONDOWN) test = true;
                else if (wp == WM_MBUTTONDOWN) test = !g_noMiddleFlag;
                else test = shift || alt;          // plain left clicks are never touched
            }
            if (test) {
                HWND under = WindowFromPoint(m->pt);
                HWND root = under ? GetAncestor(under, GA_ROOT) : nullptr;
                if (root && !IsHungAppWindow(root) && IsRollable(root)) {   // never wait on a hung app
                    DWORD t0 = GetTickCount();
                    LRESULT ht = HitTestAt(root, m->pt);
                    // Ask the child window only if the first answer came back quickly.
                    if (!IsInteresting(ht) && under != root && GetTickCount() - t0 < 30) ht = HitTestAt(under, m->pt);
                    int act = DecideAction(wp, ht, shift, alt);
                    if (act) {
                        swallowUp = (wp == WM_RBUTTONDOWN) ? WM_RBUTTONUP : (wp == WM_MBUTTONDOWN) ? WM_MBUTTONUP : WM_LBUTTONUP;
                        PostMessageW(g_main, WM_APP_ACTION, (WPARAM)act, (LPARAM)root);
                        return 1;
                    }
                }
            }
        }
    }
    return CallNextHookEx(g_mouseHook, code, wp, lp);
}

// ---------------------------------------------------------------------------
// Panel windows (settings / operations list) — owner drawn, scalable
// ---------------------------------------------------------------------------
enum ItemKind { K_CARD, K_HEAD, K_TEXT, K_TEXT_BOLD, K_TEXT_MUTED, K_CHECK, K_SEG, K_BTN, K_BTN_PRIMARY, K_LINK, K_TROW, K_THEAD, K_FOOTER, K_RADIO };
enum ItemId { I_NONE, I_SEG, I_FOLLOW, I_AUTO, I_HIDE, I_OK, I_CANCEL, I_ABOUT, I_CLOSE, I_GITHUB, I_NOMIDDLE, I_TRAY_ICON, I_TRAY_MENU, I_SPLASH };

struct Item {
    ItemKind kind; RECT rc; std::wstring text, sub; int id = I_NONE; int value = 0;
    bool on = false; bool enabled = true; bool future = false;
};

struct Panel {
    bool isSettings = false;
    Settings temp;               // edited copy (settings only)
    std::vector<Item> items;
    int baseW = 440, baseH = 400;
    int hover = -1, pressed = -1;
    double fontScale = 0;
    HFONT fN = nullptr, fB = nullptr, fS = nullptr, fSB = nullptr;
};

static Item MkItem(ItemKind k, int x, int y, int w, int h, const std::wstring& t = L"", int id = I_NONE) {
    Item it; it.kind = k; it.rc = { x, y, x + w, y + h }; it.text = t; it.id = id; return it;
}

static void BuildSettings(Panel* p) {
    p->items.clear();
    const Settings& s = p->temp;
    int W = 480, x = 16, cw = W - 32, y = 16;

    // 表示サイズ
    int cy = y;
    p->items.push_back(MkItem(K_CARD, x, cy, cw, 116));
    p->items.push_back(MkItem(K_HEAD, x + 14, cy + 12, cw - 28, 18, L"表示サイズ（設定画面・操作一覧）"));
    const int sizes[] = {100,125,150,175,200};
    int gap = 6, bw = (cw - 28 - gap * 4) / 5;
    for (int i = 0; i < 5; i++) {
        Item it = MkItem(K_SEG, x + 14 + i * (bw + gap), cy + 38, bw, 32, std::to_wstring(sizes[i]) + L"%", I_SEG);
        it.value = sizes[i]; it.on = (!s.followDpi && s.sizePct == sizes[i]); it.enabled = !s.followDpi;
        p->items.push_back(it);
    }
    Item f = MkItem(K_CHECK, x + 14, cy + 80, cw - 28, 24, L"Windowsの表示スケールに合わせる", I_FOLLOW); f.on = s.followDpi;
    p->items.push_back(f);
    y = cy + 116 + 12;

    // 起動
    cy = y;
    p->items.push_back(MkItem(K_CARD, x, cy, cw, 134));
    p->items.push_back(MkItem(K_HEAD, x + 14, cy + 12, cw - 28, 18, L"起動"));
    Item a = MkItem(K_CHECK, x + 14, cy + 38, cw - 28, 24, L"Windowsの起動時に自動で起動する", I_AUTO); a.on = s.autoStart;
    Item h = MkItem(K_CHECK, x + 14, cy + 68, cw - 28, 24, L"トレイアイコンを隠す（もう一度起動すると再表示）", I_HIDE); h.on = s.hideIcon;
    Item sp = MkItem(K_CHECK, x + 14, cy + 98, cw - 28, 24, L"起動時にスプラッシュ画面を表示する（自動起動のときも）", I_SPLASH); sp.on = s.showSplash;
    p->items.push_back(a); p->items.push_back(h); p->items.push_back(sp);
    y = cy + 134 + 12;

    // 操作
    cy = y;
    p->items.push_back(MkItem(K_CARD, x, cy, cw, 74));
    p->items.push_back(MkItem(K_HEAD, x + 14, cy + 12, cw - 28, 18, L"操作"));
    Item nm = MkItem(K_CHECK, x + 14, cy + 38, cw - 28, 24, L"マウスの中ボタンを使わない（Shift＋右クリックで代用）", I_NOMIDDLE); nm.on = s.noMiddle;
    p->items.push_back(nm);
    y = cy + 74 + 12;

    // トレイへ最小化したとき
    cy = y;
    p->items.push_back(MkItem(K_CARD, x, cy, cw, 74));
    p->items.push_back(MkItem(K_HEAD, x + 14, cy + 12, cw - 28, 18, L"トレイへ最小化したとき"));
    Item ti = MkItem(K_RADIO, x + 14, cy + 38, 170, 24, L"アイコンで表示", I_TRAY_ICON); ti.on = !s.trayAsMenu;
    Item tm = MkItem(K_RADIO, x + 14 + 190, cy + 38, 190, 24, L"メニューにまとめる", I_TRAY_MENU); tm.on = s.trayAsMenu;
    p->items.push_back(ti); p->items.push_back(tm);
    y = cy + 74 + 12;

    p->items.push_back(MkItem(K_TEXT_MUTED, x + 2, y, cw - 4, 40,
        L"半透明は、Windows の「個人用設定 → 色 → 透明効果」がオンのときだけ使えます（濃さは 80%）。"));
    y += 40 + 12;

    // footer
    p->items.push_back(MkItem(K_FOOTER, 0, y, W, 57));
    Item ab = MkItem(K_BTN, 16, y + 12, 128, 32, L"操作一覧…", I_ABOUT); ab.sub = L"F1";
    Item ok = MkItem(K_BTN_PRIMARY, W - 16 - 104 - 8 - 104, y + 12, 104, 32, L"OK", I_OK); ok.sub = L"Enter";
    Item cn = MkItem(K_BTN, W - 16 - 104, y + 12, 104, 32, L"キャンセル", I_CANCEL); cn.sub = L"Esc";
    p->items.push_back(ab); p->items.push_back(ok); p->items.push_back(cn);
    p->baseW = W; p->baseH = y + 57;
}

static void BuildAbout(Panel* p) {
    p->items.clear();
    int W = 580, x = 16, cw = W - 32, y = 16;
    struct Row { const wchar_t* op; const wchar_t* act; bool future; };
    const Row rows[] = {
        { L"タイトルバーを右クリック",       L"ウィンドウを巻き上げる",              false },
        { L"巻き上げたバーを右クリック",     L"元に戻す（ダブルクリックでも可）",     false },
        { L"巻き上げたバーをドラッグ",       L"移動（戻したときに位置が反映）",       false },
        { L"巻き上げたバーで Alt＋F4",       L"元に戻してからアプリを閉じる",         false },
        { L"タイトルバーを中クリック",       L"半透明にする／元に戻す",              false },
        { L"Shift＋タイトルバーを左クリック", L"いちばん後ろへ送る",                  false },
        { L"閉じるボタンを右クリック",       L"いちばん後ろへ送る",                  false },
        { L"閉じるボタンを中クリック",       L"常に手前に表示する／やめる",          false },
        { L"最小化ボタンを中クリック",       L"タスクトレイへ最小化する",            false },
        { L"Alt＋タイトルバーを右クリック",  L"すべて巻き上げる（バー側ではすべて戻す）", false },
        { L"Alt＋最小化・最大化・閉じるを左クリック", L"すべてのウィンドウに適用する", false },
    };
    int n = (int)(sizeof(rows) / sizeof(rows[0]));
    int th = 32 + n * 34;
    p->items.push_back(MkItem(K_CARD, x, y, cw, th));
    Item hd = MkItem(K_THEAD, x, y, cw, 32, L"操作"); hd.sub = L"はたらき"; p->items.push_back(hd);
    for (int i = 0; i < n; i++) {
        Item it = MkItem(K_TROW, x, y + 32 + i * 34, cw, 34, rows[i].op);
        it.sub = rows[i].act; it.future = rows[i].future; it.on = (i == n - 1);
        p->items.push_back(it);
    }
    y += th + 10;
    p->items.push_back(MkItem(K_TEXT_MUTED, x + 2, y, cw - 4, 40,
        L"中ボタンの操作は、すべて「Shift＋右クリック」でも代用できます。半透明は Windows の「透明効果」がオンのときだけ使えます。"));
    y += 40 + 10;

    int iy = y;
    p->items.push_back(MkItem(K_CARD, x, iy, cw, 104));
    p->items.push_back(MkItem(K_TEXT_BOLD, x + 14, iy + 10, cw - 28, 22, std::wstring(APP_NAME) + L"　ver " + APP_VERSION));
    p->items.push_back(MkItem(K_TEXT, x + 14, iy + 34, 70, 20, L"作者：KINA"));
    p->items.push_back(MkItem(K_LINK, x + 90, iy + 34, 170, 20, L"github.com/KinaPPP", I_GITHUB));
    p->items.push_back(MkItem(K_TEXT_MUTED, x + 14, iy + 58, cw - 28, 40,
        L"オリジナルの WinRoll（© 2003–2004 Wil Palma）に着想を得て、新たに作り直したソフトウェアです。オリジナル版とは別のものです。"));
    y = iy + 104 + 12;

    p->items.push_back(MkItem(K_FOOTER, 0, y, W, 57));
    Item ok = MkItem(K_BTN_PRIMARY, W - 16 - 104, y + 12, 104, 32, L"OK", I_CLOSE); ok.sub = L"Enter";
    p->items.push_back(ok);
    p->baseW = W; p->baseH = y + 57;
}

static double PanelScale(HWND hwnd, Panel* p) {
    if (p->isSettings) {
        if (p->temp.followDpi) { UINT d = GetDpiForWindow(hwnd); return (d ? d : 96) / 96.0; }
        return p->temp.sizePct / 100.0;
    }
    return ScaleForWindow(hwnd);
}

static RECT Sc(RECT r, double s) {
    RECT o = { (LONG)(r.left * s + 0.5), (LONG)(r.top * s + 0.5), (LONG)(r.right * s + 0.5), (LONG)(r.bottom * s + 0.5) };
    return o;
}

static void EnsurePanelFonts(Panel* p, double s) {
    if (p->fontScale == s && p->fN) return;
    if (p->fN) DeleteObject(p->fN); if (p->fB) DeleteObject(p->fB);
    if (p->fS) DeleteObject(p->fS); if (p->fSB) DeleteObject(p->fSB);
    p->fN = MakeFont(13 * s, false); p->fB = MakeFont(13 * s, true);
    p->fS = MakeFont(11.5 * s, false); p->fSB = MakeFont(12 * s, true);
    p->fontScale = s;
}

static void ResizePanel(HWND hwnd, Panel* p) {
    double s = PanelScale(hwnd, p);
    RECT rc = { 0, 0, (LONG)(p->baseW * s + 0.5), (LONG)(p->baseH * s + 0.5) };
    UINT dpi = GetDpiForWindow(hwnd);
    AdjustWindowRectExForDpi(&rc, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE,
                             (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE), dpi ? dpi : 96);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    RECT wr; GetWindowRect(hwnd, &wr);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    int x = wr.left, y = wr.top;
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
    if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void DrawButton(Gdiplus::Graphics& g, HDC dc, const Item& it, RECT r, double s, Panel* p, bool hov) {
    bool primary = it.kind == K_BTN_PRIMARY;
    COLORREF fill = primary ? (hov ? C_BLUE_HV : C_BLUE) : (hov ? C_HOVER : C_PANEL);
    COLORREF border = primary ? C_BLUE_ACT : C_BORDER;
    FillRound(g, r, 6 * (float)s, fill, border);
    g.Flush();
    COLORREF tc = primary ? RGB(255,255,255) : C_TEXT;
    RECT tr = r; tr.left += (int)(10 * s); tr.right -= (int)(10 * s);
    DrawTextIn(dc, it.text, tr, p->fN, tc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (!it.sub.empty()) DrawTextIn(dc, it.sub, tr, p->fS, primary ? RGB(255,255,255) : C_SUB, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

static void PaintPanel(HWND hwnd, Panel* p) {
    PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
    RECT cr; GetClientRect(hwnd, &cr);
    double s = PanelScale(hwnd, p);
    EnsurePanelFonts(p, s);
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
    HGDIOBJ ob = SelectObject(dc, bmp);
    HBRUSH bg = CreateSolidBrush(C_PANEL); FillRect(dc, &cr, bg); DeleteObject(bg);
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        for (size_t i = 0; i < p->items.size(); i++) {
            const Item& it = p->items[i];
            RECT r = Sc(it.rc, s);
            bool hov = ((int)i == p->hover) && it.enabled;
            switch (it.kind) {
            case K_CARD: FillRound(g, r, 6 * (float)s, C_CARD, C_BORDER); g.Flush(); break;
            case K_FOOTER: {
                HBRUSH b = CreateSolidBrush(C_CARD); FillRect(dc, &r, b); DeleteObject(b);
                RECT line = { r.left, r.top, r.right, r.top + (s >= 1.5 ? 2 : 1) };
                HBRUSH lb = CreateSolidBrush(C_BORDER); FillRect(dc, &line, lb); DeleteObject(lb);
                break;
            }
            case K_HEAD: DrawTextIn(dc, it.text, r, p->fSB, C_SUB, DT_LEFT | DT_VCENTER | DT_SINGLELINE); break;
            case K_TEXT: DrawTextIn(dc, it.text, r, p->fN, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE); break;
            case K_TEXT_BOLD: DrawTextIn(dc, it.text, r, p->fB, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE); break;
            case K_TEXT_MUTED: DrawTextIn(dc, it.text, r, p->fS, C_SUB, DT_LEFT | DT_WORDBREAK); break;
            case K_LINK: DrawTextIn(dc, it.text, r, p->fN, hov ? C_TEXT : C_BLUE_ACT, DT_LEFT | DT_VCENTER | DT_SINGLELINE); break;
            case K_CHECK: {
                int bx = (int)(16 * s);
                RECT box = { r.left, r.top + (r.bottom - r.top - bx) / 2, r.left + bx, r.top + (r.bottom - r.top - bx) / 2 + bx };
                if (it.on) {
                    FillRound(g, box, 3 * (float)s, hov ? C_BLUE_HV : C_BLUE, C_BLUE_ACT);
                    Gdiplus::Pen pen(Gdiplus::Color(255,255,255,255), (float)(2 * s));
                    pen.SetLineJoin(Gdiplus::LineJoinRound); pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound);
                    Gdiplus::PointF pts[3] = {
                        { box.left + bx * 0.24f, box.top + bx * 0.52f },
                        { box.left + bx * 0.43f, box.top + bx * 0.70f },
                        { box.left + bx * 0.77f, box.top + bx * 0.32f } };
                    g.DrawLines(&pen, pts, 3);
                } else {
                    FillRound(g, box, 3 * (float)s, C_CARD, hov ? C_BLUE : C_MUTED, (float)(s >= 1.5 ? 1.5 * s / 1.5 : 1.0));
                }
                g.Flush();
                RECT tr = r; tr.left += bx + (int)(8 * s);
                DrawTextIn(dc, it.text, tr, p->fN, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                break;
            }
            case K_RADIO: {
                int bx = (int)(16 * s);
                float cx0 = (float)r.left, cy0 = (float)(r.top + (r.bottom - r.top - bx) / 2);
                Gdiplus::SolidBrush wb(GC(C_CARD));
                g.FillEllipse(&wb, cx0, cy0, (float)bx - 1, (float)bx - 1);
                Gdiplus::Pen pen(GC(it.on ? C_BLUE : (hov ? C_BLUE : C_MUTED)), (float)(s >= 1.5 ? 1.5 : 1.0));
                g.DrawEllipse(&pen, cx0 + 0.5f, cy0 + 0.5f, (float)bx - 2, (float)bx - 2);
                if (it.on) {
                    Gdiplus::SolidBrush bb(GC(hov ? C_BLUE_HV : C_BLUE));
                    float d = bx * 0.5f;
                    g.FillEllipse(&bb, cx0 + (bx - 1 - d) / 2, cy0 + (bx - 1 - d) / 2, d, d);
                }
                g.Flush();
                RECT tr = r; tr.left += bx + (int)(8 * s);
                DrawTextIn(dc, it.text, tr, p->fN, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                break;
            }
            case K_SEG: {
                COLORREF fill = it.on ? C_BLUE : (hov ? C_HOVER : C_PANEL);
                COLORREF border = it.on ? C_BLUE_ACT : (hov ? C_BLUE : C_BORDER);
                if (!it.enabled) { fill = C_PANEL; border = C_BORDER; }
                FillRound(g, r, 6 * (float)s, fill, border); g.Flush();
                COLORREF tc = it.on ? RGB(255,255,255) : (it.enabled ? C_TEXT : C_MUTED);
                DrawTextIn(dc, it.text, r, it.on ? p->fB : p->fN, tc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                break;
            }
            case K_BTN: case K_BTN_PRIMARY: DrawButton(g, dc, it, r, s, p, hov); break;
            case K_THEAD: case K_TROW: {
                bool head = it.kind == K_THEAD;
                if (head) {
                    RECT hb = { r.left + 1, r.top + 1, r.right - 1, r.bottom };
                    // header background with rounded top corners
                    Gdiplus::GraphicsPath gp;
                    RoundRectPath(gp, (float)hb.left, (float)hb.top, (float)(hb.right - hb.left), (float)(hb.bottom - hb.top) + 8 * (float)s, 6 * (float)s);
                    Gdiplus::Region rg(&gp);
                    Gdiplus::Rect clip(hb.left, hb.top, hb.right - hb.left, hb.bottom - hb.top);
                    g.SetClip(clip);
                    Gdiplus::SolidBrush b(GC(C_PANEL)); g.FillPath(&b, &gp);
                    g.ResetClip(); g.Flush();
                }
                if (!it.on) {
                    RECT line = { r.left + 1, r.bottom - 1, r.right - 1, r.bottom };
                    HBRUSH lb = CreateSolidBrush(head ? C_BORDER : RGB(0xec,0xec,0xec)); FillRect(dc, &line, lb); DeleteObject(lb);
                }
                int c1 = r.left + (int)(290 * s);
                RECT a = { r.left + (int)(12 * s), r.top, c1 - (int)(6 * s), r.bottom };
                RECT b = { c1 + (int)(12 * s), r.top, r.right - (int)(12 * s), r.bottom };
                HFONT fa = head ? p->fSB : p->fB, fb = head ? p->fSB : p->fN;
                COLORREF ca = head ? C_SUB : (it.future ? C_MUTED : C_TEXT);
                COLORREF cb = head ? C_SUB : (it.future ? C_MUTED : C_TEXT);
                DrawTextIn(dc, it.text, a, fa, ca, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                DrawTextIn(dc, it.sub, b, fb, cb, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                break;
            }
            }
        }
    }
    BitBlt(hdc, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

static bool Clickable(const Item& it) {
    if (!it.enabled) return false;
    switch (it.kind) { case K_CHECK: case K_RADIO: case K_SEG: case K_BTN: case K_BTN_PRIMARY: case K_LINK: return true; default: return false; }
}

static int HitItem(HWND hwnd, Panel* p, int x, int y) {
    double s = PanelScale(hwnd, p);
    for (int i = (int)p->items.size() - 1; i >= 0; i--) {
        if (!Clickable(p->items[i])) continue;
        RECT r = Sc(p->items[i].rc, s);
        POINT pt = { x, y };
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

static void ShowAbout();
static void UpdateTrayIcon();

static void ApplySettings(const Settings& s) {
    bool scaleChanged = (s.sizePct != g_set.sizePct || s.followDpi != g_set.followDpi);
    bool trayModeChanged = (s.trayAsMenu != g_set.trayAsMenu);
    g_set = s;
    g_noMiddleFlag = s.noMiddle;
    g_forceIcon = false;        // honour "hide tray icon" right away
    SaveSettings();
    ApplyAutoStart();
    if (trayModeChanged) ApplyTrayMode();
    UpdateTrayIcon();
    if (scaleChanged) {
        RelayoutBars();
        if (g_aboutWnd) {
            Panel* ap = (Panel*)GetWindowLongPtrW(g_aboutWnd, GWLP_USERDATA);
            if (ap) ResizePanel(g_aboutWnd, ap);
        }
    }
}

static void PanelActivate(HWND hwnd, Panel* p, int id, int value) {
    switch (id) {
    case I_SEG: p->temp.sizePct = value; BuildSettings(p); ResizePanel(hwnd, p); break;
    case I_FOLLOW: p->temp.followDpi = !p->temp.followDpi; BuildSettings(p); ResizePanel(hwnd, p); break;
    case I_AUTO: p->temp.autoStart = !p->temp.autoStart; BuildSettings(p); InvalidateRect(hwnd, nullptr, FALSE); break;
    case I_HIDE: p->temp.hideIcon = !p->temp.hideIcon; BuildSettings(p); InvalidateRect(hwnd, nullptr, FALSE); break;
    case I_SPLASH: p->temp.showSplash = !p->temp.showSplash; BuildSettings(p); InvalidateRect(hwnd, nullptr, FALSE); break;
    case I_NOMIDDLE: p->temp.noMiddle = !p->temp.noMiddle; BuildSettings(p); InvalidateRect(hwnd, nullptr, FALSE); break;
    case I_TRAY_ICON: p->temp.trayAsMenu = false; BuildSettings(p); InvalidateRect(hwnd, nullptr, FALSE); break;
    case I_TRAY_MENU: p->temp.trayAsMenu = true; BuildSettings(p); InvalidateRect(hwnd, nullptr, FALSE); break;
    case I_OK: { Settings s = p->temp; DestroyWindow(hwnd); ApplySettings(s); break; }
    case I_CANCEL: case I_CLOSE: DestroyWindow(hwnd); break;
    case I_ABOUT: ShowAbout(); break;
    case I_GITHUB: ShellExecuteW(nullptr, L"open", GITHUB_URL, nullptr, nullptr, SW_SHOWNORMAL); break;
    }
}

static LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Panel* p = (Panel*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE: {
        auto cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        break;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: if (p) { PaintPanel(hwnd, p); return 0; } break;
    case WM_MOUSEMOVE: if (p) {
        int h = HitItem(hwnd, p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h != p->hover) { p->hover = h; InvalidateRect(hwnd, nullptr, FALSE); }
        TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 }; TrackMouseEvent(&t);
        return 0; }
    case WM_MOUSELEAVE: if (p && p->hover != -1) { p->hover = -1; InvalidateRect(hwnd, nullptr, FALSE); } return 0;
    case WM_SETCURSOR:
        if (p && LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursor(nullptr, p->hover >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: if (p) { p->pressed = HitItem(hwnd, p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); SetCapture(hwnd); } return 0;
    case WM_LBUTTONUP: if (p) {
        ReleaseCapture();
        int h = HitItem(hwnd, p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h >= 0 && h == p->pressed) {
            Item it = p->items[h];
            p->pressed = -1;
            PanelActivate(hwnd, p, it.id, it.value);
            return 0;
        }
        p->pressed = -1;
        return 0; }
    case WM_KEYDOWN: if (p) {
        if (wp == VK_RETURN) { PanelActivate(hwnd, p, p->isSettings ? I_OK : I_CLOSE, 0); return 0; }
        if (wp == VK_ESCAPE) { PanelActivate(hwnd, p, p->isSettings ? I_CANCEL : I_CLOSE, 0); return 0; }
        if (wp == VK_F1 && p->isSettings) { ShowAbout(); return 0; }
        } break;
    case WM_DPICHANGED: if (p) {
        RECT* sug = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, sug->left, sug->top, sug->right - sug->left, sug->bottom - sug->top, SWP_NOZORDER | SWP_NOACTIVATE);
        ResizePanel(hwnd, p);
        return 0; }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        if (hwnd == g_settingsWnd) g_settingsWnd = nullptr;
        if (hwnd == g_aboutWnd) g_aboutWnd = nullptr;
        return 0;
    case WM_NCDESTROY:
        if (p) {
            if (p->fN) DeleteObject(p->fN); if (p->fB) DeleteObject(p->fB);
            if (p->fS) DeleteObject(p->fS); if (p->fSB) DeleteObject(p->fSB);
            delete p;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND CreatePanel(Panel* p, const wchar_t* title) {
    HWND h = CreateWindowExW(0, PANEL_CLASS, title, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 400, 300, nullptr, nullptr, g_inst, p);
    if (!h) { delete p; return nullptr; }
    SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)g_appIconSmall);
    SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)g_appIconBig);
    // Center on the monitor with the mouse cursor
    POINT cur; GetCursorPos(&cur);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowPos(h, nullptr, mi.rcWork.left + 40, mi.rcWork.top + 40, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    double s = PanelScale(h, p);
    int w = (int)(p->baseW * s), hh = (int)(p->baseH * s);
    SetWindowPos(h, nullptr, (mi.rcWork.left + mi.rcWork.right - w) / 2, (mi.rcWork.top + mi.rcWork.bottom - hh) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ResizePanel(h, p);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    return h;
}

static void ShowSettings() {
    if (g_settingsWnd) { ShowWindow(g_settingsWnd, SW_RESTORE); SetForegroundWindow(g_settingsWnd); return; }
    Panel* p = new Panel(); p->isSettings = true; p->temp = g_set; BuildSettings(p);
    g_settingsWnd = CreatePanel(p, L"シンプル WinRoll — 設定");
}

static void ShowAbout() {
    if (g_aboutWnd) { ShowWindow(g_aboutWnd, SW_RESTORE); SetForegroundWindow(g_aboutWnd); return; }
    Panel* p = new Panel(); p->isSettings = false; BuildAbout(p);
    g_aboutWnd = CreatePanel(p, L"シンプル WinRoll — 操作一覧");
}

// ---------------------------------------------------------------------------
// Splash screen
// ---------------------------------------------------------------------------
static const int SPLASH_W = 440, SPLASH_H = 212;
static const DWORD SPLASH_MS = 3000, SPLASH_FADE_MS = 400;
static const UINT_PTR SPLASH_TIMER = 3;
struct Splash { DWORD start; double scale; HFONT fTitle, fN, fB, fS; HICON icon; };

static void PaintSplash(HWND hwnd, Splash* sp) {
    PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
    RECT cr; GetClientRect(hwnd, &cr);
    double s = sp->scale;
    auto S = [s](double v) { return (int)(v * s + 0.5); };
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
    HGDIOBJ ob = SelectObject(dc, bmp);
    HBRUSH bg = CreateSolidBrush(C_CARD); FillRect(dc, &cr, bg); DeleteObject(bg);
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        RECT box = { S(24), S(106), S(SPLASH_W - 24), S(168) };
        FillRound(g, box, 6 * (float)s, C_PANEL, C_BORDER);
        g.Flush();
    }
    if (sp->icon) DrawIconEx(dc, S(24), S(24), sp->icon, S(64), S(64), 0, nullptr, DI_NORMAL);
    RECT t = { S(106), S(28), S(SPLASH_W - 24), S(60) };
    DrawTextIn(dc, APP_NAME, t, sp->fTitle, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT v = { S(106), S(62), S(SPLASH_W - 24), S(84) };
    DrawTextIn(dc, std::wstring(L"ver ") + APP_VERSION + L"　／　作者：KINA", v, sp->fN, C_SUB, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // "タイトルバーを" + bold "右クリック" + "でウィンドウを巻き上げます。"
    const wchar_t* parts[3] = { L"タイトルバーを", L"右クリック", L"でウィンドウを巻き上げます。" };
    int x = S(38), y1 = S(116), lh = S(20);
    for (int i = 0; i < 3; i++) {
        HFONT f = (i == 1) ? sp->fB : sp->fN;
        HGDIOBJ o = SelectObject(dc, f);
        SIZE sz; GetTextExtentPoint32W(dc, parts[i], (int)wcslen(parts[i]), &sz);
        SelectObject(dc, o);
        RECT r = { x, y1, x + sz.cx + 2, y1 + lh };
        DrawTextIn(dc, parts[i], r, f, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        x += sz.cx;
    }
    RECT l2 = { S(38), S(138), S(SPLASH_W - 38), S(158) };
    DrawTextIn(dc, L"設定と操作一覧は、タスクトレイのアイコンから開けます。", l2, sp->fN, C_SUB, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT f1 = { S(24), S(180), S(SPLASH_W - 24), S(200) };
    DrawTextIn(dc, L"WinRoll（© 2003–2004 Wil Palma）をマジ！リスペクト！！", f1, sp->fS, C_SUB, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(dc, L"クリックで閉じる", f1, sp->fS, C_SUB, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    // progress line
    DWORD el = GetTickCount() - sp->start;
    double prog = el >= SPLASH_MS ? 1.0 : el / (double)SPLASH_MS;
    int ph = S(3) < 2 ? 2 : S(3);
    RECT track = { 0, cr.bottom - ph, cr.right, cr.bottom };
    HBRUSH tb = CreateSolidBrush(C_HOVER); FillRect(dc, &track, tb); DeleteObject(tb);
    RECT bar = { 0, cr.bottom - ph, (int)(cr.right * prog), cr.bottom };
    HBRUSH bb = CreateSolidBrush(C_BLUE); FillRect(dc, &bar, bb); DeleteObject(bb);

    BitBlt(hdc, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK SplashProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Splash* sp = (Splash*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: if (sp) { PaintSplash(hwnd, sp); return 0; } break;
    case WM_TIMER:
        if (wp == SPLASH_TIMER && sp) {
            DWORD el = GetTickCount() - sp->start;
            if (el >= SPLASH_MS + SPLASH_FADE_MS) { DestroyWindow(hwnd); return 0; }
            if (el > SPLASH_MS) {
                BYTE a = (BYTE)(255 - 255.0 * (el - SPLASH_MS) / SPLASH_FADE_MS);
                SetLayeredWindowAttributes(hwnd, 0, a, LWA_ALPHA);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        DestroyWindow(hwnd);
        return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_DESTROY: KillTimer(hwnd, SPLASH_TIMER); return 0;
    case WM_NCDESTROY:
        if (sp) {
            DeleteObject(sp->fTitle); DeleteObject(sp->fN); DeleteObject(sp->fB); DeleteObject(sp->fS);
            if (sp->icon) DestroyIcon(sp->icon);
            delete sp;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowSplash() {
    POINT cur; GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    double s;
    if (g_set.followDpi) { UINT dx = 96, dy = 96; GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy); s = dx / 96.0; }
    else s = g_set.sizePct / 100.0;
    Splash* sp = new Splash();
    sp->start = GetTickCount(); sp->scale = s;
    sp->fTitle = MakeFont(22 * s, true); sp->fN = MakeFont(13 * s, false);
    sp->fB = MakeFont(13 * s, true); sp->fS = MakeFont(11 * s, false);
    int is = (int)(64 * s + 0.5);
    sp->icon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, is, is, 0);
    int w = (int)(SPLASH_W * s + 0.5), h = (int)(SPLASH_H * s + 0.5);
    int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;
    HWND hw = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE, SPLASH_CLASS, APP_NAME,
                              WS_POPUP, x, y, w, h, nullptr, nullptr, g_inst, sp);
    if (!hw) { delete sp; return; }
    SetLayeredWindowAttributes(hw, 0, 255, LWA_ALPHA);
    DWORD pref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hw, 33, &pref, sizeof(pref));
    COLORREF bc = C_BORDER;
    DwmSetWindowAttribute(hw, 34, &bc, sizeof(bc));
    ShowWindow(hw, SW_SHOWNOACTIVATE);
    SetTimer(hw, SPLASH_TIMER, 30, nullptr);
}

// ---------------------------------------------------------------------------
// Tray icon and main window
// ---------------------------------------------------------------------------
static void UpdateTrayIcon() {
    // Keep our icon while windows are listed in its menu, even if "hide icon" is on.
    bool want = !g_set.hideIcon || g_forceIcon || (g_set.trayAsMenu && !g_trayed.empty());
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_main; nid.uID = ID_TRAY;
    if (want && !g_trayShown) {
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_APP_TRAY;
        nid.hIcon = g_appIconSmall;
        wcscpy_s(nid.szTip, L"シンプル WinRoll");
        g_trayShown = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    } else if (!want && g_trayShown) {
        Shell_NotifyIconW(NIM_DELETE, &nid);
        g_trayShown = false;
    }
}

static void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    if (g_set.trayAsMenu && !g_trayed.empty()) {
        for (size_t i = 0; i < g_trayed.size() && i < 200; i++)
            AppendMenuW(m, MF_STRING, IDM_TRAYED_BASE + i, TrayLabel(g_trayed[i]).c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"設定…");
    AppendMenuW(m, MF_STRING, IDM_ABOUT, L"操作一覧…\tF1");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | ((g_rolled.empty() && g_trayed.empty()) ? MF_GRAYED : 0), IDM_RESTOREALL, L"すべて元に戻す");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"終了");
    SetMenuDefaultItem(m, IDM_SETTINGS, FALSE);
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_main);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_main, nullptr);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void QuitApp() {
    if (g_settingsWnd) DestroyWindow(g_settingsWnd);
    if (g_aboutWnd) DestroyWindow(g_aboutWnd);
    UnrollAll();
    RestoreAllTrayed();
    RestoreAllTranslucent();
    DestroyWindow(g_main);
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) {
        g_trayShown = false; UpdateTrayIcon();
        for (auto* t : g_trayed) { t->hasIcon = false; if (!g_set.trayAsMenu) AddTrayedIcon(t); }
        return 0;
    }
    switch (msg) {
    case WM_APP_ROLL: RollWindow((HWND)wp); return 0;
    case WM_APP_ACTION: DoAction((int)wp, (HWND)lp); return 0;
    case WM_APP_TRAYWIN: {
        Trayed* t = FindTrayedById((UINT)wp);
        if (t) {
            switch (LOWORD(lp)) {
            case WM_LBUTTONUP: RestoreTrayed(t, true); break;
            case WM_RBUTTONUP: case WM_CONTEXTMENU: ShowTrayedMenu(t); break;
            }
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == MAIN_TIMER) {
            for (auto* t : std::vector<Trayed*>(g_trayed))
                if (!IsWindow(t->target) || IsWindowVisible(t->target)) RemoveTrayed(t);   // gone, or the app showed itself
            bool changed = false;
            for (int i = (int)g_trans.size() - 1; i >= 0; i--)
                if (!IsWindow(g_trans[i].h)) { g_trans.erase(g_trans.begin() + i); changed = true; }
            if (changed) SaveRolledState();
        }
        return 0;
    case WM_DWMCOLORIZATIONCOLORCHANGED: case WM_SETTINGCHANGE: case WM_SYSCOLORCHANGE:
        for (auto* r : g_rolled) InvalidateRect(r->bar, nullptr, FALSE);
        if (!g_trans.empty() && !TransparencyEnabled()) RestoreAllTranslucent();   // transparency effects turned off
        break;
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case WM_RBUTTONUP: case WM_CONTEXTMENU: ShowTrayMenu(); break;
        case WM_LBUTTONDBLCLK: ShowSettings(); break;
        }
        return 0;
    case WM_APP_REACTIVATE:
        g_forceIcon = true; UpdateTrayIcon(); ShowSettings();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_SETTINGS: ShowSettings(); break;
        case IDM_ABOUT: ShowAbout(); break;
        case IDM_RESTOREALL: UnrollAll(); RestoreAllTrayed(); break;
        case IDM_EXIT: QuitApp(); break;
        default:
            if (LOWORD(wp) >= IDM_TRAYED_BASE && LOWORD(wp) < IDM_TRAYED_BASE + 200) {
                size_t i = LOWORD(wp) - IDM_TRAYED_BASE;
                if (i < g_trayed.size()) RestoreTrayed(g_trayed[i], true);
            }
        }
        return 0;
    case WM_QUERYENDSESSION:
        RestoreAllHidden();      // let apps save their real (visible) state
        return TRUE;
    case WM_ENDSESSION:
        if (wp) { UnrollAll(); RestoreAllTrayed(); RestoreAllTranslucent(); }
        return 0;
    case WM_DESTROY: {
        KillTimer(hwnd, MAIN_TIMER);
        for (auto* t : g_trayed) RemoveTrayedIcon(t);
        NOTIFYICONDATAW nid = { sizeof(nid) }; nid.hWnd = hwnd; nid.uID = ID_TRAY;
        if (g_trayShown) Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS*) {
    RestoreAllHidden();
    return EXCEPTION_EXECUTE_HANDLER;
}

static BOOL WINAPI ConsoleHandler(DWORD) { RestoreAllHidden(); return FALSE; }

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
// The mouse hook lives on its own thread, so a busy main thread never delays mouse input.
// Windows silently drops a low-level hook that is too slow, so it is re-installed every 30 s.
static DWORD WINAPI HookThreadProc(LPVOID ready) {
    MSG m;
    PeekMessageW(&m, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // create this thread's message queue
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, g_inst, 0);
    SetEvent((HANDLE)ready);
    UINT_PTR t = SetTimer(nullptr, 0, 30000, nullptr);
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_TIMER && m.hwnd == nullptr) {
            HHOOK nh = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, g_inst, 0);
            if (nh) { if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nh; }
        }
    }
    KillTimer(nullptr, t);
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    g_mouseHook = nullptr;
    return 0;
}

// Guard: a second copy started with "--guard <pid>". If the main copy is killed
// (e.g. from Task Manager), it brings every hidden window back.
static bool RunGuardIfAsked() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    bool guard = false; DWORD pid = 0;
    for (int i = 1; i + 1 < argc; i++)
        if (wcscmp(argv[i], L"--guard") == 0) { guard = true; pid = (DWORD)wcstoul(argv[i + 1], nullptr, 10); }
    LocalFree(argv);
    if (!guard) return false;
    HANDLE ph = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
    if (ph) {
        WaitForSingleObject(ph, INFINITE);
        CloseHandle(ph);
        Sleep(300);
        InitDataDir();
        RecoverFromPreviousRun();   // after a normal exit the record is already gone, so this does nothing
    }
    return true;
}

static void StartGuard() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t cmd[MAX_PATH + 64];
    swprintf(cmd, MAX_PATH + 64, L"\"%ls\" --guard %lu", exe, (unsigned long)GetCurrentProcessId());
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                       nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    g_inst = hInst;

    // [DLL safety] Load DLLs only from System32 (never from the exe's folder or the current folder).
    // dwmapi / gdiplus / version / shcore are delay-loaded, so they are first loaded after this line.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetDllDirectoryW(L"");

    if (RunGuardIfAsked()) return 0;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(MAIN_CLASS, nullptr);
        if (other) {
            DWORD pid = 0; GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, WM_APP_REACTIVATE, 0, 0);
        }
        return 0;
    }

    SetUnhandledExceptionFilter(CrashHandler);
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&g_gdipToken, &gsi, nullptr);

    g_physToLog = (PhysToLogFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "PhysicalToLogicalPointForPerMonitorDPI");

    InitDataDir();
    LoadSettings();
    RecoverFromPreviousRun();
    StartGuard();
    if (g_set.autoStart) ApplyAutoStart();  // keep the path current if the exe was moved

    g_appIconSmall = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    g_appIconBig   = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.hInstance = hInst; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpfnWndProc = MainProc; wc.lpszClassName = MAIN_CLASS; wc.hIcon = g_appIconBig; wc.hIconSm = g_appIconSmall;
    RegisterClassExW(&wc);

    WNDCLASSEXW bc = { sizeof(bc) };
    bc.hInstance = hInst; bc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    bc.lpfnWndProc = BarProc; bc.lpszClassName = BAR_CLASS; bc.style = CS_DROPSHADOW;
    RegisterClassExW(&bc);

    WNDCLASSEXW pc = { sizeof(pc) };
    pc.hInstance = hInst; pc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pc.lpfnWndProc = PanelProc; pc.lpszClassName = PANEL_CLASS; pc.hIcon = g_appIconBig; pc.hIconSm = g_appIconSmall;
    RegisterClassExW(&pc);

    WNDCLASSEXW sc = { sizeof(sc) };
    sc.hInstance = hInst; sc.hCursor = LoadCursor(nullptr, IDC_HAND);
    sc.lpfnWndProc = SplashProc; sc.lpszClassName = SPLASH_CLASS; sc.style = CS_DROPSHADOW;
    RegisterClassExW(&sc);

    WNDCLASSEXW nc = { sizeof(nc) };
    nc.hInstance = hInst; nc.hCursor = LoadCursor(nullptr, IDC_HAND);
    nc.lpfnWndProc = NoticeProc; nc.lpszClassName = NOTICE_CLASS; nc.style = CS_DROPSHADOW;
    RegisterClassExW(&nc);

    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, MAIN_CLASS, L"Simple WinRoll", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_main, g_msgTaskbarCreated, MSGFLT_ALLOW, nullptr);

    UpdateTrayIcon();
    SetTimer(g_main, MAIN_TIMER, 1000, nullptr);

    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_hookThread = CreateThread(nullptr, 0, HookThreadProc, ready, 0, &g_hookThreadId);
    if (g_hookThread) WaitForSingleObject(ready, 5000);
    CloseHandle(ready);
    if (!g_mouseHook) {
        MessageBoxW(nullptr, L"マウスの監視を開始できませんでした。", APP_NAME, MB_ICONERROR);
    }
    if (g_set.showSplash) ShowSplash();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (g_hookThread) {
        PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_hookThread, 2000);
        CloseHandle(g_hookThread);
    }
    RestoreAllHidden();
    for (auto* r : std::vector<Rolled*>(g_rolled)) RemoveRolled(r);
    Gdiplus::GdiplusShutdown(g_gdipToken);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
