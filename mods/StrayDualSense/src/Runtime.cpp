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

// The DEFAULT for Config::submixLiveThreshold, kept here as the documented constant the
// handover was originally written against. See Config.hpp for why it is low and what that
// costs in strict mode.
constexpr float        kSubmixLiveThreshold = 1.0e-4f;
// While no pad has been adopted, probe this fast and for this long. 120 probes at 1 s is two
// minutes, which comfortably covers a load into gameplay.
constexpr float        kFastPadPollSeconds = 1.0f;
constexpr unsigned long kFastPadProbes     = 120;
// How many times the reroute watchdog will re-submit before it stops and says so. A level
// load is a handful of events a session; a hundred is a structural fault being papered over.
constexpr unsigned long kMaxRerouteRearms  = 20;

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
    LoadLoopLists();

    m_hidMode.Start(m_config);
    m_triggers.Start(m_pad, m_config);
    // The coil path re-asserts waveform mode right before every waveform: libScePad's own
    // trigger reports carry the same flag byte and may have flipped it back since the last
    // periodic write (§12).
    m_haptics.Start(kCoilRoute, m_config.endpointMatch, m_hapticDir,
                    [this] { m_hidMode.AssertNow("before waveform"); });

    StartSubmix();

    m_padThreadRunning.store(true, std::memory_order_release);
    m_padThread = std::thread(&Runtime::PadThreadMain, this);

    m_lastStatusMs = NowMs();
    m_lastReloadMs = m_lastStatusMs;
    m_lastSubmixStatusMs = m_lastStatusMs;
    m_started.store(true, std::memory_order_release);
}

// ---- the submix taps --------------------------------------------------------------------
//
// Two lanes, one mechanism: the engine's own audio mixer hands us the vibration submix and
// the controller-speaker submix already mixed, faded, looped and levelled. Everything below
// runs the same for both; only the channel pair on the pad differs.

void Runtime::StartSubmix()
{
    m_coils.tag     = "vibration";
    m_coils.owner   = "COILS";
    m_speaker.tag   = "speaker";
    m_speaker.owner = "SPEAKER";

    if (!m_config.SubmixTapWanted())
    {
        SDS_LOG_INFO("submix: HapticSource=assets, so no submix tap is created. The shipped "
                     "coil behaviour is unchanged, and the SPEAKER IS SILENT: the speaker only "
                     "comes from the tap.");
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
    m_coils.ring.Init(ringFrames);
    m_speaker.ring.Init(ringFrames);

    m_coils.tap   = submix::Tap::Create(m_coils.tag);
    m_speaker.tap = submix::Tap::Create(m_speaker.tag);
    if (m_coils.tap == nullptr || m_speaker.tap == nullptr)
    {
        SDS_LOG_ERROR("submix: a listener page could not be allocated (vibration=%p speaker=%p); "
                      "the taps are DEAD for this session.",
                      static_cast<void*>(m_coils.tap), static_cast<void*>(m_speaker.tap));
        return;
    }
    // The rings are NOT attached here. MEASURED 2026-09-03 (strict submix, first run): the tap
    // filled its ring with silence from its first callback while nothing drained it, so at the
    // handover the ring sat full (16384/16384, 1.8 M frames dropped) and STAYED near full once
    // the sink pulled at exactly the production rate - about 300 ms between the engine's mix
    // and the coils. Each ring is attached at its lane's handover instead, empty, and the
    // sink drains both from the moment it opens.
    if (m_config.submixProbeMaster)
    {
        // Meter only: never attached to a ring, so the game's whole soundtrack can never
        // reach the pad through it.
        m_tapMaster = submix::Tap::Create("master-probe");
    }

    SDS_LOG_INFO("submix: HapticSource=%s, two taps ('%s' -> RL/RR, '%s' -> FL/FR), rings %zu "
                 "frames (%d ms), probeMaster=%d, numbers every %.1fs to '%s'. The listeners "
                 "are registered lazily from the GAME THREAD, inside the first UFunction hook "
                 "that fires - reading a UObject from UE4SS's update thread is an "
                 "unsynchronised cross-thread read and this project does not do it.",
                 m_config.HapticSourceName(), m_coils.tag, m_speaker.tag,
                 m_coils.ring.CapacityFrames(), m_config.submixRingMs,
                 m_config.submixProbeMaster ? 1 : 0,
                 static_cast<double>(m_config.submixStatusSeconds), m_submixStatusPath.c_str());

    // The sink is NOT started here, nor at bind: it opens the pad endpoint only at the first
    // HANDOVER, when a tap has carried a real signal. The 2026-09-03 run streamed 36 million
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
        SDS_LOG_INFO("submix: HapticSource=measure, so NOTHING from the vibration submix reaches "
                     "the coils. The asset path still drives them; this run only answers "
                     "whether the engine is mixing haptics for us. The SPEAKER lane still plays.");
        break;
    }
}

CoilFacts Runtime::CoilFactsNow() const
{
    CoilFacts f;
    f.mode           = m_config.hapticSource;
    f.hapticsEnabled = m_config.enabled && m_config.haptics;
    f.padVibration   = m_padVibrationEnabled.load(std::memory_order_relaxed);
    f.tapCreated     = m_coils.tap != nullptr;
    f.tapRefused     = m_submixRefused.load(std::memory_order_acquire);
    f.tapBound       = m_submixBound.load(std::memory_order_acquire) && !f.tapRefused;
    f.tapCallbacks   = m_coils.tap != nullptr ? m_coils.tap->Stats().callbacks : 0;
    f.tapLive        = m_coils.live.load(std::memory_order_acquire);
    return f;
}

LaneVerdict Runtime::Speaker() const
{
    LaneFacts f;
    f.enabled      = m_config.enabled && m_config.speaker && m_config.SubmixTapWanted();
    f.gameSwitch   = true;   // the game has no speaker switch
    f.tapCreated   = m_speaker.tap != nullptr;
    f.tapRefused   = m_submixRefused.load(std::memory_order_acquire);
    f.tapBound     = m_submixBound.load(std::memory_order_acquire) && !f.tapRefused;
    f.tapCallbacks = m_speaker.tap != nullptr ? m_speaker.tap->Stats().callbacks : 0;
    f.tapLive      = m_speaker.live.load(std::memory_order_acquire);
    return JudgeLane(f);
}

bool Runtime::SubmixWantsBinding() const
{
    if (!m_config.SubmixTapWanted() || m_coils.tap == nullptr || m_speaker.tap == nullptr)
        return false;
    // The REROUTE re-arm is the second reason to want the glue again, and it exists because
    // the reroute was only ever submitted ONCE. Both halves of it are gated on this function:
    // the glue's UObject writes (ParentSubmix, OutputVolume) and our RegisterSoundSubmix call.
    // So once bound, a submix graph rebuilt by a level load could never be repaired, which is
    // exactly the "worked in BaseMap, silent in 03_Slums" shape.
    return !m_submixBound.load(std::memory_order_acquire) ||
           m_rerouteRearmWanted.load(std::memory_order_acquire);
}

// Watchdog: the taps are bound and a subtree has STOPPED being rendered. Re-arm the reroute
// so the glue re-writes the UObject links and we re-submit RegisterSoundSubmix for every
// master. The rule itself is pure (SubmixWatch.hpp, StallWatchdog): never before a lane's
// first callback, any lane that once rendered and then stalls, one re-arm at a time, capped.
//
// THIS IS NOT A FALLBACK. It does not hand the coils to the asset path or soften strict mode;
// it repairs the one thing that makes strict mode unreliable. A silent pad stays a silent pad
// until the submix genuinely renders again.
void Runtime::RerouteWatchdog(uint64_t now)
{
    if (!m_config.submixReroute || m_config.submixRerouteWatchdogSeconds <= 0.0f)
        return;
    if (m_coils.tap == nullptr || m_speaker.tap == nullptr ||
        !m_submixBound.load(std::memory_order_acquire) ||
        m_submixRefused.load(std::memory_order_acquire))
        return;

    const uint64_t callbacks[2] = { m_coils.tap->Stats().callbacks,
                                    m_speaker.tap->Stats().callbacks };
    const uint64_t stalledMs =
        static_cast<uint64_t>(m_config.submixRerouteWatchdogSeconds * 1000.0f);
    const bool wasGivenUp = m_watchdog.GaveUp();
    if (!m_watchdog.Observe(callbacks, 2, now, stalledMs, kMaxRerouteRearms))
    {
        if (m_watchdog.GaveUp() && !wasGivenUp)
            SDS_LOG_ERROR("submix: the reroute has been re-armed %lu times and a subtree still "
                          "stops rendering. Giving up on the watchdog for this session - this "
                          "is not a transient level-load effect. Read the 'submix: REROUTE' "
                          "lines and SubmixRegisterSoundSubmixSlot.",
                          kMaxRerouteRearms);
        return;
    }

    const Lane& stalled = m_watchdog.StalledLane() == 0 ? m_coils : m_speaker;
    m_rerouteRearmWanted.store(true, std::memory_order_release);
    SDS_LOG_ERROR("submix: THE '%s' SUBTREE HAS STOPPED RENDERING - callbacks stuck at %llu for "
                  "%.1fs while the taps are bound. The most likely cause is a LEVEL LOAD "
                  "re-creating the submix graph, which drops the re-parenting we submitted "
                  "at bind time. Re-arming the reroute (attempt %lu of %lu): the glue will "
                  "re-write both masters' ParentSubmix and we will re-submit "
                  "RegisterSoundSubmix for the parent and both. The listeners are NOT "
                  "re-registered - they are still attached.",
                  stalled.tag, static_cast<unsigned long long>(m_watchdog.StalledAt()),
                  static_cast<double>(m_watchdog.StalledForMs()) / 1000.0,
                  m_watchdog.Rearms(), kMaxRerouteRearms);
}

bool Runtime::BindSubmixTap(const void* worldObject, const void* engineObject,
                            void* vibrationMasterObject, void* speakerMasterObject,
                            void* rerouteParentObject, const void* imageBase,
                            std::size_t imageSize)
{
    if (!SubmixWantsBinding())
        return m_submixBound.load(std::memory_order_acquire);

    // A RE-ARM, not a first bind: the listeners are already attached and must NOT be
    // registered again (UE 4.27's FMixerSubmix::RegisterBufferListener appends without
    // de-duplicating, so a second registration would deliver every callback twice into one
    // ring). Redo ONLY the reroute, whose UObject half the glue has already re-written before
    // calling us.
    const bool rearming = m_submixBound.load(std::memory_order_acquire);
    m_rerouteRearmWanted.store(false, std::memory_order_release);

    const int attempt = m_submixBindAttempts.fetch_add(1, std::memory_order_relaxed) + 1;

    // Refuse rather than register on the wrong thing: a null submix means MASTER to the
    // engine, so an unresolved USoundSubmix would silently put the game's entire soundtrack
    // on the voice coils. And ALL of them must be there: the reroute re-registers the parent
    // first and every master after it, so binding one lane now and the other later would
    // re-init the parent underneath the first lane's link.
    const bool needParent = m_config.submixReroute;
    if (vibrationMasterObject == nullptr || speakerMasterObject == nullptr ||
        (needParent && rerouteParentObject == nullptr))
    {
        if (attempt == 1 || attempt % 20 == 0)
            SDS_LOG_WARN("submix: not every submix has loaded yet (attempt %d): '%s'=%p "
                         "'%s'=%p parent '%s'=%p. NOT registering with a null submix - the "
                         "engine reads that as the MASTER submix and the whole soundtrack "
                         "would go to the pad.",
                         attempt, m_config.submixPath.c_str(), vibrationMasterObject,
                         m_config.submixSpeakerPath.c_str(), speakerMasterObject,
                         m_config.submixRerouteParent.c_str(), rerouteParentObject);
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

    // THE REROUTE, before the listeners: the engine rebuilds the submix links on the audio
    // thread in the order the calls are queued, and the listener registration goes through the
    // same queue, so by the time our listeners are attached the trees already render. The
    // PARENT goes first and EVERY master after it: RegisterSoundSubmix(bInit=true) re-inits
    // the parent, and it is each master's own RebuildSubmixLinks that puts it back under it.
    if (m_config.submixReroute)
    {
        const int   slot = m_config.submixRegisterSoundSubmixSlot;
        const char* why  = nullptr;
        SDS_LOG_WARN("submix: REROUTE - about to call vtable slot %d as "
                     "FAudioDevice::RegisterSoundSubmix(parent=%p, bInit=true), then "
                     "(vibration=%p, bInit=true), then (speaker=%p, bInit=true). AudioDevice.h:854 "
                     "puts it two slots below RegisterSubmixBufferListener (%d); if the game dies "
                     "HERE, that count is the suspect - set SubmixRegisterSoundSubmixSlot from the "
                     "vtable dump above.",
                     slot, rerouteParentObject, vibrationMasterObject, speakerMasterObject,
                     m_config.submixRegisterSlot);
        struct Step { const char* what; void* object; };
        const Step steps[] = { { "parent", rerouteParentObject },
                               { m_coils.tag, vibrationMasterObject },
                               { m_speaker.tag, speakerMasterObject } };
        bool ok = true;
        for (const Step& s : steps)
        {
            if (!ok) break;
            ok = submix::CallRegisterSoundSubmix(d.device, slot, s.object, true, imageBase,
                                                 imageSize, &why);
            if (!ok)
                SDS_LOG_ERROR("submix: REROUTE refused on the %s: %s", s.what,
                              why != nullptr ? why : "?");
        }
        if (ok)
        {
            m_submixRerouted.store(true, std::memory_order_release);
            SDS_LOG_INFO("submix: REROUTE submitted. The engine re-links '%s' and '%s' under "
                         "'%s' on the audio thread; if it worked, both SUBMIX lines below turn "
                         "from 'NO CALLBACKS' into a steady callback rate within a second - "
                         "SILENT callbacks are the proof the subtrees now render, a real "
                         "signal needs a haptic / a controller sound to play.",
                         m_config.submixPath.c_str(), m_config.submixSpeakerPath.c_str(),
                         m_config.submixRerouteParent.c_str());
        }
    }

    if (rearming)
    {
        SDS_LOG_WARN("submix: REROUTE RE-SUBMITTED (re-arm %lu). The listeners were left alone - "
                     "they are still registered. If the SUBMIX lines' cb= rates come back within "
                     "a second or two, a level load really was dropping the re-parenting; if "
                     "they stay put, the reroute is not what is failing.",
                     m_watchdog.Rearms());
        const uint64_t callbacks[2] = { m_coils.tap->Stats().callbacks,
                                        m_speaker.tap->Stats().callbacks };
        m_watchdog.Rearmed(callbacks, 2, NowMs());
        return true;
    }

    const char* whyNot = nullptr;
    SDS_LOG_WARN("submix: about to call vtable slot %d as "
                 "FAudioDevice::RegisterSubmixBufferListener(listener=%p, submix=%p) for '%s', "
                 "then (listener=%p, submix=%p) for '%s'. The slot is derived from stock "
                 "UE 4.27.2 (see src/SubmixDiscovery.hpp); Stray is a LICENSEE build, so if the "
                 "game dies HERE, that derivation is the suspect - read the vtable dump above "
                 "and set SubmixRegisterSlot.",
                 m_config.submixRegisterSlot, m_coils.tap->ListenerPointer(),
                 vibrationMasterObject, m_coils.tag, m_speaker.tap->ListenerPointer(),
                 speakerMasterObject, m_speaker.tag);

    if (!submix::CallRegisterSubmixBufferListener(
            d.device, m_config.submixRegisterSlot, m_coils.tap->ListenerPointer(),
            vibrationMasterObject, imageBase, imageSize, &whyNot))
    {
        SDS_LOG_ERROR("submix: REFUSED to call the register slot: %s. The taps are dead for this "
                      "session.", whyNot != nullptr ? whyNot : "no reason recorded");
        m_submixRefused.store(true, std::memory_order_release);
        m_submixBound.store(true, std::memory_order_release);   // stop retrying; it is a refusal
        return false;
    }
    // The first call passed every check on the device and the slot; the second is the same
    // call on the same device with a different submix, so a refusal here is a real surprise
    // and says so.
    if (!submix::CallRegisterSubmixBufferListener(
            d.device, m_config.submixRegisterSlot, m_speaker.tap->ListenerPointer(),
            speakerMasterObject, imageBase, imageSize, &whyNot))
    {
        SDS_LOG_ERROR("submix: the '%s' listener was refused AFTER the '%s' one was accepted: "
                      "%s. The speaker lane is dead for this session; the coils are not.",
                      m_speaker.tag, m_coils.tag, whyNot != nullptr ? whyNot : "no reason recorded");
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
    SDS_LOG_INFO("submix: registration submitted for '%s' and '%s'. It is ASYNCHRONOUS - the "
                 "engine runs it on the audio thread (AudioMixerDevice.cpp:2405) - so callbacks "
                 "are expected within a frame or two, not immediately. Watch the SUBMIX lines.",
                 m_config.submixPath.c_str(), m_config.submixSpeakerPath.c_str());
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
    if (m_coils.tap   != nullptr) m_coils.tap->Detach();
    if (m_speaker.tap != nullptr) m_speaker.tap->Detach();
    if (m_tapMaster   != nullptr) m_tapMaster->Detach();
    m_submixSink.Shutdown();

    m_haptics.Shutdown();
    m_triggers.Shutdown();   // releases the triggers on the way out
    m_hidMode.Shutdown();    // hands the coils back to rumble emulation

    LogStatus();
    Log::Close();
}

// ---- the controller speaker's ROUTING ----------------------------------------------------
//
// THE REGRESSION THIS FIXES (docs §16, PadAudio.hpp). The retired libScePad shim called
// scePadSetAudioOutPath(3 = SPEAKER) and scePadSetVolumeGain(80), and the pad speaker worked.
// deploy-submix-spike.sh retired the shim and nothing had called them since, so the pad was
// sitting on its default routing, where the internal speaker is muted.
//
// NO PROXY DLL IS NEEDED. The shim intercepted scePadOpen for its handle; we already hold the
// same handle from scePadGetHandle (§11, "verified to return the same handles"), and the
// plugin runs in the game's own process. So Sony's dll stays the SINGLE writer of pad output
// reports and the §12 two-writers hazard never arises: we are asking it to change a setting,
// not writing bytes beside it.
//
// The routing says where the pad sends what it receives; it is orthogonal to the samples,
// which now come from the rerouted speaker submix through the tap.

uint64_t Runtime::SpeakerRouteSignature() const
{
    // Everything that decides what is written, in one comparable value — including the
    // enable bit, so `Speaker=0` re-applies as a change rather than being silently ignored.
    uint64_t sig = 1;   // never 0: 0 means "nothing has been applied yet"
    sig = sig * 131 + static_cast<uint64_t>(m_config.speaker ? 1 : 0);
    sig = sig * 131 + static_cast<uint64_t>(static_cast<int>(m_config.padSpeakerRoute));
    sig = sig * 131 + static_cast<uint64_t>(m_config.padSpeakerPath);
    sig = sig * 131 + static_cast<uint64_t>(m_config.padSpeakerGain);
    sig = sig * 131 + static_cast<uint64_t>(m_config.padSpeakerHidPath);
    sig = sig * 131 + static_cast<uint64_t>(m_config.padSpeakerHidVolume);
    sig = sig * 131 + static_cast<uint64_t>(m_config.padSpeakerHidPreamp);
    return sig;
}

void Runtime::ApplySpeakerRoute(const char* why)
{
    const PadSpeakerRoute route =
        m_config.speaker ? m_config.padSpeakerRoute : PadSpeakerRoute::Off;

    if (route == PadSpeakerRoute::Off)
    {
        m_hidMode.SetAudioClaim(PadAudioClaim{});
        m_speakerRouteSonyOk.store(false, std::memory_order_relaxed);
        m_speakerRouteHidOn.store(false, std::memory_order_relaxed);
        SDS_LOG_INFO("pad audio: PadSpeakerRoute=%s (%s) - nothing is written, so the pad "
                     "keeps whatever routing it has. Its DEFAULT routing mutes the internal "
                     "speaker, so this is the configuration in which the speaker is silent.",
                     PadSpeakerRouteName(route), why);
        return;
    }

    bool sonyAttempted = false;
    bool sonyOk        = false;
    if (RouteUsesSony(route))
    {
        const SceVolumeGain gain =
            BuildSceVolumeGain(m_config.padSpeakerGain, m_config.padSpeakerGain, 0);
        ScePad::AudioRouteResult r;
        sonyAttempted = true;
        sonyOk        = m_pad.ApplyAudioRoute(m_config.padSpeakerPath, gain.bytes, r);
        m_speakerRouteLastPathResult.store(r.path, std::memory_order_relaxed);

        // Every result code in hex, in one line, exactly as the shim logged them. A Sony
        // status is not a bitmask to be read leniently: 0 is success and 0x809200xx is not,
        // and 0x80920007 in particular means "this pad has no audio" (§7) — the device-tree
        // refusal that the GE-Proton ds5 patches exist to fix.
        SDS_LOG_INFO("pad audio: scePadIsSupportedAudioFunction=%s0x%08X "
                     "scePadSetAudioOutPath(%d=%s)=%s0x%08X scePadSetVolumeGain(spk=%d)=%s0x%08X "
                     "[handle=0x%X, %s]",
                     r.supportedRan ? "" : "(absent) ", static_cast<unsigned>(r.supported),
                     m_config.padSpeakerPath, SceAudioOutPathName(m_config.padSpeakerPath),
                     r.pathRan ? "" : "(absent) ", static_cast<unsigned>(r.path),
                     m_config.padSpeakerGain,
                     r.gainRan ? "" : "(absent) ", static_cast<unsigned>(r.gain),
                     static_cast<unsigned>(m_pad.Handle()), why);

        if (sonyOk)
            SDS_LOG_INFO("pad audio: SONY ACCEPTED the route. The pad's internal speaker is "
                         "selected; anything written into FL/FR of its endpoint should now be "
                         "audible.");
        else if (!r.pathRan)
            SDS_LOG_ERROR("pad audio: scePadSetAudioOutPath is not exported by this "
                          "libScePad.dll, so Sony's API cannot select the speaker.");
        else if (static_cast<uint32_t>(r.path) == 0x80920007u)
            SDS_LOG_ERROR("pad audio: scePadSetAudioOutPath returned 0x80920007 - libScePad "
                          "believes this pad HAS NO AUDIO. That is the device-tree refusal "
                          "docs §7 measured: it walks for USB audio sibling interfaces with "
                          "SetupDiGetDeviceRegistryPropertyW and finds none. It is a Proton "
                          "problem, not ours; GE-Proton's proton-ds5-haptic series is the fix.");
        else
            SDS_LOG_ERROR("pad audio: scePadSetAudioOutPath returned 0x%08X (not success). "
                          "The pad speaker will stay silent on this route.",
                          static_cast<unsigned>(r.path));
    }

    const bool wantHid = RouteShouldWriteHid(route, sonyAttempted, sonyOk);
    if (wantHid && route == PadSpeakerRoute::Auto)
        SDS_LOG_WARN("pad audio: PadSpeakerRoute=auto and Sony's API did not accept the "
                     "route, so ESCALATING to the raw HID output-report claim. It selects the "
                     "path in valid_flag0/audio_control directly. Its coil safety is argued "
                     "(a disjoint bit set, masked in ComposeValidFlag0) rather than measured - "
                     "if the coils stop working, set PadSpeakerRoute=sony or off.");

    m_hidMode.SetAudioClaim(BuildPadAudioClaim(wantHid, m_config.padSpeakerHidPath,
                                               m_config.padSpeakerHidVolume,
                                               m_config.padSpeakerHidPreamp));
    m_speakerRouteSonyOk.store(sonyOk, std::memory_order_relaxed);
    m_speakerRouteHidOn.store(wantHid, std::memory_order_relaxed);
    m_speakerRouteApplies.fetch_add(1, std::memory_order_relaxed);

    if (!sonyOk && !wantHid)
        SDS_LOG_ERROR("pad audio: NOTHING selected the pad's audio route (PadSpeakerRoute=%s). "
                      "The speaker will be silent however good the samples are. Try "
                      "PadSpeakerRoute=hid.", PadSpeakerRouteName(route));
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

        // The routing is applied HERE, on the only thread that owns a libScePad handle, and
        // re-applied whenever the handle changes (a pad re-adopted after a disconnect loses
        // its routing) or the settings change (hot reload, so the sony-versus-hid question
        // is one ini edit and one keypress rather than one relaunch per arm).
        //
        // The HID routes need NO libScePad handle — they go through HidMode's own HID device —
        // so gating the whole thing on HasPad() would make `PadSpeakerRoute=hid` silently do
        // nothing on a session where libScePad never binds, which is precisely the "silent
        // refusal" this project refuses to ship. Only the Sony routes wait for a pad.
        const PadSpeakerRoute wanted =
            m_config.speaker ? m_config.padSpeakerRoute : PadSpeakerRoute::Off;
        if (m_pad.HasPad() || !RouteUsesSony(wanted))
        {
            const uint64_t sig = SpeakerRouteSignature();
            const char*    why = nullptr;
            if (m_speakerRouteApplied == 0)              why = m_pad.HasPad() ? "pad adopted"
                                                                             : "no pad needed";
            else if (m_pad.Handle() != m_speakerRouteHandle) why = "pad handle changed";
            else if (sig != m_speakerRouteApplied)       why = "settings changed";
            if (why != nullptr)
            {
                m_speakerRouteApplied = sig;
                m_speakerRouteHandle  = m_pad.Handle();
                m_lastSpeakerRouteMs  = NowMs();
                ApplySpeakerRoute(why);
            }
            else if (m_config.padSpeakerReassertSeconds > 0.0f &&
                     NowMs() - m_lastSpeakerRouteMs >=
                         static_cast<uint64_t>(m_config.padSpeakerReassertSeconds * 1000.0f))
            {
                m_lastSpeakerRouteMs = NowMs();
                ApplySpeakerRoute("periodic re-assert");
            }
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

void Runtime::ApplySinkGains()
{
    if (!m_sinkStarted)
        return;
    // The game's own switch gates the coils and only the coils (§9); Speaker=0 gates the
    // speaker. Both are gains rather than teardowns, so the taps keep reporting numbers -
    // which is what makes "the user turned it off" distinguishable from "the tap died".
    const bool coilsOn = m_config.SubmixDrivesCoils() && m_padVibrationEnabled.load();
    m_submixSink.SetCoilGain(coilsOn ? m_config.submixGain : 0.0f);
    m_submixSink.SetSpeakerGain(m_config.speaker ? m_config.speakerGain : 0.0f);
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
        if (m_config.ReloadIfChanged(m_configPath))
            ApplySinkGains();
    }

    if (m_config.statusSeconds > 0.0f &&
        now - m_lastStatusMs >= static_cast<uint64_t>(m_config.statusSeconds * 1000.0f))
    {
        m_lastStatusMs = now;
        LogStatus();
    }

    if (m_coils.tap != nullptr && m_speaker.tap != nullptr &&
        m_config.submixStatusSeconds > 0.0f &&
        now - m_lastSubmixStatusMs >=
            static_cast<uint64_t>(m_config.submixStatusSeconds * 1000.0f))
    {
        m_submixStatusWindowMs = now - m_lastSubmixStatusMs;
        m_lastSubmixStatusMs   = now;
        SubmixStatus();
    }

    RerouteWatchdog(now);
    SubmixWarnIfDue(now);
}

// The LOUD, PERIODIC half of the owner verdicts. A configuration that asked for the submix
// and is not getting it is a problem every N seconds, not an INFO line once a second.
void Runtime::SubmixWarnIfDue(uint64_t now)
{
    if (m_config.submixWarnSeconds <= 0.0f)
        return;
    if (now - m_lastSubmixWarnMs < static_cast<uint64_t>(m_config.submixWarnSeconds * 1000.0f))
        return;
    const CoilVerdict c = Coils();
    const LaneVerdict s = Speaker();
    if (!c.warn && !s.warn)
        return;
    m_lastSubmixWarnMs = now;
    if (c.warn)
        SDS_LOG_WARN("%s | %s | HapticSource=%s", c.headline, c.detail, m_config.HapticSourceName());
    if (s.warn)
        SDS_LOG_WARN("%s: %s | %s", m_speaker.owner, s.headline, s.detail);
}

float Runtime::LiveThreshold() const
{
    const float t = m_config.submixLiveThreshold;
    // A threshold of 0 would make every callback "a real signal", including a buffer of
    // exact zeros — which is precisely the state this test exists to detect.
    if (!(t > 0.0f) || t > 1.0f)
        return kSubmixLiveThreshold;
    return t;
}

void Runtime::OpenWatch(Lane& lane, const std::string& asset)
{
    if (lane.tap == nullptr || m_config.submixWatchSeconds <= 0.0f || asset.empty())
        return;
    WatchVerdict closed;
    bool         haveClosed = false;
    {
        std::lock_guard<std::mutex> lock(m_watchMutex);
        haveClosed = lane.watch.Start(asset, NowMs(),
                                      static_cast<uint64_t>(m_config.submixWatchSeconds * 1000.0f),
                                      LiveThreshold(), closed);
    }
    if (haveClosed)
        ReportWatch(lane, closed);
}

// THE ANSWER TO "did the engine mix the asset the game asked for". One line, per start,
// naming the asset — as opposed to a per-second peak, which is 0.00000 whenever nothing
// happens to be playing and therefore cannot distinguish a dead submix from a quiet moment.
void Runtime::ReportWatch(const Lane& lane, const WatchVerdict& v)
{
    const double secs = static_cast<double>(v.ms) / 1000.0;
    const char*  path = &lane == &m_coils ? m_config.submixPath.c_str()
                                          : m_config.submixSpeakerPath.c_str();
    if (v.result == WatchResult::Mixed)
    {
        SDS_LOG_INFO("submix watch [%s] '%s': the engine MIXED it - peak %.5f over %.1fs "
                     "(%d window(s), %llu frames). This asset reaches the %s.",
                     lane.tag, v.asset.c_str(), static_cast<double>(v.peak), secs, v.windows,
                     static_cast<unsigned long long>(v.frames),
                     &lane == &m_coils ? "coils" : "speaker");
        return;
    }
    // THE THIRD STATE, and it is not a statement about the mixer at all. Distinguished
    // 2026-09-03 after "the engine mixed NOTHING - peak 0.00000 over 1.2s (0 window(s))" was
    // read as a mixing failure in 03_Slums when what it actually recorded was a tap that
    // delivered no audio: the subtree was not being rendered.
    if (v.result == WatchResult::NoData)
    {
        SDS_LOG_ERROR("submix watch [%s] '%s': NO DATA FROM THE TAP over %.1fs (%d window(s), 0 "
                      "frames) - this says NOTHING about what the engine mixed. The tap handed "
                      "us no audio at all, so '%s' is not being RENDERED. Suspect the REROUTE, "
                      "not the game: a level load re-creates the submix graph. Check 'submix: "
                      "REROUTE' and the SUBMIX line's cb= rate; the watchdog re-arms it "
                      "automatically if SubmixRerouteWatchdogSeconds > 0.",
                      lane.tag, v.asset.c_str(), secs, v.windows, path);
        return;
    }
    SDS_LOG_WARN("submix watch [%s] '%s': the engine mixed NOTHING - peak %.5f over %.1fs "
                 "(%d window(s), %llu frames). The tap WAS delivering audio, so this is a real "
                 "negative: the game ASKED for this asset and '%s' carried silence. The fault "
                 "is upstream of the tap - the Blueprint gate (ForcePS5HapticPath / "
                 "DebugPS5Haptic), the level the game passed, or the routing.",
                 lane.tag, v.asset.c_str(), static_cast<double>(v.peak), secs, v.windows,
                 static_cast<unsigned long long>(v.frames), path);
}

// The handover. Callbacks alone are not enough: a submix that renders pure silence would
// take the coils away from the asset path and give nothing back. Only a real signal proves
// the engine is putting audio in there, so that is the trigger for each lane - and the first
// lane to reach it is when the sink opens the pad endpoint, not a moment earlier.
void Runtime::StartLaneAtHandover(Lane& lane, float peak)
{
    const bool isCoils = &lane == &m_coils;
    if (isCoils && !m_config.SubmixDrivesCoils())
        return;   // measure mode: the coils stay on the asset path; the speaker still hands over
    if (lane.live.exchange(true, std::memory_order_acq_rel))
        return;
    SDS_LOG_WARN("submix: FIRST REAL SIGNAL on '%s' (peak %.5f) - HANDOVER: the SUBMIX now "
                 "drives the %s%s.", lane.tag, static_cast<double>(peak),
                 isCoils ? "coils" : "speaker",
                 isCoils && m_config.hapticSource == HapticSource::SubmixFallback
                     ? " and the asset path stands down" : "");
    if (isCoils)
        m_haptics.Stop(0.0f);
    // Attach the ring EMPTY, then make sure the sink is draining it: the sink's queue-ahead
    // (40 ms) is the whole latency budget, not the ring's capacity (see StartSubmix).
    lane.ring.ResetCounters();
    lane.tap->SetRing(&lane.ring);
    if (!m_sinkStarted)
    {
        m_sinkStarted = true;
        m_submixSink.Start(&m_coils.ring, &m_speaker.ring, m_config.endpointMatch, m_config,
                           [this] { m_hidMode.AssertNow("submix: silence -> signal"); });
        ApplySinkGains();
    }
}

// One lane's line of the NUMBERS PROOF. Silence when nothing plays, a live signal during
// rain, and a HIGHER signal when rain and a purr overlap, is the whole argument that the
// engine is mixing concurrent haptics for us. And when the tap never fires it SAYS SO: a
// listener that was never called and a submix that is genuinely silent produce identical
// audio, so they must not produce identical text.
void Runtime::LaneStatus(Lane& lane, double seconds, uint64_t nowMs, char* line,
                         std::size_t lineSize)
{
    const submix::TapStats     stats = lane.tap->Stats();
    const submix::LevelReading level = lane.tap->TakeLevels();
    const uint64_t deltaCallbacks =
        stats.callbacks >= lane.lastCallbacks ? stats.callbacks - lane.lastCallbacks : 0;
    lane.lastCallbacks = stats.callbacks;
    const double cbPerSec = static_cast<double>(deltaCallbacks) / seconds;

    // The watch is folded in BEFORE the line is built, so a start and its verdict cannot be
    // separated by a status line that says nothing about either.
    {
        WatchVerdict closed;
        bool         haveClosed = false;
        {
            std::lock_guard<std::mutex> lock(m_watchMutex);
            // ALWAYS sample, frames included — a frameless window is data, and skipping the
            // call is what made "the tap gave us nothing" print as "the engine mixed nothing".
            lane.watch.Sample(level.peak, level.frames);
            haveClosed = lane.watch.Poll(nowMs, closed);
        }
        if (haveClosed)
            ReportWatch(lane, closed);
    }

    char headline[400];
    if (&lane == &m_coils)
    {
        const CoilVerdict v = Coils();
        std::snprintf(headline, sizeof(headline), "%s", v.headline);
    }
    else
    {
        const LaneVerdict v = Speaker();
        std::snprintf(headline, sizeof(headline), "%s: %s", lane.owner, v.headline);
    }

    if (level.frames == 0)
    {
        char since[80];
        if (stats.lastCallbackMs == 0)
            std::snprintf(since, sizeof(since), "NEVER - the listener has never been called");
        else
            std::snprintf(since, sizeof(since), "%.1fs ago",
                          static_cast<double>(nowMs - stats.lastCallbackMs) / 1000.0);
        std::snprintf(line, lineSize,
                      "%s | SUBMIX %s bound=%d NO CALLBACKS in the last %.1fs (total=%llu, last %s) "
                      "| %s",
                      headline, lane.tag, m_submixBound.load() ? 1 : 0, seconds,
                      static_cast<unsigned long long>(stats.callbacks), since,
                      m_submixBound.load()
                          ? "the tap IS registered, so the engine is rendering nothing into "
                            "this submix"
                          : "the tap is NOT registered yet (see the WARN lines above)");
        return;
    }

    if (level.peak > lane.peakEver)
        lane.peakEver = level.peak;
    if (level.peak >= LiveThreshold())
        lane.lastSignalMs = nowMs;

    // `peak` is the LAST WINDOW ONLY, so on its own it says nothing about the session.
    // These two make one pasted line self-diagnosing: peakEver 0.00000 means the engine
    // has never put anything in this submix, and a lastSignal of minutes ago against a
    // peakEver near the ~0.7 a VIBE asset measures means it did and this second is quiet.
    char ever[120];
    if (lane.lastSignalMs == 0)
        std::snprintf(ever, sizeof(ever), " peakEver=%.5f lastSignal=NEVER",
                      static_cast<double>(lane.peakEver));
    else
        std::snprintf(ever, sizeof(ever), " peakEver=%.5f lastSignal=%.1fs ago",
                      static_cast<double>(lane.peakEver),
                      static_cast<double>(nowMs - lane.lastSignalMs) / 1000.0);

    std::snprintf(line, lineSize,
                  "%s | SUBMIX %s bound=%d live=%d cb=%llu (%.1f/s) ch=%d rate=%d "
                  "frames/cb=%d peak=%.5f rms=%.5f bad=%llu%s | ring fill=%zu/%zu drop=%llu "
                  "under=%llu",
                  headline, lane.tag, m_submixBound.load() ? 1 : 0,
                  lane.live.load() ? 1 : 0,
                  static_cast<unsigned long long>(stats.callbacks), cbPerSec,
                  stats.lastNumChannels, stats.lastSampleRate,
                  stats.lastNumChannels > 0 ? stats.lastNumSamples / stats.lastNumChannels : 0,
                  static_cast<double>(level.peak), static_cast<double>(level.rms),
                  static_cast<unsigned long long>(stats.badCallbacks), ever,
                  lane.ring.Available(), lane.ring.CapacityFrames(),
                  static_cast<unsigned long long>(lane.ring.Dropped()),
                  static_cast<unsigned long long>(lane.ring.Underruns()));

    // Learned from the engine, never assumed: a project ini can change the rate.
    if (stats.lastSampleRate > 0)
        m_submixSink.SetSourceRate(static_cast<uint32_t>(stats.lastSampleRate));

    if (level.peak >= LiveThreshold())
        StartLaneAtHandover(lane, level.peak);
}

// Both lanes' lines, once a second, to the log and to <gamedir>/stray-dualsense-submix.txt
// (rewritten in place, so it can be read with `cat` over ssh with no overlay). The shared
// tail — master probe, reroute, sink — rides on the coil line.
void Runtime::SubmixStatus()
{
    const double seconds = m_submixStatusWindowMs > 0
                               ? static_cast<double>(m_submixStatusWindowMs) / 1000.0
                               : 1.0;
    const uint64_t nowMs = NowMs();

    char coils[1200];
    char speaker[1200];
    LaneStatus(m_coils,   seconds, nowMs, coils,   sizeof(coils));
    LaneStatus(m_speaker, seconds, nowMs, speaker, sizeof(speaker));

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

    char rest[400];
    std::snprintf(rest, sizeof(rest),
                  "%s | rerouted=%d | sink open=%d '%s' %uch %uHz frames=%llu fail=%llu",
                  master, m_submixRerouted.load() ? 1 : 0,
                  m_submixSink.StreamOpen() ? 1 : 0, m_submixSink.EndpointName().c_str(),
                  m_submixSink.EndpointChannels(), m_submixSink.EndpointRate(),
                  static_cast<unsigned long long>(m_submixSink.FramesWritten()),
                  static_cast<unsigned long long>(m_submixSink.Failures()));

    SDS_LOG_INFO("%s%s", coils, rest);
    SDS_LOG_INFO("%s", speaker);

    if (!m_submixStatusFile.empty())
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, m_submixStatusFile.c_str(), L"w") == 0 && f != nullptr)
        {
            std::fprintf(f, "%s%s\n%s\n", coils, rest, speaker);
            std::fclose(f);
        }
    }
}

void Runtime::LogStatus()
{
    const TriggerEffect fx      = m_triggers.Effect();
    const CoilVerdict   coils   = Coils();
    const LaneVerdict   speaker = Speaker();
    SDS_LOG_INFO("STATUS coils=%s (%s) speaker=%s (%s) "
                 "pad=%s(user=%d handle=0x%X probes=%lu misses=%lu) "
                 "hid[open=%d writes=%lu fail=%lu] "
                 "trig[events=%lu tx=%lu ok=%lu fail=%lu L=%d R=%d effect=%d/%u/%u/%u %s] "
                 "hap[starts=%lu played=%lu done=%lu missing=%lu fail=%lu endpoint=%d now='%s' "
                 "compStops ok=%lu ignored=%lu padVibe=%d] "
                 "gate[open=%d writes=%lu misses=%lu] "
                 "spk[starts=%lu stops=%lu live=%d] "
                 "padaudio[route=%s api=%d sonyOk=%d pathResult=0x%08X hidClaim=%d applies=%lu "
                 "fail=%lu]",
                 CoilOwnerName(coils.owner), coils.detail,
                 LaneOwnerName(speaker.owner), speaker.detail,
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
                 m_hapticGateOpen.load() ? 1 : 0, m_hapticGateWrites.load(),
                 m_hapticGateMisses.load(),
                 m_speakerStarts.load(), m_speakerStops.load(),
                 m_speaker.live.load() ? 1 : 0,
                 // ONE pasted STATUS line must answer "why is the pad speaker silent". The
                 // three fields that do it: `api` says libScePad exports the routing call at
                 // all, `sonyOk` says it accepted, and `pathResult` is its verbatim status.
                 PadSpeakerRouteName(m_config.speaker ? m_config.padSpeakerRoute
                                                      : PadSpeakerRoute::Off),
                 m_pad.HasAudioApi() ? 1 : 0,
                 m_speakerRouteSonyOk.load(std::memory_order_relaxed) ? 1 : 0,
                 static_cast<unsigned>(m_speakerRouteLastPathResult.load(std::memory_order_relaxed)),
                 m_speakerRouteHidOn.load(std::memory_order_relaxed) ? 1 : 0,
                 m_pad.AudioApplies(), m_pad.AudioFailures());
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

    // Open the correlation window BEFORE the verdict, and in every mode: "the game asked and
    // the submix stayed silent" is the measurement that matters whether or not the submix is
    // what currently drives the coils.
    OpenWatch(m_coils, name);

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

// The speaker has no player of ours: the engine plays the _CONTROL asset itself once the
// Blueprint gate is open, into Submix_controllerMaster, which the tap carries to FL/FR. These
// hooks LOG the intent and open the lane's watch, so one line per start says what the engine
// put in that submix for the asset the game asked for.
void Runtime::OnStartControllerSound(const std::string& soundFullName, float level,
                                     bool levelSeen, float fadeIn)
{
    m_speakerStarts.fetch_add(1, std::memory_order_relaxed);
    const std::string name     = ShortAssetName(soundFullName);
    const float       useLevel = levelSeen ? level : 1.0f;
    SDS_LOG_INFO("StartPS5ControllerSound '%s' -> %s level=%.3f(seen=%d) fadeIn=%.2f (the engine "
                 "plays it; the tap carries it)",
                 soundFullName.c_str(), name.c_str(), static_cast<double>(useLevel),
                 levelSeen ? 1 : 0, static_cast<double>(fadeIn));
    if (name.empty())
    {
        SDS_LOG_ERROR("StartPS5ControllerSound: could not derive an asset name from '%s'",
                      soundFullName.c_str());
        return;
    }
    OpenWatch(m_speaker, name);
}

void Runtime::OnStopControllerSound(float fadeOut)
{
    m_speakerStops.fetch_add(1, std::memory_order_relaxed);
    SDS_LOG_INFO("StopPS5ControllerSound fadeOut=%.2f (the engine fades it out)",
                 static_cast<double>(fadeOut));
}

void Runtime::OnSetControllerSoundLevel(float level)
{
    // The engine applies it upstream of the tap. Never log it per call.
    (void)level;
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
    ApplySinkGains();
    SDS_LOG_INFO("%s", Coils().headline);
}

void Runtime::OnHapticGate(bool open, bool wrote)
{
    const bool was = m_hapticGateOpen.exchange(open, std::memory_order_relaxed);
    if (wrote)
        m_hapticGateWrites.fetch_add(1, std::memory_order_relaxed);
    else
        m_hapticGateMisses.fetch_add(1, std::memory_order_relaxed);
    if (was != open)
        SDS_LOG_INFO("haptics: the PS5 Blueprint gate (DebugPS5Haptic) is now %s",
                     open ? "OPEN" : "SHUT");
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
