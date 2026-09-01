/*
 * libScePad.dll shim: log every call, and DRIVE ADAPTIVE TRIGGERS.
 *
 * Stray's PC build implements adaptive triggers but never asks for them: the
 * dispatcher (RVA 0x9FC470) only reaches scePadSetTriggerEffect when something sets
 * a device property named "PS5TriggerEffect", and no shipped asset ever does.
 * So we drive it ourselves.
 *
 * Signal: the game already streams real haptics through scePadSetVibration at 60Hz,
 * fed from its m_PS5VibrationSubmix. Sustained vibration means a haptic event is
 * playing (scratching, landing, a Zurk grab), which is exactly when the PS5 build
 * engages the triggers. Hysteresis keeps it from flapping at 60Hz, and we only ever
 * transmit on a STATE CHANGE - resending identical trigger params every frame causes
 * audible buzzing and latency.
 *
 * Param layout is from the game's own construction site, verified by the library
 * accepting it (0x00000000) where four guessed strides returned 0x80920001:
 *   +0x00 triggerMask   +0x08 cmd0.mode  +0x10.. cmd0 data
 *                       +0x40 cmd1.mode  +0x48.. cmd1 data   (command stride 0x38)
 * Mode 1 = FEEDBACK{position, strength}, cross-checked against the game's dispatch.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

static HMODULE g_real;
static FILE *g_log;
static CRITICAL_SECTION g_cs;
static int g_ready;
static char g_stateFile[MAX_PATH];
static char g_vibeCmd[MAX_PATH];
static char g_vibeDir[MAX_PATH];
static volatile int g_vibModeOverride = -1;   /* -1 = pass the game's own mode through */
static volatile unsigned long long g_lastPadHandle;
static DWORD WINAPI audio_endpoints(LPVOID);   /* defined below; started from scePadOpen */
static DWORD WINAPI spk_thread(LPVOID);
static DWORD WINAPI spk_watch(LPVOID);
static char  g_spkDir[MAX_PATH];
static char  g_spkCmd[MAX_PATH];
static char  g_spkReq[96];
static volatile long  g_spkSeq, g_spkCur;
static volatile int   g_spkStop, g_spkLoop;
static volatile float g_spkLevel = 1.0f;
static volatile float g_spkBoost = 1.7783f;   /* SBFX_Boost InputGainDb=+5.0 dB */
enum { kSideLeft = 0, kSideRight = 1 };  /* EPS5TriggersSide, read from the exe's enum strings */
enum { kMaxPlaySamples = 12000 };        /* ~60 s at 5 ms/sample: runaway cap if Stop is missed */
static volatile int  g_playStop;         /* set to 1 to cut the current envelope short */
static volatile int  g_playLevel = 255;  /* per-sound level from the game, 0..255 */
static volatile int  g_gain      = 255;   /* master gain 0..255; raw envelopes felt too strong */
/* Request slot. The watcher thread ONLY writes here and never blocks; the playback
   thread drains it. Keeping them separate is what makes `stop` land immediately —
   v11 played inline on the watcher, so a looping sound deafened it to every command. */
static char          g_reqName[96];
static volatile int  g_reqLoop;
static volatile long g_reqSeq;           /* bumped per request */
static volatile long g_curSeq;           /* what the playback thread has picked up */

/* tuning */
static int  g_enabled   = 0;  /* vibration heuristic: opt-in only now */
static int  g_threshold = 0x20;   /* vibration amplitude counted as "active" */
static int  g_onFrames  = 2;      /* consecutive active frames before engaging */
static int  g_offFrames = 24;     /* consecutive quiet frames before releasing */
static int  g_position  = 0;  /* game-authored: position 0 */      /* trigger travel where resistance starts (0..9) */
static int  g_strength  = 2;  /* game-authored: strength 2 of 8 */      /* resistance (0..8) */

static void log_open(void)
{
	char p[MAX_PATH];
	HMODULE self = NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&log_open, &self);
	if (!GetModuleFileNameA(self, p, sizeof p)) return;
	char *slash = strrchr(p, '\\');
	if (slash) slash[1] = 0;
	char real[MAX_PATH], logp[MAX_PATH];
	snprintf(real, sizeof real, "%slibScePad_orig.dll", p);
	snprintf(logp, sizeof logp, "%sscepad_shim.log", p);
	snprintf(g_stateFile, sizeof g_stateFile, "%sstray_trigger.state", p);
	snprintf(g_vibeCmd, sizeof g_vibeCmd, "%sstray_vibe.cmd", p);
	snprintf(g_vibeDir, sizeof g_vibeDir, "%svibe\\", p);
	snprintf(g_spkDir, sizeof g_spkDir, "%sspk\\", p);
	snprintf(g_spkCmd, sizeof g_spkCmd, "%sstray_spk.cmd", p);
	g_log = fopen(logp, "w");
	g_real = LoadLibraryA(real);
	char *e;
	if ((e = getenv("STRAY_TRIGGERS"))    ) g_enabled   = atoi(e);
	if ((e = getenv("STRAY_TRIG_THRESH")) ) g_threshold = atoi(e);
	if ((e = getenv("STRAY_TRIG_STR"))    ) g_strength  = atoi(e);
	if ((e = getenv("STRAY_TRIG_POS"))    ) g_position  = atoi(e);
	if (g_log) {
		fprintf(g_log, "shim v3 attached. real=%p enabled=%d thresh=%d pos=%d str=%d\n",
			(void *)g_real, g_enabled, g_threshold, g_position, g_strength);
		fflush(g_log);
	}
	g_ready = 1;
}

static void LG(const char *fmt, ...)
{
	EnterCriticalSection(&g_cs);
	if (!g_ready) log_open();
	if (g_log) {
		va_list ap; va_start(ap, fmt);
		fprintf(g_log, "[%8lu] ", GetTickCount());
		vfprintf(g_log, fmt, ap);
		fputc('\n', g_log);
		va_end(ap);
		fflush(g_log);
	}
	LeaveCriticalSection(&g_cs);
}

typedef long long (*genfn)(unsigned long long, unsigned long long,
	unsigned long long, unsigned long long);

static genfn resolve(const char *n)
{
	EnterCriticalSection(&g_cs);
	if (!g_ready) log_open();
	LeaveCriticalSection(&g_cs);
	return g_real ? (genfn)(void *)GetProcAddress(g_real, n) : NULL;
}

/* ---- the trigger driver ---------------------------------------------------- */
static genfn g_setTrigger;
static int   g_trigOn = -1;          /* -1 = never set yet */
static int   g_activeRun, g_quietRun;
static unsigned long g_engaged, g_released, g_failed;

static void set_triggers(unsigned long long handle, int onL, int onR)
{
	if (!g_setTrigger) g_setTrigger = resolve("scePadSetTriggerEffect");
	if (!g_setTrigger) return;
	unsigned char P[256];
	memset(P, 0, sizeof P);
	const int wantL = onL, wantR = onR;
	const int on = onL || onR;
	P[0x00] = 0x03;                                  /* always address L2|R2 */
	*(uint32_t *)(P + 0x08) = wantL ? 1u : 0u;       /* 1 = FEEDBACK (EPS5TriggerEffectMode) */
	P[0x10] = (unsigned char)g_position; P[0x11] = (unsigned char)g_strength;
	*(uint32_t *)(P + 0x40) = wantR ? 1u : 0u;
	P[0x48] = (unsigned char)g_position; P[0x49] = (unsigned char)g_strength;
	long long r = g_setTrigger(handle, (unsigned long long)(uintptr_t)P, 0, 0);
	if (r == 0) { if (on) g_engaged++; else g_released++; }
	else g_failed++;
	LG("TRIGGERS %-8s L2=%d R2=%d pos=%d str=%d -> 0x%08X   (engaged=%lu released=%lu failed=%lu)",
		on ? "ENGAGE" : "release", wantL, wantR, g_position, g_strength, (unsigned)r,
		g_engaged, g_released, g_failed);

	/* hardware readback */
	static genfn getState;
	if (!getState) getState = resolve("scePadGetTriggerEffectState");
	if (getState) {
		uint32_t st[8]; memset(st, 0, sizeof st);
		Sleep(120);   /* let the output report reach the pad and an input report come back */
		long long gr = getState(handle, (unsigned long long)(uintptr_t)st, 0, 0);
		LG("   readback after %-7s -> ret=0x%08X  L2state=%u R2state=%u",
			on ? "ENGAGE" : "release", (unsigned)gr, st[0], st[1]);
	}
}

static unsigned long long g_demoHandle;
static int g_demo;

static DWORD WINAPI demo_thread(LPVOID unused)
{
    (void)unused;
    LG("DEMO thread running: triggers will alternate stiff/off every 4s");
    for (;;) {
        set_triggers(g_demoHandle, 1, 1);
        Sleep(4000);
        set_triggers(g_demoHandle, 0, 0);
        Sleep(4000);
    }
    return 0;
}

static DWORD WINAPI state_thread(LPVOID unused)
{
    (void)unused;
    int last = -1;
    LG("STATE thread watching %s", g_stateFile);
    for (;;) {
        Sleep(80);
        FILE *f = fopen(g_stateFile, "r");
        if (!f) continue;
        int wantL = 0, wantR = 0;
        if (fscanf(f, "%d %d", &wantL, &wantR) < 1) { wantL = 0; wantR = 0; }
        fclose(f);
        const int cur = (wantL ? 1 : 0) | (wantR ? 2 : 0);
        if (cur != last) {
            last = cur;
            LG("STATE file -> L=%d R=%d", wantL, wantR);
            set_triggers(g_demoHandle, wantL, wantR);
        }
    }
    return 0;
}

/* Drive scePadSetVibration directly. ScePadVibrationParam is {uint8 large, uint8 small}. */
static void set_rumble(unsigned long long h, int large, int small)
{
    static genfn f;
    if (!f) f = resolve("scePadSetVibration");
    if (!f) return;
    unsigned char p[2];
    p[0] = (unsigned char)(large < 0 ? 0 : (large > 255 ? 255 : large));
    p[1] = (unsigned char)(small < 0 ? 0 : (small > 255 ? 255 : small));
    f(h, (unsigned long long)(uintptr_t)p, 0, 0);
}

static void play_envelope(unsigned long long h, const char *name, int loop)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s%s.env", g_vibeDir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { LG("VIBE envelope not found: %s", path); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); LG("VIBE empty envelope %s", name); return; }
    unsigned char *env = (unsigned char *)malloc((size_t)n);
    if (!env) { fclose(f); return; }
    fread(env, 1, (size_t)n, f);
    fclose(f);
    LG("VIBE play %s (%ld samples, %.2fs, loop=%d, gain=%d)", name, n, n * 0.005, loop, g_gain);
    long seq = g_curSeq;                  /* a newer request supersedes this one */
    long guard = 0;                       /* runaway cap: a missed Stop must not buzz forever */
    do {
        for (long i = 0; i < n; i++) {
            if (g_playStop || g_reqSeq != seq || guard > kMaxPlaySamples) goto done;
            int a = (env[i] * g_playLevel * g_gain + 32512) / 65025;   /* rounded, not truncated */
            set_rumble(h, a, a);
            Sleep(5);                     /* 5 ms per sample = the envelope rate */
            guard++;
        }
    } while (loop);
done:
    free(env);
    set_rumble(h, 0, 0);
    LG("VIBE play %s ended (stop=%d superseded=%d capped=%d)",
       name, g_playStop, g_reqSeq != seq, guard > kMaxPlaySamples);
}

static DWORD WINAPI play_thread(LPVOID handle_v)
{
    unsigned long long h = (unsigned long long)(uintptr_t)handle_v;
    for (;;) {
        if (g_reqSeq == g_curSeq) { Sleep(5); continue; }
        g_curSeq = g_reqSeq;
        if (g_playStop) continue;          /* the pending request was a stop */
        char name[96];
        snprintf(name, sizeof name, "%s", g_reqName);
        play_envelope(h, name, g_reqLoop);
    }
    return 0;
}

static DWORD WINAPI vibe_thread(LPVOID handle_v)
{
    unsigned long long h = (unsigned long long)(uintptr_t)handle_v;
    LG("VIBE thread watching %s", g_vibeCmd);
    for (;;) {
        Sleep(100);
        FILE *f = fopen(g_vibeCmd, "r");
        if (!f) continue;
        char cmd[64] = {0}, arg[96] = {0};
        int a = 0, b = 0;
        int got = fscanf(f, "%63s %95s %d %d", cmd, arg, &a, &b);
        fclose(f);
        remove(g_vibeCmd);                 /* one-shot */
        if (got < 1) continue;
        if (!strcmp(cmd, "const")) {
            a = atoi(arg); LG("VIBE const large=%d small=%d for 3s", a, a);
            for (int i = 0; i < 30; i++) { set_rumble(h, a, a); Sleep(100); }
            set_rumble(h, 0, 0);
            LG("VIBE const done");
        } else if (!strcmp(cmd, "ramp")) {
            LG("VIBE ramp 0..255 over 3s");
            for (int i = 0; i <= 60; i++) { set_rumble(h, i * 4, i * 4); Sleep(50); }
            set_rumble(h, 0, 0);
            LG("VIBE ramp done");
        } else if (!strcmp(cmd, "play") && got >= 2) {
            g_playLevel = (got >= 3 && a > 0) ? a : 255;
            snprintf(g_reqName, sizeof g_reqName, "%s", arg);
            g_reqLoop  = (got >= 4) ? b : 0;
            g_playStop = 0;
            g_reqSeq++;                    /* hand off; never play on this thread */
        } else if (!strcmp(cmd, "stop")) {
            g_playStop = 1;
            g_reqSeq++;                    /* also supersedes anything mid-flight */
            set_rumble(h, 0, 0);
            LG("VIBE stop");
        } else if (!strcmp(cmd, "spk") && got >= 2) {
            snprintf(g_spkReq, sizeof g_spkReq, "%s", arg);
            g_spkLevel = (got >= 3 ? a : 255) / 255.0f;
            g_spkLoop  = (got >= 4) ? b : 0;
            g_spkStop  = 0;
            g_spkSeq++;
        } else if (!strcmp(cmd, "spkstop")) {
            g_spkStop = 1; g_spkSeq++; LG("SPK stop");
        } else if (!strcmp(cmd, "spklevel") && got >= 2) {
            g_spkLevel = atoi(arg) / 255.0f;
        } else if (!strcmp(cmd, "spkboost") && got >= 2) {
            g_spkBoost = (float)atof(arg);   /* A/B the SBFX_Boost +5 dB (1.7783) */
            LG("SPK boost -> %.4f", g_spkBoost);
        } else if (!strcmp(cmd, "vibmode") && got >= 2) {
            g_vibModeOverride = atoi(arg);
            genfn f = resolve("scePadSetVibrationMode");
            unsigned long long ph = g_lastPadHandle ? g_lastPadHandle : h;
            long long r = f ? f(ph, (unsigned long long)g_vibModeOverride, 0, 0) : -1;
            LG("VIBE vibmode -> %d applied to 0x%llX = 0x%08X",
               g_vibModeOverride, ph, (unsigned)r);
        } else if (!strcmp(cmd, "gain") && got >= 2) {
            g_gain = atoi(arg);
            if (g_gain < 0) g_gain = 0;
            if (g_gain > 255) g_gain = 255;
            LG("VIBE gain -> %d", g_gain);
        } else if (!strcmp(cmd, "level") && got >= 2) {
            g_playLevel = atoi(arg);
        } else if (!strcmp(cmd, "pulse")) {
            LG("VIBE pulse x5");
            for (int i = 0; i < 5; i++) { set_rumble(h, 255, 255); Sleep(200); set_rumble(h, 0, 0); Sleep(300); }
            LG("VIBE pulse done");
        }
    }
    return 0;
}

static DWORD WINAPI audio_probe(LPVOID handle_v)
{
    unsigned long long h = (unsigned long long)(uintptr_t)handle_v;
    Sleep(5000);                      /* let the game settle and open its audio */
    genfn supp = resolve("scePadIsSupportedAudioFunction");
    genfn cont = resolve("scePadGetContainerIdInformation");
    genfn path = resolve("scePadSetAudioOutPath");
    genfn gain = resolve("scePadSetVolumeGain");
    LG("AUDIO PROBE (in-game) handle=0x%llX  supp=%p cont=%p path=%p gain=%p",
       h, supp, cont, path, gain);
    if (supp) LG("AUDIO  scePadIsSupportedAudioFunction = 0x%08X", (unsigned)supp(h, 0, 0, 0));
    /* GetContainerIdInformation crashed the game on a guessed struct; we do not need it. */
    (void)cont;
    /* scePadSetAudioOutPath(handle, path) takes the path BY VALUE; 3 = SPEAKER */
    /* 0 STEREO_HEADSET 1 MONO_HEADSET 2 MONO_HEADSET_SPEAKER 3 SPEAKER 4 OFF.
       3 = SPEAKER is the one that routes audio into the pad. Try it alone first. */
    if (path) LG("AUDIO  scePadSetAudioOutPath(3=SPEAKER) = 0x%08X",
                 (unsigned)path(h, (unsigned long long)3, 0, 0));
    if (gain) {
        /* s_ScePadVolumeGain { uint8 SpeakerVol, JackVol, Reserved, MicGain } */
        unsigned char g[8] = { 80, 80, 0, 0, 0, 0, 0, 0 };
        LG("AUDIO  scePadSetVolumeGain(spk=80) = 0x%08X",
           (unsigned)gain(h, (unsigned long long)(uintptr_t)g, 0, 0));
    }
    LG("AUDIO PROBE done");
    return 0;
}

static void observe_vibration(unsigned long long handle, unsigned long long param)
{
	if (!g_enabled || !param) return;
	const unsigned char *v = (const unsigned char *)(uintptr_t)param;
	int amp = v[0] > v[1] ? v[0] : v[1];
	if (amp >= g_threshold) { g_activeRun++; g_quietRun = 0; }
	else                    { g_quietRun++;  g_activeRun = 0; }

	if (g_trigOn != 1 && g_activeRun >= g_onFrames)       { g_trigOn = 1; set_triggers(handle, 1, 1); }
	else if (g_trigOn != 0 && g_quietRun >= g_offFrames)  { g_trigOn = 0; set_triggers(handle, 0, 0); }
}
/* --------------------------------------------------------------------------- */

#define WRAP(name)                                                            \
__declspec(dllexport) long long name(unsigned long long a, unsigned long long b,\
	unsigned long long c, unsigned long long d)                               \
{                                                                             \
	static genfn f; static int tried;                                         \
	if (!tried) { f = resolve(#name); tried = 1; }                            \
	if (!f) { LG(#name " -> REAL EXPORT MISSING"); return 0x80920009LL; }     \
	long long r = f(a, b, c, d);                                              \
	LG(#name "(0x%llX, 0x%llX, 0x%llX, 0x%llX) = 0x%08X", a, b, c, d, (unsigned)r); \
	return r;                                                                 \
}

#define WRAP_HOT(name)                                                        \
__declspec(dllexport) long long name(unsigned long long a, unsigned long long b,\
	unsigned long long c, unsigned long long d)                               \
{                                                                             \
	static genfn f; static int tried; static unsigned long n;                 \
	if (!tried) { f = resolve(#name); tried = 1; }                            \
	if (!f) { if (n++ < 3) LG(#name " -> REAL EXPORT MISSING"); return 0x80920009LL; } \
	long long r = f(a, b, c, d);                                              \
	if (n < 3 || n % 20000 == 0) LG(#name "(0x%llX, ...) = 0x%08X [#%lu]", a, (unsigned)r, n); \
	n++;                                                                      \
	return r;                                                                 \
}

__declspec(dllexport) long long scePadSetVibrationMode(unsigned long long a,
	unsigned long long b, unsigned long long c, unsigned long long d)
{
	static genfn f; static int tried;
	if (!tried) { f = resolve("scePadSetVibrationMode"); tried = 1; }
	if (!f) { LG("scePadSetVibrationMode -> REAL EXPORT MISSING"); return 0x80920009LL; }
	unsigned long long mode = b;
	if (g_vibModeOverride >= 0) mode = (unsigned long long)g_vibModeOverride;
	g_lastPadHandle = a;
	long long r = f(a, mode, c, d);
	LG("scePadSetVibrationMode(0x%llX, game=%llu used=%llu) = 0x%08X%s",
		a, b, mode, (unsigned)r, g_vibModeOverride >= 0 ? "  [OVERRIDDEN]" : "");
	return r;
}

/* Vibration is our signal, so it gets a custom wrapper rather than WRAP. */
__declspec(dllexport) long long scePadSetVibration(unsigned long long a,
	unsigned long long b, unsigned long long c, unsigned long long d)
{
	static genfn f; static int tried; static unsigned long n;
	if (!tried) { f = resolve("scePadSetVibration"); tried = 1; }
	if (!f) return 0x80920009LL;
	long long r = f(a, b, c, d);
	observe_vibration(a, b);
	if (n < 3 || n % 3000 == 0) {
		const unsigned char *v = (const unsigned char *)(uintptr_t)b;
		LG("scePadSetVibration(0x%llX, large=%u small=%u) = 0x%08X [#%lu]",
			a, b ? v[0] : 0, b ? v[1] : 0, (unsigned)r, n);
	}
	n++;
	return r;
}

WRAP(scePadInit)
__declspec(dllexport) long long scePadOpen(unsigned long long a, unsigned long long b,
    unsigned long long c, unsigned long long d)
{
    static genfn f; static int tried;
    if (!tried) { f = resolve("scePadOpen"); tried = 1; }
    if (!f) { LG("scePadOpen -> REAL EXPORT MISSING"); return 0x80920009LL; }
    long long r = f(a, b, c, d);
    LG("scePadOpen(0x%llX, 0x%llX, 0x%llX, 0x%llX) = 0x%08X", a, b, c, d, (unsigned)r);
    if (r > 0 && !g_demoHandle) {
        g_demoHandle = (unsigned long long)r;
        char *e = getenv("STRAY_TRIG_DEMO");
        g_demo = e ? atoi(e) : 0;
        if (g_demo) CreateThread(NULL, 0, demo_thread, NULL, 0, NULL);
        CreateThread(NULL, 0, state_thread, NULL, 0, NULL);
        CreateThread(NULL, 0, audio_probe, (LPVOID)(uintptr_t)r, 0, NULL);
        CreateThread(NULL, 0, vibe_thread, (LPVOID)(uintptr_t)r, 0, NULL);
        CreateThread(NULL, 0, play_thread, (LPVOID)(uintptr_t)r, 0, NULL);
        CreateThread(NULL, 0, audio_endpoints, NULL, 0, NULL);
        CreateThread(NULL, 0, spk_thread, NULL, 0, NULL);
        CreateThread(NULL, 0, spk_watch, NULL, 0, NULL);
        LG("first handle 0x%llX captured; demo=%d", g_demoHandle, g_demo);
    }
    return r;
}
WRAP(scePadClose)
WRAP(scePadGetControllerInformation)
WRAP(scePadGetControllerType)
WRAP(scePadGetHandle)
WRAP(scePadSetLightBar)
WRAP(scePadResetLightBar)
WRAP(scePadSetTriggerEffect)
WRAP_HOT(scePadReadState)
WRAP_HOT(scePadRead)
WRAP_HOT(scePadGetTriggerEffectState)


/* Is the DualSense exposed as a WINDOWS audio endpoint? PROTON_SONY_WINDOWS_DEVICE_NAMES
   and PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE are both set, so it should be - and that is
   the designed route for controller-speaker audio: no ALSA, and Sony's own
   scePadSetVolumeGain governs the level. Enumerate before building any playback on it. */
static DWORD WINAPI audio_endpoints(LPVOID unused)
{
    (void)unused;
    Sleep(8000);
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr) || !en) { LG("ENDPOINTS CoCreateInstance failed hr=0x%08X", (unsigned)hr); return 0; }
    IMMDeviceCollection *col = NULL;
    hr = IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col);
    if (FAILED(hr) || !col) { LG("ENDPOINTS EnumAudioEndpoints failed hr=0x%08X", (unsigned)hr); return 0; }
    UINT n = 0; IMMDeviceCollection_GetCount(col, &n);
    LG("ENDPOINTS %u active render endpoint(s):", n);
    for (UINT i = 0; i < n; i++) {
        IMMDevice *dev = NULL;
        if (FAILED(IMMDeviceCollection_Item(col, i, &dev)) || !dev) continue;
        IPropertyStore *ps = NULL;
        char name[256] = "?";
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps)) && ps) {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &pv)) && pv.pwszVal)
                WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, name, sizeof name, NULL, NULL);
            PropVariantClear(&pv);
            IPropertyStore_Release(ps);
        }
        /* Mix format tells us what a stream on this endpoint must look like. */
        IAudioClient *ac = NULL; WAVEFORMATEX *wf = NULL;
        char fmt[96] = "";
        if (SUCCEEDED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac)) && ac) {
            if (SUCCEEDED(IAudioClient_GetMixFormat(ac, &wf)) && wf) {
                snprintf(fmt, sizeof fmt, "  %uch %uHz %ubit", wf->nChannels,
                         (unsigned)wf->nSamplesPerSec, wf->wBitsPerSample);
                CoTaskMemFree(wf);
            }
            IAudioClient_Release(ac);
        }
        LG("   [%u] %s%s", i, name, fmt);
        IMMDevice_Release(dev);
    }
    IMMDeviceCollection_Release(col);
    IMMDeviceEnumerator_Release(en);
    LG("ENDPOINTS done");
    return 0;
}


/* Play a controller sound on the DualSense's own WINDOWS audio endpoint.
   The endpoint is 4ch: FL/FR are the speaker, RL/RR the haptic coils. We fill only
   FL/FR - the coils are driven by the rumble path, and writing them here did not reach
   them in earlier testing. Sony's scePadSetAudioOutPath/scePadSetVolumeGain govern
   routing and level, so nothing here touches ALSA. */
static IMMDevice *find_pad_endpoint(void)
{
    IMMDeviceEnumerator *en = NULL;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator, (void **)&en)) || !en) return NULL;
    IMMDeviceCollection *col = NULL;
    IMMDevice *found = NULL;
    if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
        UINT n = 0; IMMDeviceCollection_GetCount(col, &n);
        for (UINT i = 0; i < n && !found; i++) {
            IMMDevice *dev = NULL;
            if (FAILED(IMMDeviceCollection_Item(col, i, &dev)) || !dev) continue;
            IPropertyStore *ps = NULL; char name[256] = "";
            if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps)) && ps) {
                PROPVARIANT pv; PropVariantInit(&pv);
                if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &pv)) && pv.pwszVal)
                    WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, name, sizeof name, NULL, NULL);
                PropVariantClear(&pv); IPropertyStore_Release(ps);
            }
            if (strstr(name, "DualSense")) { found = dev; break; }
            IMMDevice_Release(dev);
        }
        IMMDeviceCollection_Release(col);
    }
    IMMDeviceEnumerator_Release(en);
    return found;
}

static void spk_play(const char *name, int loop)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s%s.f32", g_spkDir, name);
    FILE *fp = fopen(path, "rb");
    if (!fp) { LG("SPK not found: %s", path); return; }
    fseek(fp, 0, SEEK_END); long bytes = ftell(fp); fseek(fp, 0, SEEK_SET);
    long frames = bytes / 4;
    if (frames <= 0) { fclose(fp); return; }
    float *mono = (float *)malloc((size_t)bytes);
    if (!mono) { fclose(fp); return; }
    fread(mono, 1, (size_t)bytes, fp);
    fclose(fp);

    IMMDevice *dev = find_pad_endpoint();
    if (!dev) { LG("SPK no DualSense endpoint"); free(mono); return; }
    IAudioClient *ac = NULL; WAVEFORMATEX *wf = NULL;
    long seq = g_spkCur;
    if (FAILED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac)) || !ac) {
        LG("SPK Activate failed"); IMMDevice_Release(dev); free(mono); return; }
    if (FAILED(IAudioClient_GetMixFormat(ac, &wf)) || !wf) {
        LG("SPK GetMixFormat failed"); goto out; }
    HRESULT hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, wf, NULL);
    if (FAILED(hr)) { LG("SPK Initialize failed hr=0x%08X", (unsigned)hr); goto out; }
    UINT32 bufFrames = 0; IAudioClient_GetBufferSize(ac, &bufFrames);
    IAudioRenderClient *rc = NULL;
    if (FAILED(IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc)) || !rc) {
        LG("SPK GetService failed"); goto out; }
    LG("SPK play %s (%ld frames, %.2fs) -> %uch %uHz buf=%u loop=%d",
       name, frames, frames / 48000.0, wf->nChannels, (unsigned)wf->nSamplesPerSec, bufFrames, loop);
    IAudioClient_Start(ac);
    long pos = 0;
    const UINT ch = wf->nChannels;
    for (;;) {
        if (g_spkStop || g_spkSeq != seq) break;
        UINT32 pad = 0;
        if (FAILED(IAudioClient_GetCurrentPadding(ac, &pad))) break;
        UINT32 avail = bufFrames - pad;
        if (avail == 0) { Sleep(5); continue; }
        BYTE *buf = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(rc, avail, &buf)) || !buf) break;
        float *out = (float *)buf;
        const float lvl = g_spkLevel * g_spkBoost;
        for (UINT32 i = 0; i < avail; i++) {
            float v = 0.0f;
            if (pos < frames) v = mono[pos] * lvl;
            else if (loop) { pos = 0; v = mono[0] * lvl; }
            if (v >  1.0f) v =  1.0f;
            if (v < -1.0f) v = -1.0f;
            pos++;
            for (UINT c = 0; c < ch; c++)
                out[i * ch + c] = (c < 2) ? v : 0.0f;   /* FL/FR only */
        }
        IAudioRenderClient_ReleaseBuffer(rc, avail, 0);
        if (pos >= frames && !loop) { Sleep(120); break; }
    }
    IAudioClient_Stop(ac);
    IAudioRenderClient_Release(rc);
    LG("SPK play %s ended (stop=%d superseded=%d)", name, g_spkStop, g_spkSeq != seq);
out:
    if (wf) CoTaskMemFree(wf);
    if (ac) IAudioClient_Release(ac);
    IMMDevice_Release(dev);
    free(mono);
}


/* Watches the SPEAKER command file. Separate from the vibe watcher so the two paths
   cannot clobber each other's pending command - they routinely fire in the same frame. */
static DWORD WINAPI spk_watch(LPVOID unused)
{
    (void)unused;
    LG("SPK thread watching %s", g_spkCmd);
    for (;;) {
        Sleep(50);
        FILE *f = fopen(g_spkCmd, "r");
        if (!f) continue;
        char cmd[64] = {0}, arg[96] = {0};
        int a = 0, b = 0;
        int got = fscanf(f, "%63s %95s %d %d", cmd, arg, &a, &b);
        fclose(f);
        remove(g_spkCmd);
        if (got < 1) continue;
        if (!strcmp(cmd, "spk") && got >= 2) {
            snprintf(g_spkReq, sizeof g_spkReq, "%s", arg);
            g_spkLevel = (got >= 3 ? a : 255) / 255.0f;
            g_spkLoop  = (got >= 4) ? b : 0;
            g_spkStop  = 0;
            g_spkSeq++;
        } else if (!strcmp(cmd, "spkstop")) {
            g_spkStop = 1; g_spkSeq++; LG("SPK stop");
        } else if (!strcmp(cmd, "spklevel") && got >= 2) {
            g_spkLevel = atoi(arg) / 255.0f;
        } else if (!strcmp(cmd, "spkboost") && got >= 2) {
            g_spkBoost = (float)atof(arg);   /* A/B the SBFX_Boost +5 dB (1.7783) */
            LG("SPK boost -> %.4f", g_spkBoost);
        }
    }
    return 0;
}

static DWORD WINAPI spk_thread(LPVOID unused)
{
    (void)unused;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    for (;;) {
        if (g_spkSeq == g_spkCur) { Sleep(5); continue; }
        g_spkCur = g_spkSeq;
        if (g_spkStop) continue;
        char n[96]; snprintf(n, sizeof n, "%s", g_spkReq);
        spk_play(n, g_spkLoop);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
	(void)h; (void)r;
	if (reason == DLL_PROCESS_ATTACH) InitializeCriticalSection(&g_cs);
	return TRUE;
}
