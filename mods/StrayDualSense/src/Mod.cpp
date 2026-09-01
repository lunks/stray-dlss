// StrayDualSense — the UE4SS glue, and NOTHING else.
//
// This is the only file in the mod that includes a UE4SS header. Everything it does is
// translate a UFunction callback into a call on `sds::Runtime`, which knows nothing about
// UE4SS and is compiled and link-tested without it. That split is deliberate: the SDK is the
// part that cannot be verified from here, so it is kept as small as it can be made.
//
// PROVENANCE of every SDK call below, read out of RE-UE4SS at commit 68caddcf (the build on
// the target box) and out of a public vendored copy of its private `deps/first/Unreal`
// submodule at the same tree:
//
//   CppUserModBase, on_unreal_init/on_update/on_program_start   UE4SS/include/Mod/CppUserModBase.hpp   HARD
//   start_mod / uninstall_mod resolved by literal name          UE4SS/src/Mod/CppMod.cpp               HARD
//   UObjectGlobals::RegisterHook(UFunction*, pre, post, void*)
//     -> std::pair<int,int>                                     Unreal/UObjectGlobals.hpp              HARD
//   UnrealScriptFunctionCallable =
//     std::function<void(Context&, void*)>                      Unreal/UFunctionStructs.hpp            HARD
//   ctx.TheStack.Locals() -> uint8*                             Unreal/FFrame.hpp                      HARD
//   UStruct::ForEachProperty() -> TFieldRange<FProperty>        Unreal/CoreUObject/UObject/Class.hpp   HARD
//   FProperty::GetOffset_ForInternal / HasAnyPropertyFlags      Unreal/CoreUObject/UObject/UnrealType.hpp HARD
//   CPF_Parm / CPF_ReturnParm                                   Unreal/UnrealFlags.hpp                 HARD
//   FBoolProperty::GetPropertyValueInContainer                  Unreal/CoreUObject/UObject/UnrealType.hpp HARD
//   UObjectGlobals::FindFirstOf / StaticFindObject              Unreal/UObjectGlobals.hpp              HARD
//   UObject::GetPropertyByNameInChain / GetFullName             Unreal/UObject.hpp                     HARD
//
// What is NOT verified is that any of it BEHAVES as intended in the game. See README.md.

// These headers were split up on main and the old paths now emit a #pragma message. We
// include the new paths; the defines keep a mixed tree quiet rather than failing /WX.
#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealFlags.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "Log.hpp"
#include "Runtime.hpp"
#include "Version.hpp"

namespace {

using RC::Unreal::FBoolProperty;
using RC::Unreal::FProperty;
using RC::Unreal::UFunction;
using RC::Unreal::UnrealScriptFunctionCallableContext;
using RC::Unreal::UObject;

// ---------------------------------------------------------------------------------------
// Small string helpers. Our own logger is narrow (the user greps it next to ReShade's log);
// UE4SS's Output is wide.
// ---------------------------------------------------------------------------------------
std::string Narrow(const RC::StringType& s)
{
    std::string out;
    out.reserve(s.size());
    for (const auto c : s)
        out.push_back((c >= 32 && c < 127) ? static_cast<char>(c) : '?');
    return out;
}

RC::StringType Widen(const std::string& s)
{
    RC::StringType out;
    out.reserve(s.size());
    for (const unsigned char c : s)
        out.push_back(static_cast<RC::StringType::value_type>(c));
    return out;
}

// Everything that reaches UE4SS's console goes through here, for two reasons. The text is a
// finished string rather than a fmt format string, so a stray '{' in a game asset name cannot
// become a format error; and Output::send THROWS when no output device is open
// (Output.hpp: THROW_INTERNAL_FILE_ERROR "there were no opened devices"), which must never be
// the thing that kills the mod. The file log is the real one.
void Say(const RC::StringType& line)
{
    try
    {
        RC::Output::send<RC::LogLevel::Verbose>(line + STR("\n"));
    }
    catch (...)
    {
        // No output device open yet, or UE4SS is tearing down. The file log already has it.
    }
}

// ---------------------------------------------------------------------------------------
// Parameter binding.
//
// Params are read through the UFunction's OWN reflection rather than a hand-declared struct
// laid over TheStack.Locals(). It costs a little more SDK surface and buys two things this
// project cares about more: the layout cannot be silently wrong, and the discovered layout is
// LOGGED, so one pasted log answers "did we read the right bytes" without another round trip.
//
// The failure this guards against is concrete: SetPS5TriggerActivated's second argument is
// the SIDE, and reading it wrongly leaves one trigger hardened forever (§8).
// ---------------------------------------------------------------------------------------
enum class ParamKind : uint8_t { Unknown, Bool, UInt8, Int32, Int64, Float, Double, Object };

const char* KindName(ParamKind k)
{
    switch (k)
    {
    case ParamKind::Bool:   return "bool";
    case ParamKind::UInt8:  return "uint8/enum";
    case ParamKind::Int32:  return "int32";
    case ParamKind::Int64:  return "int64";
    case ParamKind::Float:  return "float";
    case ParamKind::Double: return "double";
    case ParamKind::Object: return "object";
    case ParamKind::Unknown:
    default:                return "raw";
    }
}

struct Param
{
    std::string    name;
    int32_t        offset   = 0;
    int32_t        size     = 0;         // the property's own size, from reflection
    ParamKind      kind     = ParamKind::Unknown;
    FBoolProperty* boolProp = nullptr;   // bitfield-correct bool reads
    bool           isReturn = false;
};

ParamKind ClassifyProperty(FProperty* prop)
{
    // Ordered most-derived first where it matters; FByteProperty covers TEnumAsByte, which is
    // what EPS5TriggersSide is expected to be.
    if (prop->IsA<RC::Unreal::FBoolProperty>())   return ParamKind::Bool;
    if (prop->IsA<RC::Unreal::FByteProperty>())   return ParamKind::UInt8;
    if (prop->IsA<RC::Unreal::FIntProperty>())    return ParamKind::Int32;
    if (prop->IsA<RC::Unreal::FInt64Property>())  return ParamKind::Int64;
    if (prop->IsA<RC::Unreal::FFloatProperty>())  return ParamKind::Float;
    if (prop->IsA<RC::Unreal::FDoubleProperty>()) return ParamKind::Double;
    if (prop->IsA<RC::Unreal::FObjectProperty>()) return ParamKind::Object;
    // Anything else — an FEnumProperty for a UE `enum class`, most plausibly EPS5TriggersSide —
    // is read by its reflected SIZE instead. Refusing here would turn a perfectly readable
    // 1-byte enum into a hard "NOT touching the triggers".
    return ParamKind::Unknown;
}

bool EqualsNoCase(const std::string& a, const char* b)
{
    size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i)
    {
        const char x = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
        const char y = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + 32) : b[i];
        if (x != y) return false;
    }
    return i == a.size() && b[i] == '\0';
}

struct HookInfo
{
    RC::StringType path;              // owned: HookInfo outlives every lookup
    const char*    shortName = nullptr;
    RC::Unreal::UnrealScriptFunctionCallable callback;

    bool                registered = false;
    std::pair<int, int> ids{};
    UFunction*          function = nullptr;
    std::vector<Param>  params;
};

// By NAME first, then by POSITION among the non-return params — which is exactly what the Lua
// mod did with A[2] and A[3]. Position rather than kind, because EPS5TriggersSide may reflect as
// an FEnumProperty rather than an FByteProperty and a kind-filtered fallback would miss it.
const Param* FindParam(const HookInfo& hook, const char* name, size_t ordinal)
{
    for (const Param& p : hook.params)
        if (!p.isReturn && EqualsNoCase(p.name, name))
            return &p;
    size_t seen = 0;
    for (const Param& p : hook.params)
    {
        if (p.isReturn) continue;
        if (seen++ == ordinal)
            return &p;
    }
    return nullptr;
}

const Param* FirstOfKind(const HookInfo& hook, ParamKind kind)
{
    for (const Param& p : hook.params)
        if (!p.isReturn && p.kind == kind)
            return &p;
    return nullptr;
}

bool ReadBool(const Param* p, uint8_t* base, bool& out)
{
    if (p == nullptr || base == nullptr) return false;
    if (p->kind == ParamKind::Bool && p->boolProp != nullptr)
    {
        // Handles both a native bool and a `uint8 bFoo : 1` bitfield.
        out = p->boolProp->GetPropertyValueInContainer(base);
        return true;
    }
    if (p->kind == ParamKind::UInt8) { out = *(base + p->offset) != 0; return true; }
    return false;
}

bool ReadInt(const Param* p, uint8_t* base, int64_t& out)
{
    if (p == nullptr || base == nullptr) return false;
    uint8_t* at = base + p->offset;
    switch (p->kind)
    {
    case ParamKind::UInt8: out = *at; return true;
    case ParamKind::Int32: { int32_t v; std::memcpy(&v, at, sizeof(v)); out = v; return true; }
    case ParamKind::Int64: { int64_t v; std::memcpy(&v, at, sizeof(v)); out = v; return true; }
    case ParamKind::Bool:  { bool b = false; if (!ReadBool(p, base, b)) return false; out = b ? 1 : 0; return true; }
    case ParamKind::Unknown:
        // Size-driven fallback: an FEnumProperty carries no kind we classify, but its width is
        // reflected and an enum is just an unsigned integer of that width.
        switch (p->size)
        {
        case 1: out = *at; return true;
        case 2: { uint16_t v; std::memcpy(&v, at, sizeof(v)); out = v; return true; }
        case 4: { uint32_t v; std::memcpy(&v, at, sizeof(v)); out = v; return true; }
        case 8: { uint64_t v; std::memcpy(&v, at, sizeof(v)); out = static_cast<int64_t>(v); return true; }
        default: return false;
        }
    default: return false;
    }
}

bool ReadFloat(const Param* p, uint8_t* base, float& out)
{
    if (p == nullptr || base == nullptr) return false;
    uint8_t* at = base + p->offset;
    if (p->kind == ParamKind::Float)  { float  v; std::memcpy(&v, at, sizeof(v)); out = v; return true; }
    if (p->kind == ParamKind::Double) { double v; std::memcpy(&v, at, sizeof(v)); out = static_cast<float>(v); return true; }
    return false;
}

UObject* ReadObject(const Param* p, uint8_t* base)
{
    if (p == nullptr || base == nullptr || p->kind != ParamKind::Object) return nullptr;
    UObject* obj = nullptr;
    std::memcpy(&obj, base + p->offset, sizeof(obj));
    return obj;
}

// The asset argument, as a full name. Empty if there is no object param or it is null.
std::string ObjectArgFullName(const HookInfo& hook, uint8_t* base)
{
    const Param* p = nullptr;
    for (const char* candidate : { "SoundVibration", "Sound", "SoundWave", "AudioComponent" })
    {
        for (const Param& q : hook.params)
            if (!q.isReturn && EqualsNoCase(q.name, candidate) && q.kind == ParamKind::Object)
                p = &q;
        if (p != nullptr) break;
    }
    if (p == nullptr) p = FirstOfKind(hook, ParamKind::Object);
    UObject* obj = ReadObject(p, base);
    if (obj == nullptr)
        return {};
    return Narrow(obj->GetFullName());
}

// StartPS5Vibration(SoundVibration, FadeInTime, Level). "Absent" and "0.0" need different
// fixes, so they are distinguished rather than both collapsing to a fallback — that is the
// `lvSeen` flag the working Lua mod carried.
bool LevelArg(const HookInfo& hook, uint8_t* base, float& out)
{
    const Param* named = nullptr;
    for (const Param& p : hook.params)
        if (!p.isReturn && EqualsNoCase(p.name, "Level"))
            named = &p;
    if (ReadFloat(named, base, out))
        return true;
    // Fall back to the LAST float parameter, which is what the Lua mod did.
    const Param* last = nullptr;
    for (const Param& p : hook.params)
        if (!p.isReturn && (p.kind == ParamKind::Float || p.kind == ParamKind::Double))
            last = &p;
    return ReadFloat(last, base, out);
}

// ---------------------------------------------------------------------------------------
// The hook table. Paths are the ones the working Lua mod used, verbatim.
// ---------------------------------------------------------------------------------------
constexpr const wchar_t* kComp =
    L"/Game/Technical/Components/COMP_CatScratchableComponent.COMP_CatScratchableComponent_C:";
constexpr const wchar_t* kPc =
    L"/Game/Technical/BP_HKPlayerController.BP_HKPlayerController_C:";

std::vector<HookInfo> g_hooks;

void RegisterAll();
void BuildHookTable();

// ---------------------------------------------------------------------------------------
// PadVibrationEnabled — HKGameUserSettings' only vibration control (§9).
//
// READ ON THE GAME THREAD, INSIDE A HOOK. It used to be polled from on_update, and that was
// wrong: on_update runs on UE4SS's OWN event-loop jthread
// (UE4SSProgram.cpp:431 `m_event_loop = std::jthread{&UE4SSProgram::update, this}`, looping
// with a 5 ms sleep), so reading a UObject there is an unsynchronised cross-thread read of
// state the engine mutates and the GC can move. A UFunction hook, by contrast, runs on
// whatever thread called the function — the game thread for these Blueprints.
//
// It is read only from StartPS5Vibration, which is rare. FindFirstOf walks the object array,
// so it must not go anywhere near the ~60 Hz SetPS5VibrationLevel path.
// ---------------------------------------------------------------------------------------
int  g_padVibeMisses = 0;
bool g_padVibeBound  = false;

void ReadPadVibrationEnabledOnGameThread()
{
    // Deliberately NOT cached across calls: a cached UObject*/FProperty* would have to survive
    // level transitions and GC, and nothing here establishes that it does. This runs on a rare
    // event, so the scan is affordable and correctness is worth more.
    UObject* settings = RC::Unreal::UObjectGlobals::FindFirstOf(STR("HKGameUserSettings"));
    if (settings == nullptr)
    {
        if (++g_padVibeMisses == 1)
            SDS_LOG_WARN("HKGameUserSettings not found; PadVibrationEnabled cannot be honoured "
                         "and haptics will play regardless of the setting.");
        return;
    }

    FProperty* prop = settings->GetPropertyByNameInChain(STR("PadVibrationEnabled"));
    // A UE bool is usually a `uint8 b : 1` bitfield, so it MUST go through FBoolProperty. A raw
    // bool* read would be right for a native bool and wrong for a bitfield — correct on someone
    // else's machine, which is the worst kind of wrong.
    if (prop == nullptr || !prop->IsA<FBoolProperty>())
    {
        if (++g_padVibeMisses == 1)
            SDS_LOG_WARN("HKGameUserSettings has no bool property 'PadVibrationEnabled'; "
                         "haptics will play regardless of the setting.");
        return;
    }

    if (!g_padVibeBound)
    {
        g_padVibeBound = true;
        SDS_LOG_INFO("HKGameUserSettings.PadVibrationEnabled bound");
    }
    sds::Rt().OnPadVibrationEnabled(
        static_cast<FBoolProperty*>(prop)->GetPropertyValueInContainer(settings));
}

// ---------------------------------------------------------------------------------------
// The mod object.
// ---------------------------------------------------------------------------------------
class StrayDualSenseMod : public RC::CppUserModBase
{
  public:
    StrayDualSenseMod() : CppUserModBase()
    {
        ModName        = STR("StrayDualSense");
        ModVersion     = Widen(SDS_VERSION_STRING);
        ModDescription = STR("DualSense adaptive triggers, haptics and controller speaker for Stray");
        ModAuthors     = STR("stray-dlss");

        // Deliberately NOT set: ModIntendedSDKVersion. Leaving it empty makes UE4SS fill in
        // the version this DLL was BUILT against, which is the honest value.

        // The runtime brings up the pad, the workers and our own log file. It touches nothing
        // Unreal, so it is safe this early — and safe on whatever thread UE4SS constructs us on.
        sds::Rt().Startup(reinterpret_cast<void*>(&ReadPadVibrationEnabledOnGameThread));
        BuildHookTable();
        Say(STR("[StrayDualSense] ") + Widen(SDS_VERSION_STRING) +
            STR(" loaded; log is <game>/stray-dualsense.log"));
    }

    ~StrayDualSenseMod() override
    {
        sds::Rt().Shutdown();
    }

    // Fired from UE4SSProgram::init (UE4SSProgram.cpp:421), before the event-loop thread even
    // exists — so this is NOT the game thread either. Registering hooks off the game thread is
    // what UE4SS itself does (LiveView.cpp does the same), so it is the supported pattern.
    auto on_unreal_init() -> void override
    {
        SDS_LOG_INFO("on_unreal_init: the Unreal module is up; hook registration begins");
        RegisterAll();
    }

    // THIS IS NOT THE GAME THREAD. UE4SS fires it from its own event-loop jthread
    // (UE4SSProgram.cpp:431), which loops with a 5 ms sleep — so roughly 200 Hz, unsynchronised
    // with game frames. Nothing here may read a UObject; everything here is our own state,
    // guarded by its own mutexes and atomics.
    auto on_update() -> void override
    {
        sds::Rt().Tick();
        RegisterAll();          // self-limiting: returns immediately once everything is bound
        DrainLogMirror();
    }

  private:
    // Our logger runs on five threads (the game thread via hooks, this one, and the trigger,
    // haptic and speaker workers) and deliberately never calls UE4SS's Output itself. Lines are
    // mirrored here instead, so UE4SS's logging is only ever reached from the thread UE4SS
    // itself calls us on.
    static void DrainLogMirror()
    {
        for (const std::string& line : sds::Log::TakeMirrorLines())
            Say(STR("[StrayDualSense] ") + Widen(line));
    }
};

// ---------------------------------------------------------------------------------------
// Registration.
// ---------------------------------------------------------------------------------------
std::chrono::steady_clock::time_point g_lastAttempt{};
int  g_attempts = 0;
bool g_gaveUp   = false;

void DescribeParams(HookInfo& hook)
{
    hook.params.clear();
    if (hook.function == nullptr)
        return;

    std::string summary;
    for (FProperty* prop : hook.function->ForEachProperty())
    {
        // CPF_Parm excludes a Blueprint function's LOCAL variables, which also live in this
        // list; without it the ordinals would silently drift.
        if (!prop->HasAnyPropertyFlags(RC::Unreal::CPF_Parm))
            continue;

        Param p;
        p.name     = Narrow(prop->GetName());
        p.offset   = prop->GetOffset_ForInternal();
        p.size     = prop->GetSize();
        p.kind     = ClassifyProperty(prop);
        p.isReturn = prop->HasAnyPropertyFlags(RC::Unreal::CPF_ReturnParm);
        if (p.kind == ParamKind::Bool)
            p.boolProp = static_cast<FBoolProperty*>(prop);

        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s%s:%s[%d]@%d%s", summary.empty() ? "" : ", ",
                      p.name.c_str(), KindName(p.kind), p.size, p.offset,
                      p.isReturn ? "(ret)" : "");
        summary += buf;
        hook.params.push_back(std::move(p));
    }
    // This line is the whole point of reading params reflectively: it says exactly which
    // bytes we will read, so a layout surprise is visible in one pasted log.
    SDS_LOG_INFO("  params of %s: %s", hook.shortName,
                 summary.empty() ? "(none)" : summary.c_str());
}

void RegisterAll()
{
    if (g_gaveUp)
        return;

    bool allDone = true;
    for (const HookInfo& h : g_hooks)
        if (!h.registered) { allDone = false; break; }
    if (allDone)
        return;

    const auto  now  = std::chrono::steady_clock::now();
    const float wait = sds::Rt().Cfg().hookRetrySeconds;
    if (g_lastAttempt.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastAttempt).count() <
            static_cast<long long>(wait * 1000.0f))
        return;
    g_lastAttempt = now;
    ++g_attempts;

    for (HookInfo& hook : g_hooks)
    {
        if (hook.registered)
            continue;

        // Resolve the UFunction OURSELVES rather than using RegisterHook's string overload:
        // that overload does not null-check the result of the lookup, so a name that has not
        // loaded yet is a null dereference rather than a retryable miss.
        hook.function =
            RC::Unreal::UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, hook.path);
        if (hook.function == nullptr)
            continue;

        DescribeParams(hook);

        try
        {
            // RegisterHook handles BOTH native and Blueprint UFunctions, and throws
            // std::runtime_error for anything it cannot classify.
            //
            // The POST callback must be a real callable, never a default-constructed
            // std::function: UObjectGlobals.cpp's GlobalScriptHookPost invokes
            // `Callable.CallablePost(...)` UNCONDITIONALLY, so an empty one would throw
            // std::bad_function_call inside the game's script VM. We want no post behaviour,
            // so it is an explicit no-op.
            hook.ids = RC::Unreal::UObjectGlobals::RegisterHook(
                hook.function, hook.callback,
                [](UnrealScriptFunctionCallableContext&, void*) {}, &hook);
            hook.registered = true;
            sds::Rt().NoteHookRegistered(hook.shortName);
        }
        catch (const std::exception& e)
        {
            SDS_LOG_ERROR("RegisterHook threw for %s: %s", hook.shortName, e.what());
        }
    }

    // 40 attempts x 3 s ~= 2 minutes, the same budget the working Lua mod used.
    if (g_attempts > 40)
    {
        g_gaveUp = true;
        for (const HookInfo& h : g_hooks)
            if (!h.registered)
                sds::Rt().NoteHookMissing(h.shortName);
    }
}

// ---------------------------------------------------------------------------------------
// Callbacks. Each does the minimum on the game thread: read the params, hand the intent over.
// ---------------------------------------------------------------------------------------
#define SDS_HOOK_PROLOGUE(varName)                                                              \
    auto* varName = static_cast<HookInfo*>(customData);                                          \
    if (varName == nullptr) return;                                                              \
    uint8_t* base = context.TheStack.Locals()

void CbTriggerActivated(UnrealScriptFunctionCallableContext& context, void* customData)
{
    SDS_HOOK_PROLOGUE(hook);
    bool    state = false;
    int64_t side  = -1;
    const bool gotState = ReadBool(FindParam(*hook, "State", 0), base, state);
    const bool gotSide  = ReadInt(FindParam(*hook, "Side", 1), base, side);
    if (!gotState || !gotSide)
    {
        // Loud: guessing here is how one trigger ends up hardened for the rest of the session.
        SDS_LOG_ERROR("SetPS5TriggerActivated: could not read State(%d)/Side(%d) from the "
                      "reflected parameters. NOT touching the triggers.",
                      gotState ? 1 : 0, gotSide ? 1 : 0);
        return;
    }
    sds::Rt().OnTriggerActivated(state, static_cast<int>(side));
}

void CbUseStarted(UnrealScriptFunctionCallableContext&, void*)
{
    sds::Rt().OnUseStarted();
}

void CbAfterUseDone(UnrealScriptFunctionCallableContext&, void*)
{
    sds::Rt().OnAfterUseDone();
}

void CbStartVibration(UnrealScriptFunctionCallableContext& context, void* customData)
{
    SDS_HOOK_PROLOGUE(hook);
    // We are on the game thread here, which is the only place a UObject read is sound.
    ReadPadVibrationEnabledOnGameThread();
    float      level = 1.0f;
    const bool seen  = LevelArg(*hook, base, level);
    sds::Rt().OnStartVibration(ObjectArgFullName(*hook, base), level, seen);
}

// StartPS5VibrationOnAudioComponent is OBSERVED, NOT ACTED ON — the working Lua mod logged it
// and nothing more. Its object argument is an AudioComponent, not the SoundWave, so deriving an
// envelope name from it would look up an asset that cannot exist and produce a "MISSING"
// error every time it fires. Log what it actually carries so a future session can decide.
void CbStartVibrationOnAudioComponent(UnrealScriptFunctionCallableContext& context,
                                      void* customData)
{
    SDS_HOOK_PROLOGUE(hook);
    float      level = 1.0f;
    const bool seen  = LevelArg(*hook, base, level);
    SDS_LOG_INFO("StartPS5VibrationOnAudioComponent object='%s' level=%.3f (seen=%d) "
                 "- OBSERVED ONLY, nothing is played",
                 ObjectArgFullName(*hook, base).c_str(), static_cast<double>(level),
                 seen ? 1 : 0);
}

void CbStopVibration(UnrealScriptFunctionCallableContext&, void*)
{
    sds::Rt().OnStopVibration();
}

void CbSetVibrationLevel(UnrealScriptFunctionCallableContext& context, void* customData)
{
    SDS_HOOK_PROLOGUE(hook);
    float level = 1.0f;
    if (LevelArg(*hook, base, level))
        sds::Rt().OnSetVibrationLevel(level);
}

void CbStartControllerSound(UnrealScriptFunctionCallableContext& context, void* customData)
{
    SDS_HOOK_PROLOGUE(hook);
    float      level = 1.0f;
    const bool seen  = LevelArg(*hook, base, level);
    sds::Rt().OnStartControllerSound(ObjectArgFullName(*hook, base), level, seen);
}

void CbStopControllerSound(UnrealScriptFunctionCallableContext&, void*)
{
    sds::Rt().OnStopControllerSound();
}

void CbSetControllerSoundLevel(UnrealScriptFunctionCallableContext& context, void* customData)
{
    SDS_HOOK_PROLOGUE(hook);
    float level = 1.0f;
    if (LevelArg(*hook, base, level))
        sds::Rt().OnSetControllerSoundLevel(level);
}

#undef SDS_HOOK_PROLOGUE

RC::StringType Join(const wchar_t* prefix, const wchar_t* name)
{
    return RC::StringType(prefix) + RC::StringType(name);
}

void BuildHookTable()
{
    struct Row { const wchar_t* prefix; const wchar_t* name; const char* shortName;
                 RC::Unreal::UnrealScriptFunctionCallable cb; };
    const Row rows[] = {
        // Adaptive triggers — the game drives each side separately (§8).
        { kComp, L"SetPS5TriggerActivated", "SetPS5TriggerActivated", &CbTriggerActivated },
        { kComp, L"_OnUseStarted",          "_OnUseStarted",          &CbUseStarted },
        { kComp, L"_OnAfterUseDone",        "_OnAfterUseDone",        &CbAfterUseDone },
        // Haptics.
        { kPc,   L"StartPS5Vibration",                 "StartPS5Vibration",                 &CbStartVibration },
        { kPc,   L"StartPS5VibrationOnAudioComponent", "StartPS5VibrationOnAudioComponent", &CbStartVibrationOnAudioComponent },
        { kPc,   L"StopPS5Vibration",                  "StopPS5Vibration",                  &CbStopVibration },
        { kPc,   L"SetPS5VibrationLevel",              "SetPS5VibrationLevel",              &CbSetVibrationLevel },
        // Controller speaker.
        { kPc,   L"StartPS5ControllerSound",                 "StartPS5ControllerSound",                 &CbStartControllerSound },
        { kPc,   L"StartPS5ControllerSoundOnAudioComponent", "StartPS5ControllerSoundOnAudioComponent", &CbStartControllerSound },
        { kPc,   L"StopPS5ControllerSound",                  "StopPS5ControllerSound",                  &CbStopControllerSound },
        { kPc,   L"SetPS5ControllerSoundLevel",              "SetPS5ControllerSoundLevel",              &CbSetControllerSoundLevel },
    };

    g_hooks.reserve(std::size(rows));
    for (const Row& r : rows)
    {
        HookInfo info;
        info.path      = Join(r.prefix, r.name);
        info.shortName = r.shortName;
        info.callback  = r.cb;
        g_hooks.push_back(std::move(info));
    }
}

} // namespace

#define STRAY_DUALSENSE_API __declspec(dllexport)
extern "C"
{
    // Resolved by literal name in UE4SS/src/Mod/CppMod.cpp; if either is missing UE4SS
    // FreeLibrary's the DLL and logs a warning.
    STRAY_DUALSENSE_API RC::CppUserModBase* start_mod()
    {
        return new StrayDualSenseMod();
    }

    STRAY_DUALSENSE_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
