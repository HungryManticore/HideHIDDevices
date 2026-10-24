/*
 * HideHIDDevices.asi
 * -------------------
 * Ultimate ASI Loader plugin that hides selected HID devices from DirectInput,
 * written for Need for Speed: Hot Pursuit 2 (but generic for any DirectInput
 * 3/5/7/8 game).
 *
 * How it works
 *   1. On load it patches the game executable's import table so that calls to
 *      DirectInputCreateA/W/Ex (dinput.dll), DirectInput8Create (dinput8.dll)
 *      and GetProcAddress (for games that load DirectInput dynamically) go
 *      through this plugin first.
 *   2. When the game creates a DirectInput object, the plugin reads
 *      HideHIDDevices.ini (same folder, same name as this .asi) and patches the
 *      object's EnumDevices / CreateDevice / CreateDeviceEx / GetDeviceStatus
 *      methods.
 *   3. Devices whose USB Vendor/Product IDs match an .ini entry are silently
 *      skipped during enumeration, and any attempt to open them fails with
 *      DIERR_DEVICENOTREG, so the game never sees them.
 *
 * A log is written to HideHIDDevices.log next to the .asi on every run.
 *
 * Build (32-bit, the game is 32-bit):
 *   i686-w64-mingw32-gcc -O2 -shared -s -static -o HideHIDDevices.asi \
 *       src/HideHIDDevices.c -lsetupapi
 *   MSVC: cl /O2 /LD HideHIDDevices.c setupapi.lib user32.lib /Fe:HideHIDDevices.asi
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

#ifdef _MSC_VER
#pragma comment(lib, "setupapi.lib")
#endif

#define HHD_VERSION "1.2.0"

/* ------------------------------------------------------------------------- */
/* DirectInput bits we need (defined locally so no SDK/dxguid is required)    */
/* ------------------------------------------------------------------------- */

#define DIENUM_CONTINUE     1
#define DIERR_DEVICENOTREG  ((HRESULT)0x80040154L) /* REGDB_E_CLASSNOTREG */

static const GUID IID_DI_A  = {0x89521360,0xAA8A,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const GUID IID_DI_W  = {0x89521361,0xAA8A,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const GUID IID_DI2_A = {0x5944E662,0xAA8A,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const GUID IID_DI2_W = {0x5944E663,0xAA8A,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const GUID IID_DI7_A = {0x9A4CB684,0x236D,0x11D3,{0x8E,0x9D,0x00,0xC0,0x4F,0x68,0x44,0xAE}};
static const GUID IID_DI7_W = {0x9A4CB685,0x236D,0x11D3,{0x8E,0x9D,0x00,0xC0,0x4F,0x68,0x44,0xAE}};
static const GUID IID_DI8_A = {0xBF798030,0x483A,0x4DA2,{0xAA,0x99,0x5D,0x64,0xED,0x36,0x97,0x00}};
static const GUID IID_DI8_W = {0xBF798031,0x483A,0x4DA2,{0xAA,0x99,0x5D,0x64,0xED,0x36,0x97,0x00}};

/* vtable slots common to IDirectInput{A,W,2,7,8} */
enum { SLOT_QI = 0, SLOT_CREATEDEVICE = 3, SLOT_ENUMDEVICES = 4,
       SLOT_GETDEVICESTATUS = 5, SLOT_CREATEDEVICEEX = 9 /* IDirectInput7 only */ };
/* IDirectInputDevice* vtable slots */
enum { DEVSLOT_RELEASE = 2, DEVSLOT_GETDEVICEINFO = 15 };

/* DIDEVICEINSTANCE layout (A and W share the first 36 bytes) */
#define DDI_OFS_GUIDINSTANCE  4
#define DDI_OFS_GUIDPRODUCT  20
#define DDI_OFS_DEVTYPE      36
#define DDI_OFS_INSTNAME     40
#define DIDEVTYPE_HID        0x00010000
#define DDI_SIZE_DX3_A      (40 + 260 * 2)
#define DDI_SIZE_DX3_W      (40 + 260 * 2 * 2)
#define DDI_SIZE_DX5_A      (DDI_SIZE_DX3_A + 20)   /* + guidFFDriver, wUsagePage, wUsage */
#define DDI_SIZE_DX5_W      (DDI_SIZE_DX3_W + 20)

typedef HRESULT (WINAPI *PFN_DICreateA)(HINSTANCE, DWORD, void **, void *);
typedef HRESULT (WINAPI *PFN_DICreateEx)(HINSTANCE, DWORD, REFIID, void **, void *);
typedef FARPROC (WINAPI *PFN_GetProcAddress)(HMODULE, LPCSTR);

typedef HRESULT (WINAPI *PFN_QI)(void *, REFIID, void **);
typedef HRESULT (WINAPI *PFN_CreateDevice)(void *, REFGUID, void **, void *);
typedef HRESULT (WINAPI *PFN_EnumDevices)(void *, DWORD, void *, void *, DWORD);
typedef HRESULT (WINAPI *PFN_GetDeviceStatus)(void *, REFGUID);
typedef HRESULT (WINAPI *PFN_CreateDeviceEx)(void *, REFGUID, REFIID, void **, void *);
typedef BOOL    (CALLBACK *PFN_EnumCallback)(const void *, void *);
typedef ULONG   (WINAPI *PFN_Release)(void *);
typedef HRESULT (WINAPI *PFN_GetDeviceInfo)(void *, void *);

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

static char g_iniPath[MAX_PATH];
static char g_logPath[MAX_PATH];
static CRITICAL_SECTION g_cs;

typedef struct { WORD vid, pid; char entry[256]; } HiddenId;
#define MAX_HIDDEN 64
static HiddenId g_hidden[MAX_HIDDEN];
static int g_hiddenCount;
static BOOL g_hideNonGameControllers = TRUE;
static volatile LONG g_initDone;

#define MAX_GUIDS 128
static GUID g_hiddenInstances[MAX_GUIDS];
static int  g_hiddenInstanceCount;
static GUID g_seenInstances[MAX_GUIDS];
static int  g_seenInstanceCount;

typedef struct {
    void **vtbl;
    BOOL   wide;
    BOOL   has7;
    PFN_QI              oQI;
    PFN_CreateDevice    oCreateDevice;
    PFN_EnumDevices     oEnumDevices;
    PFN_GetDeviceStatus oGetDeviceStatus;
    PFN_CreateDeviceEx  oCreateDeviceEx;
} VtblHook;
#define MAX_VTBLS 16
static VtblHook g_vtbls[MAX_VTBLS];
static int g_vtblCount;

static PFN_DICreateA      o_DirectInputCreateA;
static PFN_DICreateA      o_DirectInputCreateW;
static PFN_DICreateEx     o_DirectInputCreateEx;
static PFN_DICreateEx     o_DirectInput8Create;
static PFN_GetProcAddress o_GetProcAddress;

/* ------------------------------------------------------------------------- */
/* Logging                                                                   */
/* ------------------------------------------------------------------------- */

static void Log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    int n;
    HANDLE f;
    DWORD written;
    SYSTEMTIME st;

    GetLocalTime(&st);
    n = _snprintf(buf, sizeof(buf) - 3, "[%02u:%02u:%02u.%03u] ",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    n += _vsnprintf(buf + n, sizeof(buf) - 3 - n, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof(buf) - 3) n = (int)sizeof(buf) - 3;
    buf[n++] = '\r'; buf[n++] = '\n'; buf[n] = 0;

    f = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, buf, (DWORD)n, &written, NULL);
        CloseHandle(f);
    }
}

static void GuidToStr(const GUID *g, char *out, size_t len)
{
    _snprintf(out, len, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
              (unsigned long)g->Data1, g->Data2, g->Data3,
              g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
              g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    out[len - 1] = 0;
}

/* ------------------------------------------------------------------------- */
/* .ini parsing and device matching                                          */
/* ------------------------------------------------------------------------- */

static int ParseHex4(const char *p, WORD *out)
{
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        if (c >= '0' && c <= '9') v = v * 16 + (unsigned)(c - '0');
        else if (c >= 'A' && c <= 'F') v = v * 16 + (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = (WORD)v;
    return 1;
}

/* Does a hardware ID reported by Windows start with the .ini entry, ending
   on a boundary? ("HID\VID_1&PID_2&REV_3" matches "...&REV_3&Col01") */
static BOOL HwIdMatches(const char *hwid, const char *entryUpper)
{
    size_t n = strlen(entryUpper), i;
    for (i = 0; i < n; i++) {
        if (!hwid[i]) return FALSE;
        if (toupper((unsigned char)hwid[i]) != entryUpper[i]) return FALSE;
    }
    return hwid[n] == 0 || hwid[n] == '&';
}

/* Returns 1 if a present device has a hardware ID matching the entry,
   0 if none does, -1 if SetupAPI could not be queried. */
static int PresentDeviceMatches(const char *entryUpper, char *matched, size_t matchedLen)
{
    HDEVINFO h;
    SP_DEVINFO_DATA d;
    DWORD i;
    int result = 0;
    static char ids[8192];

    h = SetupDiGetClassDevsA(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (h == INVALID_HANDLE_VALUE) return -1;

    for (i = 0; result == 0; i++) {
        DWORD type = 0;
        const char *p;
        memset(&d, 0, sizeof(d));
        d.cbSize = sizeof(d);
        if (!SetupDiEnumDeviceInfo(h, i, &d)) break;
        memset(ids, 0, sizeof(ids));
        if (!SetupDiGetDeviceRegistryPropertyA(h, &d, SPDRP_HARDWAREID, &type,
                                               (BYTE *)ids, sizeof(ids) - 2, NULL))
            continue;
        for (p = ids; *p; p += strlen(p) + 1) {
            if (HwIdMatches(p, entryUpper)) {
                lstrcpynA(matched, p, (int)matchedLen);
                result = 1;
                break;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(h);
    return result;
}

static void ProcessIniLine(char *line)
{
    char *s = line, *e, *vidp, *pidp, *end;
    char matched[512];
    WORD vid, pid;
    BOOL qualified;
    size_t k;

    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '"')) *--e = 0;
    if (*s == '"') s++;
    if (!*s || *s == ';' || *s == '#' || *s == '[') return;

    /* HideNonGameControllers=0/1 */
    if (!_strnicmp(s, "HideNonGameControllers", 22)) {
        e = strchr(s, '=');
        if (e) {
            e++;
            while (*e == ' ' || *e == '\t') e++;
            g_hideNonGameControllers = !(*e == '0' || *e == 'n' || *e == 'N' || *e == 'f' || *e == 'F');
        }
        Log("INI: HideNonGameControllers = %d", g_hideNonGameControllers);
        return;
    }

    /* allow "Device1=HID\VID_..." style lines too */
    if ((e = strchr(s, '=')) != NULL) s = e + 1;
    while (*s == ' ' || *s == '\t' || *s == '"') s++;

    for (k = 0; s[k]; k++) s[k] = (char)toupper((unsigned char)s[k]);

    vidp = strstr(s, "VID_");
    pidp = strstr(s, "PID_");
    if (!vidp || !pidp || !ParseHex4(vidp + 4, &vid) || !ParseHex4(pidp + 4, &pid)) {
        Log("INI: ignoring line (no VID_xxxx / PID_xxxx found): %s", s);
        return;
    }
    if (g_hiddenCount >= MAX_HIDDEN) {
        Log("INI: too many entries, ignoring: %s", s);
        return;
    }

    /* Anything after PID_xxxx (REV_, MI_, COL...) makes the entry "qualified":
       then it only applies if a connected device carries that exact ID. */
    end = pidp + 8;
    qualified = (*end != 0);
    if (qualified) {
        int r = PresentDeviceMatches(s, matched, sizeof(matched));
        if (r == 0) {
            Log("INI: %s -> no connected device has this hardware ID, not hiding VID_%04X&PID_%04X",
                s, vid, pid);
            return;
        }
        if (r == 1)
            Log("INI: %s -> matches connected device %s", s, matched);
        else
            Log("INI: %s -> could not query Windows device list, hiding by VID/PID anyway", s);
    } else {
        Log("INI: %s", s);
    }

    g_hidden[g_hiddenCount].vid = vid;
    g_hidden[g_hiddenCount].pid = pid;
    lstrcpynA(g_hidden[g_hiddenCount].entry, s, sizeof(g_hidden[0].entry));
    g_hiddenCount++;
    Log("      will hide DirectInput devices with VID_%04X&PID_%04X", vid, pid);
}

static void LoadIni(void)
{
    HANDLE f;
    DWORD size, read = 0;
    char *buf, *p, *line;

    f = CreateFileA(g_iniPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        Log("INI: %s not found - nothing will be hidden", g_iniPath);
        return;
    }
    size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE || size > 1024 * 1024) { CloseHandle(f); return; }
    buf = (char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 4);
    if (!buf) { CloseHandle(f); return; }
    ReadFile(f, buf, size, &read, NULL);
    CloseHandle(f);

    p = buf;
    /* UTF-16 (Notepad "Unicode") -> convert to ANSI */
    if (read >= 2 && (BYTE)buf[0] == 0xFF && (BYTE)buf[1] == 0xFE) {
        int wl = (int)(read / 2) - 1;
        int al = WideCharToMultiByte(CP_ACP, 0, (WCHAR *)(buf + 2), wl, NULL, 0, NULL, NULL);
        char *a = (char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)al + 4);
        if (a) {
            WideCharToMultiByte(CP_ACP, 0, (WCHAR *)(buf + 2), wl, a, al, NULL, NULL);
            HeapFree(GetProcessHeap(), 0, buf);
            buf = p = a;
        }
    } else if (read >= 3 && (BYTE)buf[0] == 0xEF && (BYTE)buf[1] == 0xBB && (BYTE)buf[2] == 0xBF) {
        p += 3; /* UTF-8 BOM */
    }

    Log("INI: reading %s", g_iniPath);
    while (*p) {
        line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        ProcessIniLine(line);
    }
    HeapFree(GetProcessHeap(), 0, buf);
    Log("INI: %d device ID(s) to hide", g_hiddenCount);
    Log("INI: HID devices that are not game controllers will be %s",
        g_hideNonGameControllers ? "hidden (HideNonGameControllers=1)" : "kept (HideNonGameControllers=0)");
}

/* Log every connected HID device so that other problem devices can be identified. */
static void LogConnectedHidDevices(void)
{
    HDEVINFO h;
    SP_DEVINFO_DATA d;
    DWORD i, type;
    static char ids[8192];
    char desc[256];

    h = SetupDiGetClassDevsA(NULL, "HID", NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (h == INVALID_HANDLE_VALUE) {
        Log("Windows HID list: could not be read (error %lu)", (unsigned long)GetLastError());
        return;
    }
    Log("Connected HID devices reported by Windows:");
    for (i = 0; ; i++) {
        memset(&d, 0, sizeof(d));
        d.cbSize = sizeof(d);
        if (!SetupDiEnumDeviceInfo(h, i, &d)) break;
        memset(ids, 0, sizeof(ids));
        desc[0] = 0;
        if (!SetupDiGetDeviceRegistryPropertyA(h, &d, SPDRP_HARDWAREID, &type,
                                               (BYTE *)ids, sizeof(ids) - 2, NULL))
            continue;
        if (!SetupDiGetDeviceRegistryPropertyA(h, &d, SPDRP_DEVICEDESC, &type,
                                               (BYTE *)desc, sizeof(desc) - 1, NULL))
            desc[0] = 0;
        desc[sizeof(desc) - 1] = 0;
        Log("  %s  (%s)", ids, desc);
    }
    SetupDiDestroyDeviceInfoList(h);
}

static void EnsureInit(void)
{
    if (g_initDone) return;
    EnterCriticalSection(&g_cs);
    if (!g_initDone) {
        LogConnectedHidDevices();
        LoadIni();
        InterlockedExchange(&g_initDone, 1);
    }
    LeaveCriticalSection(&g_cs);
}

static BOOL ProductGuidIsHidden(const GUID *prod, WORD *vidOut, WORD *pidOut)
{
    static const BYTE pidvid[8] = {0, 0, 'P', 'I', 'D', 'V', 'I', 'D'};
    WORD vid, pid;
    int i;
    if (prod->Data2 != 0 || prod->Data3 != 0 || memcmp(prod->Data4, pidvid, 8) != 0)
        return FALSE;
    vid = LOWORD(prod->Data1);
    pid = HIWORD(prod->Data1);
    if (vidOut) *vidOut = vid;
    if (pidOut) *pidOut = pid;
    for (i = 0; i < g_hiddenCount; i++)
        if (g_hidden[i].vid == vid && g_hidden[i].pid == pid) return TRUE;
    return FALSE;
}

/* Why should this DIDEVICEINSTANCE be hidden? NULL = keep it. */
static const char *HideReason(const BYTE *ddi, WORD *vid, WORD *pid)
{
    DWORD type, sub;
    *vid = *pid = 0;
    if (ProductGuidIsHidden((const GUID *)(ddi + DDI_OFS_GUIDPRODUCT), vid, pid))
        return "listed in .ini";
    if (!g_hideNonGameControllers) return NULL;
    type = *(const DWORD *)(ddi + DDI_OFS_DEVTYPE);
    if (!(type & DIDEVTYPE_HID)) return NULL;  /* system mouse/keyboard: always keep */
    sub = type & 0xFF;
    /* DirectInput 3-7: 1 device, 2 mouse, 3 keyboard (4 = joystick)
       DirectInput 8: 0x11 device, 0x12 mouse, 0x13 keyboard, 0x19 device control,
                      0x1A screen pointer, 0x1B remote (0x14-0x18, 0x1C = game controllers) */
    if (sub == 1 || sub == 2 || sub == 3 || sub == 0x11 || sub == 0x12 || sub == 0x13 ||
        sub == 0x19 || sub == 0x1A || sub == 0x1B)
        return "not a game controller";
    return NULL;
}

static BOOL GuidInList(const GUID *g, const GUID *list, int count)
{
    int i;
    for (i = 0; i < count; i++)
        if (IsEqualGUID(g, &list[i])) return TRUE;
    return FALSE;
}

static void AddGuid(const GUID *g, GUID *list, int *count)
{
    if (*count < MAX_GUIDS && !GuidInList(g, list, *count)) list[(*count)++] = *g;
}

static BOOL InstanceIsHidden(const GUID *g)
{
    BOOL r;
    EnterCriticalSection(&g_cs);
    r = GuidInList(g, g_hiddenInstances, g_hiddenInstanceCount);
    LeaveCriticalSection(&g_cs);
    return r;
}

/* ------------------------------------------------------------------------- */
/* IDirectInput hooks                                                        */
/* ------------------------------------------------------------------------- */

static VtblHook *FindHook(void *obj)
{
    void **vtbl = *(void ***)obj;
    int i;
    for (i = 0; i < g_vtblCount; i++)
        if (g_vtbls[i].vtbl == vtbl) return &g_vtbls[i];
    return NULL;
}

static void PatchSlot(void **vtbl, int slot, void *fn)
{
    DWORD old;
    if (VirtualProtect(&vtbl[slot], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) {
        vtbl[slot] = fn;
        VirtualProtect(&vtbl[slot], sizeof(void *), old, &old);
        FlushInstructionCache(GetCurrentProcess(), &vtbl[slot], sizeof(void *));
    }
}

static void PatchObject(void *obj, BOOL wide, BOOL has7);

static void DescribeIID(REFIID riid, BOOL *isDI, BOOL *wide, BOOL *has7)
{
    *isDI = TRUE; *wide = FALSE; *has7 = FALSE;
    if (IsEqualGUID(riid, &IID_DI_A) || IsEqualGUID(riid, &IID_DI2_A) || IsEqualGUID(riid, &IID_DI8_A)) return;
    if (IsEqualGUID(riid, &IID_DI_W) || IsEqualGUID(riid, &IID_DI2_W) || IsEqualGUID(riid, &IID_DI8_W)) { *wide = TRUE; return; }
    if (IsEqualGUID(riid, &IID_DI7_A)) { *has7 = TRUE; return; }
    if (IsEqualGUID(riid, &IID_DI7_W)) { *has7 = TRUE; *wide = TRUE; return; }
    *isDI = FALSE;
}

/* If a freshly created device turns out to be a hidden one, refuse it. */
static BOOL CreatedDeviceIsHidden(void *dev, BOOL wide)
{
    BYTE info[DDI_SIZE_DX5_W + 16];
    void **vt = *(void ***)dev;
    PFN_GetDeviceInfo getInfo = (PFN_GetDeviceInfo)vt[DEVSLOT_GETDEVICEINFO];
    WORD vid, pid;
    /* DirectInput 8 only accepts the DX5+ structure size, DirectInput 3 only the DX3 size */
    memset(info, 0, sizeof(info));
    *(DWORD *)info = wide ? DDI_SIZE_DX5_W : DDI_SIZE_DX5_A;
    if (FAILED(getInfo(dev, info))) {
        memset(info, 0, sizeof(info));
        *(DWORD *)info = wide ? DDI_SIZE_DX3_W : DDI_SIZE_DX3_A;
        if (FAILED(getInfo(dev, info))) return FALSE;
    }
    if (HideReason(info, &vid, &pid)) {
        EnterCriticalSection(&g_cs);
        AddGuid((const GUID *)(info + DDI_OFS_GUIDINSTANCE), g_hiddenInstances, &g_hiddenInstanceCount);
        LeaveCriticalSection(&g_cs);
        return TRUE;
    }
    return FALSE;
}

static HRESULT WINAPI Hk_QueryInterface(void *self, REFIID riid, void **ppv)
{
    VtblHook *h = FindHook(self);
    HRESULT hr;
    BOOL isDI, wide, has7;
    if (!h) return E_NOINTERFACE;
    hr = h->oQI(self, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) {
        DescribeIID(riid, &isDI, &wide, &has7);
        if (isDI) PatchObject(*ppv, wide, has7);
    }
    return hr;
}

typedef struct { PFN_EnumCallback cb; void *ref; BOOL wide; } EnumCtx;

static BOOL CALLBACK EnumFilter(const void *ddi, void *ref)
{
    EnumCtx *ctx = (EnumCtx *)ref;
    const BYTE *p = (const BYTE *)ddi;
    const GUID *inst, *prod;
    WORD vid = 0, pid = 0;
    BOOL hidden, firstTime;
    const char *reason;
    char name[260], gs[64];

    if (!ddi || *(const DWORD *)p < DDI_OFS_GUIDPRODUCT + sizeof(GUID))
        return ctx->cb(ddi, ctx->ref);

    inst = (const GUID *)(p + DDI_OFS_GUIDINSTANCE);
    prod = (const GUID *)(p + DDI_OFS_GUIDPRODUCT);
    reason = HideReason(p, &vid, &pid);
    hidden = reason != NULL;

    EnterCriticalSection(&g_cs);
    firstTime = !GuidInList(inst, g_seenInstances, g_seenInstanceCount);
    AddGuid(inst, g_seenInstances, &g_seenInstanceCount);
    if (hidden) AddGuid(inst, g_hiddenInstances, &g_hiddenInstanceCount);
    LeaveCriticalSection(&g_cs);

    if (firstTime) {
        name[0] = 0;
        if (*(const DWORD *)p >= DDI_OFS_INSTNAME + 2) {
            if (ctx->wide)
                WideCharToMultiByte(CP_ACP, 0, (const WCHAR *)(p + DDI_OFS_INSTNAME), -1,
                                    name, sizeof(name), NULL, NULL);
            else
                lstrcpynA(name, (const char *)(p + DDI_OFS_INSTNAME), sizeof(name));
            name[sizeof(name) - 1] = 0;
        }
        GuidToStr(prod, gs, sizeof(gs));
        if (vid || pid)
            Log("Enum: \"%s\" VID_%04X&PID_%04X type 0x%lX -> %s%s", name, vid, pid,
                (unsigned long)*(const DWORD *)(p + DDI_OFS_DEVTYPE),
                hidden ? "HIDDEN, " : "visible", hidden ? reason : "");
        else
            Log("Enum: \"%s\" product %s type 0x%lX -> %s%s", name, gs,
                (unsigned long)*(const DWORD *)(p + DDI_OFS_DEVTYPE),
                hidden ? "HIDDEN, " : "visible", hidden ? reason : "");
    }

    if (hidden) return DIENUM_CONTINUE;
    return ctx->cb(ddi, ctx->ref);
}

static HRESULT WINAPI Hk_EnumDevices(void *self, DWORD type, void *cb, void *ref, DWORD flags)
{
    VtblHook *h = FindHook(self);
    EnumCtx ctx;
    static LONG calls;
    if (!h) return E_FAIL;
    if (InterlockedIncrement(&calls) <= 20)
        Log("EnumDevices(type 0x%lX, flags 0x%lX)", (unsigned long)type, (unsigned long)flags);
    if (!cb || (g_hiddenCount == 0 && !g_hideNonGameControllers)) return h->oEnumDevices(self, type, cb, ref, flags);
    ctx.cb = (PFN_EnumCallback)cb;
    ctx.ref = ref;
    ctx.wide = h->wide;
    return h->oEnumDevices(self, type, (void *)EnumFilter, &ctx, flags);
}

static HRESULT WINAPI Hk_CreateDevice(void *self, REFGUID rguid, void **out, void *outer)
{
    VtblHook *h = FindHook(self);
    HRESULT hr;
    char gs[64];
    if (!h) return E_FAIL;
    if (rguid && InstanceIsHidden(rguid)) {
        if (out) *out = NULL;
        GuidToStr(rguid, gs, sizeof(gs));
        Log("CreateDevice %s blocked (hidden device)", gs);
        return DIERR_DEVICENOTREG;
    }
    hr = h->oCreateDevice(self, rguid, out, outer);
    if (SUCCEEDED(hr) && out && *out && (g_hiddenCount || g_hideNonGameControllers) && CreatedDeviceIsHidden(*out, h->wide)) {
        ((PFN_Release)(*(void ***)*out)[DEVSLOT_RELEASE])(*out);
        *out = NULL;
        GuidToStr(rguid, gs, sizeof(gs));
        Log("CreateDevice %s blocked (hidden device)", gs);
        return DIERR_DEVICENOTREG;
    }
    if (rguid) {
        GuidToStr(rguid, gs, sizeof(gs));
        Log("CreateDevice %s -> 0x%08lX", gs, (unsigned long)hr);
    }
    return hr;
}

static HRESULT WINAPI Hk_CreateDeviceEx(void *self, REFGUID rguid, REFIID riid, void **out, void *outer)
{
    VtblHook *h = FindHook(self);
    HRESULT hr;
    char gs[64];
    if (!h || !h->oCreateDeviceEx) return E_FAIL;
    if (rguid && InstanceIsHidden(rguid)) {
        if (out) *out = NULL;
        GuidToStr(rguid, gs, sizeof(gs));
        Log("CreateDeviceEx %s blocked (hidden device)", gs);
        return DIERR_DEVICENOTREG;
    }
    hr = h->oCreateDeviceEx(self, rguid, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out && (g_hiddenCount || g_hideNonGameControllers) && CreatedDeviceIsHidden(*out, h->wide)) {
        ((PFN_Release)(*(void ***)*out)[DEVSLOT_RELEASE])(*out);
        *out = NULL;
        GuidToStr(rguid, gs, sizeof(gs));
        Log("CreateDeviceEx %s blocked (hidden device)", gs);
        return DIERR_DEVICENOTREG;
    }
    if (rguid) {
        GuidToStr(rguid, gs, sizeof(gs));
        Log("CreateDeviceEx %s -> 0x%08lX", gs, (unsigned long)hr);
    }
    return hr;
}

static HRESULT WINAPI Hk_GetDeviceStatus(void *self, REFGUID rguid)
{
    VtblHook *h = FindHook(self);
    if (!h) return E_FAIL;
    if (rguid && InstanceIsHidden(rguid)) return DIERR_DEVICENOTREG;
    return h->oGetDeviceStatus(self, rguid);
}

static void PatchObject(void *obj, BOOL wide, BOOL has7)
{
    VtblHook *h;
    void **vtbl;

    if (!obj) return;
    EnsureInit();
    EnterCriticalSection(&g_cs);
    vtbl = *(void ***)obj;
    h = FindHook(obj);
    if (!h && g_vtblCount < MAX_VTBLS) {
        h = &g_vtbls[g_vtblCount++];
        memset(h, 0, sizeof(*h));
        h->vtbl = vtbl;
        h->wide = wide;
        h->oQI              = (PFN_QI)vtbl[SLOT_QI];
        h->oCreateDevice    = (PFN_CreateDevice)vtbl[SLOT_CREATEDEVICE];
        h->oEnumDevices     = (PFN_EnumDevices)vtbl[SLOT_ENUMDEVICES];
        h->oGetDeviceStatus = (PFN_GetDeviceStatus)vtbl[SLOT_GETDEVICESTATUS];
        PatchSlot(vtbl, SLOT_QI,              (void *)Hk_QueryInterface);
        PatchSlot(vtbl, SLOT_CREATEDEVICE,    (void *)Hk_CreateDevice);
        PatchSlot(vtbl, SLOT_ENUMDEVICES,     (void *)Hk_EnumDevices);
        PatchSlot(vtbl, SLOT_GETDEVICESTATUS, (void *)Hk_GetDeviceStatus);
        Log("Hooked IDirectInput%s interface (vtable %p)", wide ? "W" : "A", (void *)vtbl);
    }
    if (h && has7 && !h->has7) {
        h->has7 = TRUE;
        h->oCreateDeviceEx = (PFN_CreateDeviceEx)vtbl[SLOT_CREATEDEVICEEX];
        PatchSlot(vtbl, SLOT_CREATEDEVICEEX, (void *)Hk_CreateDeviceEx);
    }
    LeaveCriticalSection(&g_cs);
}

/* ------------------------------------------------------------------------- */
/* DirectInput factory hooks                                                 */
/* ------------------------------------------------------------------------- */

static FARPROC RealExport(const char *dll, const char *fn)
{
    HMODULE m = GetModuleHandleA(dll);
    if (!m) m = LoadLibraryA(dll);
    return m ? GetProcAddress(m, fn) : NULL;
}

static HRESULT WINAPI Hk_DirectInputCreateA(HINSTANCE hi, DWORD ver, void **out, void *outer)
{
    HRESULT hr;
    if (!o_DirectInputCreateA) o_DirectInputCreateA = (PFN_DICreateA)RealExport("dinput.dll", "DirectInputCreateA");
    if (!o_DirectInputCreateA) return E_FAIL;
    EnsureInit();
    Log("DirectInputCreateA(version 0x%04lX)", (unsigned long)ver);
    hr = o_DirectInputCreateA(hi, ver, out, outer);
    if (SUCCEEDED(hr) && out && *out) PatchObject(*out, FALSE, FALSE);
    return hr;
}

static HRESULT WINAPI Hk_DirectInputCreateW(HINSTANCE hi, DWORD ver, void **out, void *outer)
{
    HRESULT hr;
    if (!o_DirectInputCreateW) o_DirectInputCreateW = (PFN_DICreateA)RealExport("dinput.dll", "DirectInputCreateW");
    if (!o_DirectInputCreateW) return E_FAIL;
    EnsureInit();
    Log("DirectInputCreateW(version 0x%04lX)", (unsigned long)ver);
    hr = o_DirectInputCreateW(hi, ver, out, outer);
    if (SUCCEEDED(hr) && out && *out) PatchObject(*out, TRUE, FALSE);
    return hr;
}

static HRESULT WINAPI Hk_DirectInputCreateEx(HINSTANCE hi, DWORD ver, REFIID riid, void **out, void *outer)
{
    HRESULT hr;
    BOOL isDI, wide, has7;
    if (!o_DirectInputCreateEx) o_DirectInputCreateEx = (PFN_DICreateEx)RealExport("dinput.dll", "DirectInputCreateEx");
    if (!o_DirectInputCreateEx) return E_FAIL;
    EnsureInit();
    Log("DirectInputCreateEx(version 0x%04lX)", (unsigned long)ver);
    hr = o_DirectInputCreateEx(hi, ver, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        DescribeIID(riid, &isDI, &wide, &has7);
        if (isDI) PatchObject(*out, wide, has7);
    }
    return hr;
}

static HRESULT WINAPI Hk_DirectInput8Create(HINSTANCE hi, DWORD ver, REFIID riid, void **out, void *outer)
{
    HRESULT hr;
    BOOL isDI, wide, has7;
    if (!o_DirectInput8Create) o_DirectInput8Create = (PFN_DICreateEx)RealExport("dinput8.dll", "DirectInput8Create");
    if (!o_DirectInput8Create) return E_FAIL;
    EnsureInit();
    Log("DirectInput8Create(version 0x%04lX)", (unsigned long)ver);
    hr = o_DirectInput8Create(hi, ver, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        DescribeIID(riid, &isDI, &wide, &has7);
        if (isDI) PatchObject(*out, wide, has7);
    }
    return hr;
}

static FARPROC WINAPI Hk_GetProcAddress(HMODULE mod, LPCSTR name)
{
    FARPROC real = o_GetProcAddress ? o_GetProcAddress(mod, name) : GetProcAddress(mod, name);
    if (!real || !name || !HIWORD((DWORD_PTR)name)) return real;

    if (!lstrcmpA(name, "DirectInputCreateA")) {
        if (!o_DirectInputCreateA) o_DirectInputCreateA = (PFN_DICreateA)real;
        return (FARPROC)Hk_DirectInputCreateA;
    }
    if (!lstrcmpA(name, "DirectInputCreateW")) {
        if (!o_DirectInputCreateW) o_DirectInputCreateW = (PFN_DICreateA)real;
        return (FARPROC)Hk_DirectInputCreateW;
    }
    if (!lstrcmpA(name, "DirectInputCreateEx")) {
        if (!o_DirectInputCreateEx) o_DirectInputCreateEx = (PFN_DICreateEx)real;
        return (FARPROC)Hk_DirectInputCreateEx;
    }
    if (!lstrcmpA(name, "DirectInput8Create")) {
        if (!o_DirectInput8Create) o_DirectInput8Create = (PFN_DICreateEx)real;
        return (FARPROC)Hk_DirectInput8Create;
    }
    return real;
}

/* ------------------------------------------------------------------------- */
/* Import table patching                                                     */
/* ------------------------------------------------------------------------- */

typedef struct { const char *dll; const char *fn; void **orig; void *hook; } IatHook;

static IatHook g_iatHooks[] = {
    { "dinput.dll",   "DirectInputCreateA",  (void **)&o_DirectInputCreateA,  (void *)Hk_DirectInputCreateA },
    { "dinput.dll",   "DirectInputCreateW",  (void **)&o_DirectInputCreateW,  (void *)Hk_DirectInputCreateW },
    { "dinput.dll",   "DirectInputCreateEx", (void **)&o_DirectInputCreateEx, (void *)Hk_DirectInputCreateEx },
    { "dinput8.dll",  "DirectInput8Create",  (void **)&o_DirectInput8Create,  (void *)Hk_DirectInput8Create },
    { "kernel32.dll", "GetProcAddress",      (void **)&o_GetProcAddress,      (void *)Hk_GetProcAddress },
};
#define IAT_HOOK_COUNT (sizeof(g_iatHooks) / sizeof(g_iatHooks[0]))

static void HookImports(HMODULE mod)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY *dir;
    IMAGE_IMPORT_DESCRIPTOR *desc;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return;

    for (desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); desc->Name; desc++) {
        const char *dllName = (const char *)(base + desc->Name);
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + desc->FirstThunk);
        IMAGE_THUNK_DATA *names = desc->OriginalFirstThunk
            ? (IMAGE_THUNK_DATA *)(base + desc->OriginalFirstThunk) : NULL;
        size_t h;
        int i;
        BOOL relevant = FALSE;

        /* Report legacy joystick API use - those calls are not filtered. */
        if (!lstrcmpiA(dllName, "winmm.dll") && names) {
            for (i = 0; names[i].u1.AddressOfData; i++) {
                const char *fn;
                if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) continue;
                fn = (const char *)((IMAGE_IMPORT_BY_NAME *)(base + names[i].u1.AddressOfData))->Name;
                if (!strncmp(fn, "joy", 3)) Log("Note: game imports winmm!%s (not filtered)", fn);
            }
        }

        for (h = 0; h < IAT_HOOK_COUNT; h++)
            if (!lstrcmpiA(dllName, g_iatHooks[h].dll)) relevant = TRUE;
        if (!relevant) continue;

        for (i = 0; iat[i].u1.Function; i++) {
            const char *fn = NULL;
            if (names && !IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal))
                fn = (const char *)((IMAGE_IMPORT_BY_NAME *)(base + names[i].u1.AddressOfData))->Name;

            for (h = 0; h < IAT_HOOK_COUNT; h++) {
                IatHook *ih = &g_iatHooks[h];
                BOOL match;
                DWORD old;
                if (lstrcmpiA(dllName, ih->dll)) continue;
                if (fn) match = !lstrcmpA(fn, ih->fn);
                else    match = ((FARPROC)iat[i].u1.Function == RealExport(ih->dll, ih->fn));
                if (!match) continue;

                if (!*ih->orig) *ih->orig = (void *)iat[i].u1.Function;
                if (VirtualProtect(&iat[i].u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                    iat[i].u1.Function = (DWORD_PTR)ih->hook;
                    VirtualProtect(&iat[i].u1.Function, sizeof(void *), old, &old);
                    Log("Hooked %s!%s in game executable", ih->dll, ih->fn);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Crash logging                                                             */
/* ------------------------------------------------------------------------- */

static void DescribeAddress(DWORD_PTR addr, char *out, size_t len)
{
    HMODULE m = NULL;
    char path[MAX_PATH], *name;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &m) && m &&
        GetModuleFileNameA(m, path, MAX_PATH)) {
        path[MAX_PATH - 1] = 0;
        name = strrchr(path, '\\');
        _snprintf(out, len, "%s+0x%lX", name ? name + 1 : path,
                  (unsigned long)(addr - (DWORD_PTR)m));
    } else {
        _snprintf(out, len, "0x%08lX (no module)", (unsigned long)addr);
    }
    out[len - 1] = 0;
}

static BOOL AddressInCode(DWORD_PTR addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (addr < 0x10000 || !VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) return FALSE;
    return mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE &&
           (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY));
}

static LONG CALLBACK CrashLogger(EXCEPTION_POINTERS *ep)
{
    static LONG count;
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    DWORD code = er->ExceptionCode;
    char where[MAX_PATH + 32];
    DWORD_PTR *sp;
    int i, found;

    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != 0xC0000374 /* heap corruption */)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&count) > 5) return EXCEPTION_CONTINUE_SEARCH;

    DescribeAddress((DWORD_PTR)er->ExceptionAddress, where, sizeof(where));
    Log("*** EXCEPTION 0x%08lX at %s", (unsigned long)code, where);
    if (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        Log("    %s address 0x%08lX",
            er->ExceptionInformation[0] == 0 ? "read from" :
            er->ExceptionInformation[0] == 1 ? "write to" : "execute at",
            (unsigned long)er->ExceptionInformation[1]);
#ifdef _M_IX86
    Log("    EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX",
        c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi);
    Log("    EIP=%08lX ESP=%08lX EBP=%08lX thread %lu",
        c->Eip, c->Esp, c->Ebp, (unsigned long)GetCurrentThreadId());
    sp = (DWORD_PTR *)c->Esp;
#elif defined(__i386__)
    Log("    EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX",
        c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi);
    Log("    EIP=%08lX ESP=%08lX EBP=%08lX thread %lu",
        c->Eip, c->Esp, c->Ebp, (unsigned long)GetCurrentThreadId());
    sp = (DWORD_PTR *)c->Esp;
#else
    sp = NULL;
#endif
    /* Heuristic call stack: code addresses found on the stack. */
    if (sp && code != EXCEPTION_STACK_OVERFLOW) {
        Log("    Possible callers (from stack):");
        DWORD_PTR *top = (DWORD_PTR *)((NT_TIB *)NtCurrentTeb())->StackBase;
        for (i = 0, found = 0; i < 1024 && found < 20 && sp + i < top; i++) {
            DWORD_PTR v = sp[i];
            if (AddressInCode(v)) {
                DescribeAddress(v, where, sizeof(where));
                Log("      [ESP+0x%03X] %s", i * 4, where);
                found++;
            }
        }
    }
    Log("    (first-chance; the game may still handle this exception)");
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ------------------------------------------------------------------------- */

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH], *dot, *slash;
        DisableThreadLibraryCalls(hinst);
        InitializeCriticalSection(&g_cs);

        GetModuleFileNameA(hinst, path, MAX_PATH);
        path[MAX_PATH - 1] = 0;
        dot = strrchr(path, '.');
        slash = strrchr(path, '\\');
        if (dot && (!slash || dot > slash)) *dot = 0;
        _snprintf(g_iniPath, MAX_PATH, "%s.ini", path); g_iniPath[MAX_PATH - 1] = 0;
        _snprintf(g_logPath, MAX_PATH, "%s.log", path); g_logPath[MAX_PATH - 1] = 0;

        /* keep the previous run's log, so a crash log survives the next launch */
        {
            char prev[MAX_PATH];
            _snprintf(prev, MAX_PATH, "%s.prev.log", path); prev[MAX_PATH - 1] = 0;
            MoveFileExA(g_logPath, prev, MOVEFILE_REPLACE_EXISTING);
        }
        Log("HideHIDDevices %s loaded", HHD_VERSION);
        AddVectoredExceptionHandler(1, CrashLogger);
        HookImports(GetModuleHandleA(NULL));
    }
    return TRUE;
}
