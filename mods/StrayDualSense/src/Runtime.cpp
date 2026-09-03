#include "Runtime.hpp"

#include "AssetName.hpp"
#include "Log.hpp"
#include "Platform.hpp"
#include "Version.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace sds {
namespace {

// Resolve a configured relative directory against the game's Binaries/Win64 first (where the
// assets are generated) and the mod's own directory second. Never hardcoded; always logged,
// because "the asset was not found" and "we looked in the wrong place" are the same symptom
// otherwise.
std::wstring ResolveDir(const std::string& configured, const std::wstring& gameDir,
                        const std::wstring& modDir, const char* what)
{
    const std::wstring rel = Widen(configured);
    const bool absolute = rel.size() > 1 && (rel[1] == L':' || rel[0] == L'\\' || rel[0] == L'/');
    if (absolute)
    {
        std::wstring p = rel;
        if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p += L'\\';
        SDS_LOG_INFO("assets: %s -> %ls (absolute, exists=%d)", what, p.c_str(),
                     DirectoryExists(p) ? 1 : 0);
        return p;
    }
    const std::wstring a = gameDir + rel + L"\\";
    const std::wstring b = modDir + rel + L"\\";
    const bool aOk = DirectoryExists(a);
    const bool bOk = DirectoryExists(b);
    SDS_LOG_INFO("assets: %s candidates: [game] %ls exists=%d | [mod] %ls exists=%d",
                 what, a.c_str(), aOk ? 1 : 0, b.c_str(), bOk ? 1 : 0);
    if (aOk) return a;
    if (bOk) return b;
    SDS_LOG_ERROR("assets: neither candidate for '%s' exists. Generate them with "
                  "tools/dualsense/extract_assets.sh and wavegen.sh.", what);
    return a;   // keep the game-dir path so the per-asset error names the expected location
}

// While no pad has been adopted, probe this fast and for this long. 120 probes at 1 s is two
// minutes, which comfortably covers a load into gameplay.
// The peak that counts as "the engine really is rendering haptics into this submix". Well above
// dither and well below anything audible/feelable.
constexpr float        kSubmixLiveThreshold = 1.0e-4f;
constexpr float        kFastPadPollSeconds = 1.0f;
constexpr unsigned long kFastPadProbes     = 120;

// The directory above `dir`, with its trailing separator kept. Empty if there is none.
// <Mods>/StrayDualSense/dlls/ -> <Mods>/StrayDualSense/
std::wstring ParentDir(const std::wstring& dir)
{
    if (dir.size() < 2)
        return {};
    const size_t end = dir.find_last_of(L"\\/", dir.size() - 2);
    if (end == std::wstring::npos)
        return {};
    return dir.substr(0, end + 1);
}

} // namespace

Runtime& Rt()
{
    static Runtime instance;
    return instance;
}

bool Runtime::LoadLoopList(LoopList& list, const std::string& fileName, const char* what)
{
    // Game dir first, mod dir second — the same order as the asset directories.
    for (const std::wstring& dir : { m_gameDir, m_modDir })
    {
        if (dir.empty()) continue;
        const std::wstring   path = dir + Widen(fileName);
        std::vector<uint8_t> bytes;
        if (!ReadWholeFile(path, bytes))
            continue;
        list.Parse(std::string(bytes.begin(), bytes.end()));
        SDS_LOG_INFO("loops: %s -> %ls (%zu looping asset(s))", what, path.c_str(), list.Count());
        return true;
    }
    return false;
}

void Runtime::LoadLoopLists()
{
    if (!LoadLoopList(m_hapticLoops, m_config.hapticLoopsFile, "haptic"))
    {
        // Loud: without the list every asset is a one-shot, so the purr and the rain stop after
        // one pass. That is the safe direction (a bump cannot buzz forever) but it is wrong.
        SDS_LOG_ERROR("loops: haptic '%s' not found in the game dir or the mod dir. EVERY haptic "
                      "asset will play as a ONE-SHOT; loops (purr, rain, scratch) will end after "
                      "one pass. Generate it with tools/dualsense/extract_assets.sh + wavegen.sh.",
                      m_config.hapticLoopsFile.c_str());
    }
    if (!LoadLoopList(m_spkLoops, m_config.spkLoopsFile, "spk"))
    {
        // extract_assets.sh writes ONE list covering every controller-class SoundWave, both
        // families; wavegen.sh splits it. Accept the combined list rather than silencing the
        // purr's loop over a missing split file.
        if (LoadLoopList(m_spkLoops, m_config.hapticLoopsFile, "spk (from the combined haptic list)"))
            SDS_LOG_INFO("loops: '%s' absent; the speaker is using '%s', which lists every "
                         "controller-class asset", m_config.spkLoopsFile.c_str(),
                         m_config.hapticLoopsFile.c_str());
        else
            SDS_LOG_ERROR("loops: spk '%s' not found (nor '%s'). EVERY speaker asset will play "
                          "as a ONE-SHOT; the purr will end after one pass.",
                          m_config.spkLoopsFile.c_str(), m_config.hapticLoopsFile.c_str());
    }
}

void Runtime::Startup(const void* addressInsideThisModule)
{
    if (m_started.load(std::memory_order_acquire))
        return;

    m_gameDir = GameBinariesDir();
    m_modDir  = ModuleDir(addressInsideThisModule);

    // The log goes next to the game binaries, alongside stray-dlss.log, because that is the
    // directory the user already collects from.
    const std::wstring logDir = m_gameDir.empty() ? m_modDir : m_gameDir;
    Log::Open(logDir + L"stray-dualsense.log", LogLevel::Info);

    SDS_LOG_INFO("StrayDualSense %s starting", SDS_VERSION_STRING);
    SDS_LOG_INFO("  game binaries dir: %ls", m_gameDir.c_str());
    SDS_LOG_INFO("  mod dir          : %ls", m_modDir.c_str());

    // MEASURED 2026-09-02: the first live run of the submix spike silently used
    // HapticSource=assets because the deploy script wrote the ini to the mod's ROOT
    // (<Mods>/StrayDualSense/) while only the DLL's own directory (<Mods>/StrayDualSense/dlls/)
    // and the game directory were searched. The log said which file it loaded, so it was
    // diagnosable — but the conventional place for a UE4SS mod's config IS the mod root, so
    // the search now includes it and EVERY candidate is logged whether it hit or missed.
    m_configPath.clear();
    for (const std::wstring& dir : { m_modDir, ParentDir(m_modDir), logDir })
    {
        if (dir.empty())
            continue;
        const std::wstring candidate = dir + L"StrayDualSense.ini";
        if (m_config.Load(candidate))
        {
            m_configPath = candidate;
            SDS_LOG_INFO("config: LOADED %ls", candidate.c_str());
            break;
        }
        SDS_LOG_INFO("config: not at %ls", candidate.c_str());
    }
    if (m_configPath.empty())
        SDS_LOG_INFO("config: no StrayDualSense.ini in any of those; using built-in defaults. "
                     "Every default is documented in mods/StrayDualSense/StrayDualSense.ini.");
    Log::SetMinLevel(m_config.logLevel);
    m_config.LogSummary("loaded");

    if (!m_config.enabled)
        SDS_LOG_WARN("Enabled=0: hooks will still register but nothing reaches the pad.");

    m_hapticDir = ResolveDir(m_config.hapticDir, m_gameDir, m_modDir, "haptic");
    m_spkDir    = ResolveDir(m_config.spkDir,    m_gameDir, m_modDir, "spk");
    LoadLoopLists();

    m_hidMode.Start(m_config);
    m_triggers.Start(m_pad, m_config);
    // The coil path re-asserts waveform mode right before every waveform: libScePad's own
    // trigger reports carry the same flag byte and may have flipped it back since the last
    // periodic write (§12).
    m_haptics.Start(kCoilRoute, m_config.endpointMatch, m_hapticDir,
                    [this] { m_hidMode.AssertNow("before waveform"); });
    m_speaker.Start(kSpeakerRoute, m_config.endpointMatch, m_spkDir, nullptr);

    StartSubmix();

    m_padThreadRunning.store(true, std::memory_order_release);
    m_padThread = std::thread(&Runtime::PadThreadMain, this);

    m_lastStatusMs = NowMs();
    m_lastReloadMs = m_lastStatusMs;
    m_lastSubmixStatusMs = m_lastStatusMs;
    m_started.store(true, std::memory_order_release);
}

// ---- the submix spike -------------------------------------------------------------------
//
// SPIKE, and nothing about it has run in the game. It answers ONE question: does UE's own
// audio engine hand us the vibration submix already mixed? If it does, the concurrency limit
// (one playback slot, so touching the cat kills the rain), the asset extraction, the loop
// list and the fades all become the engine's problem instead of ours.

void Runtime::StartSubmix()
{
    if (!m_config.SubmixTapWanted())
    {
        SDS_LOG_INFO("submix: HapticSource=assets, so no submix tap is created. The shipped "
                     "behaviour is unchanged.");
        return;
    }

    const std::wstring dir = m_gameDir.empty() ? m_modDir : m_gameDir;
    m_submixStatusFile = dir + Widen(m_config.submixStatusFile);
    m_submixStatusPath = Narrow(m_submixStatusFile);

    // Sized at the default rate; the ring holds frames, and the engine's real rate only
    // changes how many milliseconds that is. Off by a few ms is irrelevant here.
    const std::size_t ringFrames =
        static_cast<std::size_t>(submix::kSubmixDefaultRate) *
        static_cast<std::size_t>(m_config.submixRingMs) / 1000u;
    m_submixRing.Init(ringFrames);

    m_tapVibration = submix::Tap::Create("vibration");
    if (m_tapVibration == nullptr)
    {
        SDS_LOG_ERROR("submix: the listener page could not be allocated; the tap is DEAD for "
                      "this session and the coils will stay on the asset path.");
        return;
    }
    if (m_config.SubmixDrivesCoils())
        m_tapVibration->SetRing(&m_submixRing);

    if (m_config.submixProbeMaster)
    {
        // Meter only: never attached to the ring, so the game's whole soundtrack can never
        // reach the coils through it.
        m_tapMaster = submix::Tap::Create("master-probe");
    }

    SDS_LOG_INFO("submix: HapticSource=%s, ring %zu frames (%d ms), probeMaster=%d, numbers "
                 "every %.1fs to '%s'. The listener is registered lazily from the GAME THREAD, "
                 "inside the first UFunction hook that fires - reading a UObject from UE4SS's "
                 "update thread is an unsynchronised cross-thread read and this project does "
                 "not do it.",
                 m_config.HapticSourceName(), m_submixRing.CapacityFrames(),
                 m_config.submixRingMs, m_config.submixProbeMaster ? 1 : 0,
                 static_cast<double>(m_config.submixStatusSeconds), m_submixStatusPath.c_str());

    // The sink is NOT started here, nor at bind: it opens the pad endpoint only at the
    // HANDOVER, when the tap has carried a real signal. The 2026-09-03 run streamed 36 million
    // frames of pure underrun to the pad from a bound tap that was never called once.
    switch (m_config.hapticSource)
    {
    case HapticSource::Submix:
        SDS_LOG_WARN("submix: HapticSource=submix is STRICT: the asset haptic path will NOT play. "
                     "Until the tap carries a real signal the coils are SILENT, and the log says "
                     "so every %.0fs. Anything you feel in this mode came from the submix. Use "
                     "HapticSource=submix-fallback to keep the asset path meanwhile.",
                     static_cast<double>(m_config.submixWarnSeconds));
        break;
    case HapticSource::SubmixFallback:
        SDS_LOG_WARN("submix: HapticSource=submix-fallback: the ASSET path drives the coils until "
                     "the tap is bound AND carrying signal. WHAT YOU FEEL BEFORE THE HANDOVER IS "
                     "THE ASSET PATH, NOT THE SUBMIX - every COILS: line says which.");
        break;
    default:
        SDS_LOG_INFO("submix: HapticSource=measure, so NOTHING from the submix reaches the "
                     "pad. The asset path still drives the coils; this run only answers "
                     "whether the engine is mixing haptics for us.");
        break;
    }
}

CoilFacts Runtime::CoilFactsNow() const
{
    CoilFacts f;
    f.mode           = m_config.hapticSource;
    f.hapticsEnabled = m_config.enabled && m_config.haptics;
    f.padVibration   = m_padVibrationEnabled.load(std::memory_order_relaxed);
    f.tapCreated     = m_tapVibration != nullptr;
    f.tapRefused     = m_submixRefused.load(std::memory_order_acquire);
    f.tapBound       = m_submixBound.load(std::memory_order_acquire) && !f.tapRefused;
    f.tapCallbacks   = m_tapVibration != nullptr ? m_tapVibration->Stats().callbacks : 0;
    f.tapLive        = m_submixLive.load(std::memory_order_acquire);
    return f;
}

bool Runtime::SubmixWantsBinding() const
{
    return m_config.SubmixTapWanted() && m_tapVibration != nullptr &&
           !m_submixBound.load(std::memory_order_acquire);
}

bool Runtime::BindSubmixTap(const void* worldObject, const void* engineObject,
                            void* submixObject, bool submixObjectResolved,
                            void* rerouteMasterObject, void* rerouteParentObject,
                            const void* imageBase, std::size_t imageSize)
{
    if (!SubmixWantsBinding())
        return m_submixBound.load(std::memory_order_acquire);

    const int attempt = m_submixBindAttempts.fetch_add(1, std::memory_order_relaxed) + 1;

    // Refuse rather than register on the wrong thing: a null submix means MASTER to the
    // engine, so an unresolved USoundSubmix would silently put the game's entire soundtrack
    // on the voice coils. Only the literal "master" may pass null.
    const bool wantMaster = m_config.submixPath == "master";
    if (!wantMaster && !submixObjectResolved)
    {
        if (attempt == 1 || attempt % 20 == 0)
            SDS_LOG_WARN("submix: '%s' has not loaded yet (attempt %d). NOT registering with a "
                         "null submix - the engine reads that as the MASTER submix and the "
                         "whole soundtrack would go to the coils.",
                         m_config.submixPath.c_str(), attempt);
        return false;
    }

    submix::DiscoveryInput in;
    in.worldObject  = worldObject;
    in.engineObject = engineObject;
    in.imageBase    = imageBase;
    in.imageSize    = imageSize;
    in.useWorld     = m_config.submixDeviceSource != "engine";
    in.useEngine    = m_config.submixDeviceSource != "world";
    // This runs once a second until it binds, and the log is this project's only feedback
    // channel, so the candidate dump follows the same 1-then-every-20 cadence as the WARN
    // below rather than writing several lines a second for a whole session.
    in.logCandidates = (attempt == 1 || attempt % 20 == 0);
    in.scanBytes     = static_cast<std::size_t>(m_config.submixScanBytes);
    in.dumpWords     = m_config.submixDumpWords;

    const submix::DiscoveryResult d = submix::FindAudioDevice(in);
    if (!d.ok)
    {
        if (attempt == 1 || attempt % 20 == 0)
            SDS_LOG_WARN("submix: FAudioDevice not found (attempt %d): %s. World=%p Engine=%p "
                         "image=%p+0x%zX candidates=%zu uobjectTest=%d", attempt,
                         d.refusal != nullptr ? d.refusal : "no reason recorded",
                         worldObject, engineObject, imageBase, imageSize, d.candidates.size(),
                         d.uobjectTestUsable ? 1 : 0);
        return false;
    }

    SDS_LOG_INFO("submix: FAudioDevice %p (vtable %p, deviceId %u, sampleRate %d) found at "
                 "%s+0x%zX. Accepted because %s.",
                 d.device, d.vtable, d.deviceId, d.sampleRate,
                 d.fromWorld ? "UWorld" : "UEngine", d.offset,
                 d.why != nullptr ? d.why : "no reason recorded");
    submix::LogVtable(d.device, imageBase, imageSize);

    // THE REROUTE, before the listener: the engine rebuilds the submix links on the audio
    // thread in the order the calls are queued, and the listener registration goes through the
    // same queue, so by the time our listener is attached the tree already renders.
    if (m_config.submixReroute)
    {
        if (rerouteMasterObject == nullptr || rerouteParentObject == nullptr)
        {
            SDS_LOG_ERROR("submix: SubmixReroute=1 but the objects did not resolve (master=%p "
                          "parent=%p). NOT rerouting; the tap will bind to a subtree the engine "
                          "never renders, and the status line will say so.",
                          rerouteMasterObject, rerouteParentObject);
        }
        else
        {
            const int   slot = m_config.submixRegisterSoundSubmixSlot;
            const char* why  = nullptr;
            SDS_LOG_WARN("submix: REROUTE - about to call vtable slot %d as "
                         "FAudioDevice::RegisterSoundSubmix(parent=%p, bInit=true) then "
                         "(master=%p, bInit=true). AudioDevice.h:854 puts it two slots below "
                         "RegisterSubmixBufferListener (%d); if the game dies HERE, that count "
                         "is the suspect - set SubmixRegisterSoundSubmixSlot from the vtable "
                         "dump above.",
                         slot, rerouteParentObject, rerouteMasterObject, m_config.submixRegisterSlot);
            const bool okParent = submix::CallRegisterSoundSubmix(
                d.device, slot, rerouteParentObject, true, imageBase, imageSize, &why);
            if (!okParent)
                SDS_LOG_ERROR("submix: REROUTE refused on the parent: %s", why != nullptr ? why : "?");
            const bool okMaster = okParent && submix::CallRegisterSoundSubmix(
                d.device, slot, rerouteMasterObject, true, imageBase, imageSize, &why);
            if (okParent && !okMaster)
                SDS_LOG_ERROR("submix: REROUTE refused on the master: %s", why != nullptr ? why : "?");
            if (okParent && okMaster)
            {
                m_submixRerouted.store(true, std::memory_order_release);
                SDS_LOG_INFO("submix: REROUTE submitted. The engine re-links '%s' under '%s' on "
                             "the audio thread; if it worked, the SUBMIX line below turns from "
                             "'NO CALLBACKS' into a steady callback rate within a second - "
                             "SILENT callbacks are the proof the subtree now renders, a real "
                             "signal needs a haptic to play.",
                             m_config.submixRerouteMaster.c_str(),
                             m_config.submixRerouteParent.c_str());
            }
        }
    }

    const char* whyNot = nullptr;
    SDS_LOG_WARN("submix: about to call vtable slot %d as "
                 "FAudioDevice::RegisterSubmixBufferListener(listener=%p, submix=%p). The slot "
                 "is derived from stock UE 4.27.2 (see src/SubmixDiscovery.hpp); Stray is a "
                 "LICENSEE build, so if the game dies HERE, that derivation is the suspect - "
                 "read the vtable dump above and set SubmixRegisterSlot.",
                 m_config.submixRegisterSlot, m_tapVibration->ListenerPointer(),
                 wantMaster ? nullptr : submixObject);

    if (!submix::CallRegisterSubmixBufferListener(
            d.device, m_config.submixRegisterSlot, m_tapVibration->ListenerPointer(),
            wantMaster ? nullptr : submixObject, imageBase, imageSize, &whyNot))
    {
        SDS_LOG_ERROR("submix: REFUSED to call the register slot: %s. The tap is dead for this "
                      "session.", whyNot != nullptr ? whyNot : "no reason recorded");
        m_submixRefused.store(true, std::memory_order_release);
        m_submixBound.store(true, std::memory_order_release);   // stop retrying; it is a refusal
        return false;
    }

    if (m_tapMaster != nullptr)
    {
        const char* masterWhy = nullptr;
        if (!submix::CallRegisterSubmixBufferListener(
                d.device, m_config.submixRegisterSlot, m_tapMaster->ListenerPointer(),
                nullptr, imageBase, imageSize, &masterWhy))
        {
            SDS_LOG_WARN("submix: the master probe could not be registered: %s",
                         masterWhy != nullptr ? masterWhy : "no reason recorded");
        }
    }

    m_submixDevice = d.device;
    m_submixBound.store(true, std::memory_order_release);
    SDS_LOG_INFO("submix: registration submitted for '%s'. It is ASYNCHRONOUS - the engine "
                 "runs it on the audio thread (AudioMixerDevice.cpp:2405) - so callbacks are "
                 "expected within a frame or two, not immediately. Watch the SUBMIX line.",
                 wantMaster ? "the MASTER submix" : m_config.submixPath.c_str());
    return true;
}

void Runtime::Shutdown()
{
    if (!m_started.exchange(false, std::memory_order_acq_rel))
        return;
    SDS_LOG_INFO("StrayDualSense shutting down");

    m_padThreadRunning.store(false, std::memory_order_release);
    if (m_padThread.joinable())
        m_padThread.join();

    // Detach BEFORE anything else: the taps are the only things the engine can still call
    // into. They are never deleted (SubmixTap.hpp) — after this the leaked trampoline is a
    // `ret` and the engine can hold the pointer for the rest of the process.
    if (m_tapVibration != nullptr) m_tapVibration->Detach();
    if (m_tapMaster    != nullptr) m_tapMaster->Detach();
    m_submixSink.Shutdown();

    m_haptics.Shutdown();
    m_speaker.Shutdown();
    m_triggers.Shutdown();   // releases the triggers on the way out
    m_hidMode.Shutdown();    // hands the coils back to rumble emulation

    LogStatus();
    Log::Close();
}

void Runtime::PadThreadMain()
{
    // libScePad is DELAY-loaded (import #9), so at mod init it is usually not mapped yet.
    // Poll until it is; log the wait so "nothing happened" is never mysterious.
    int  waited    = 0;
    bool announced = false;
    while (m_padThreadRunning.load(std::memory_order_acquire))
    {
        if (!m_pad.IsBound())
        {
            if (m_pad.Bind())
            {
                m_pad.SelectPad(m_config.padUserId);
            }
            else
            {
                waited += 500;
                if (!announced && waited >= 30000)
                {
                    announced = true;
                    SDS_LOG_WARN("libScePad.dll has not been loaded by the game after 30 s. It "
                                 "is a DELAY-LOAD import, so it only maps when the game first "
                                 "touches the pad API - if the DualSense is not visible to the "
                                 "prefix as a HID device that may never happen. Check that "
                                 "Steam's *global* PlayStation Controller Support is OFF.");
                }
                ::Sleep(500);
                continue;
            }
        }
        else if (m_config.padPollSeconds > 0.0f)
        {
            m_pad.RefreshIfLost();
        }

        // WHILE THERE IS NO PAD, POLL FAST. MEASURED 2026-09-02: two launches of the same
        // build, one adopted slot 1 and transmitted triggers, the next found every slot
        // handing back a handle and an all-zero information struct. libScePad only knows about
        // a pad the GAME has opened, and this thread starts as soon as the module maps, so the
        // probe RACES the game's own scePadOpen. A 2 s cadence turns a race we could win into
        // one we lose for the whole session; 1 s for the first two minutes closes it, and the
        // steady-state cadence takes over afterwards so a genuinely absent pad is not polled
        // hard forever.
        float pollSeconds = m_config.padPollSeconds;
        if (!m_pad.HasPad() && m_pad.Probes() < kFastPadProbes)
            pollSeconds = kFastPadPollSeconds;

        const DWORD sleepMs = static_cast<DWORD>(std::max(0.25f, pollSeconds) * 1000.0f);
        for (DWORD slept = 0; slept < sleepMs && m_padThreadRunning.load(std::memory_order_acquire);
             slept += 100)
            ::Sleep(100);
    }
    SDS_LOG_INFO("pad watcher exiting");
}

void Runtime::Tick()
{
    if (!m_started.load(std::memory_order_acquire))
        return;
    const uint64_t now = NowMs();

    if (m_config.configReloadSeconds > 0.0f &&
        now - m_lastReloadMs >= static_cast<uint64_t>(m_config.configReloadSeconds * 1000.0f))
    {
        m_lastReloadMs = now;
        m_config.ReloadIfChanged(m_configPath);
    }

    if (m_config.statusSeconds > 0.0f &&
        now - m_lastStatusMs >= static_cast<uint64_t>(m_config.statusSeconds * 1000.0f))
    {
        m_lastStatusMs = now;
        LogStatus();
    }

    if (m_tapVibration != nullptr && m_config.submixStatusSeconds > 0.0f &&
        now - m_lastSubmixStatusMs >=
            static_cast<uint64_t>(m_config.submixStatusSeconds * 1000.0f))
    {
        m_submixStatusWindowMs = now - m_lastSubmixStatusMs;
        m_lastSubmixStatusMs   = now;
        SubmixStatus();
    }

    SubmixWarnIfDue(now);
}

// The LOUD, PERIODIC half of the coil-owner verdict. A configuration that asked for the
// submix and is not getting it is a problem every N seconds, not an INFO line once a second.
void Runtime::SubmixWarnIfDue(uint64_t now)
{
    if (m_config.submixWarnSeconds <= 0.0f)
        return;
    if (now - m_lastSubmixWarnMs < static_cast<uint64_t>(m_config.submixWarnSeconds * 1000.0f))
        return;
    const CoilVerdict v = Coils();
    if (!v.warn)
        return;
    m_lastSubmixWarnMs = now;
    SDS_LOG_WARN("%s | %s | HapticSource=%s", v.headline, v.detail, m_config.HapticSourceName());
}

// The handover. Callbacks alone are not enough: a submix that renders pure silence would
// take the coils away from the asset path and give nothing back. Only a real signal proves
// the engine is putting haptics in there, so that is the trigger - and it is when the sink
// opens the pad endpoint, not a moment earlier.
void Runtime::StartSinkAtHandover(float peak)
{
    if (!m_config.SubmixDrivesCoils())
        return;
    if (m_submixLive.exchange(true, std::memory_order_acq_rel))
        return;
    SDS_LOG_WARN("submix: FIRST REAL SIGNAL (peak %.5f) - HANDOVER: the SUBMIX now drives the "
                 "coils%s.", static_cast<double>(peak),
                 m_config.hapticSource == HapticSource::SubmixFallback
                     ? " and the asset path stands down" : "");
    m_haptics.Stop(0.0f);
    if (!m_sinkStarted)
    {
        m_sinkStarted = true;
        m_submixSink.Start(&m_submixRing, m_config.endpointMatch, m_config,
                           [this] { m_hidMode.AssertNow("submix: silence -> signal"); });
        m_submixSink.SetGain(m_padVibrationEnabled.load() ? m_config.submixGain : 0.0f);
    }
}

// The NUMBERS PROOF. Silence when nothing plays, a live signal during rain, and a HIGHER
// signal when rain and a purr overlap, is the whole argument that the engine is mixing
// concurrent haptics for us — which is the thing our own one-slot player cannot do.
//
// It is written to a file as well as the log so it can be read with `cat` over ssh, with no
// overlay and no log grepping. And when the tap never fires it SAYS SO: a listener that was
// never called and a submix that is genuinely silent produce identical audio, so they must
// not produce identical text.
void Runtime::SubmixStatus()
{
    const submix::TapStats     vib   = m_tapVibration->Stats();
    const submix::LevelReading level = m_tapVibration->TakeLevels();
    const double seconds = m_submixStatusWindowMs > 0
                               ? static_cast<double>(m_submixStatusWindowMs) / 1000.0
                               : 1.0;
    const uint64_t deltaCallbacks =
        vib.callbacks >= m_lastSubmixCallbacks ? vib.callbacks - m_lastSubmixCallbacks : 0;
    m_lastSubmixCallbacks = vib.callbacks;
    const double cbPerSec = static_cast<double>(deltaCallbacks) / seconds;

    const CoilVerdict verdict = Coils();

    char line[1100];
    if (level.frames == 0)
    {
        const uint64_t now  = NowMs();
        const uint64_t last = vib.lastCallbackMs;
        char since[80];
        if (last == 0)
            std::snprintf(since, sizeof(since), "NEVER - the listener has never been called");
        else
            std::snprintf(since, sizeof(since), "%.1fs ago",
                          static_cast<double>(now - last) / 1000.0);
        std::snprintf(line, sizeof(line),
                      "%s | SUBMIX %s bound=%d NO CALLBACKS in the last %.1fs (total=%llu, last %s) "
                      "| %s",
                      verdict.headline,
                      m_config.HapticSourceName(), m_submixBound.load() ? 1 : 0, seconds,
                      static_cast<unsigned long long>(vib.callbacks), since,
                      m_submixBound.load()
                          ? "the tap IS registered, so the engine is rendering nothing into "
                            "this submix"
                          : "the tap is NOT registered yet (see the WARN lines above)");
    }
    else
    {
        std::snprintf(line, sizeof(line),
                      "%s | SUBMIX %s bound=%d live=%d cb=%llu (%.1f/s) ch=%d rate=%d "
                      "frames/cb=%d peak=%.5f rms=%.5f bad=%llu",
                      verdict.headline,
                      m_config.HapticSourceName(), m_submixBound.load() ? 1 : 0,
                      m_submixLive.load() ? 1 : 0,
                      static_cast<unsigned long long>(vib.callbacks), cbPerSec,
                      vib.lastNumChannels, vib.lastSampleRate,
                      vib.lastNumChannels > 0 ? vib.lastNumSamples / vib.lastNumChannels : 0,
                      static_cast<double>(level.peak), static_cast<double>(level.rms),
                      static_cast<unsigned long long>(vib.badCallbacks));
        // Learned from the engine, never assumed: a project ini can change the rate.
        if (vib.lastSampleRate > 0)
            m_submixSink.SetSourceRate(static_cast<uint32_t>(vib.lastSampleRate));

        if (level.peak >= kSubmixLiveThreshold)
            StartSinkAtHandover(level.peak);
    }

    char master[220] = "";
    if (m_tapMaster != nullptr)
    {
        const submix::TapStats     ms = m_tapMaster->Stats();
        const submix::LevelReading ml = m_tapMaster->TakeLevels();
        std::snprintf(master, sizeof(master), " | master-probe cb=%llu peak=%.5f%s",
                      static_cast<unsigned long long>(ms.callbacks),
                      static_cast<double>(ml.peak),
                      ms.callbacks == 0
                          ? "  <- the MASTER submix is silent too, so the TAP is broken, not "
                            "the game"
                          : "");
    }

    char rest[520];
    std::snprintf(rest, sizeof(rest),
                  "%s | rerouted=%d | ring fill=%zu/%zu drop=%llu under=%llu | sink open=%d '%s' %uch %uHz "
                  "frames=%llu fail=%llu",
                  master, m_submixRerouted.load() ? 1 : 0,
                  m_submixRing.Available(), m_submixRing.CapacityFrames(),
                  static_cast<unsigned long long>(m_submixRing.Dropped()),
                  static_cast<unsigned long long>(m_submixRing.Underruns()),
                  m_submixSink.StreamOpen() ? 1 : 0, m_submixSink.EndpointName().c_str(),
                  m_submixSink.EndpointChannels(), m_submixSink.EndpointRate(),
                  static_cast<unsigned long long>(m_submixSink.FramesWritten()),
                  static_cast<unsigned long long>(m_submixSink.Failures()));

    SDS_LOG_INFO("%s%s", line, rest);

    if (!m_submixStatusFile.empty())
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, m_submixStatusFile.c_str(), L"w") == 0 && f != nullptr)
        {
            std::fprintf(f, "%s%s\n", line, rest);
            std::fclose(f);
        }
    }
}

void Runtime::LogStatus()
{
    const TriggerEffect fx = m_triggers.Effect();
    const CoilVerdict   coils = Coils();
    SDS_LOG_INFO("STATUS coils=%s (%s) pad=%s(user=%d handle=0x%X probes=%lu misses=%lu) "
                 "hid[open=%d writes=%lu fail=%lu] "
                 "trig[events=%lu tx=%lu ok=%lu fail=%lu L=%d R=%d effect=%d/%u/%u/%u %s] "
                 "hap[starts=%lu played=%lu done=%lu missing=%lu fail=%lu endpoint=%d now='%s' "
                 "compStops ok=%lu ignored=%lu padVibe=%d] "
                 "spk[starts=%lu played=%lu missing=%lu fail=%lu endpoint=%d now='%s']",
                 CoilOwnerName(coils.owner), coils.detail,
                 m_pad.HasPad() ? "yes" : "NO", m_pad.UserId(), static_cast<unsigned>(m_pad.Handle()),
                 m_pad.Probes(), m_pad.ProbeMisses(),
                 m_hidMode.Opened() ? 1 : 0, m_hidMode.Writes(), m_hidMode.Failures(),
                 m_triggerEvents.load(), m_triggers.Transmits(), m_pad.TriggerOk(),
                 m_pad.TriggerFail(), m_triggers.Left() ? 1 : 0, m_triggers.Right() ? 1 : 0,
                 fx.mode, fx.value1, fx.value2, fx.value3,
                 m_effectFromGame.load() ? "(game)" : "(FALLBACK)",
                 m_vibrationStarts.load(), m_haptics.Started(), m_haptics.Finished(),
                 m_haptics.Missing(), m_haptics.Failures(), m_haptics.EndpointFound() ? 1 : 0,
                 m_haptics.CurrentName().c_str(), m_componentStopsHonoured.load(),
                 m_componentStopsIgnored.load(), m_padVibrationEnabled.load() ? 1 : 0,
                 m_speakerStarts.load(), m_speaker.Started(), m_speaker.Missing(),
                 m_speaker.Failures(), m_speaker.EndpointFound() ? 1 : 0,
                 m_speaker.CurrentName().c_str());
}

// ---- game intent ----------------------------------------------------------------------

void Runtime::OnTriggerEffectRead(const TriggerEffect& effect, bool ok)
{
    if (!ok)
    {
        if (!m_effectFromGame.load())
            SDS_LOG_WARN("trigger effect: m_scratchablePS5TriggerEffect could not be read; "
                         "using the FALLBACK game-space effect %d(%s) v=%u/%u/%u",
                         kFallbackTriggerEffect.mode, GameModeName(kFallbackTriggerEffect.mode),
                         kFallbackTriggerEffect.value1, kFallbackTriggerEffect.value2,
                         kFallbackTriggerEffect.value3);
        return;
    }
    const TriggerEffect previous = m_triggers.Effect();
    if (!m_effectFromGame.exchange(true) || previous != effect)
    {
        SDS_LOG_INFO("trigger effect (authored by the game): mode=%d(%s) v1=%u v2=%u v3=%u -> "
                     "Sony %s%s", effect.mode, GameModeName(effect.mode), effect.value1,
                     effect.value2, effect.value3, SonyModeName(ToSonyMode(effect.mode)),
                     IsKnownGameMode(effect.mode) ? "" : "  [UNKNOWN game mode, mapped to Feedback]");
    }
    m_triggers.SetEffect(effect);
}

void Runtime::OnTriggerActivated(bool state, int side)
{
    m_triggerEvents.fetch_add(1, std::memory_order_relaxed);
    if (side != static_cast<int>(TriggerSide::Left) && side != static_cast<int>(TriggerSide::Right))
    {
        // EPS5TriggersSide is 0/1. Anything else means the parameter was read wrongly, which
        // is exactly the class of mistake that silently hardens one trigger forever.
        SDS_LOG_ERROR("SetPS5TriggerActivated: side=%d is not 0(Left) or 1(Right). The "
                      "parameter read is WRONG; ignoring.", side);
        return;
    }
    SDS_LOG_INFO("SetPS5TriggerActivated state=%d side=%d(%s)", state ? 1 : 0, side,
                 side == 0 ? "Left" : "Right");
    if (!m_config.enabled || !m_config.triggers)
        return;
    m_triggers.SetSide(static_cast<TriggerSide>(side), state);
}

void Runtime::OnUseStarted()
{
    SDS_LOG_DEBUG("_OnUseStarted");
}

void Runtime::OnAfterUseDone()
{
    SDS_LOG_DEBUG("_OnAfterUseDone -> release both triggers");
    m_triggers.ReleaseAll();
}

void Runtime::OnStartVibration(const VibrationStart& s)
{
    m_vibrationStarts.fetch_add(1, std::memory_order_relaxed);
    const std::string name         = ShortAssetName(s.soundFullName);
    const bool        viaComponent = !s.componentFullName.empty();

    // An absent Level and a Level of 0 need different fixes, so they are distinguished. And
    // component-attached vibrations arrive with Level=0.0: their level lives in the submix
    // send (PS5VibrationAttenuation: bAttenuate=False, send constant 1.0), so 0 on that path
    // means 1.0.
    float level = s.levelSeen ? s.level : 1.0f;
    if (viaComponent && level <= 0.0f)
        level = 1.0f;

    const bool loop = m_hapticLoops.Contains(name);   // the ASSET decides, never the caller
    SDS_LOG_INFO("StartPS5Vibration%s '%s' -> %s level=%.3f(seen=%d) fadeIn=%.2f loop=%d(asset)%s%s",
                 viaComponent ? "OnAudioComponent" : "", s.soundFullName.c_str(), name.c_str(),
                 static_cast<double>(level), s.levelSeen ? 1 : 0, static_cast<double>(s.fadeIn),
                 loop ? 1 : 0, viaComponent ? " component=" : "",
                 viaComponent ? s.componentFullName.c_str() : "");

    {
        std::lock_guard<std::mutex> lock(m_componentMutex);
        m_playingComponent = s.componentFullName;   // empty on the plain path
    }

    // The verdict decides. The call is still LOGGED above, because the log is how a null
    // submix result gets diagnosed — "the game asked for CatPurr2_VIBE and the submix stayed
    // silent" is a completely different finding from "the game never asked".
    const CoilVerdict verdict = Coils();
    if (!verdict.assetPathActive)
    {
        SDS_LOG_INFO("haptics: '%s' NOT played on the asset path: %s (%s)", name.c_str(),
                     verdict.headline, verdict.detail);
        return;
    }
    if (name.empty())
    {
        SDS_LOG_ERROR("StartPS5Vibration: could not derive an asset name from '%s'",
                      s.soundFullName.c_str());
        return;
    }
    m_haptics.Play(name, level, s.fadeIn, loop);
}

void Runtime::OnStopVibration(float fadeOut)
{
    SDS_LOG_INFO("StopPS5Vibration fadeOut=%.2f (global)", static_cast<double>(fadeOut));
    {
        std::lock_guard<std::mutex> lock(m_componentMutex);
        m_playingComponent.clear();
    }
    if (SubmixOwnsCoils())
        return;      // the engine's own fade-out is already in the submix we are listening to
    m_haptics.Stop(fadeOut);
}

void Runtime::OnStopVibrationOnComponent(const std::string& componentFullName, float fadeOut)
{
    // ~700 calls a session, most of them housekeeping for components that are not playing.
    // Only the owner of the haptic in flight may stop it.
    bool owner = false;
    {
        std::lock_guard<std::mutex> lock(m_componentMutex);
        owner = !m_playingComponent.empty() && m_playingComponent == componentFullName;
        if (owner)
            m_playingComponent.clear();
    }
    if (!owner)
    {
        m_componentStopsIgnored.fetch_add(1, std::memory_order_relaxed);
        SDS_LOG_DEBUG("StopPS5VibrationOnAudioComponent ignored: '%s' is not the playing "
                      "component", componentFullName.c_str());
        return;
    }
    m_componentStopsHonoured.fetch_add(1, std::memory_order_relaxed);
    SDS_LOG_INFO("StopPS5VibrationOnAudioComponent fadeOut=%.2f (owner) %s",
                 static_cast<double>(fadeOut), componentFullName.c_str());
    if (SubmixOwnsCoils())
        return;
    m_haptics.Stop(fadeOut);
}

void Runtime::OnSetVibrationLevel(float level)
{
    // ~60 Hz. Never log it per call.
    if (SubmixOwnsCoils())
        return;   // the level is already applied by the engine, upstream of the submix
    m_haptics.SetLevel(level);
}

void Runtime::OnStartControllerSound(const std::string& soundFullName, float level,
                                     bool levelSeen, float fadeIn)
{
    m_speakerStarts.fetch_add(1, std::memory_order_relaxed);
    const std::string name     = ShortAssetName(soundFullName);
    const float       useLevel = levelSeen ? level : 1.0f;
    const bool        loop     = m_spkLoops.Contains(name);
    SDS_LOG_INFO("StartPS5ControllerSound '%s' -> %s level=%.3f(seen=%d) fadeIn=%.2f loop=%d(asset)",
                 soundFullName.c_str(), name.c_str(), static_cast<double>(useLevel),
                 levelSeen ? 1 : 0, static_cast<double>(fadeIn), loop ? 1 : 0);
    if (!m_config.enabled || !m_config.speaker)
        return;
    if (name.empty())
    {
        SDS_LOG_ERROR("StartPS5ControllerSound: could not derive an asset name from '%s'",
                      soundFullName.c_str());
        return;
    }
    m_speaker.Play(name, useLevel, fadeIn, loop);
}

void Runtime::OnStopControllerSound(float fadeOut)
{
    SDS_LOG_INFO("StopPS5ControllerSound fadeOut=%.2f", static_cast<double>(fadeOut));
    m_speaker.Stop(fadeOut);
}

void Runtime::OnSetControllerSoundLevel(float level)
{
    m_speaker.SetLevel(level);
}

void Runtime::OnPadVibrationEnabled(bool enabled)
{
    const bool was = m_padVibrationEnabled.exchange(enabled, std::memory_order_relaxed);
    if (was == enabled)
        return;
    SDS_LOG_INFO("haptics: PadVibrationEnabled -> %d", enabled ? 1 : 0);
    if (!enabled)
        m_haptics.Stop(0.0f);
    // The game's only vibration control (§9) must gate the submix path too, and it is a gain
    // rather than a teardown: the tap keeps reporting numbers, which is what makes "the user
    // turned it off in the menu" distinguishable from "the tap died".
    if (m_config.SubmixDrivesCoils())
        m_submixSink.SetGain(enabled ? m_config.submixGain : 0.0f);
    SDS_LOG_INFO("%s", Coils().headline);
}

void Runtime::NoteHookRegistered(const char* name)
{
    SDS_LOG_INFO("hook registered: %s", name);
}

void Runtime::NoteHookMissing(const char* name)
{
    // A hook that never registers is the single most likely reason for "nothing happens", and
    // it is otherwise indistinguishable from "the game never called it".
    SDS_LOG_ERROR("hook NEVER REGISTERED: %s - that path of the mod is dead this session", name);
}

} // namespace sds
