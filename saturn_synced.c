/*
 * Saturn.Synced - frame-cap replacement for Hardwar (HardwarW.exe, Steam 2023 / build 2005-01-03)
 *
 * Loaded as a dinput8.dll proxy (HardwarW.exe statically imports DINPUT8.dll).
 *
 * Background (from disassembly of HardwarW.exe):
 *   The in-game frame function (0x428A30) calls a frame limiter at 0x48D100(ecx=minMs, edx=sleepMs).
 *   That limiter busy-waits until minMs has passed since the previous frame and then publishes the
 *   frame delta that ALL game logic uses:
 *       0x541740  int  dt in milliseconds (clamped to 166)
 *       0x541734  int  dt in seconds, 1.14 fixed point  ((dt_ms << 14) / 1000)
 *       0x541744  int  fps (1000 / dt)
 *       0x541728  int  previous dt (ms), 0x541730 accumulated ms, 0x54172c frame counter
 *       0x54173c  int  timeGetTime() of the last frame, 0x541748 "skip next N frames" counter
 *   The game is therefore a *variable timestep* engine: physics, AI and timers scale with dt.
 *   Single-player passes minMs = 37 (0x25)  -> ~27 fps cap.
 *   Net games pass 50 (0x32), record/playback modes 41/43 (and 'recordsavefast' forces dt=41).
 *
 * What this DLL does:
 *   It redirects the single-player limiter call (0x428ABF) - and optionally the net-game calls -
 *   to a replacement that paces frames with QueryPerformanceCounter at a configurable rate and
 *   publishes the same globals, but with sub-millisecond carry so that the sum of all dt values
 *   equals real elapsed time exactly (no drift from integer truncation at high frame rates).
 *   Game logic keeps running in real time; only the number of frames per second changes.
 *
 *   At higher frame rates some per-frame limits in the pilot code make AI moths stall during slow,
 *   precise manoeuvres (e.g. lining up with hangar entrances). Computer pilots therefore receive
 *   their control/steering tick at the original ~27 Hz (AIUpdateRate), and the steering limits in
 *   0x45F8D0 are scaled with the frame time (FixAITurning).
 */

#include "win32min.h"

/* ---------------------------------------------------------------- game addresses (HardwarW.exe) */
#define G_PREV_DT      (*(volatile int *)0x541728)
#define G_FRAMECOUNT   (*(volatile int *)0x54172c)
#define G_ACCUM_MS     (*(volatile int *)0x541730)
#define G_DT_FIXED     (*(volatile int *)0x541734)
#define G_LAST_TGT     (*(volatile int *)0x54173c)
#define G_DT_MS        (*(volatile int *)0x541740)
#define G_FPS          (*(volatile int *)0x541744)
#define G_SKIPFRAMES   (*(volatile int *)0x541748)

#define ADDR_LIMITER   0x48D100u
#define DT_CLAMP_MS    166

typedef void (__fastcall *LimiterFn)(int minMs, int sleepMs);


/* ---------------------------------------------------------------- config */
static int  cfg_TargetFPS     = 60;
static int  cfg_NetGameFPS    = 0;    /* 0 = leave net games at original 20 fps */
static int  cfg_Log           = 1;
static int  cfg_Verbose       = 0;    /* 0 = no periodic [stats] lines in the log */
static int  cfg_FixAITurning  = 1;    /* scale per-frame rotation thresholds with dt */
static int  cfg_AIUpdateRate  = 25;   /* AI pilots get their tick at this rate (0 = every frame) */
static int  cfg_CrashLog      = 1;    /* write saturn_synced_crash.log when the game crashes */
static int  cfg_CrashGuard    = 1;    /* recover from the known AI crashes (see crash_guard) */
static int  cfg_SafeDelete    = 1;    /* keep deleted moths/pilots as inert records (see install_safedelete) */
static int  cfg_PreciseMovement = 0;  /* sub-unit accurate object movement (see install_movement) */
static int  cfg_PlanCheck     = 1;    /* drop AI plans that refer to moths/pilots/buildings that do not exist */
static volatile DWORD g_frameNo;
static void crash_reassert(void);
static int  cfg_SpinMs        = 2;    /* final part of each wait is spun for accuracy */

static char g_dir[MAX_PATH];
static HANDLE g_log = INVALID_HANDLE_VALUE;

/* ---------------------------------------------------------------- tiny libc */
int _fltused = 0;
void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
static int  s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void s_cat(char *d, const char *s) { d += s_len(d); while ((*d++ = *s++)) {} }
static int  m_eq(const void *a, const void *b, int n) { const unsigned char *p = a, *q = b; while (n--) if (*p++ != *q++) return 0; return 1; }

static void logf(const char *fmt, ...)
{
    char buf[512];
    DWORD w;
    va_list ap;
    int n;
    if (g_log == INVALID_HANDLE_VALUE) return;
    va_start(ap, fmt);
    n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    if (n > 0) { WriteFile(g_log, buf, (DWORD)n, &w, 0); WriteFile(g_log, "\r\n", 2, &w, 0); FlushFileBuffers(g_log); }
}

/* ---------------------------------------------------------------- timing */
static double g_tick_per_ms;       /* QPC ticks per millisecond */
static double g_lastQpc;            /* QPC at the end of the previous paced frame */
static double g_deadline;           /* QPC tick value of next frame deadline */
static double g_carryMs;            /* fractional ms not yet given to the game */
static double g_carryFixed;         /* fractional 1/16384 s not yet given to the game */
static int    g_lastWritten = 0x7fffffff; /* value we last wrote to G_LAST_TGT */
static int    g_haveBase;

/* stats */
static double g_statStart;
static int g_statFrames;
static int g_statGameMs;

static double qpc(void) { LARGE_INTEGER v; QueryPerformanceCounter(&v); return (double)v.QuadPart; }

static void wait_until(double deadlineTicks)
{
    for (;;) {
        double remainMs = (deadlineTicks - qpc()) / g_tick_per_ms;
        if (remainMs <= 0.0) return;
        if (remainMs > (double)cfg_SpinMs + 0.5) Sleep(1);
        else YieldProcessor();
    }
}

static void publish(int dtMs, int dtFixed)
{
    G_ACCUM_MS += G_PREV_DT;
    G_PREV_DT   = dtMs;
    G_DT_MS     = dtMs;
    G_DT_FIXED  = dtFixed;
    G_FPS       = dtMs ? 1000 / dtMs : 0;
}

static void stats(int dtMs, double now)
{
    double secs;
    if (!cfg_Log || !cfg_Verbose) return;
    if (!g_statStart) { g_statStart = now; g_statFrames = 0; g_statGameMs = 0; return; }
    g_statFrames++;
    g_statGameMs += dtMs;
    secs = (double)(now - g_statStart) / (g_tick_per_ms * 1000.0);
    if (secs >= 10.0) {
        int realMs = (int)(secs * 1000.0);
        int fps10  = (int)(g_statFrames * 10.0 / secs);
        int ratio  = realMs ? (int)((double)g_statGameMs * 10000.0 / realMs) : 0;
        logf("[stats] %d frames in %d ms -> %d.%d fps | game time %d ms = %d.%02d%% of real time",
             g_statFrames, realMs, fps10 / 10, fps10 % 10, g_statGameMs, ratio / 100, ratio % 100);
        g_statStart = now; g_statFrames = 0; g_statGameMs = 0;
    }
}

/* true if [addr, addr+len) lies inside the main executable's image */
static int in_main_image(DWORD addr, DWORD len)
{
    unsigned char *base = (unsigned char *)GetModuleHandleA(0);
    DWORD peOff, sizeOfImage;
    if (!base || base[0] != 'M' || base[1] != 'Z') return 0;
    peOff = *(DWORD *)(base + 0x3C);
    if (*(DWORD *)(base + peOff) != 0x00004550) return 0;          /* "PE\0\0" */
    sizeOfImage = *(DWORD *)(base + peOff + 24 + 56);               /* OptionalHeader.SizeOfImage */
    return addr >= (DWORD)base && addr + len <= (DWORD)base + sizeOfImage;
}

/* ---------------------------------------------------------------- AI turning fix
 * The moth steering code (0x45F8D0) turns the moth by a small rotation each frame. That rotation
 * is proportional to dt, but the code ignores it when it is smaller than a fixed number of units
 * per frame (16 for pitch/yaw, 32 for bank; yaw steps of 17..31 are rounded up to 32). Those
 * limits were tuned for ~37 ms frames. At 60 fps every step is ~2.2x smaller, so slow, precise
 * turns fall below the limit and the moth stops rotating while still a few degrees off. AI pilots
 * docking at a hangar need to be within ~3.6 degrees of the entrance direction, so the front moth
 * of the queue hovers forever just outside the entrance.
 * Fix: every frame, scale those limits by dt / 37 ms (never above the original values), so the
 * smallest turn rate that still moves the moth is the same as at the original 27 fps. */
static unsigned char *g_tfImm8[10];
static signed char    g_tfBase8[10];
static int           *g_tfImm32[2];
static int            g_tfBase32[2];
static int            g_tfOk, g_tfLast16 = -1;

static int install_turnfix(void)
{
    static const DWORD a8[10]  = { 0x45FA0D, 0x45FA12, 0x45FA62, 0x45FA67, 0x45FAA3,
                                   0x45FAA8, 0x45FADA, 0x45FADF, 0x45FAE8, 0x45FAF8 };
    static const signed char v8[10] = { 0x20, -0x20, 0x10, -0x10, 0x10, -0x10, 0x10, -0x10, 0x20, -0x20 };
    static const DWORD a32[2]  = { 0x45FAED, 0x45FAFB };   /* mov edx, +/-0x20 */
    static const int   v32[2]  = { 0x20, -0x20 };
    DWORD old;
    int i;
    for (i = 0; i < 10; i++) {
        unsigned char *p = (unsigned char *)a8[i];
        if (!in_main_image(a8[i], 3) || p[0] != 0x83 || p[1] != 0xF8 || (signed char)p[2] != v8[i]) return 0;
    }
    for (i = 0; i < 2; i++) {
        unsigned char *p = (unsigned char *)a32[i];
        if (!in_main_image(a32[i], 5) || p[0] != 0xBA || *(int *)(p + 1) != v32[i]) return 0;
    }
    if (!VirtualProtect((void *)0x45FA00, 0x100, PAGE_EXECUTE_READWRITE, &old)) return 0;
    for (i = 0; i < 10; i++) { g_tfImm8[i] = (unsigned char *)a8[i] + 2; g_tfBase8[i] = v8[i]; }
    for (i = 0; i < 2; i++)  { g_tfImm32[i] = (int *)(a32[i] + 1);    g_tfBase32[i] = v32[i]; }
    g_tfOk = 1;
    return 1;
}

static void apply_turnfix(int dtFixed)
{
    int t16, i;
    if (!g_tfOk || !cfg_FixAITurning || dtFixed <= 0) return;
    /* 606 = 37 ms in the game's 1/16384 s units */
    t16 = dtFixed >= 606 ? 16 : (16 * dtFixed + 303) / 606;
    if (t16 == g_tfLast16) return;
    g_tfLast16 = t16;
    for (i = 0; i < 10; i++)
        *g_tfImm8[i] = (unsigned char)(signed char)(g_tfBase8[i] * t16 / 16);
    for (i = 0; i < 2; i++)
        *g_tfImm32[i] = g_tfBase32[i] * t16 / 16;
    FlushInstructionCache(GetCurrentProcess(), (void *)0x45FA00, 0x100);
}

/* Replacement for the 0x48D100 call sites we patch. minMs is the game's original value. */
static void __fastcall PacedLimiter(int minMs, int sleepMs, int targetFps)
{
    double now;
    double periodTicks, elapsedMs, fx;
    int dtMs, dtFixed;

    G_FRAMECOUNT++;

    /* game asked to skip the timing of the next N frames (after loads etc.) */
    if (G_SKIPFRAMES != 0) {
        G_SKIPFRAMES--;
        G_DT_MS = 0; G_DT_FIXED = 0; G_FPS = 0;
        G_LAST_TGT = g_lastWritten = (int)timeGetTime();
        g_lastQpc = qpc();
        g_deadline = g_lastQpc;
        g_carryMs = 0.0; g_carryFixed = 0.0; g_haveBase = 1;
        return;
    }

    /* Someone else (other game states, pause handling at 0x48D290) moved the time base:
       fall back to the game's own millisecond base for this frame. */
    if (!g_haveBase || G_LAST_TGT != g_lastWritten) {
        int sinceMs = (int)(timeGetTime() - (DWORD)G_LAST_TGT);
        if (sinceMs < 0) sinceMs = 0;
        if (sinceMs > 10000) sinceMs = 10000;
        g_lastQpc = qpc() - (double)sinceMs * g_tick_per_ms;
        g_deadline = g_lastQpc;
        g_carryMs = 0.0; g_carryFixed = 0.0; g_haveBase = 1;
    }

    /* pacing */
    if (targetFps > 0) {
        periodTicks = g_tick_per_ms * 1000.0 / (double)targetFps;
        g_deadline += periodTicks;
        now = qpc();
        if (now > g_deadline + periodTicks)      /* fell behind: don't try to catch up */
            g_deadline = now;
        else
            wait_until(g_deadline);
    }
    now = qpc();

    /* exact real elapsed time -> integer ms for the game, remainder carried forward */
    elapsedMs = (now - g_lastQpc) / g_tick_per_ms + g_carryMs;
    g_lastQpc = now;
    if (elapsedMs > (double)DT_CLAMP_MS) { elapsedMs = DT_CLAMP_MS; }   /* same clamp as the game */
    dtMs = (int)elapsedMs;
    g_carryMs = elapsedMs - (double)dtMs;

    fx = (double)dtMs * 16.384 + g_carryFixed;    /* 16384 units per second */
    dtFixed = (int)fx;
    g_carryFixed = fx - (double)dtFixed;

    publish(dtMs, dtFixed);
    apply_turnfix(dtFixed);
    if ((++g_frameNo & 1023) == 1) crash_reassert();   /* in case something replaced our filter */
    G_LAST_TGT = g_lastWritten = (int)timeGetTime();
    stats(dtMs, now);
    (void)minMs; (void)sleepMs;
}

static void __fastcall Limiter_SinglePlayer(int minMs, int sleepMs) { PacedLimiter(minMs, sleepMs, cfg_TargetFPS); }
static void __fastcall Limiter_NetGame(int minMs, int sleepMs)      { PacedLimiter(minMs, sleepMs, cfg_NetGameFPS); }

/* ---------------------------------------------------------------- patching */
static int patch_rel(DWORD site, const unsigned char *expect, int expectLen, int callOffset, unsigned char opcode,
                     void *target, const char *name);
static int patch_call(DWORD site, const unsigned char *expect, int expectLen, int callOffset, void *target, const char *name)
{
    return patch_rel(site, expect, expectLen, callOffset, 0xE8, target, name);
}

static int patch_rel(DWORD site, const unsigned char *expect, int expectLen, int callOffset, unsigned char opcode,
                     void *target, const char *name)
{
    DWORD old;
    unsigned char *p = (unsigned char *)site;
    unsigned char *call = p + callOffset;
    if (!m_eq(p, expect, expectLen)) {
        logf("  %s @%08X: bytes do not match - NOT patched (different exe version?)", name, site);
        return 0;
    }
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old)) { logf("  %s: VirtualProtect failed", name); return 0; }
    call[0] = opcode;
    *(int *)(call + 1) = (int)((unsigned char *)target - (call + 5));
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    logf("  %s @%08X: patched", name, (DWORD)call);
    return 1;
}

/* ---------------------------------------------------------------- AI pilots at the original tick rate
 * Every frame 0x49D0C0 sends message 0x104 ("tick", with &dtFixed and &dtMs) to every object via
 * 0x49D1D0. For pilots (handler 0x460740) that tick runs the controls, steering (0x45F8D0),
 * throttle and the rest of the pilot logic. That code contains several per-frame limits tuned for
 * ~37 ms frames, which makes AI pilots stall during slow, precise manoeuvres at higher frame rates
 * (lining up with hangar entrances, creeping into the hangar).
 * Fix: computer pilots receive their tick at AIUpdateRate (default 25/s) with the accumulated dt,
 * exactly like the original game. Position updates still happen every frame, so their movement
 * stays smooth; only their control/steering updates run at the original rate. The player's own
 * pilot (character record +0x40 == 1) is always ticked every frame. */
#define MAX_OBJ 4096
static int g_aiAccFixed[MAX_OBJ];
static int g_aiAccMs[MAX_OBJ];
static unsigned char g_aiInit[MAX_OBJ];
static int g_aiPeriodFixed;     /* tick period in 1/16384 s */

typedef int (__fastcall *Tick_t)(int msg, int *pFixed, int *pMs, int objId);

static int is_ai_pilot_obj(int objId)
{
    unsigned char *o, *pilot, *rec;
    if (objId < 0 || objId >= MAX_OBJ) return 0;
    o = ((unsigned char **)0x5452E0)[objId];
    if (!o || *(DWORD *)(o + 0x38) != 0x460740) return 0;          /* not a pilot */
    pilot = *(unsigned char **)(o + 0x62);
    if (!pilot) return 0;
    rec = *(unsigned char **)(pilot + 0x2DC);
    if (rec && *(int *)(rec + 0x40) == 1) return 0;                 /* human player */
    return 1;
}

static volatile int g_tickObj = -1, g_tickIsAI, g_lastAITickObj = -1;

static int __fastcall Tick_Hook(int msg, int *pFixed, int *pMs, int objId)
{
    int id = objId & 0xFFFF, r;
    if (msg == 0x104 && g_aiPeriodFixed > 0 && is_ai_pilot_obj(id)) {
        int f, m;
        if (!g_aiInit[id]) {                    /* stagger AI pilots over the period */
            g_aiInit[id] = 1;
            g_aiAccFixed[id] = (id * 97) % g_aiPeriodFixed;
            g_aiAccMs[id] = 0;
        }
        g_aiAccFixed[id] += *pFixed;
        g_aiAccMs[id]    += *pMs;
        if (g_aiAccFixed[id] < g_aiPeriodFixed) return 0;
        f = g_aiAccFixed[id]; m = g_aiAccMs[id];
        if (m <= 0) m = 1;
        if (f > 2720) f = 2720;                 /* same 166 ms clamp as the game */
        if (m > 166) m = 166;
        g_aiAccFixed[id] = 0; g_aiAccMs[id] = 0;
        g_tickObj = id; g_tickIsAI = 1; g_lastAITickObj = id;
        r = ((Tick_t)0x49D1D0)(msg, &f, &m, objId);
        g_tickObj = -1;
        return r;
    }
    if (msg == 0x104 && id < MAX_OBJ) g_aiInit[id] = 0;  /* object slot not an AI pilot (now) */
    g_tickObj = id; g_tickIsAI = 0;
    r = ((Tick_t)0x49D1D0)(msg, pFixed, pMs, objId);
    g_tickObj = -1;
    return r;
}

static void install_aitick(void)
{
    static const unsigned char site[] = { 0xB9, 0x04, 0x01, 0x00, 0x00, 0xE8, 0xCB, 0x00, 0x00, 0x00 };
    if (cfg_AIUpdateRate <= 0) return;
    if (cfg_AIUpdateRate > cfg_TargetFPS && cfg_TargetFPS > 0) return;
    g_aiPeriodFixed = 16384 / cfg_AIUpdateRate;
    patch_call(0x49D0FB, site, sizeof site, 5, (void *)Tick_Hook, "AI pilot tick rate");
    logf("  AI pilots ticked at %d/s (player every frame)", cfg_AIUpdateRate);
}

/* ---------------------------------------------------------------- crash logger
 * Writes saturn_synced_crash.log when the game hits an unhandled exception (crash to desktop):
 * exception code and address (module + offset), registers, a stack excerpt and what the mod was
 * doing at that moment. Also records up to 20 "first-chance" access violations etc. that happen
 * before a crash. Nothing is changed in the game's behaviour. */
static HMODULE g_self;
static TOPLEVEL_FILTER g_prevFilter;
static int g_fcCount;
static char g_crashPath[MAX_PATH];

static void clog(HANDLE f, const char *fmt, ...)
{
    char buf[600];
    DWORD w;
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    if (n > 0) { WriteFile(f, buf, (DWORD)n, &w, 0); WriteFile(f, "\r\n", 2, &w, 0); }
}

static HANDLE clog_open(void)
{
    HANDLE f = CreateFileA(g_crashPath, GENERIC_WRITE, FILE_SHARE_READ, 0, 4 /*OPEN_ALWAYS*/, FILE_ATTRIBUTE_NORMAL, 0);
    if (f != INVALID_HANDLE_VALUE) SetFilePointer(f, 0, 0, 2 /*FILE_END*/);
    return f;
}

static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return 0;
    if (mbi.State != 0x1000 /*MEM_COMMIT*/) return 0;
    if (mbi.Protect & (0x01 | 0x100)) return 0;          /* NOACCESS, GUARD */
    return (const char *)p + n <= (const char *)mbi.BaseAddress + mbi.RegionSize;
}

/* "HardwarW.exe+0x5F8D0" style description of an address */
static void addr_desc(DWORD a, char *out)
{
    HMODULE m = 0;
    char name[MAX_PATH];
    int i, j;
    out[0] = 0;
    if (GetModuleHandleExA(0x4 /*FROM_ADDRESS*/ | 0x2 /*UNCHANGED_REFCOUNT*/, (const char *)a, &m) && m &&
        GetModuleFileNameA(m, name, MAX_PATH)) {
        for (i = s_len(name); i > 0 && name[i - 1] != '\\'; i--) {}
        for (j = 0; name[i + j]; j++) out[j] = name[i + j];
        out[j] = 0;
        wsprintfA(out + j, "+0x%X", a - (DWORD)m);
    } else {
        wsprintfA(out, "?");
    }
}

static void write_report(HANDLE f, const char *kind, void *epv)
{
    DWORD *rec = ((DWORD **)epv)[0];
    unsigned char *ctx = ((unsigned char **)epv)[1];
    DWORD eip = *(DWORD *)(ctx + 0xB8), esp = *(DWORD *)(ctx + 0xC4);
    SYSTEMTIME st;
    char d[MAX_PATH + 32];
    int i, shown;

    GetLocalTime(&st);
    clog(f, "");
    clog(f, "=== %s  %04d-%02d-%02d %02d:%02d:%02d  (Saturn.Synced 1.3, frame %u) ===",
         kind, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, g_frameNo);
    addr_desc(rec[3], d);
    clog(f, "exception %08X at %08X  %s", rec[0], rec[3], d);
    if (rec[0] == 0xC0000005 && rec[4] >= 2)
        clog(f, "  %s address %08X", rec[5] == 0 ? "read from" : rec[5] == 1 ? "write to" : "execute at", rec[6]);
    clog(f, "eax %08X ebx %08X ecx %08X edx %08X esi %08X edi %08X",
         *(DWORD *)(ctx + 0xB0), *(DWORD *)(ctx + 0xA4), *(DWORD *)(ctx + 0xAC),
         *(DWORD *)(ctx + 0xA8), *(DWORD *)(ctx + 0xA0), *(DWORD *)(ctx + 0x9C));
    clog(f, "ebp %08X esp %08X eip %08X", *(DWORD *)(ctx + 0xB4), esp, eip);
    clog(f, "mod: tick obj %d (%s), last AI tick obj %d, dt %d ms / %d, AIUpdateRate %d, FixAITurning %d (limit %d)",
         g_tickObj, g_tickObj < 0 ? "none" : (g_tickIsAI ? "AI pilot" : "other"), g_lastAITickObj,
         G_DT_MS, G_DT_FIXED, cfg_AIUpdateRate, cfg_FixAITurning, g_tfLast16);
    /* return addresses on the stack that point into code */
    clog(f, "stack (code addresses only):");
    shown = 0;
    for (i = 0; i < 256 && shown < 24; i++) {
        DWORD *p = (DWORD *)esp + i, v;
        if (!readable(p, 4)) break;
        v = *p;
        if ((v >= 0x401000 && v < 0x4D2000) || (v >= (DWORD)g_self && v < (DWORD)g_self + 0x10000)) {
            addr_desc(v, d);
            clog(f, "  [esp+%03X] %08X  %s", i * 4, v, d);
            shown++;
        }
    }
}

static long __stdcall Crash_Filter(void *ep)
{
    HANDLE f = clog_open();
    if (f != INVALID_HANDLE_VALUE) { write_report(f, "CRASH", ep); CloseHandle(f); }
    logf("Game crashed - details written to saturn_synced_crash.log");
    if (g_prevFilter) return g_prevFilter(ep);
    return 0;  /* EXCEPTION_CONTINUE_SEARCH */
}

/* first-chance exceptions in the game's or this DLL's code of the kinds that usually precede a
   crash (max 20 per session) */

/* ---------------------------------------------------------------- crash guard
   Two access violations seen in the field, both in the game's own AI code reading a pointer to
   another entity that is no longer valid:
     0046CB54  (called from DoPcBuzzShip 00443BB0) - the pilot being "buzzed" is gone.
               Recovery: continue at 0046CDB1, the game's own "target not in the world" exit,
               which ends that AI task normally.
     0040E700  (building -> crater getter, 12 callers) - invalid building pointer.
               Recovery: return 0 to the caller. When the caller is the AI routine at 00468349
               (cached destination building), clear that cache and let it pick a new building
               (continue at 00468323, the game's own "no building cached yet" path). */
static int g_guardCount;

static int crash_guard(void *ep)
{
    DWORD *rec = ((DWORD **)ep)[0];
    unsigned char *ctx = ((unsigned char **)ep)[1];
    DWORD *pEip = (DWORD *)(ctx + 0xB8), *pEsp = (DWORD *)(ctx + 0xC4), *pEax = (DWORD *)(ctx + 0xB0);
    DWORD at = rec[3], ret = 0, bad = rec[4] >= 2 ? rec[6] : 0;
    const char *what;
    HANDLE f;

    if (!cfg_CrashGuard || rec[0] != 0xC0000005) return 0;
    if (at == 0x46CB54) {
        *pEip = 0x46CDB1;
        what = "AI 'buzz ship' target pilot invalid -> task ended";
    } else if (at == 0x463C40) {                     /* moth -> building getter: mov eax,[ecx+1D0h] ; ret */
        if (!readable((void *)*pEsp, 4)) return 0;
        ret = *(DWORD *)*pEsp;
        if (ret < 0x401000 || ret >= 0x4D2000) return 0;
        *pEax = 0; *pEsp += 4; *pEip = ret;
        what = "invalid moth pointer -> returned 0";
    } else if (at == 0x40E700) {
        if (!readable((void *)*pEsp, 4)) return 0;
        ret = *(DWORD *)*pEsp;
        if (ret < 0x401000 || ret >= 0x4D2000) return 0;
        *pEax = 0;
        if (ret == 0x46834E && readable((void *)*(DWORD *)(ctx + 0x9C), 4)) {
            *(DWORD *)*(DWORD *)(ctx + 0x9C) = 0;          /* [edi] = cached building */
            *pEsp += 4;
            *pEip = 0x468323;
            what = "AI cached building invalid -> new building picked";
        } else {
            *pEsp += 4;
            *pEip = ret;
            what = "invalid building pointer -> returned 0";
        }
    } else {
        return 0;
    }
    g_guardCount++;
    if (g_guardCount <= 50 && (f = clog_open()) != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        clog(f, "[guard] %04d-%02d-%02d %02d:%02d:%02d frame %u: AV at %08X (read %08X, caller %08X, ecx %08X edx %08X ebx %08X) - %s | last AI tick obj %d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, g_frameNo, at, bad, ret,
             *(DWORD *)(ctx + 0xAC), *(DWORD *)(ctx + 0xA8), *(DWORD *)(ctx + 0xA4), what, g_lastAITickObj);
        CloseHandle(f);
    }
    if (g_guardCount <= 50) logf("crash guard: recovered from a game AI error at %08X (see saturn_synced_crash.log)", at);
    return 1;
}

static long __stdcall Crash_Vectored(void *ep)
{
    if (crash_guard(ep)) return -1;  /* EXCEPTION_CONTINUE_EXECUTION */
    DWORD code = ((DWORD **)ep)[0][0], at = ((DWORD **)ep)[0][3];
    int inGame = (at >= 0x401000 && at < 0x4D2000) || (at >= (DWORD)g_self && at < (DWORD)g_self + 0x10000);
    if (cfg_CrashLog && inGame && (code == 0xC0000005 || code == 0xC000001D || code == 0xC0000094 || code == 0xC00000FD ||
         code == 0xC0000096) && g_fcCount < 20) {
        HANDLE f;
        g_fcCount++;
        f = clog_open();
        if (f != INVALID_HANDLE_VALUE) { write_report(f, "first-chance exception", ep); CloseHandle(f); }
    }
    return 0;  /* EXCEPTION_CONTINUE_SEARCH */
}

static void crash_reassert(void)
{
    TOPLEVEL_FILTER cur;
    if (!cfg_CrashLog) return;
    cur = SetUnhandledExceptionFilter(Crash_Filter);
    if (cur && cur != Crash_Filter) g_prevFilter = cur;
}

static void install_crashlog(void)
{
    if (!cfg_CrashLog && !cfg_CrashGuard) return;
    g_crashPath[0] = 0; s_cat(g_crashPath, g_dir); s_cat(g_crashPath, "saturn_synced_crash.log");
    crash_reassert();
    AddVectoredExceptionHandler(1, Crash_Vectored);
    logf("  crash logger: saturn_synced_crash.log%s", cfg_CrashGuard ? " | crash guard on" : "");
}


/* ---------------------------------------------------------------- safe delete
 * Hardwar keeps raw pointers to moths (vehicles, 0x448-byte records, list 0x52DEC8) and pilots
 * (0x37C-byte records, list 0x52C998) in many places, e.g. in the AI "plan" stack of every pilot
 * (15 entries of 0x18 bytes at pilot+0x12C: type, then parameters). When a moth is deleted
 * (VhDelete 0x4637F0: trade-in, death, ...) the game clears references in only a few plan types
 * (3 and 0x11, in PilVhDeleted 0x453EE0); others, such as "buzz ship" (0x1C) and "rescue moth"
 * (0x25), keep pointing at freed memory. Reading it later crashes the game (seen at 0046CB54) or
 * silently reads garbage. The same holds for deleted pilots (PilDelete 0x451120).
 * Fix: the memory of a deleted moth or pilot is not given back to the heap but kept as an inert
 * record. A deleted moth is marked "not in the world" (object id 0x7FFF), which every AI routine
 * already handles as "target gone", so stale references end their plan normally instead of
 * crashing. The records are small; the oldest are released after MAX_ZOMBIES deletions. */
static int g_setDirty = 1;
#define MAX_ZOMBIES 4096
typedef void (__fastcall *GameFree_t)(void *);
static void *g_zombie[MAX_ZOMBIES];
static int g_zombieNext, g_zombieVh, g_zombiePil;

static void zombie_keep(void *p)
{
    void *old = g_zombie[g_zombieNext];
    g_zombie[g_zombieNext] = p;
    g_zombieNext = (g_zombieNext + 1) % MAX_ZOMBIES;
    if (old) ((GameFree_t)0x47AA80)(old);
}

static void __fastcall Vh_Free_Hook(unsigned char *vh)
{
    if (!vh) return;
    *(unsigned short *)(vh + 0x1D4) = 0x7FFF;   /* object id: not in the world */
    *(unsigned short *)(vh + 0x1F8) = 0x7FFF;
    *(unsigned short *)(vh + 0x444) = 0x7FFF;
    *(DWORD *)(vh + 0x2E8) = 0;                 /* no longer in the moth list */
    zombie_keep(vh);
    g_setDirty = 1;
    g_zombieVh++;
    if (cfg_Verbose) logf("[safe-delete] moth record %08X kept as inert record (%d moths, %d pilots so far)", (DWORD)vh, g_zombieVh, g_zombiePil);
}

static void __fastcall Pil_Free_Hook(unsigned char *pil)
{
    if (!pil) return;
    *(DWORD *)(pil + 0x334) = 0;                /* no longer in the pilot list */
    zombie_keep(pil);
    g_setDirty = 1;
    g_zombiePil++;
    if (cfg_Verbose) logf("[safe-delete] pilot record %08X kept as inert record (%d moths, %d pilots so far)", (DWORD)pil, g_zombieVh, g_zombiePil);
}

static void install_safedelete(void)
{
    /* VhDelete tail: mov ecx,esi ; pop edi ; pop esi ; pop ebp ; jmp 0047AA80 (free) */
    static const unsigned char vhTail[]  = { 0x8B, 0xCE, 0x5F, 0x5E, 0x5D, 0xE9, 0x62, 0x71, 0x01, 0x00 };
    /* PilDelete: mov ecx,ebx ; call 0047AA80 (free) */
    static const unsigned char pilFree[] = { 0x8B, 0xCB, 0xE8, 0xA1, 0x98, 0x02, 0x00 };
    if (!cfg_SafeDelete) return;
    patch_rel(0x463914, vhTail, sizeof vhTail, 5, 0xE9, (void *)Vh_Free_Hook, "safe delete (moths)");
    patch_rel(0x4511D8, pilFree, sizeof pilFree, 2, 0xE8, (void *)Pil_Free_Hook, "safe delete (pilots)");
}


/* ---------------------------------------------------------------- precise movement
 * Every frame the game moves every object that has a speed (0x4A3650 -> 0x49D3C0):
 *     step = (speed * dt) >> 14 ;  pos[k] += (step * dir[k]) >> 14      (dir = obj+0x5C, unit 16384)
 * Both shifts round down and the remainders are thrown away. At 37 ms frames (dt 606) that loses
 * little, but at high frame rates the per-frame step gets small: at 90 fps (dt ~182) an object
 * slower than 90 units/s does not move at all, and slow movement in a negative direction is
 * exaggerated. AI moths creeping into a hangar at low speed then hang still in front of the door.
 * Fix: replace 0x49D3C0 with the same calculation in one exact step, keeping the leftover fraction
 * per object and axis for the next frame, so objects travel exactly speed * time at any frame
 * rate. The fraction is always smaller than one unit, so a reused object slot or a teleport is
 * harmless. */
static int g_mvRem[MAX_OBJ][3];

static void __fastcall Move_Predict(int objId, int *outX, int *outY, int *outZ)
{
    unsigned id = (unsigned)objId & 0xFFFF;
    unsigned char *o = ((unsigned char **)0x5452E0)[id];
    int *pos = (int *)(o + 0x10);
    int speed = *(int *)(o + 0x28);
    short *dir = (short *)(o + 0x5C);
    int *out[3];
    long long sd;
    int k;
    out[0] = outX; out[1] = outY; out[2] = outZ;
    if (!speed) { *outX = pos[0]; *outY = pos[1]; *outZ = pos[2]; return; }
    sd = (long long)speed * (long long)G_DT_FIXED;
    for (k = 0; k < 3; k++) {
        long long n = sd * (long long)dir[k], d;
        if (id < MAX_OBJ) n += g_mvRem[id][k];
        d = n >> 28;                                  /* floor */
        if (id < MAX_OBJ) g_mvRem[id][k] = (int)(n - (d << 28));
        *out[k] = pos[k] + (int)d;
    }
}

static void install_movement(void)
{
    /* 0049D3C0: and ecx,0FFFFh ; push ebx ; push esi */
    static const unsigned char head[] = { 0x81, 0xE1, 0xFF, 0xFF, 0x00, 0x00, 0x53, 0x56 };
    if (!cfg_PreciseMovement) return;
    patch_rel(0x49D3C0, head, sizeof head, 0, 0xE9, (void *)Move_Predict, "precise movement");
}


/* ---------------------------------------------------------------- plan check
 * Every pilot has a stack of up to 15 AI "plans" (pilot+0x12C, 0x18 bytes each: type, params).
 * Several plan types keep a raw pointer to a moth, pilot or building as parameter; the game's own
 * savegame code (0x454330) tells which: moth at param 0 for types 3, 0x11, 0x1C, 0x25; pilot at
 * param 0 for 7, 0xA; building at param 0 for 2, 0x10, 0x18-0x1A, 0x20, 0x21, 0x23, 0x79, 0x7A,
 * 0x80 and at param 4 (+0x10) for 8, 9, 0xB, 0xC, 0x13. The game only cleans up a few of these
 * when the moth or pilot disappears, so a plan can keep pointing at something that no longer
 * exists (crashes seen at 0046CB54 and 00463C40).
 * Fix: right before the game runs a pilot's current plan (0x446F40, called from 18 places in the
 * plan manager), check that the plan's moth/pilot/building is in the game's own list of existing
 * moths (0x52DEC8), pilots (0x52C998) or buildings (0x514D08). If not, the plan is removed with
 * the game's own function (0x451B10), exactly as the game does when a target disappears. */
#define SET_SLOTS 8192
static unsigned g_setPtr[SET_SLOTS];
static unsigned char g_setKind[SET_SLOTS];
static int g_planLogged;

static unsigned set_hash(unsigned p) { return ((p >> 3) * 2654435761u) >> 19; }   /* 13 bits */

static void set_add(unsigned p, unsigned char kind)
{
    unsigned h = set_hash(p), n;
    for (n = 0; n < SET_SLOTS; n++, h = (h + 1) & (SET_SLOTS - 1)) {
        if (!g_setPtr[h]) { g_setPtr[h] = p; g_setKind[h] = kind; return; }
        if (g_setPtr[h] == p) return;
    }
}

static int set_has(unsigned p, unsigned char kind)
{
    unsigned h = set_hash(p), n;
    for (n = 0; n < SET_SLOTS; n++, h = (h + 1) & (SET_SLOTS - 1)) {
        if (!g_setPtr[h]) return 0;
        if (g_setPtr[h] == p) return g_setKind[h] == kind;
    }
    return 0;
}

static void set_add_list(unsigned head, int next, unsigned char kind)
{
    unsigned p = head;
    int n = 0;
    while (p && n++ < 2500) {
        set_add(p, kind);
        p = *(unsigned *)(p + next);
    }
}

static void set_rebuild(void)
{
    memset(g_setPtr, 0, sizeof g_setPtr);
    set_add_list(*(unsigned *)0x52DEC8, 0x2E8, 1);   /* moths */
    set_add_list(*(unsigned *)0x52C998, 0x334, 2);   /* pilots */
    set_add_list(*(unsigned *)0x514D08, 0x938, 3);   /* buildings */
    g_setDirty = 0;
}

static int exists(unsigned p, unsigned char kind)
{
    if (g_setDirty) set_rebuild();
    if (set_has(p, kind)) return 1;
    set_rebuild();                                    /* maybe created since the last rebuild */
    return set_has(p, kind);
}

/* kind of entity referenced by a plan type (1 moth, 2 pilot, 3 building) and its offset */
static int plan_ref(int type, int *off)
{
    *off = 0;
    switch (type) {
    case 0x03: case 0x11: case 0x1C: case 0x25: return 1;
    case 0x07: case 0x0A: return 2;
    case 0x02: case 0x10: case 0x18: case 0x19: case 0x1A: case 0x20: case 0x21: case 0x23:
    case 0x79: case 0x7A: case 0x80: return 3;
    case 0x08: case 0x09: case 0x0B: case 0x0C: case 0x13: *off = 0x10; return 3;
    }
    return 0;
}

typedef void (__fastcall *PlanUpd_t)(void *, int);
typedef const char *(__fastcall *PilName_t)(void *);

static void plan_validate(unsigned char *pil)
{
    static const char *kindName[] = { "", "moth", "pilot", "building" };
    int i;
    for (i = 0; i < 15; i++) {
        int type = *(int *)(pil + 0x12C), off, kind;
        unsigned ref;
        kind = plan_ref(type, &off);
        if (!kind) return;
        ref = *(unsigned *)(pil + 0x130 + off);
        if (!ref || exists(ref, (unsigned char)kind)) return;
        if (g_planLogged < 100) {
            g_planLogged++;
            logf("[plan-check] pilot \"%s\": plan %X refers to a %s that does not exist (%08X) - plan removed",
                 ((PilName_t)0x451460)(pil), type, kindName[kind], ref);
        }
        ((PlanUpd_t)0x451B10)(pil, 0);                /* remove plan 0, like the game does */
        if (*(int *)(pil + 0x12C) == type && *(unsigned *)(pil + 0x130 + off) == ref) return;
    }
}

static void __fastcall Plan_Update_Hook(unsigned char *pil, int edx)
{
    if (pil) plan_validate(pil);
    ((PlanUpd_t)0x446F40)(pil, edx);
}

static void install_plancheck(void)
{
    static const DWORD sites[] = {
        0x452901, 0x452998, 0x452A18, 0x452A6E, 0x452A93, 0x452AE6, 0x452B17, 0x452C2D, 0x452C58,
        0x452C90, 0x452CD4, 0x452D10, 0x4533C4, 0x4533FF, 0x453560, 0x453578, 0x453610, 0x4536B7 };
    int i, ok = 0;
    if (!cfg_PlanCheck) return;
    for (i = 0; i < (int)(sizeof sites / sizeof sites[0]); i++) {
        unsigned char *c = (unsigned char *)sites[i];
        unsigned char expect[7];
        int rel = 0x446F40 - (int)(sites[i] + 5);
        expect[0] = 0x8B; expect[1] = 0xCE; expect[2] = 0xE8;          /* mov ecx,esi ; call 446F40 */
        *(int *)(expect + 3) = rel;
        if (!m_eq(c - 2, expect, 7)) continue;
        {
            DWORD old;
            if (!VirtualProtect(c, 5, PAGE_EXECUTE_READWRITE, &old)) continue;
            *(int *)(c + 1) = (int)((unsigned char *)Plan_Update_Hook - (c + 5));
            VirtualProtect(c, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), c, 5);
            ok++;
        }
    }
    logf("  plan check: %d of %d call sites patched", ok, (int)(sizeof sites / sizeof sites[0]));
}

static void install(void)
{
    static const unsigned char limiterHead[] = { 0xA1,0x48,0x17,0x54,0x00,0x55,0x56,0x57,0x8B,0xF9,0x33,0xED };
    /* mov ecx,25h ; call 48D100 ; jmp short */
    static const unsigned char spSite[]  = { 0xB9,0x25,0x00,0x00,0x00,0xE8,0x3C,0x46,0x06,0x00,0xEB,0x16 };
    /* mov ecx,32h ; call 48D100   (net game, server flag set) */
    static const unsigned char netA[]    = { 0xB9,0x32,0x00,0x00,0x00,0xE8,0x58,0x46,0x06,0x00,0xEB,0x32 };
    /* mov ecx,32h ; call 48D100   (net game) */
    static const unsigned char netB[]    = { 0xB9,0x32,0x00,0x00,0x00,0xE8,0x4A,0x46,0x06,0x00,0xEB,0x24 };
    LARGE_INTEGER f;
    char exe[MAX_PATH];

    GetModuleFileNameA(0, exe, MAX_PATH);
    logf("Saturn.Synced 1.3 - host: %s", exe);
    if (!in_main_image(ADDR_LIMITER, sizeof limiterHead) ||
        !m_eq((void *)ADDR_LIMITER, limiterHead, sizeof limiterHead)) {
        logf("Frame limiter not found at %08X - this is not the expected HardwarW.exe. Nothing patched.", ADDR_LIMITER);
        return;
    }

    QueryPerformanceFrequency(&f);
    g_tick_per_ms = (double)f.QuadPart / 1000.0;
    timeBeginPeriod(1);

    logf("TargetFPS=%d NetGameFPS=%d SpinMs=%d (QPC %d Hz)", cfg_TargetFPS, cfg_NetGameFPS, cfg_SpinMs, (int)f.QuadPart);
    if (cfg_TargetFPS != 27)
    {
        patch_call(0x428ABA, spSite, sizeof spSite, 5, (void *)Limiter_SinglePlayer, "single-player limiter (37 ms)");
        if (cfg_FixAITurning)
            logf("  AI turning fix @0045FA00: %s", install_turnfix() ? "patched" : "bytes do not match - NOT patched");
    }
    if (cfg_TargetFPS != 27) install_aitick();
    install_safedelete();
    install_plancheck();
    if (cfg_TargetFPS != 27) install_movement();
    install_crashlog();
    if (cfg_NetGameFPS > 0) {
        patch_call(0x428A9E, netA, sizeof netA, 5, (void *)Limiter_NetGame, "net-game limiter A (50 ms)");
        patch_call(0x428AAC, netB, sizeof netB, 5, (void *)Limiter_NetGame, "net-game limiter B (50 ms)");
    }
}

/* ---------------------------------------------------------------- config / init */
static void load_config(void)
{
    char ini[MAX_PATH], logp[MAX_PATH];
    int i;
    GetModuleFileNameA(0, g_dir, MAX_PATH);
    for (i = s_len(g_dir); i > 0 && g_dir[i - 1] != '\\' && g_dir[i - 1] != '/'; i--) {}
    g_dir[i] = 0;

    ini[0] = 0; s_cat(ini, g_dir); s_cat(ini, "saturn_synced.ini");
    cfg_TargetFPS  = GetPrivateProfileIntA("FPS", "TargetFPS", 60, ini);
    cfg_NetGameFPS = GetPrivateProfileIntA("FPS", "NetGameFPS", 0, ini);
    cfg_SpinMs     = GetPrivateProfileIntA("FPS", "SpinMs", 2, ini);
    cfg_Log        = GetPrivateProfileIntA("FPS", "Log", 1, ini);
    cfg_Verbose    = GetPrivateProfileIntA("FPS", "Verbose", 0, ini);
    cfg_FixAITurning = GetPrivateProfileIntA("FPS", "FixAITurning", 1, ini);
    cfg_AIUpdateRate = GetPrivateProfileIntA("AI", "AIUpdateRate", 25, ini);
    cfg_CrashLog     = GetPrivateProfileIntA("Debug", "CrashLog", 1, ini);
    cfg_CrashGuard   = GetPrivateProfileIntA("Debug", "CrashGuard", 1, ini);
    cfg_SafeDelete   = GetPrivateProfileIntA("Stability", "SafeDelete", 1, ini);
    cfg_PreciseMovement = GetPrivateProfileIntA("FPS", "PreciseMovement", 0, ini);
    cfg_PlanCheck    = GetPrivateProfileIntA("Stability", "PlanCheck", 1, ini);
    if (cfg_TargetFPS < 0) cfg_TargetFPS = 0;
    if (cfg_TargetFPS > 1000) cfg_TargetFPS = 1000;
    if (cfg_NetGameFPS < 0) cfg_NetGameFPS = 0;
    if (cfg_NetGameFPS > 1000) cfg_NetGameFPS = 1000;
    if (cfg_SpinMs < 0) cfg_SpinMs = 0;

    if (cfg_Log) {
        logp[0] = 0; s_cat(logp, g_dir); s_cat(logp, "saturn_synced.log");
        g_log = CreateFileA(logp, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    }
}

/* ---------------------------------------------------------------- dinput8 proxy */
static HMODULE g_real;
static FARPROC p_DirectInput8Create, p_DllCanUnloadNow, p_DllGetClassObject, p_DllRegisterServer, p_DllUnregisterServer;

static void load_real(void)
{
    char path[MAX_PATH], ini[MAX_PATH];
    if (g_real) return;
    ini[0] = 0; s_cat(ini, g_dir); s_cat(ini, "saturn_synced.ini");
    /* optional: chain to another dinput8 wrapper (e.g. renamed to dinput8_chain.dll) */
    GetPrivateProfileStringA("Proxy", "ChainDll", "", path, MAX_PATH, ini);
    if (path[0]) {
        char full[MAX_PATH];
        full[0] = 0;
        if (!(path[1] == ':' || path[0] == '\\')) s_cat(full, g_dir);
        s_cat(full, path);
        g_real = LoadLibraryA(full);
        logf("Chain-loading %s: %s", full, g_real ? "ok" : "FAILED");
    }
    if (!g_real) {
        GetSystemDirectoryA(path, MAX_PATH);
        s_cat(path, "\\dinput8.dll");
        g_real = LoadLibraryA(path);
        if (!g_real) logf("Could not load system %s", path);
    }
    if (g_real) {
        p_DirectInput8Create  = GetProcAddress(g_real, "DirectInput8Create");
        p_DllCanUnloadNow     = GetProcAddress(g_real, "DllCanUnloadNow");
        p_DllGetClassObject   = GetProcAddress(g_real, "DllGetClassObject");
        p_DllRegisterServer   = GetProcAddress(g_real, "DllRegisterServer");
        p_DllUnregisterServer = GetProcAddress(g_real, "DllUnregisterServer");
    }
}

typedef HRESULT (__stdcall *DI8Create_t)(HINSTANCE, DWORD, const void *, void **, void *);
HRESULT __stdcall DirectInput8Create(HINSTANCE hinst, DWORD ver, const void *riid, void **out, void *outer)
{
    load_real();
    if (!p_DirectInput8Create) return E_FAIL;
    return ((DI8Create_t)p_DirectInput8Create)(hinst, ver, riid, out, outer);
}
HRESULT __stdcall DllCanUnloadNow(void)
{ load_real(); return p_DllCanUnloadNow ? ((HRESULT(__stdcall *)(void))p_DllCanUnloadNow)() : S_FALSE; }
HRESULT __stdcall DllGetClassObject(const void *a, const void *b, void **c)
{ load_real(); return p_DllGetClassObject ? ((HRESULT(__stdcall *)(const void *, const void *, void **))p_DllGetClassObject)(a, b, c) : E_FAIL; }
HRESULT __stdcall DllRegisterServer(void)
{ load_real(); return p_DllRegisterServer ? ((HRESULT(__stdcall *)(void))p_DllRegisterServer)() : E_FAIL; }
HRESULT __stdcall DllUnregisterServer(void)
{ load_real(); return p_DllUnregisterServer ? ((HRESULT(__stdcall *)(void))p_DllUnregisterServer)() : E_FAIL; }

BOOL __stdcall DllMain(HINSTANCE h, DWORD reason, void *reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        g_self = h;
        load_config();
        install();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_tick_per_ms > 0.0) timeEndPeriod(1);
        if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    }
    return TRUE;
}
