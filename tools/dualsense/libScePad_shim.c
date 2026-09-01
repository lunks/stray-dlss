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
#include <math.h>
#include <setupapi.h>
#include <hidsdi.h>
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
static DWORD WINAPI tone_thread(LPVOID);
static DWORD WINAPI tone_excl(LPVOID);
static DWORD WINAPI hid_mode(LPVOID);
static DWORD WINAPI haptic_mode_thread(LPVOID);
static HANDLE open_pad_hid(void);
static void set_valid_flag0(unsigned char);
static char  g_spkDir[MAX_PATH];
static char  g_spkCmd[MAX_PATH];
static char  g_spkReq[96];
static volatile long  g_spkSeq, g_spkCur;
static volatile int   g_spkStop, g_spkLoop;
static volatile float g_spkLevel = 1.0f;
static volatile float g_spkBoost = 1.7783f;
static volatile int   g_spkChanMask = 0x3;   /* bit0 FL, bit1 FR, bit2 RL, bit3 RR */
/* Haptics played as AUDIO on the coils - the native DualSense mechanism. The VIBE assets
   are STEREO 48kHz precisely because there are two coils, one per grip; the CONTROL
   (speaker) assets are mono because there is one speaker. Collapsing the stereo waveform
   into a single motor amplitude is what made it feel wrong. */
static char  g_hapDir[MAX_PATH];
static char  g_loopList[8192];      /* newline-separated names with bLooping=true */

/* The game's own bLooping flag decides, not the caller. */
static int asset_loops(const char *name)
{
    if (!g_loopList[0]) return 0;
    const size_t n = strlen(name);
    for (const char *p = g_loopList; *p; ) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        while (len && (p[len-1] == '\r' || p[len-1] == ' ')) len--;
        if (len == n && !memcmp(p, name, n)) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}
static char  g_hapReq[96];
static volatile long  g_hapSeq, g_hapCur;
static volatile int   g_hapStop, g_hapLoop;
static volatile float g_hapLevel = 1.0f;
static volatile int   g_hapChanMask = 0xC;   /* RL|RR - the two coils */
static volatile int   g_hapticMode  = 1;     /* keep the coils in waveform mode */
enum { kValidFlag0Haptics = 0x00,   /* claim nothing - notably NOT compatible-vibration */
       kValidFlag0Rumble  = 0x01 }; /* bit0 only: hand the coils back to motor emulation */
static volatile int g_flag0 = 0x00;  /* live-overridable via the "hapflag" command */
static DWORD WINAPI hap_thread(LPVOID);
/* Captured from the GAME's scePadOpen so we can test whether scePadGetHandle returns the
   same handle. That is the one assumption behind replacing this shim with a UE4SS C++
   plugin: a plugin cannot intercept Open, so it must retrieve the handle instead. */
/* The game opens FOUR user slots (1-4) and every one returns a positive handle, so a
   positive Open result does NOT mean a pad is present. Record them all and ask the API
   which slot actually has hardware - binding to the first is luck, not selection. */
static unsigned long long g_slotUser[8], g_slotHandle[8];
static volatile int g_openCount;
static DWORD WINAPI gethandle_probe(LPVOID);
static DWORD WINAPI container_probe(LPVOID);
static void enumerate_all_endpoints(void);   /* SBFX_Boost InputGainDb=+5.0 dB */
enum { kSideLeft = 0, kSideRight = 1 };  /* EPS5TriggersSide, read from the exe's enum strings */
enum { kMaxPlaySamples = 240000 };       /* ~20 min at 5 ms/sample. Ambient loops (rain is
                                            38.4 s) are legitimately long and the game does send
                                            Stop; this is only a backstop for a missed one. */
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
/* Defaults only; overwritten by whatever the game authored, once we can read it. */
static int  g_trigMode  = 3;  /* EPS5TriggerEffectMode: 3 = Feedback (NOT Sony's 1) */
static int  g_position  = 0;  /* Feedback: where resistance starts (0..9) */
static int  g_strength  = 2;  /* Feedback: resistance (0..8) */
static int  g_trigV3    = 0;  /* only meaningful for Weapon / Vibration modes */

/* EPS5TriggerEffectMode (game) -> ScePadTriggerEffectMode (Sony). */
static int sony_trigger_mode(int game_mode)
{
    switch (game_mode) {
    case 0: return 0;   /* None      -> Off       */
    case 1: return 2;   /* Weapon    -> Weapon    */
    case 2: return 3;   /* Vibration -> Vibration */
    case 3: return 1;   /* Feedback  -> Feedback  */
    default: return 1;  /* unknown -> Sony Feedback, the known-working effect */
    }
}

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
	snprintf(g_hapDir, sizeof g_hapDir, "%shaptic\\", p);
	{
		char lp[MAX_PATH];
		snprintf(lp, sizeof lp, "%shaptic_loops.txt", p);
		FILE *lf = fopen(lp, "rb");
		if (lf) {
			size_t got = fread(g_loopList, 1, sizeof g_loopList - 1, lf);
			g_loopList[got] = 0;
			fclose(lf);
		}
	}
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
	const uint32_t sony_mode = (uint32_t)sony_trigger_mode(g_trigMode);
	*(uint32_t *)(P + 0x08) = wantL ? sony_mode : 0u;
	P[0x10] = (unsigned char)g_position; P[0x11] = (unsigned char)g_strength;
	P[0x12] = (unsigned char)g_trigV3;               /* ignored by Feedback, used by Weapon/Vibration */
	*(uint32_t *)(P + 0x40) = wantR ? sony_mode : 0u;
	P[0x48] = (unsigned char)g_position; P[0x49] = (unsigned char)g_strength;
	P[0x4A] = (unsigned char)g_trigV3;
	long long r = g_setTrigger(handle, (unsigned long long)(uintptr_t)P, 0, 0);
	if (r == 0) { if (on) g_engaged++; else g_released++; }
	else g_failed++;
	LG("TRIGGERS %-8s L2=%d R2=%d gameMode=%d sonyMode=%u pos=%d str=%d -> 0x%08X   (engaged=%lu released=%lu failed=%lu)",
		on ? "ENGAGE" : "release", wantL, wantR, g_trigMode, (unsigned)sony_mode, g_position, g_strength, (unsigned)r,
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
        int m = -1, v1 = -1, v2 = -1, v3 = -1;
        const int n = fscanf(f, "%d %d %d %d %d %d", &wantL, &wantR, &m, &v1, &v2, &v3);
        if (n < 1) { wantL = 0; wantR = 0; }
        if (n >= 6 && m >= 0) {          /* the game's own values win over our defaults */
            if (m != g_trigMode || v1 != g_position || v2 != g_strength || v3 != g_trigV3)
                LG("TRIGGERS authored effect: mode=%d v1=%d v2=%d v3=%d (was %d/%d/%d/%d)",
                   m, v1, v2, v3, g_trigMode, g_position, g_strength, g_trigV3);
            g_trigMode = m; g_position = v1; g_strength = v2; g_trigV3 = v3;
        }
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

static int fopen_ok(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void play_envelope(unsigned long long h, const char *name, int loop)
{
    char path[MAX_PATH];
    int stereo = 1;
    snprintf(path, sizeof path, "%s%s.env2", g_vibeDir, name);   /* L,R interleaved */
    if (!fopen_ok(path)) { stereo = 0; snprintf(path, sizeof path, "%s%s.env", g_vibeDir, name); }
    FILE *f = fopen(path, "rb");
    if (!f) { LG("VIBE envelope not found: %s", path); return; }
    fseek(f, 0, SEEK_END); long bytes = ftell(f); fseek(f, 0, SEEK_SET);
    if (bytes <= 0) { fclose(f); LG("VIBE empty envelope %s", name); return; }
    const long n = stereo ? bytes / 2 : bytes;    /* n counts SAMPLES; .env2 is 2B each */
    unsigned char *env = (unsigned char *)malloc((size_t)bytes);
    if (!env) { fclose(f); return; }
    fread(env, 1, (size_t)bytes, f);
    fclose(f);
    if (g_playLevel <= 0 || g_gain <= 0) {
        LG("VIBE play %s SKIPPED: level=%d gain=%d would be silent, and writing zeros at "
           "200Hz fights the game's own rumble on the same pad", name, g_playLevel, g_gain);
        free(env);
        return;
    }
    LG("VIBE play %s (%ld samples, %.2fs, %s, loop=%d, level=%d gain=%d)",
       name, n, n * 0.005, stereo ? "STEREO per-coil" : "mono", loop, g_playLevel, g_gain);
    long seq = g_curSeq;                  /* a newer request supersedes this one */
    long guard = 0;                       /* runaway cap: a missed Stop must not buzz forever */
    do {
        for (long i = 0; i < n; i++) {
            if (g_playStop || g_reqSeq != seq || guard > kMaxPlaySamples) goto done;
            if (g_playLevel <= 0) { set_rumble(h, 0, 0); Sleep(20); continue; }  /* yield the pad */
            const int sl = stereo ? env[i * 2]     : env[i];
            const int sr = stereo ? env[i * 2 + 1] : env[i];
            const int al = (sl * g_playLevel * g_gain + 32512) / 65025;
            const int ar = (sr * g_playLevel * g_gain + 32512) / 65025;
            set_rumble(h, al, ar);          /* large=left coil, small=right coil */
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
        } else if (!strcmp(cmd, "hap") && got >= 2) {
            snprintf(g_hapReq, sizeof g_hapReq, "%s", arg);
            g_hapLevel = (got >= 3 ? a : 255) / 255.0f;
            g_hapLoop  = (got >= 4) ? b : 0;
            g_hapStop  = 0;
            g_hapSeq++;
        } else if (!strcmp(cmd, "hapch") && got >= 2) {
            g_hapChanMask = (int)strtol(arg, NULL, 0);
            LG("HAP channel mask -> 0x%X", g_hapChanMask);
        } else if (!strcmp(cmd, "haplevel") && got >= 2) {
            g_hapLevel = atoi(arg) / 255.0f;
        } else if (!strcmp(cmd, "hapstop")) {
            g_hapStop = 1; g_hapSeq++; LG("HAP stop");
        } else if (!strcmp(cmd, "stop")) {
            g_playStop = 1;
            g_hapStop  = 1; g_hapSeq++;
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
        } else if (!strcmp(cmd, "hapflag") && got >= 2) {
            g_flag0 = (int)strtol(arg, NULL, 0);
            set_valid_flag0((unsigned char)g_flag0);
            LG("HAPTICMODE valid_flag0 -> 0x%02X", g_flag0);
        } else if (!strcmp(cmd, "hidmode") && got >= 2) {
            CreateThread(NULL, 0, hid_mode, (LPVOID)(uintptr_t)strtol(arg, NULL, 0), 0, NULL);
        } else if (!strcmp(cmd, "tonex") && got >= 2) {
            CreateThread(NULL, 0, tone_excl, (LPVOID)(intptr_t)strtol(arg, NULL, 0), 0, NULL);
        } else if (!strcmp(cmd, "tone") && got >= 2) {
            CreateThread(NULL, 0, tone_thread, (LPVOID)(intptr_t)strtol(arg, NULL, 0), 0, NULL);
        } else if (!strcmp(cmd, "spkch") && got >= 2) {
            g_spkChanMask = (int)strtol(arg, NULL, 0);
            LG("SPK channel mask -> 0x%X", g_spkChanMask);
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
    if (r > 0 && g_openCount < 8) {          /* record every slot the game opens */
        g_slotUser[g_openCount] = a;
        g_slotHandle[g_openCount] = (unsigned long long)r;
        g_openCount++;
    }
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
        CreateThread(NULL, 0, hap_thread, NULL, 0, NULL);
        CreateThread(NULL, 0, haptic_mode_thread, NULL, 0, NULL);
        CreateThread(NULL, 0, container_probe, NULL, 0, NULL);
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
                out[i * ch + c] = (g_spkChanMask & (1 << c)) ? v : 0.0f;
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

/* Decisive test for the coil channels. A haptic actuator responds to LOW frequency, so a
   60 Hz sine is the right probe - a 1 kHz beep could be inaudible on a coil and prove
   nothing. Routed by channel mask so FL/FR and RL/RR can be compared back to back. */

/* Play a STEREO haptic waveform on the coil channels. Separate IAudioClient from the
   speaker path so both can sound at once - shared mode mixes them on the one endpoint. */
static void hap_play(const char *name, int loop)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s%s.f32", g_hapDir, name);
    FILE *fp = fopen(path, "rb");
    if (!fp) { LG("HAP not found: %s", path); return; }
    fseek(fp, 0, SEEK_END); long bytes = ftell(fp); fseek(fp, 0, SEEK_SET);
    long frames = bytes / 8;                 /* stereo float32 */
    if (frames <= 0) { fclose(fp); return; }
    float *pcm = (float *)malloc((size_t)bytes);
    if (!pcm) { fclose(fp); return; }
    fread(pcm, 1, (size_t)bytes, fp);
    fclose(fp);

    IMMDevice *dev = find_pad_endpoint();
    if (!dev) { LG("HAP no DualSense endpoint"); free(pcm); return; }
    IAudioClient *ac = NULL; WAVEFORMATEX *wf = NULL; IAudioRenderClient *rc = NULL;
    long seq = g_hapCur;
    if (FAILED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac)) || !ac) goto out;
    if (FAILED(IAudioClient_GetMixFormat(ac, &wf)) || !wf) goto out;
    if (FAILED(IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, wf, NULL))) goto out;
    UINT32 bufFrames = 0; IAudioClient_GetBufferSize(ac, &bufFrames);
    if (FAILED(IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc)) || !rc) goto out;
    if (g_hapticMode) set_valid_flag0((unsigned char)g_flag0);
    LG("HAP play %s (%ld frames, %.2fs) -> mask 0x%X of %uch loop=%d (bLooping) level=%.2f",
       name, frames, frames / 48000.0, g_hapChanMask, wf->nChannels, loop, g_hapLevel);
    IAudioClient_Start(ac);
    const UINT ch = wf->nChannels;
    long pos = 0;
    for (;;) {
        if (g_hapStop || g_hapSeq != seq) break;
        UINT32 pad = 0;
        if (FAILED(IAudioClient_GetCurrentPadding(ac, &pad))) break;
        UINT32 avail = bufFrames - pad;
        if (!avail) { Sleep(5); continue; }
        BYTE *buf = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(rc, avail, &buf)) || !buf) break;
        float *out = (float *)buf;
        const float lvl = g_hapLevel;
        for (UINT32 i = 0; i < avail; i++) {
            float l = 0.0f, r = 0.0f;
            if (pos >= frames) { if (loop) pos = 0; }
            if (pos < frames) { l = pcm[pos * 2] * lvl; r = pcm[pos * 2 + 1] * lvl; }
            pos++;
            for (UINT c = 0; c < ch; c++) {
                float v = 0.0f;
                if (g_hapChanMask & (1 << c)) v = (c & 1) ? r : l;   /* even=L, odd=R */
                if (v >  1.0f) v =  1.0f;
                if (v < -1.0f) v = -1.0f;
                out[i * ch + c] = v;
            }
        }
        IAudioRenderClient_ReleaseBuffer(rc, avail, 0);
        if (pos >= frames && !loop) { Sleep(120); break; }
    }
    IAudioClient_Stop(ac);
    LG("HAP play %s ended (stop=%d superseded=%d)", name, g_hapStop, g_hapSeq != seq);
out:
    if (rc) IAudioRenderClient_Release(rc);
    if (wf) CoTaskMemFree(wf);
    if (ac) IAudioClient_Release(ac);
    IMMDevice_Release(dev);
    free(pcm);
}

static DWORD WINAPI hap_thread(LPVOID unused)
{
    (void)unused;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    for (;;) {
        if (g_hapSeq == g_hapCur) { Sleep(5); continue; }
        g_hapCur = g_hapSeq;
        if (g_hapStop) continue;
        char n[96]; snprintf(n, sizeof n, "%s", g_hapReq);
        const int loops = asset_loops(n);      /* the asset decides, not the caller */
        hap_play(n, loops);
    }
    return 0;
}


/* SHARED mode gives us a mixer-negotiated format, and the rear pair vanished there - the
   tone was not merely quieter on the speaker, it was absent, i.e. discarded rather than
   downmixed. EXCLUSIVE mode asks the device for its own format instead. The hardware
   really does advertise 4ch S16_LE 48000 with channel map FL FR RL RR, so if anything
   between us and it is dropping the rear pair, this is the way past it. */

/* Put the coils into AUDIO-HAPTIC mode.
   DualSense USB output report 0x02, byte 1 is valid_flag0:
     bit0 COMPATIBLE_VIBRATION, bit1 HAPTICS_SELECT.
   0xFF (both set) = emulate rumble on the coils - what libScePad uses, and what we have
   been feeling. 0xFC clears both = take the audio waveform instead. Byte 2 (valid_flag1)
   stays 0 so we assert nothing else and cannot disturb LEDs or triggers.
   Values from Dualsense-Multiplatform's EDSVibrationMode (MIT). */
static HANDLE open_pad_hid(void)
{
    GUID g; HidD_GetHidGuid(&g);
    HDEVINFO set = SetupDiGetClassDevsA(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return NULL;
    SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof ifd;
    HANDLE found = NULL;
    for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(set, NULL, &g, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailA(set, &ifd, NULL, 0, &need, NULL);
        if (!need) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *det =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)malloc(need);
        if (!det) continue;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (SetupDiGetDeviceInterfaceDetailA(set, &ifd, det, need, NULL, NULL)) {
            HANDLE h = CreateFileA(det->DevicePath, GENERIC_WRITE | GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES at; at.Size = sizeof at;
                if (HidD_GetAttributes(h, &at) && at.VendorID == 0x054C && at.ProductID == 0x0CE6) {
                    LG("HIDMODE opened DualSense HID: %s", det->DevicePath);
                    found = h;
                } else CloseHandle(h);
            }
        }
        free(det);
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!found) LG("HIDMODE no DualSense HID device found (VID 054C PID 0CE6)");
    return found;
}

/* Write valid_flag0 directly, reusing one open handle. Cheap enough to re-assert often. */
static void set_valid_flag0(unsigned char flag0)
{
    static HANDLE h;
    if (!h) h = open_pad_hid();
    if (!h) return;
    unsigned char rep[48];
    memset(rep, 0, sizeof rep);
    rep[0] = 0x02;
    rep[1] = flag0;     /* 0xFC coils take the waveform, 0xFF coils emulate motors */
    rep[2] = 0x00;      /* assert nothing else: no LEDs, no triggers disturbed */
    DWORD wrote = 0;
    if (!WriteFile(h, rep, sizeof rep, &wrote, NULL)) {
        LG("HAPTICMODE write failed err=%lu; reopening next time", GetLastError());
        CloseHandle(h); h = NULL;
    }
}

/* Slow re-assert so the mode survives the game's own pad writes. */
static DWORD WINAPI haptic_mode_thread(LPVOID unused)
{
    (void)unused;
    Sleep(3000);
    set_valid_flag0((unsigned char)g_flag0);
    LG("HAPTICMODE coils set to WAVEFORM mode (valid_flag0=0x%02X), re-asserted every 2s",
       kValidFlag0Haptics);
    for (;;) {
        Sleep(2000);
        if (g_hapticMode) set_valid_flag0((unsigned char)g_flag0);
    }
    return 0;
}

static DWORD WINAPI hid_mode(LPVOID arg)
{
    const unsigned char flag0 = (unsigned char)(uintptr_t)arg;
    g_hapticMode = (flag0 == kValidFlag0Haptics);
    HANDLE h = open_pad_hid();
    if (!h) return 0;
    unsigned char rep[48];
    memset(rep, 0, sizeof rep);
    rep[0] = 0x02;      /* USB output report id */
    rep[1] = flag0;     /* valid_flag0: 0xFC = haptics, 0xFF = compatible rumble */
    rep[2] = 0x00;      /* valid_flag1: assert nothing else */
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, rep, sizeof rep, &wrote, NULL);
    LG("HIDMODE wrote valid_flag0=0x%02X ok=%d bytes=%lu err=%lu  (%s)",
       flag0, ok, wrote, ok ? 0UL : GetLastError(),
       flag0 == 0xFC ? "HapticsRumble - coils take the waveform"
                     : "DefaultRumble - coils emulate motors");
    CloseHandle(h);
    return 0;
}

static DWORD WINAPI tone_excl(LPVOID arg)
{
    const int mask = (int)(intptr_t)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDevice *dev = find_pad_endpoint();
    if (!dev) { LG("EXCL no DualSense endpoint"); return 0; }
    IAudioClient *ac = NULL; IAudioRenderClient *rc = NULL;
    if (FAILED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac)) || !ac) {
        LG("EXCL Activate failed"); goto done; }

    /* Exactly what /proc/asound reports the hardware accepts. */
    WAVEFORMATEXTENSIBLE wfx;
    memset(&wfx, 0, sizeof wfx);
    wfx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels       = 4;
    wfx.Format.nSamplesPerSec  = 48000;
    wfx.Format.wBitsPerSample  = 16;
    wfx.Format.nBlockAlign     = (WORD)(4 * 16 / 8);
    wfx.Format.nAvgBytesPerSec = 48000 * wfx.Format.nBlockAlign;
    wfx.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 16;
    wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                        SPEAKER_BACK_LEFT  | SPEAKER_BACK_RIGHT;
    wfx.SubFormat = (GUID){0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};

    WAVEFORMATEX *closest = NULL;
    HRESULT hr = IAudioClient_IsFormatSupported(ac, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                (WAVEFORMATEX *)&wfx, &closest);
    LG("EXCL IsFormatSupported(4ch/48k/S16) hr=0x%08X%s", (unsigned)hr,
       hr == S_OK ? " SUPPORTED" : (closest ? " (a closest match was offered)" : " NOT supported"));
    if (closest) {
        LG("EXCL closest: %uch %uHz %ubit", closest->nChannels,
           (unsigned)closest->nSamplesPerSec, closest->wBitsPerSample);
        CoTaskMemFree(closest);
    }

    REFERENCE_TIME def = 0, minp = 0;
    IAudioClient_GetDevicePeriod(ac, &def, &minp);
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_EXCLUSIVE, 0, def, def,
                                 (WAVEFORMATEX *)&wfx, NULL);
    if (FAILED(hr)) { LG("EXCL Initialize failed hr=0x%08X (0x88890019=misaligned, "
                         "0x8889000A=device in use, 0x88890008=unsupported format)", (unsigned)hr);
                      goto done; }
    UINT32 bufFrames = 0; IAudioClient_GetBufferSize(ac, &bufFrames);
    if (FAILED(IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc)) || !rc) {
        LG("EXCL GetService failed"); goto done; }
    LG("EXCL OPENED 4ch/48k/S16 exclusive, buf=%u frames. 60Hz on mask 0x%X for 3s", bufFrames, mask);
    IAudioClient_Start(ac);
    double ph = 0.0, step = 2.0 * 3.14159265358979 * 60.0 / 48000.0;
    for (int done_frames = 0; done_frames < 48000 * 3; ) {
        UINT32 pad = 0; IAudioClient_GetCurrentPadding(ac, &pad);
        UINT32 avail = bufFrames - pad;
        if (!avail) { Sleep(2); continue; }
        BYTE *buf = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(rc, avail, &buf)) || !buf) break;
        short *out = (short *)buf;
        for (UINT32 i = 0; i < avail; i++, ph += step) {
            short v = (short)(26000.0 * sin(ph));
            for (int c = 0; c < 4; c++) out[i * 4 + c] = (mask & (1 << c)) ? v : 0;
        }
        IAudioRenderClient_ReleaseBuffer(rc, avail, 0);
        done_frames += avail;
    }
    IAudioClient_Stop(ac);
    LG("EXCL mask 0x%X done", mask);
done:
    if (rc) IAudioRenderClient_Release(rc);
    if (ac) IAudioClient_Release(ac);
    IMMDevice_Release(dev);
    return 0;
}

static DWORD WINAPI tone_thread(LPVOID arg)
{
    const int mask = (int)(intptr_t)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDevice *dev = find_pad_endpoint();
    if (!dev) { LG("TONE no DualSense endpoint"); return 0; }
    IAudioClient *ac = NULL; WAVEFORMATEX *wf = NULL; IAudioRenderClient *rc = NULL;
    if (FAILED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac)) || !ac) goto done;
    if (FAILED(IAudioClient_GetMixFormat(ac, &wf)) || !wf) goto done;
    if (FAILED(IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, wf, NULL))) goto done;
    UINT32 bufFrames = 0; IAudioClient_GetBufferSize(ac, &bufFrames);
    if (FAILED(IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc)) || !rc) goto done;
    LG("TONE 60Hz on channel mask 0x%X (%uch) for 3s", mask, wf->nChannels);
    IAudioClient_Start(ac);
    const UINT ch = wf->nChannels;
    double ph = 0.0, step = 2.0 * 3.14159265358979 * 60.0 / (double)wf->nSamplesPerSec;
    for (int frames = 0; frames < (int)wf->nSamplesPerSec * 3; ) {
        UINT32 pad = 0; IAudioClient_GetCurrentPadding(ac, &pad);
        UINT32 avail = bufFrames - pad;
        if (!avail) { Sleep(5); continue; }
        BYTE *buf = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(rc, avail, &buf)) || !buf) break;
        float *out = (float *)buf;
        for (UINT32 i = 0; i < avail; i++, ph += step) {
            float v = (float)(0.8 * sin(ph));
            for (UINT c = 0; c < ch; c++) out[i * ch + c] = (mask & (1 << c)) ? v : 0.0f;
        }
        IAudioRenderClient_ReleaseBuffer(rc, avail, 0);
        frames += avail;
    }
    IAudioClient_Stop(ac);
    LG("TONE mask 0x%X done", mask);
done:
    if (rc) IAudioRenderClient_Release(rc);
    if (wf) CoTaskMemFree(wf);
    if (ac) IAudioClient_Release(ac);
    IMMDevice_Release(dev);
    return 0;
}

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
        } else if (!strcmp(cmd, "hidmode") && got >= 2) {
            CreateThread(NULL, 0, hid_mode, (LPVOID)(uintptr_t)strtol(arg, NULL, 0), 0, NULL);
        } else if (!strcmp(cmd, "tonex") && got >= 2) {
            CreateThread(NULL, 0, tone_excl, (LPVOID)(intptr_t)strtol(arg, NULL, 0), 0, NULL);
        } else if (!strcmp(cmd, "tone") && got >= 2) {
            CreateThread(NULL, 0, tone_thread, (LPVOID)(intptr_t)strtol(arg, NULL, 0), 0, NULL);
        } else if (!strcmp(cmd, "spkch") && got >= 2) {
            g_spkChanMask = (int)strtol(arg, NULL, 0);
            LG("SPK channel mask -> 0x%X", g_spkChanMask);
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


/* Which of the opened slots actually has a DualSense on it?
   Dumps ScePadControllerInformation raw rather than trusting a guessed struct layout -
   the differences between an occupied and an empty slot identify the connected field. */

/* Every render endpoint in EVERY state: a haptic device that is disabled, unplugged or
   not-present would be invisible to the ACTIVE-only enumeration we ran before. */
static void enumerate_all_endpoints(void)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator, (void **)&en)) || !en) return;
    IMMDeviceCollection *col = NULL;
    if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eAll, DEVICE_STATEMASK_ALL, &col)) && col) {
        UINT n = 0; IMMDeviceCollection_GetCount(col, &n);
        LG("ENDPOINTS-ALL %u endpoint(s) in every state:", n);
        for (UINT i = 0; i < n; i++) {
            IMMDevice *dev = NULL;
            if (FAILED(IMMDeviceCollection_Item(col, i, &dev)) || !dev) continue;
            DWORD st = 0; IMMDevice_GetState(dev, &st);
            LPWSTR id = NULL; IMMDevice_GetId(dev, &id);
            char idn[160] = "";
            if (id) WideCharToMultiByte(CP_UTF8, 0, id, -1, idn, sizeof idn, NULL, NULL);
            IPropertyStore *ps = NULL; char name[200] = "?";
            if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps)) && ps) {
                PROPVARIANT pv; PropVariantInit(&pv);
                if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &pv)) && pv.pwszVal)
                    WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, name, sizeof name, NULL, NULL);
                PropVariantClear(&pv); IPropertyStore_Release(ps);
            }
            LG("   [%u] state=0x%X %s", i, (unsigned)st, name);
            LG("        id=%s", idn);
            if (id) CoTaskMemFree(id);
            IMMDevice_Release(dev);
        }
        IMMDeviceCollection_Release(col);
    }
    IMMDeviceEnumerator_Release(en);
    LG("ENDPOINTS-ALL done");
}

/* libScePad has NO haptic-audio export (all 25 enumerated; only SetVibration/SetVibrationMode
   relate to haptics at all). So the intended PC route must be: ask the pad for its audio
   CONTAINER ID, then open that device yourself. Two things to establish here:
     1. what container id the pad reports, and
     2. whether any endpoint we have not looked at matches it - our earlier enumeration
        listed only ACTIVE render endpoints, so a haptic device that is disabled,
        unplugged or not-present was invisible. */
static DWORD WINAPI container_probe(LPVOID unused)
{
    (void)unused;
    Sleep(10000);
    LG("CONTAINER probe starting");
    /* Enumerate FIRST: it is already proven safe at the 8s mark, so we bank that result
       before touching the API whose signature we are guessing. */
    LG("CONTAINER step 1: enumerating every endpoint in every state");
    enumerate_all_endpoints();
    if (!getenv("STRAY_CONTAINER_PROBE")) {
        LG("CONTAINER step 2 SKIPPED: scePadGetContainerIdInformation's signature is a "
           "guess and a previous run died in this window. Set STRAY_CONTAINER_PROBE=1 to try it.");
        return 0;
    }
    LG("CONTAINER step 2: calling scePadGetContainerIdInformation (signature UNCONFIRMED)");
    genfn ci = resolve("scePadGetContainerIdInformation");
    if (ci) {
        for (int i = 0; i < g_openCount; i++) {
            unsigned char buf[256];
            memset(buf, 0, sizeof buf);
            long long r = ci(g_slotHandle[i], (unsigned long long)(uintptr_t)buf, sizeof buf, 0);
            char hex[200]; int o = 0;
            for (int k = 0; k < 48 && o < (int)sizeof hex - 4; k++)
                o += snprintf(hex + o, sizeof hex - o, "%02x", buf[k]);
            /* it may be a wide string device path, so try that reading too */
            char asc[130]; int n = 0;
            for (int k = 0; k < 128 && n < 128; k += 2)
                asc[n++] = (buf[k] >= 32 && buf[k] < 127) ? (char)buf[k] : '.';
            asc[n] = 0;
            LG("CONTAINER slot user=%llu handle=0x%llX ret=0x%08X", g_slotUser[i], g_slotHandle[i], (unsigned)r);
            LG("   raw=%s", hex);
            LG("   wide-as-ascii=%s", asc);
        }
    } else LG("CONTAINER scePadGetContainerIdInformation missing");

    return 0;
}


static DWORD WINAPI gethandle_probe(LPVOID unused)
{
    (void)unused;
    Sleep(12000);
    genfn gh   = resolve("scePadGetHandle");
    genfn info = resolve("scePadGetControllerInformation");
    LG("SLOTS the game opened %d pad slot(s)", g_openCount);
    for (int i = 0; i < g_openCount; i++) {
        unsigned long long u = g_slotUser[i], h = g_slotHandle[i];
        long long got = gh ? gh(u, 0, 0, 0) : -1;
        unsigned char buf[64];
        memset(buf, 0, sizeof buf);
        long long ir = info ? info(h, (unsigned long long)(uintptr_t)buf, 0, 0) : -1;
        char hex[160]; int o = 0;
        for (int k = 0; k < 24 && o < (int)sizeof hex - 4; k++)
            o += snprintf(hex + o, sizeof hex - o, "%02x", buf[k]);
        LG("SLOT user=%llu handle=0x%llX getHandle=0x%llX%s info=0x%08X raw=%s",
           u, h, (unsigned long long)got,
           ((unsigned long long)got == h) ? " MATCH" : " MISMATCH",
           (unsigned)ir, hex);
    }
    /* Prove which slot drives hardware: buzz each in turn, clearly separated. */
    genfn sv = resolve("scePadSetVibration");
    if (sv) {
        for (int i = 0; i < g_openCount; i++) {
            unsigned char on[2] = { 120, 120 }, off[2] = { 0, 0 };
            LG("SLOTS buzzing user=%llu handle=0x%llX now", g_slotUser[i], g_slotHandle[i]);
            sv(g_slotHandle[i], (unsigned long long)(uintptr_t)on, 0, 0);
            Sleep(600);
            sv(g_slotHandle[i], (unsigned long long)(uintptr_t)off, 0, 0);
            Sleep(1400);
        }
        LG("SLOTS buzz sweep done");
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
	(void)h; (void)r;
	if (reason == DLL_PROCESS_ATTACH) InitializeCriticalSection(&g_cs);
	return TRUE;
}
