// Deadlock Outlines: small Win32 tool that edits Deadlock's outline, glow and enemy color settings.
// Writes the ConVars block in gameinfo.gi and the enemy color in every user_convars*.vcfg.
#ifndef UNICODE
#define UNICODE
#endif
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace Gdiplus;
using std::string;
using std::wstring;
using std::vector;

// ---------------------------------------------------------------- files

static bool Exists(const wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static bool ReadAll(const wstring& p, string& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    BOOL ok = out.empty() || ReadFile(h, &out[0], (DWORD)out.size(), &rd, nullptr);
    CloseHandle(h);
    return ok && rd == out.size();
}

// Write to a temp file first, then swap it in, so a failed write never leaves a half file behind.
static bool WriteAll(const wstring& p, const string& data) {
    wstring tmp = p + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = data.empty() || WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || wr != data.size()) { DeleteFileW(tmp.c_str()); return false; }
    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// The first backup is the original file. It is never overwritten, so "Restore originals" always goes back to it.
static void BackupOnce(const wstring& p) {
    wstring bak = p + L".bak";
    if (!Exists(bak)) CopyFileW(p.c_str(), bak.c_str(), TRUE);
}

static wstring Widen(const string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// ---------------------------------------------------------------- KeyValues editing
// Valve KeyValues text: "key" "value" pairs inside named { } blocks. Edits keep everything else byte for byte.

struct Block { size_t open = 0, close = 0; };  // open: just after '{', close: index of matching '}'
struct Entry { bool found = false; size_t lineStart = 0, lineEnd = 0, valStart = 0, valEnd = 0; };

static bool IsWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static size_t FindNoCase(const string& t, const char* needle, size_t from) {
    size_t n = strlen(needle);
    for (size_t i = from; i + n <= t.size(); ++i)
        if (_strnicmp(t.c_str() + i, needle, n) == 0) return i;
    return string::npos;
}

static bool FindBlock(const string& t, const char* name, Block& b) {
    size_t n = strlen(name);
    for (size_t pos = 0; (pos = FindNoCase(t, name, pos)) != string::npos; pos += n) {
        char before = pos ? t[pos - 1] : '\n';
        char after = pos + n < t.size() ? t[pos + n] : '\0';
        if (!(IsWs(before) || before == '"') || !(IsWs(after) || after == '"' || after == '{')) continue;
        size_t i = pos + n;
        if (i < t.size() && t[i] == '"') ++i;
        while (i < t.size() && IsWs(t[i])) ++i;
        if (i >= t.size() || t[i] != '{') continue;
        int depth = 0;
        for (size_t j = i; j < t.size(); ++j) {
            char c = t[j];
            if (c == '"') { ++j; while (j < t.size() && t[j] != '"' && t[j] != '\n') ++j; continue; }
            if (c == '/' && j + 1 < t.size() && t[j + 1] == '/') { while (j < t.size() && t[j] != '\n') ++j; continue; }
            if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) { b.open = i + 1; b.close = j; return true; }
        }
        return false;
    }
    return false;
}

// Finds a key that sits directly in the block (not inside a nested block).
static Entry FindEntry(const string& t, const Block& b, const char* key) {
    size_t klen = strlen(key);
    int depth = 0;
    size_t i = b.open;
    while (i < b.close) {
        size_t ls = i, le = t.find('\n', i);
        le = (le == string::npos || le >= b.close) ? b.close : le + 1;
        size_t p = ls;
        while (p < le && (t[p] == ' ' || t[p] == '\t')) ++p;
        if (depth == 0 && p < le && t[p] == '"') {
            size_t ke = t.find('"', p + 1);
            if (ke != string::npos && ke < le && ke - p - 1 == klen && t.compare(p + 1, klen, key) == 0) {
                size_t v = ke + 1;
                while (v < le && (t[v] == ' ' || t[v] == '\t')) ++v;
                if (v < le && t[v] == '"') {
                    size_t ve = t.find('"', v + 1);
                    if (ve != string::npos && ve < le) return {true, ls, le, v, ve + 1};
                } else {
                    size_t ve = v;
                    while (ve < le && !IsWs(t[ve])) ++ve;
                    if (ve > v) return {true, ls, le, v, ve};
                }
            }
        }
        for (size_t q = ls; q < le; ++q) {
            char c = t[q];
            if (c == '"') { ++q; while (q < le && t[q] != '"') ++q; continue; }
            if (c == '/' && q + 1 < le && t[q + 1] == '/') break;
            if (c == '{') ++depth; else if (c == '}') --depth;
        }
        i = le;
    }
    return {};
}

static string EntryValue(const string& t, const Entry& e) {
    string v = t.substr(e.valStart, e.valEnd - e.valStart);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    return v;
}

static bool SetEntry(string& t, const char* blockName, const char* key, const string& val) {
    Block b;
    if (!FindBlock(t, blockName, b)) return false;
    Entry e = FindEntry(t, b, key);
    string quoted = "\"" + val + "\"";
    if (e.found) { t.replace(e.valStart, e.valEnd - e.valStart, quoted); return true; }
    const char* eol = t.find("\r\n") != string::npos ? "\r\n" : "\n";
    size_t nl = t.find('\n', b.open);
    if (nl == string::npos || nl > b.close) return false;
    size_t at = nl + 1;
    string indent;
    for (size_t p = at; p < t.size() && (t[p] == ' ' || t[p] == '\t'); ++p) indent += t[p];
    if (indent.empty()) indent = "\t\t";
    t.insert(at, indent + "\"" + key + "\"\t\t" + quoted + eol);
    return true;
}

static void RemoveEntry(string& t, const char* blockName, const char* key) {
    Block b;
    if (!FindBlock(t, blockName, b)) return;
    Entry e = FindEntry(t, b, key);
    if (e.found) t.erase(e.lineStart, e.lineEnd - e.lineStart);
}

// ---------------------------------------------------------------- settings model

enum Kind { kToggle, kSlider };

struct Setting {
    Kind kind;
    const wchar_t* label;
    const wchar_t* hint;
    const char* key;
    bool invert;      // toggle: the convar is a "disabled" flag, so ON writes false
    double lo, hi, step, def;
    int decimals;
    double val = 0;   // toggle: 0/1
    bool set = false; // slider: false means "game default" (key removed)
    bool inFile = false;
    bool touched = false;
};

static Setting Toggle(const wchar_t* l, const wchar_t* h, const char* k, bool inv, bool def) {
    return {kToggle, l, h, k, inv, 0, 1, 1, def ? 1.0 : 0.0, 0};
}
static Setting Slider(const wchar_t* l, const wchar_t* h, const char* k, double lo, double hi, double st, double def, int dec) {
    return {kSlider, l, h, k, false, lo, hi, st, def, dec};
}

static vector<Setting> g_settings = {
    Toggle(L"Enemy hero outlines", L"Outline enemy heroes", "citadel_player_outline_enemies", false, true),
    Toggle(L"Ally hero outlines", L"Outline your teammates too", "citadel_player_outline_allies", false, false),
    Toggle(L"Outline renderer", L"Master switch for all outlines", "r_citadel_npr_outlines", false, true),
    Toggle(L"Trooper outlines", L"Outline lane troopers", "citadel_trooper_outline_enabled", false, true),
    Toggle(L"Hero glow", L"Glow around heroes", "citadel_player_glow_disabled", true, true),
    Toggle(L"Enemy trooper glow", L"Glow around enemy troopers", "citadel_trooper_glow_disabled", true, true),
    Toggle(L"Friendly trooper glow", L"Glow around your troopers", "citadel_trooper_friendly_glow_disabled", true, true),
    Toggle(L"Boss glow", L"Guardians, walkers and patron", "citadel_boss_glow_disabled", true, true),
    Slider(L"Outline distance", L"How far away outlines still draw", "r_citadel_npr_outlines_max_dist", 250, 5000, 50, 1000, 0),
    Slider(L"Glow brightness", L"Brightness of hero halos", "cl_glow_brightness", 0.5, 5, 0.1, 1, 1),
    Slider(L"Outline width", L"Thickness of the outline", "r_citadel_selection_outline2_width", 0.5, 8, 0.5, 1, 1),
    Slider(L"Outline opacity", L"0 is invisible, 255 is solid", "r_citadel_selection_outline2_alpha", 0, 255, 5, 255, 0),
};
static const int kOutlineFirst = 0, kGlowFirst = 4, kSliderFirst = 8, kRowsPerCard = 4;

static const COLORREF kPresets[] = {
    RGB(255, 0, 255), RGB(255, 40, 40), RGB(255, 140, 0), RGB(255, 234, 0),
    RGB(57, 255, 20), RGB(0, 240, 255), RGB(255, 255, 255),
};
static const int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

struct AppState {
    wstring gameinfo;
    wstring autoexec;  // runs on every launch, so it re-applies everything even if Steam Cloud or a mod tool resets the rest
    vector<wstring> vcfgs;
    COLORREF color = RGB(255, 0, 255);
    bool colorTouched = false;
    bool dirty = false;
    bool running = false;
    bool testMode = false;
    wstring status;
    Color statusColor{255, 150, 156, 172};
    float scale = 1.0f;
    int hover = -1, pressed = -1;
    bool tracking = false;
} A;

static double Snap(const Setting& s, double v) {
    v = s.lo + std::round((v - s.lo) / s.step) * s.step;
    return v < s.lo ? s.lo : v > s.hi ? s.hi : v;
}

static string FormatValue(const Setting& s) {
    char buf[32];
    snprintf(buf, sizeof buf, s.decimals ? "%.1f" : "%.0f", s.val);
    return buf;
}

static bool ParseBool(const string& v) { return v == "true" || v == "1" || v == "True" || v == "TRUE"; }

// ---------------------------------------------------------------- discovery

static void Discover() {
    wchar_t buf[MAX_PATH];
    DWORD sz = sizeof buf;
    wstring steam = L"C:\\Program Files (x86)\\Steam";
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS) {
        steam = buf;
        for (auto& c : steam) if (c == L'/') c = L'\\';
    }

    vector<wstring> libs{steam};
    string vdf;
    if (ReadAll(steam + L"\\steamapps\\libraryfolders.vdf", vdf)) {
        for (size_t p = 0; (p = vdf.find("\"path\"", p)) != string::npos; p += 6) {
            size_t a = vdf.find('"', p + 6);
            if (a == string::npos) break;
            string path;
            size_t i = a + 1;
            for (; i < vdf.size() && vdf[i] != '"'; ++i) {
                if (vdf[i] == '\\' && i + 1 < vdf.size()) ++i;
                path += vdf[i];
            }
            libs.push_back(Widen(path));
        }
    }
    for (auto& lib : libs) {
        wstring p = lib + L"\\steamapps\\common\\Deadlock\\game\\citadel\\gameinfo.gi";
        if (Exists(p)) {
            A.gameinfo = p;
            A.autoexec = lib + L"\\steamapps\\common\\Deadlock\\game\\citadel\\cfg\\autoexec.cfg";
            break;
        }
    }

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((steam + L"\\userdata\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    vector<wstring> remotes;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        wstring base = steam + L"\\userdata\\" + fd.cFileName + L"\\1422450";
        wstring cfg = base + L"\\local\\cfg\\";
        WIN32_FIND_DATAW fv;
        HANDLE hv = FindFirstFileW((cfg + L"user_convars*.vcfg").c_str(), &fv);
        if (hv != INVALID_HANDLE_VALUE) {
            do {
                wstring n = fv.cFileName;
                if (n.size() > 5 && _wcsicmp(n.c_str() + n.size() - 5, L".vcfg") == 0) A.vcfgs.push_back(cfg + n);
            } while (FindNextFileW(hv, &fv));
            FindClose(hv);
        }
        wstring remote = base + L"\\remote\\user_convars.vcfg";
        if (Exists(remote)) remotes.push_back(remote);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    A.vcfgs.insert(A.vcfgs.end(), remotes.begin(), remotes.end());
}

// Test hook: --root <folder> uses <folder>\gameinfo.gi and every .vcfg in that folder instead of the real game.
static void UseRoot(const wstring& root) {
    A.gameinfo = root + L"\\gameinfo.gi";
    A.autoexec = root + L"\\autoexec.cfg";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*.vcfg").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wstring n = fd.cFileName;
        if (n.size() > 5 && _wcsicmp(n.c_str() + n.size() - 5, L".vcfg") == 0) A.vcfgs.push_back(root + L"\\" + n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static bool GameRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof pe};
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (!_wcsicmp(pe.szExeFile, L"project8.exe") || !_wcsicmp(pe.szExeFile, L"deadlock.exe")) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// ---------------------------------------------------------------- load / apply

static void LoadState() {
    for (auto& s : g_settings) {
        s.val = s.def;
        s.set = s.inFile = s.touched = false;
    }
    string t;
    Block b;
    if (!A.gameinfo.empty() && ReadAll(A.gameinfo, t) && FindBlock(t, "ConVars", b)) {
        for (auto& s : g_settings) {
            Entry e = FindEntry(t, b, s.key);
            if (!e.found) continue;
            string v = EntryValue(t, e);
            s.inFile = true;
            if (s.kind == kToggle) {
                bool on = ParseBool(v);
                s.val = (s.invert ? !on : on) ? 1 : 0;
            } else {
                s.set = true;
                s.val = Snap(s, atof(v.c_str()));
            }
        }
    }
    for (auto& path : A.vcfgs) {
        if (!ReadAll(path, t) || !FindBlock(t, "convars", b)) continue;
        Entry r = FindEntry(t, b, "citadel_enemy_ui_color_r");
        Entry g = FindEntry(t, b, "citadel_enemy_ui_color_g");
        Entry bl = FindEntry(t, b, "citadel_enemy_ui_color_b");
        if (r.found && g.found && bl.found) {
            A.color = RGB(atoi(EntryValue(t, r).c_str()), atoi(EntryValue(t, g).c_str()), atoi(EntryValue(t, bl).c_str()));
            break;
        }
    }
    A.colorTouched = false;
    A.dirty = false;
}

static void SetStatus(const wstring& s, Color c) { A.status = s; A.statusColor = c; }
static const Color kOk(255, 96, 214, 140), kWarn(255, 255, 190, 70), kErr(255, 255, 99, 99), kDim(255, 150, 156, 172);

static const char* kBlockStart = "// deadlock-outlines start";
static const char* kBlockEnd = "// deadlock-outlines end";

// Replaces this tool's block in autoexec.cfg, leaving any lines the user wrote themselves alone.
static bool WriteAutoexec() {
    string t;
    if (Exists(A.autoexec)) {
        if (!ReadAll(A.autoexec, t)) return false;
        BackupOnce(A.autoexec);
    }
    size_t s = t.find(kBlockStart), e = t.find(kBlockEnd);
    if (s != string::npos && e != string::npos && e > s) {
        e += strlen(kBlockEnd);
        while (e < t.size() && (t[e] == '\r' || t[e] == '\n')) ++e;
        t.erase(s, e - s);
    }
    const char* eol = "\r\n";
    if (!t.empty() && t.back() != '\n') t += eol;
    string block = string(kBlockStart) + eol;
    for (auto& st : g_settings) {
        if (st.kind == kToggle) {
            if (!st.inFile && !st.touched) continue;
            bool on = st.val > 0.5;
            block += string(st.key) + " " + ((st.invert ? !on : on) ? "true" : "false") + eol;
        } else if (st.set) {
            block += string(st.key) + " " + FormatValue(st) + eol;
        }
    }
    char rgb[160];
    snprintf(rgb, sizeof rgb, "citadel_custom_ui_colors true%scitadel_enemy_ui_color_r %d%scitadel_enemy_ui_color_g %d%scitadel_enemy_ui_color_b %d%s",
             eol, GetRValue(A.color), eol, GetGValue(A.color), eol, GetBValue(A.color), eol);
    block += rgb;
    block += string(kBlockEnd) + eol;
    return WriteAll(A.autoexec, t + block);
}

static void Apply(HWND hwnd) {
    if (A.gameinfo.empty() && A.vcfgs.empty()) { SetStatus(L"Deadlock was not found, nothing to save.", kErr); return; }
    if (!A.testMode && GameRunning()) {
        int r = MessageBoxW(hwnd,
            L"Deadlock is running. It saves its own settings when it closes and can undo these.\n\nSave anyway?",
            L"Deadlock Outlines", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (r != IDYES) return;
    }
    int saved = 0, failed = 0;
    string t;
    if (!A.gameinfo.empty()) {
        bool ok = ReadAll(A.gameinfo, t);
        if (ok) {
            BackupOnce(A.gameinfo);
            for (auto& s : g_settings) {
                if (s.kind == kToggle) {
                    if (!s.inFile && !s.touched) continue;  // leave unknown game defaults alone
                    bool on = s.val > 0.5;
                    ok = ok && SetEntry(t, "ConVars", s.key, (s.invert ? !on : on) ? "true" : "false");
                } else if (s.set) {
                    ok = ok && SetEntry(t, "ConVars", s.key, FormatValue(s));
                } else {
                    RemoveEntry(t, "ConVars", s.key);
                }
            }
            ok = ok && WriteAll(A.gameinfo, t);
        }
        ok ? ++saved : ++failed;
    }
    if (!A.autoexec.empty()) WriteAutoexec() ? ++saved : ++failed;
    for (auto& path : A.vcfgs) {
        bool ok = ReadAll(path, t);
        if (ok) {
            BackupOnce(path);
            char r[8], g[8], b[8];
            snprintf(r, sizeof r, "%d", GetRValue(A.color));
            snprintf(g, sizeof g, "%d", GetGValue(A.color));
            snprintf(b, sizeof b, "%d", GetBValue(A.color));
            ok = SetEntry(t, "convars", "citadel_custom_ui_colors", "true") &&
                 SetEntry(t, "convars", "citadel_enemy_ui_color_r", r) &&
                 SetEntry(t, "convars", "citadel_enemy_ui_color_g", g) &&
                 SetEntry(t, "convars", "citadel_enemy_ui_color_b", b) &&
                 WriteAll(path, t);
        }
        ok ? ++saved : ++failed;
    }
    if (failed) {
        SetStatus(L"Could not save " + std::to_wstring(failed) + L" file(s). Try running as administrator.", kErr);
        return;
    }
    LoadState();
    SetStatus(L"Saved " + std::to_wstring(saved) + L" files. Start Deadlock to see it.", kOk);
}

static void RestoreOriginals(HWND hwnd) {
    vector<wstring> files = A.vcfgs;
    if (!A.gameinfo.empty()) files.push_back(A.gameinfo);
    if (!A.autoexec.empty()) files.push_back(A.autoexec);
    int have = 0;
    for (auto& f : files) if (Exists(f + L".bak")) ++have;
    if (!have) { SetStatus(L"No backups yet. They are made the first time you save.", kDim); return; }
    if (MessageBoxW(hwnd, L"Put back the original files from before this tool first saved?",
                    L"Deadlock Outlines", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    int restored = 0;
    for (auto& f : files) {
        wstring bak = f + L".bak";
        if (Exists(bak) && CopyFileW(bak.c_str(), f.c_str(), FALSE)) ++restored;
    }
    LoadState();
    SetStatus(L"Restored " + std::to_wstring(restored) + L" original files.", kOk);
}

// ---------------------------------------------------------------- layout

enum HitType { hSwatch, hPreset, hToggle, hSlider, hReset, hApply, hClose, hRestore };
struct Hit { RectF r; HitType type; int idx; };

static const float kW = 860, kM = 24, kGap = 16, kTop = 84;
static const float kRowT = 46, kRowS = 66, kCardHead = 44;

struct Layout {
    float colW, x1, x2, H;
    RectF colorCard, strengthCard, outlineCard, glowCard;
    RectF swatch, preset[kPresetCount];
    RectF row[12], control[12], reset[12];
    RectF apply, close, restore, status;
    vector<Hit> hits;
} L;

static void BuildLayout() {
    L.hits.clear();
    L.colW = (kW - 2 * kM - kGap) / 2;
    L.x1 = kM;
    L.x2 = kM + L.colW + kGap;
    float cw = L.colW;
    float toggleCardH = kCardHead + kRowsPerCard * kRowT + 8;

    L.colorCard = {L.x1, kTop, cw, 2 * toggleCardH + kGap - (kCardHead + kRowsPerCard * kRowS + 8) - kGap};
    L.swatch = {L.x1 + 18, kTop + 46, 88, 88};
    L.hits.push_back({L.swatch, hSwatch, 0});
    for (int i = 0; i < kPresetCount; ++i) {
        L.preset[i] = {L.x1 + 126 + i * 36.0f, kTop + 108, 26, 26};
        L.hits.push_back({L.preset[i], hPreset, i});
    }

    float sy = kTop + L.colorCard.Height + kGap;
    L.strengthCard = {L.x1, sy, cw, kCardHead + kRowsPerCard * kRowS + 8};
    L.outlineCard = {L.x2, kTop, cw, toggleCardH};
    L.glowCard = {L.x2, kTop + toggleCardH + kGap, cw, toggleCardH};

    for (int i = 0; i < (int)g_settings.size(); ++i) {
        if (g_settings[i].kind == kToggle) {
            const RectF& card = i < kGlowFirst ? L.outlineCard : L.glowCard;
            int k = i - (i < kGlowFirst ? kOutlineFirst : kGlowFirst);
            L.row[i] = {card.X, card.Y + kCardHead + k * kRowT, cw, kRowT};
            L.control[i] = {card.X + cw - 18 - 48, L.row[i].Y + 10, 48, 26};
            L.hits.push_back({RectF(card.X + 6, L.row[i].Y + 2, cw - 12, kRowT - 4), hToggle, i});
        } else {
            int k = i - kSliderFirst;
            L.row[i] = {L.x1, sy + kCardHead + k * kRowS, cw, kRowS};
            L.control[i] = {L.x1 + 18, L.row[i].Y + 49, cw - 36, 6};
            L.reset[i] = {L.x1 + cw - 18 - 72 - 52, L.row[i].Y + 6, 48, 20};
            L.hits.push_back({L.reset[i], hReset, i});
            L.hits.push_back({RectF(L.x1 + 8, L.row[i].Y + 36, cw - 16, 30), hSlider, i});
        }
    }

    float fy = kTop + 2 * toggleCardH + kGap + 20;
    L.apply = {kW - kM - 132, fy, 132, 42};
    L.close = {L.apply.X - 12 - 104, fy, 104, 42};
    L.restore = {kM, fy, 150, 42};
    L.status = {kM + 160, fy, L.close.X - kM - 172, 42};
    L.hits.push_back({L.apply, hApply, 0});
    L.hits.push_back({L.close, hClose, 0});
    L.hits.push_back({L.restore, hRestore, 0});
    L.H = fy + 42 + 22;
}

static int HitTest(float x, float y) {
    for (int i = (int)L.hits.size() - 1; i >= 0; --i) {
        const Hit& h = L.hits[i];
        if (h.type == hReset && !g_settings[h.idx].set) continue;
        if (h.r.Contains(x, y)) return i;
    }
    return -1;
}

// ---------------------------------------------------------------- drawing

static Font *fTitle, *fBody, *fBodyBold, *fSmall, *fCaps, *fHex;

static Font* MakeFont(const wchar_t* family, float px, int style) {
    Font* f = new Font(family, px, style, UnitPixel);
    if (f->GetLastStatus() != Ok) { delete f; f = new Font(L"Segoe UI", px, style ? FontStyleBold : FontStyleRegular, UnitPixel); }
    return f;
}

static void CreateFonts() {
    fTitle = MakeFont(L"Segoe UI Semibold", 25, FontStyleRegular);
    fBody = MakeFont(L"Segoe UI", 15, FontStyleRegular);
    fBodyBold = MakeFont(L"Segoe UI Semibold", 15, FontStyleRegular);
    fSmall = MakeFont(L"Segoe UI", 12.5f, FontStyleRegular);
    fCaps = MakeFont(L"Segoe UI", 11.5f, FontStyleBold);
    fHex = MakeFont(L"Segoe UI Semibold", 22, FontStyleRegular);
}

static const Color kBg(255, 21, 22, 27), kCard(255, 31, 33, 41), kBorder(255, 45, 48, 59),
    kText(255, 242, 243, 247), kSub(255, 150, 156, 172), kTrack(255, 60, 64, 78), kHover(255, 39, 41, 51);

static float Luma(COLORREF c) { return (0.2126f * GetRValue(c) + 0.7152f * GetGValue(c) + 0.0722f * GetBValue(c)) / 255.f; }
static Color ToColor(COLORREF c, BYTE a = 255) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

// The chosen color doubles as the UI accent; very dark picks get lifted so switches stay visible.
static Color Accent(BYTE a = 255) {
    COLORREF c = A.color;
    if (Luma(c) < 0.22f) {
        auto lift = [](int v) { return v + (255 - v) * 45 / 100; };
        c = RGB(lift(GetRValue(c)), lift(GetGValue(c)), lift(GetBValue(c)));
    }
    return ToColor(c, a);
}
static Color OnAccent() { return Luma(A.color) > 0.6f ? Color(255, 18, 18, 22) : Color(255, 255, 255, 255); }

static void RoundPath(GraphicsPath& p, RectF r, float rad) {
    float d = rad * 2;
    if (d > r.Height) d = r.Height;
    if (d > r.Width) d = r.Width;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

static void FillRound(Graphics& g, RectF r, float rad, Color c) {
    GraphicsPath p;
    RoundPath(p, r, rad);
    SolidBrush b(c);
    g.FillPath(&b, &p);
}

static void StrokeRound(Graphics& g, RectF r, float rad, Color c, float w) {
    GraphicsPath p;
    RoundPath(p, r, rad);
    Pen pen(c, w);
    g.DrawPath(&pen, &p);
}

static void Text(Graphics& g, const wstring& s, Font* f, Color c, RectF r,
                 StringAlignment h = StringAlignmentNear, StringAlignment v = StringAlignmentNear) {
    StringFormat sf;
    sf.SetAlignment(h);
    sf.SetLineAlignment(v);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush b(c);
    g.DrawString(s.c_str(), -1, f, r, &sf, &b);
}

static bool IsHover(HitType t, int idx) {
    return A.hover >= 0 && L.hits[A.hover].type == t && L.hits[A.hover].idx == idx;
}

static void DrawCard(Graphics& g, RectF r, const wchar_t* title) {
    FillRound(g, r, 14, kCard);
    StrokeRound(g, r, 14, kBorder, 1);
    Text(g, title, fCaps, kSub, RectF(r.X + 18, r.Y + 16, r.Width - 36, 20));
}

// Small hero silhouette in the header, outlined in the chosen color as a live preview.
static void DrawPreview(Graphics& g, float cx, float top) {
    GraphicsPath body;
    body.AddEllipse(cx - 11, top, 22.f, 22.f);
    RoundPath(body, RectF(cx - 19, top + 26, 38, 34), 12);
    for (int i = 3; i >= 1; --i) {
        Pen glow(Accent((BYTE)(28 * i)), 3.0f + i * 4.0f);
        glow.SetLineJoin(LineJoinRound);
        g.DrawPath(&glow, &body);
    }
    SolidBrush fill(Color(255, 48, 51, 63));
    g.FillPath(&fill, &body);
    Pen edge(Accent(), 3);
    edge.SetLineJoin(LineJoinRound);
    g.DrawPath(&edge, &body);
}

static void Draw(Graphics& g) {
    SolidBrush bg(kBg);
    g.FillRectangle(&bg, RectF(-1, -1, kW + 2, L.H + 2));

    Text(g, L"Deadlock Outlines", fTitle, kText, RectF(kM, 18, 500, 36));
    wstring sub;
    Color subColor = kSub;
    if (A.testMode) sub = L"Test mode: editing files in the --root folder";
    else if (A.gameinfo.empty()) { sub = L"Deadlock install not found"; subColor = kErr; }
    else sub = L"Edits gameinfo.gi, autoexec.cfg and " + std::to_wstring(A.vcfgs.size()) + L" Steam config files. Backups are kept.";
    if (A.running) { sub = L"Deadlock is running. Close it before you save."; subColor = kWarn; }
    Text(g, sub, fSmall, subColor, RectF(kM, 52, 640, 20));
    DrawPreview(g, kW - kM - 26, 14);

    // enemy color
    DrawCard(g, L.colorCard, L"ENEMY COLOR");
    if (IsHover(hSwatch, 0)) FillRound(g, RectF(L.swatch.X - 5, L.swatch.Y - 5, L.swatch.Width + 10, L.swatch.Height + 10), 17, Accent(60));
    FillRound(g, L.swatch, 13, ToColor(A.color));
    StrokeRound(g, L.swatch, 13, Color(70, 255, 255, 255), 1.5f);
    wchar_t hex[16];
    swprintf(hex, 16, L"#%02X%02X%02X", GetRValue(A.color), GetGValue(A.color), GetBValue(A.color));
    Text(g, hex, fHex, kText, RectF(L.x1 + 124, kTop + 42, 200, 30));
    Text(g, L"Click the swatch to pick any color", fSmall, kSub, RectF(L.x1 + 126, kTop + 75, L.colW - 140, 20));
    for (int i = 0; i < kPresetCount; ++i) {
        RectF r = L.preset[i];
        bool sel = kPresets[i] == A.color;
        if (sel || IsHover(hPreset, i)) {
            Pen ring(sel ? kText : kSub, 2);
            g.DrawEllipse(&ring, r.X - 4, r.Y - 4, r.Width + 8, r.Height + 8);
        }
        SolidBrush b(ToColor(kPresets[i]));
        g.FillEllipse(&b, r);
    }

    // sliders
    DrawCard(g, L.strengthCard, L"STRENGTH");
    for (int i = kSliderFirst; i < (int)g_settings.size(); ++i) {
        const Setting& s = g_settings[i];
        RectF row = L.row[i];
        if (i > kSliderFirst) {
            Pen sep(kBorder, 1);
            g.DrawLine(&sep, row.X + 18, row.Y, row.X + row.Width - 18, row.Y);
        }
        Text(g, s.label, fBody, kText, RectF(row.X + 18, row.Y + 5, 220, 22));
        Text(g, s.hint, fSmall, kSub, RectF(row.X + 18, row.Y + 25, 240, 18));
        wstring val = s.set ? Widen(FormatValue(s)) : L"Default";
        Text(g, val, s.set ? fBodyBold : fBody, s.set ? kText : kSub,
             RectF(row.X + row.Width - 18 - 72, row.Y + 5, 72, 22), StringAlignmentFar);
        if (s.set) {
            Color rc = IsHover(hReset, i) ? kText : kSub;
            Text(g, L"Reset", fSmall, rc, L.reset[i], StringAlignmentFar, StringAlignmentCenter);
        }
        RectF tr = L.control[i];
        float t = (float)((s.val - s.lo) / (s.hi - s.lo));
        float tx = tr.X + t * tr.Width;
        FillRound(g, tr, 3, kTrack);
        Color fillC = s.set ? Accent() : Color(255, 92, 97, 112);
        FillRound(g, RectF(tr.X, tr.Y, tx - tr.X + 3, tr.Height), 3, fillC);
        bool hot = IsHover(hSlider, i) || (A.pressed >= 0 && L.hits[A.pressed].type == hSlider && L.hits[A.pressed].idx == i);
        if (hot) { SolidBrush halo(s.set ? Accent(55) : Color(55, 200, 200, 210)); g.FillEllipse(&halo, tx - 15, tr.Y + 3 - 15, 30.f, 30.f); }
        SolidBrush knob(s.set ? Color(255, 255, 255, 255) : Color(255, 165, 170, 184));
        g.FillEllipse(&knob, tx - 9, tr.Y + 3 - 9, 18.f, 18.f);
    }

    // toggles
    DrawCard(g, L.outlineCard, L"OUTLINES");
    DrawCard(g, L.glowCard, L"GLOW");
    for (int i = 0; i < kSliderFirst; ++i) {
        const Setting& s = g_settings[i];
        RectF row = L.row[i];
        if (IsHover(hToggle, i)) FillRound(g, RectF(row.X + 6, row.Y + 2, row.Width - 12, row.Height - 4), 9, kHover);
        else if (i != kOutlineFirst && i != kGlowFirst) {
            Pen sep(kBorder, 1);
            g.DrawLine(&sep, row.X + 18, row.Y, row.X + row.Width - 18, row.Y);
        }
        Text(g, s.label, fBody, kText, RectF(row.X + 18, row.Y + 5, 260, 22));
        Text(g, s.hint, fSmall, kSub, RectF(row.X + 18, row.Y + 24, 260, 18));
        RectF sw = L.control[i];
        bool on = s.val > 0.5;
        FillRound(g, sw, 13, on ? Accent() : kTrack);
        float kx = on ? sw.X + sw.Width - 23 : sw.X + 3;
        SolidBrush knob(on && Luma(A.color) > 0.6f ? Color(255, 22, 22, 28) : Color(255, 255, 255, 255));
        g.FillEllipse(&knob, kx, sw.Y + 3, 20.f, 20.f);
    }

    // footer
    Color rc = IsHover(hRestore, 0) ? kText : kSub;
    Text(g, L"Restore originals", fBody, rc, L.restore, StringAlignmentNear, StringAlignmentCenter);
    wstring st = A.status;
    Color stc = A.statusColor;
    if (A.dirty) { st = L"Unsaved changes"; stc = kWarn; }
    Text(g, st, fSmall, stc, L.status, StringAlignmentFar, StringAlignmentCenter);

    FillRound(g, L.close, 10, IsHover(hClose, 0) ? Color(255, 56, 60, 73) : Color(255, 44, 47, 58));
    Text(g, L"Close", fBodyBold, kText, L.close, StringAlignmentCenter, StringAlignmentCenter);
    FillRound(g, L.apply, 10, IsHover(hApply, 0) ? Accent(225) : Accent());
    Text(g, L"Save", fBodyBold, OnAccent(), L.apply, StringAlignmentCenter, StringAlignmentCenter);
}

// ---------------------------------------------------------------- window

static void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.ScaleTransform(A.scale, A.scale);
        Draw(g);
    }
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static void MarkDirty(HWND hwnd) {
    A.dirty = true;
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void DragSlider(HWND hwnd, int idx, float x) {
    Setting& s = g_settings[idx];
    RectF tr = L.control[idx];
    float t = (x - tr.X) / tr.Width;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    double v = Snap(s, s.lo + t * (s.hi - s.lo));
    if (!s.set || v != s.val) { s.val = v; s.set = true; MarkDirty(hwnd); }
}

static void PickColor(HWND hwnd) {
    static COLORREF custom[16];
    static bool seeded = false;
    if (!seeded) { for (int i = 0; i < kPresetCount; ++i) custom[i] = kPresets[i]; seeded = true; }
    CHOOSECOLORW cc{sizeof cc};
    cc.hwndOwner = hwnd;
    cc.rgbResult = A.color;
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (ChooseColorW(&cc)) { A.color = cc.rgbResult; A.colorTouched = true; MarkDirty(hwnd); }
}

static void Click(HWND hwnd, const Hit& h) {
    switch (h.type) {
    case hSwatch: PickColor(hwnd); break;
    case hPreset: A.color = kPresets[h.idx]; A.colorTouched = true; MarkDirty(hwnd); break;
    case hToggle: {
        Setting& s = g_settings[h.idx];
        s.val = s.val > 0.5 ? 0 : 1;
        s.touched = true;
        MarkDirty(hwnd);
        break;
    }
    case hReset: g_settings[h.idx].set = false; g_settings[h.idx].val = g_settings[h.idx].def; MarkDirty(hwnd); break;
    case hApply: Apply(hwnd); InvalidateRect(hwnd, nullptr, FALSE); break;
    case hClose: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
    case hRestore: RestoreOriginals(hwnd); InvalidateRect(hwnd, nullptr, FALSE); break;
    case hSlider: break;
    }
}

static SIZE WindowSizeFor(UINT dpi) {
    RECT r{0, 0, (LONG)std::ceil(kW * dpi / 96.f), (LONG)std::ceil(L.H * dpi / 96.f)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, dpi);
    return {r.right - r.left, r.bottom - r.top};
}

static HICON MakeIcon(int size) {
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.Clear(Color(0, 0, 0, 0));
        float s = size / 32.f;
        SolidBrush bg(Color(255, 31, 33, 41));
        g.FillEllipse(&bg, 1 * s, 1 * s, 30 * s, 30 * s);
        Pen ring(Color(255, 255, 0, 255), 3.2f * s);
        g.DrawEllipse(&ring, 6 * s, 6 * s, 20 * s, 20 * s);
        SolidBrush dot(Color(255, 255, 0, 255));
        g.FillEllipse(&dot, 12.5f * s, 12.5f * s, 7 * s, 7 * s);
    }
    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    return icon;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)MakeIcon(32));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)MakeIcon(16));
        if (!A.testMode) {
            A.running = GameRunning();
            SetTimer(hwnd, 1, 2000, nullptr);
        }
        return 0;
    }
    case WM_TIMER: {
        bool r = GameRunning();
        if (r != A.running) { A.running = r; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }
    case WM_DPICHANGED: {
        A.scale = HIWORD(wp) / 96.f;
        SIZE sz = WindowSizeFor(HIWORD(wp));
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, sz.cx, sz.cy, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: Paint(hwnd); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, A.hover >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        float x = GET_X_LPARAM(lp) / A.scale, y = GET_Y_LPARAM(lp) / A.scale;
        if (!A.tracking) {
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            A.tracking = true;
        }
        if (A.pressed >= 0 && L.hits[A.pressed].type == hSlider) { DragSlider(hwnd, L.hits[A.pressed].idx, x); return 0; }
        int h = HitTest(x, y);
        if (h != A.hover) { A.hover = h; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE:
        A.tracking = false;
        if (A.hover >= 0 && A.pressed < 0) { A.hover = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONDOWN: {
        float x = GET_X_LPARAM(lp) / A.scale, y = GET_Y_LPARAM(lp) / A.scale;
        A.pressed = HitTest(x, y);
        if (A.pressed >= 0) {
            SetCapture(hwnd);
            if (L.hits[A.pressed].type == hSlider) DragSlider(hwnd, L.hits[A.pressed].idx, x);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        float x = GET_X_LPARAM(lp) / A.scale, y = GET_Y_LPARAM(lp) / A.scale;
        int p = A.pressed;
        A.pressed = -1;
        ReleaseCapture();
        if (p >= 0 && L.hits[p].type != hSlider && HitTest(x, y) == p) Click(hwnd, L.hits[p]);
        A.hover = HitTest(x, y);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        int h = HitTest(pt.x / A.scale, pt.y / A.scale);
        if (h >= 0 && L.hits[h].type == hSlider) {
            Setting& s = g_settings[L.hits[h].idx];
            s.val = Snap(s, s.val + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? s.step : -s.step));
            s.set = true;
            MarkDirty(hwnd);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_RETURN) { Apply(hwnd); InvalidateRect(hwnd, nullptr, FALSE); }
        else if (wp == VK_ESCAPE) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_CLOSE:
        if (A.dirty && !A.testMode &&
            MessageBoxW(hwnd, L"You have unsaved changes. Close without saving?", L"Deadlock Outlines",
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
            return 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i + 1 < argc; ++i)
        if (!_wcsicmp(argv[i], L"--root")) { A.testMode = true; UseRoot(argv[i + 1]); }
    LocalFree(argv);
    if (!A.testMode) Discover();
    LoadState();

    ULONG_PTR gdipToken;
    GdiplusStartupInput gsi;
    GdiplusStartup(&gdipToken, &gsi, nullptr);
    CreateFonts();
    BuildLayout();

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DeadlockOutlines";
    RegisterClassExW(&wc);

    POINT origin{0, 0};
    HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    UINT dpiX = 96, dpiY = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
        if (auto fn = (GetDpiForMonitorFn)GetProcAddress(shcore, "GetDpiForMonitor")) fn(mon, 0, &dpiX, &dpiY);
    }
    A.scale = dpiX / 96.f;
    SIZE sz = WindowSizeFor(dpiX);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - sz.cx) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - sz.cy) / 2;
    if (y < mi.rcWork.top) y = mi.rcWork.top;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Deadlock Outlines",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                x, y, sz.cx, sz.cy, nullptr, nullptr, inst, nullptr);
    ShowWindow(hwnd, A.testMode ? SW_SHOWNOACTIVATE : show);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    GdiplusShutdown(gdipToken);
    return 0;
}
