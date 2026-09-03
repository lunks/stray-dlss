// StrayDualSense — the UE4SS glue, and NOTHING else.
//
// This is the only file in the mod that includes a UE4SS header. Everything it does is
// translate a UFunction callback into a call on `sds::Runtime`, which knows nothing about
// UE4SS and is compiled and link-tested without it. That split is deliberate: the SDK is the
// part that cannot be verified from here, so it is kept as small as it can be made.
//
// PROVENANCE of every SDK call below, read out of RE-UE4SS at commit 68caddcf (the build on
// the target box) and out of a public vendored copy of its private `deps/first/Unreal`
// submodule at the same tree. HARD = read in the header. UNCONFIRMED = plausible from the
// tree but not yet compiled by CI or run in the game.
//
//   CppUserModBase, on_unreal_init/on_update                 UE4SS/include/Mod/CppUserModBase.hpp   HARD
//   start_mod / uninstall_mod resolved by literal name       UE4SS/src/Mod/CppMod.cpp               HARD
//   UObjectGlobals::RegisterHook(UFunction*, pre, post, void*)
//     -> std::pair<int,int>                                  Unreal/UObjectGlobals.hpp              HARD
//   UnrealScriptFunctionCallable =
//     std::function<void(Context&, void*)>                   Unreal/UFunctionStructs.hpp            HARD
//   ctx.TheStack.Locals() -> uint8*                          Unreal/FFrame.hpp                      HARD
//   UStruct::ForEachProperty() is DEPRECATED at 68caddcf; use RC::Unreal::TFieldRange<FProperty>(
//     owner, RC::Unreal::EFieldIterationFlags::IncludeDeprecated) instead (the header's own
//     [[deprecated(...)]] message names this exact replacement) Unreal/CoreUObject/UObject/
//     UnrealType.hpp                                          HARD (read in the header's own
//     deprecation attribute, which the compiler printed verbatim in CI run 33581494376)
//   FProperty::GetOffset_ForInternal / GetSize / IsA<>       Unreal/CoreUObject/UObject/UnrealType.hpp HARD
//   CPF_Parm / CPF_ReturnParm                                Unreal/UnrealFlags.hpp                 HARD
//   FBoolProperty::GetPropertyValueInContainer               Unreal/CoreUObject/UObject/UnrealType.hpp HARD
//   UObjectGlobals::FindFirstOf / StaticFindObject           Unreal/UObjectGlobals.hpp              HARD
//   UObject::GetPropertyByNameInChain / GetFullName          Unreal/UObject.hpp                     HARD
//   FStructProperty::GetStruct() -> UScriptStruct*           Unreal/Property/FStructProperty.hpp    UNCONFIRMED
//   UScriptStruct is-a UStruct (so ForEachProperty applies)  Unreal/UScriptStruct.hpp               UNCONFIRMED
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
#include <Unreal/Property/FStructProperty.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UScriptStruct.hpp>
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
#include "Platform.hpp"
#include "Runtime.hpp"
#include "TriggerEffect.hpp"
#include "Version.hpp"

namespace {

using RC::Unreal::FBoolProperty;
using RC::Unreal::FProperty;
using RC::Unreal::FStructProperty;
using RC::Unreal::UFunction;
using RC::Unreal::UnrealScriptFunctionCallableContext;
using RC::Unreal::UObject;
using RC::Unreal::UScriptStruct;

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

// Everything that reaches UE4SS's console goes through here. The text is a finished string
// rather than a fmt format string, so a stray '{' in a game asset name cannot become a format
// error; and Output::send THROWS when no output device is open (Output.hpp:
// THROW_INTERNAL_FILE_ERROR "there were no opened devices"), which must never be the thing
// that kills the mod. The file log is the real one.
void Say(const RC::StringType& line)
{
    try
    {
        RC::Output::send<RC::LogLevel::Verbose>(line + STR("\n"));
    }
    catch (...)
    {
    }
}

bool Contains(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
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

// ---------------------------------------------------------------------------------------
// Reflected fields. Used for both UFunction parameters and the members of the authored
// trigger-effect struct, because the failure mode is the same in both: a wrong offset is a
// silently wrong byte. Everything discovered is LOGGED so one pasted log answers "did we read
// the right bytes".
// ---------------------------------------------------------------------------------------
enum class FieldKind : uint8_t { Unknown, Bool, UInt8, Int32, Int64, Float, Double, Object };

const char* KindName(FieldKind k)
{
    switch (k)
    {
    case FieldKind::Bool:   return "bool";
    case FieldKind::UInt8:  return "uint8/enum";
    case FieldKind::Int32:  return "int32";
    case FieldKind::Int64:  return "int64";
    case FieldKind::Float:  return "float";
    case FieldKind::Double: return "double";
    case FieldKind::Object: return "object";
    case FieldKind::Unknown:
    default:                return "raw";
    }
}

struct Field
{
    std::string    name;
    int32_t        offset   = 0;
    int32_t        size     = 0;
    FieldKind      kind     = FieldKind::Unknown;
    FBoolProperty* boolProp = nullptr;   // bitfield-correct bool reads
    bool           isReturn = false;
};

FieldKind Classify(FProperty* prop)
{
    // FByteProperty covers TEnumAsByte; a UE `enum class` reflects as FEnumProperty, which we
    // do not classify and instead read by its reflected SIZE (see ReadInt).
    if (prop->IsA<RC::Unreal::FBoolProperty>())   return FieldKind::Bool;
    if (prop->IsA<RC::Unreal::FByteProperty>())   return FieldKind::UInt8;
    if (prop->IsA<RC::Unreal::FIntProperty>())    return FieldKind::Int32;
    if (prop->IsA<RC::Unreal::FInt64Property>())  return FieldKind::Int64;
    if (prop->IsA<RC::Unreal::FFloatProperty>())  return FieldKind::Float;
    if (prop->IsA<RC::Unreal::FDoubleProperty>()) return FieldKind::Double;
    if (prop->IsA<RC::Unreal::FObjectProperty>()) return FieldKind::Object;
    return FieldKind::Unknown;
}

// Walk a UStruct's properties into Fields. `paramsOnly` keeps CPF_Parm members only, which
// excludes a Blueprint function's LOCAL variables (they live in the same list and would
// otherwise shift the ordinals).
std::vector<Field> DescribeFields(RC::Unreal::UStruct* owner, bool paramsOnly, const char* what)
{
    std::vector<Field> fields;
    if (owner == nullptr)
        return fields;

    std::string summary;
    // ForEachProperty() itself is deprecated at this SHA (68caddcf); the header's own
    // [[deprecated(...)]] message names this exact replacement, IncludeDeprecated flag and all
    // — matching ForEachProperty()'s prior behaviour rather than TFieldRange's own ::Default.
    for (FProperty* prop : RC::Unreal::TFieldRange<FProperty>(owner, RC::Unreal::EFieldIterationFlags::IncludeDeprecated))
    {
        if (paramsOnly && !prop->HasAnyPropertyFlags(RC::Unreal::CPF_Parm))
            continue;
        Field f;
        f.name     = Narrow(prop->GetName());
        f.offset   = prop->GetOffset_ForInternal();
        f.size     = prop->GetSize();
        f.kind     = Classify(prop);
        f.isReturn = prop->HasAnyPropertyFlags(RC::Unreal::CPF_ReturnParm);
        if (f.kind == FieldKind::Bool)
            f.boolProp = static_cast<FBoolProperty*>(prop);

        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s%s:%s[%d]@%d%s", summary.empty() ? "" : ", ",
                      f.name.c_str(), KindName(f.kind), f.size, f.offset, f.isReturn ? "(ret)" : "");
        summary += buf;
        fields.push_back(std::move(f));
    }
    // This line is the whole point of reading reflectively: it says exactly which bytes we
    // will read, so a layout surprise is visible in one pasted log.
    SDS_LOG_INFO("  fields of %s: %s", what, summary.empty() ? "(none)" : summary.c_str());
    return fields;
}

const Field* FindByName(const std::vector<Field>& fields, const char* name)
{
    for (const Field& f : fields)
        if (!f.isReturn && EqualsNoCase(f.name, name))
            return &f;
    return nullptr;
}

// By NAME first, then by POSITION among the non-return params — what the working Lua mod did
// with A[2] and A[3].
const Field* FindByNameOrOrdinal(const std::vector<Field>& fields, const char* name, size_t ordinal)
{
    if (const Field* f = FindByName(fields, name))
        return f;
    size_t seen = 0;
    for (const Field& f : fields)
    {
        if (f.isReturn) continue;
        if (seen++ == ordinal)
            return &f;
    }
    return nullptr;
}

bool ReadBool(const Field* f, uint8_t* base, bool& out)
{
    if (f == nullptr || base == nullptr) return false;
    if (f->kind == FieldKind::Bool && f->boolProp != nullptr)
    {
        // Handles both a native bool and a `uint8 bFoo : 1` bitfield.
        out = f->boolProp->GetPropertyValueInContainer(base);
        return true;
    }
    if (f->kind == FieldKind::UInt8) { out = *(base + f->offset) != 0; return true; }
    return false;
}

bool ReadInt(const Field* f, uint8_t* base, int64_t& out)
{
    if (f == nullptr || base == nullptr) return false;
    uint8_t* at = base + f->offset;
    switch (f->kind)
    {
    case FieldKind::UInt8: out = *at; return true;
    case FieldKind::Int32: { int32_t v; std::memcpy(&v, at, sizeof(v)); out = v; return true; }
    case FieldKind::Int64: { int64_t v; std::memcpy(&v, at, sizeof(v)); out = v; return true; }
    case FieldKind::Bool:  { bool b = false; if (!ReadBool(f, base, b)) return false; out = b ? 1 : 0; return true; }
    case FieldKind::Unknown:
        // Size-driven fallback: an FEnumProperty carries no kind we classify, but its width is
        // reflected and an enum is just an unsigned integer of that width.
        switch (f->size)
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

bool ReadFloat(const Field* f, uint8_t* base, float& out)
{
    if (f == nullptr || base == nullptr) return false;
    uint8_t* at = base + f->offset;
    if (f->kind == FieldKind::Float)  { float  v; std::memcpy(&v, at, sizeof(v)); out = v; return true; }
    if (f->kind == FieldKind::Double) { double v; std::memcpy(&v, at, sizeof(v)); out = static_cast<float>(v); return true; }
    return false;
}

UObject* ReadObject(const Field* f, uint8_t* base)
{
    if (f == nullptr || base == nullptr || f->kind != FieldKind::Object) return nullptr;
    UObject* obj = nullptr;
    std::memcpy(&obj, base + f->offset, sizeof(obj));
    return obj;
}

// ---------------------------------------------------------------------------------------
// Argument discovery BY TYPE, not by position.
//
// The two hook shapes put the sound in different slots:
//   StartPS5Vibration(SoundVibration, FadeInTime, Level)
//   StartPS5VibrationOnAudioComponent(AudioComponent, SoundVibration, FadeInTime, Level,
//                                     VibrationComponent)
// Position-guessing was wrong three separate times in this project. So: the sound is the
// object argument that RESOLVES to a SoundWave, the component is the one that resolves to an
// AudioComponent, and the level is the LAST float argument (FadeInTime precedes Level).
// Names are honoured first when the reflected parameter carries the expected one.
// ---------------------------------------------------------------------------------------
struct ResolvedArgs
{
    std::string soundFullName;
    std::string componentFullName;
    float       level      = 1.0f;
    bool        levelSeen  = false;
    float       fadeIn     = 0.0f;
    float       fadeOut    = 0.0f;
    std::string description;   // every argument, for the log
};

ResolvedArgs ResolveArgs(const std::vector<Field>& fields, uint8_t* base)
{
    ResolvedArgs r;
    std::vector<float> floats;
    for (const Field& f : fields)
    {
        if (f.isReturn) continue;
        char buf[200];
        if (f.kind == FieldKind::Object)
        {
            UObject* obj = ReadObject(&f, base);
            const std::string full = obj != nullptr ? Narrow(obj->GetFullName()) : std::string("null");
            if (obj != nullptr)
            {
                if (r.soundFullName.empty() && Contains(full, "SoundWave"))
                    r.soundFullName = full;
                else if (r.componentFullName.empty() && Contains(full, "AudioComponent"))
                    r.componentFullName = full;
            }
            std::snprintf(buf, sizeof(buf), " %s=%s", f.name.c_str(), full.c_str());
        }
        else if (f.kind == FieldKind::Float || f.kind == FieldKind::Double)
        {
            float v = 0.0f;
            ReadFloat(&f, base, v);
            floats.push_back(v);
            std::snprintf(buf, sizeof(buf), " %s=%.3f", f.name.c_str(), static_cast<double>(v));
        }
        else
        {
            int64_t v = 0;
            const bool ok = ReadInt(&f, base, v);
            std::snprintf(buf, sizeof(buf), " %s=%s%lld", f.name.c_str(), ok ? "" : "?",
                          static_cast<long long>(v));
        }
        r.description += buf;
    }

    // Level: by name, else the last float. Fade-in: by name, else the first float when there
    // are at least two (a lone float is the level). Fade-out: by name, else the first float.
    if (ReadFloat(FindByName(fields, "Level"), base, r.level))
        r.levelSeen = true;
    else if (!floats.empty())
    {
        r.level     = floats.back();
        r.levelSeen = true;
    }
    if (!ReadFloat(FindByName(fields, "FadeInTime"), base, r.fadeIn) && floats.size() >= 2)
        r.fadeIn = floats.front();
    if (!ReadFloat(FindByName(fields, "FadeOutTime"), base, r.fadeOut) && !floats.empty())
        r.fadeOut = floats.front();
    return r;
}

// ---------------------------------------------------------------------------------------
// The hook table. Paths are the ones the working Lua mod used, verbatim.
// ---------------------------------------------------------------------------------------
constexpr const wchar_t* kComp =
    L"/Game/Technical/Components/COMP_CatScratchableComponent.COMP_CatScratchableComponent_C:";
constexpr const wchar_t* kPc =
    L"/Game/Technical/BP_HKPlayerController.BP_HKPlayerController_C:";
constexpr const wchar_t* kInput = L"/Script/Hk_project.InputSubsystem:";

struct HookInfo
{
    RC::StringType path;              // owned: HookInfo outlives every lookup
    const char*    shortName = nullptr;
    RC::Unreal::UnrealScriptFunctionCallable callback;   // PRE: before the body runs
    RC::Unreal::UnrealScriptFunctionCallable post;       // POST: after it; may be empty

    bool                registered = false;
    std::pair<int, int> ids{};
    UFunction*          function = nullptr;
    std::vector<Field>  params;
};

std::vector<HookInfo> g_hooks;

void RegisterAll();
void BuildHookTable();
void MaybeBindSubmixOnGameThread();

// ---------------------------------------------------------------------------------------
// Reads of GAME STATE. Both run ON THE GAME THREAD, INSIDE A HOOK, and nowhere else: on_update
// runs on UE4SS's own event-loop jthread (UE4SSProgram.cpp:431), so a UObject read there is
// an unsynchronised cross-thread read of state the engine mutates and the GC can move.
// Neither caches a UObject* or FProperty* across calls — nothing establishes that they survive
// a level transition — and both run on rare events, so the FindFirstOf scan is affordable.
// ---------------------------------------------------------------------------------------
int  g_padVibeMisses = 0;
bool g_padVibeBound  = false;

void ReadPadVibrationEnabledOnGameThread()
{
    UObject* settings = RC::Unreal::UObjectGlobals::FindFirstOf(STR("HKGameUserSettings"));
    if (settings == nullptr)
    {
        if (++g_padVibeMisses == 1)
            SDS_LOG_WARN("HKGameUserSettings not found; PadVibrationEnabled cannot be honoured "
                         "and haptics will play regardless of the setting.");
        return;
    }
    FProperty* prop = settings->GetPropertyByNameInChain(STR("PadVibrationEnabled"));
    // A UE bool is usually a `uint8 b : 1` bitfield, so it MUST go through FBoolProperty.
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

// HKPlayerController::m_scratchablePS5TriggerEffect — PS5TriggerEffectData {Mode, Value1,
// Value2, Value3}, MEASURED at +0x730 reading mode=3 v1=0 v2=2 v3=0 (§13). Read through the
// struct's own reflection rather than the measured offset: the offset is logged and compared,
// so a licensee edit shows up as a log line rather than as a wrong byte.
constexpr int32_t kMeasuredEffectOffset = 0x730;
int  g_effectMisses  = 0;
bool g_effectLayoutLogged = false;

void ReadAuthoredTriggerEffectOnGameThread()
{
    sds::TriggerEffect effect;
    bool ok = false;

    UObject* pc = RC::Unreal::UObjectGlobals::FindFirstOf(STR("HKPlayerController"));
    if (pc == nullptr)
    {
        if (++g_effectMisses == 1)
            SDS_LOG_WARN("trigger effect: no HKPlayerController instance found");
        sds::Rt().OnTriggerEffectRead(effect, false);
        return;
    }

    FProperty* prop = pc->GetPropertyByNameInChain(STR("m_scratchablePS5TriggerEffect"));
    if (prop == nullptr || !prop->IsA<FStructProperty>())
    {
        if (++g_effectMisses == 1)
            SDS_LOG_WARN("trigger effect: HKPlayerController has no struct property "
                         "'m_scratchablePS5TriggerEffect' (found=%d)", prop != nullptr ? 1 : 0);
        sds::Rt().OnTriggerEffectRead(effect, false);
        return;
    }

    const int32_t offset = prop->GetOffset_ForInternal();
    UScriptStruct* type = static_cast<FStructProperty*>(prop)->GetStruct();
    if (!g_effectLayoutLogged)
    {
        g_effectLayoutLogged = true;
        SDS_LOG_INFO("trigger effect: m_scratchablePS5TriggerEffect at +0x%X (measured +0x%X%s), "
                     "struct %s", static_cast<unsigned>(offset),
                     static_cast<unsigned>(kMeasuredEffectOffset),
                     offset == kMeasuredEffectOffset ? ", matches" : ", DIFFERS",
                     type != nullptr ? Narrow(type->GetName()).c_str() : "null");
    }
    if (type == nullptr)
    {
        sds::Rt().OnTriggerEffectRead(effect, false);
        return;
    }

    // Fields are re-described only until the layout is logged once; cheap either way.
    static std::vector<Field> fields;
    if (fields.empty())
        fields = DescribeFields(type, false, "PS5TriggerEffectData");

    uint8_t* base = reinterpret_cast<uint8_t*>(pc) + offset;
    int64_t mode = 0, v1 = 0, v2 = 0, v3 = 0;
    ok = ReadInt(FindByName(fields, "Mode"),   base, mode) &&
         ReadInt(FindByName(fields, "Value1"), base, v1)   &&
         ReadInt(FindByName(fields, "Value2"), base, v2)   &&
         ReadInt(FindByName(fields, "Value3"), base, v3);
    if (!ok)
    {
        if (++g_effectMisses == 1)
            SDS_LOG_WARN("trigger effect: PS5TriggerEffectData lacks readable Mode/Value1..3 "
                         "(see the 'fields of' line above)");
        sds::Rt().OnTriggerEffectRead(effect, false);
        return;
    }
    effect.mode   = static_cast<int>(mode);
    effect.value1 = static_cast<uint8_t>(v1 < 0 ? 0 : (v1 > 255 ? 255 : v1));
    effect.value2 = static_cast<uint8_t>(v2 < 0 ? 0 : (v2 > 255 ? 255 : v2));
    effect.value3 = static_cast<uint8_t>(v3 < 0 ? 0 : (v3 > 255 ? 255 : v3));
    sds::Rt().OnTriggerEffectRead(effect, true);
}

// ---------------------------------------------------------------------------------------
// BP_HKPlayerController_C.DebugPS5Haptic — the PS5 haptic gate, MEASURED 2026-09-03 with the
// StrayAudioProbe Lua: with it false, StartPS5Vibration is entered and does nothing; with it
// true, the Blueprint sets the ControllerVibration AudioComponent's sound and plays it
// (AC.SetSound / AC.Play fired, IsPlaying()=true). It is a Blueprint variable, so it is a
// whole-byte bool, but it is written through FBoolProperty regardless. Written from the START
// pre-hooks — before the body evaluates its gate — and cached per controller instance so the
// FindFirstOf does not run on the 60 Hz level hook.
// ---------------------------------------------------------------------------------------
UObject* g_gateWrittenOn = nullptr;
int      g_gateMisses    = 0;

void ForcePS5HapticPathOnGameThread()
{
    if (!sds::Rt().Cfg().forcePS5HapticPath)
        return;
    UObject* pc = RC::Unreal::UObjectGlobals::FindFirstOf(STR("HKPlayerController"));
    if (pc == nullptr)
    {
        if (++g_gateMisses == 1)
            SDS_LOG_WARN("ForcePS5HapticPath: no HKPlayerController instance yet");
        return;
    }
    FProperty* prop = pc->GetPropertyByNameInChain(STR("DebugPS5Haptic"));
    if (prop == nullptr || !prop->IsA<FBoolProperty>())
    {
        if (++g_gateMisses == 1)
            SDS_LOG_ERROR("ForcePS5HapticPath: the player controller has no bool property "
                          "'DebugPS5Haptic' (found=%d). The PS5 haptic gate cannot be opened.",
                          prop != nullptr ? 1 : 0);
        return;
    }
    auto* boolProp = static_cast<FBoolProperty*>(prop);
    const bool before = boolProp->GetPropertyValueInContainer(pc);
    if (before && g_gateWrittenOn == pc)
        return;
    boolProp->SetPropertyValueInContainer(pc, true);
    const bool after = boolProp->GetPropertyValueInContainer(pc);
    g_gateWrittenOn = pc;
    SDS_LOG_INFO("ForcePS5HapticPath: %s.DebugPS5Haptic %d -> %d (offset +0x%X)%s",
                 Narrow(pc->GetName()).c_str(), before ? 1 : 0, after ? 1 : 0,
                 static_cast<unsigned>(prop->GetOffset_ForInternal()),
                 after ? "" : "   <- THE WRITE DID NOT TAKE");
}

// ---------------------------------------------------------------------------------------
// Button glyphs: /Script/Hk_project.InputSubsystem:GetGameControllerType, a NATIVE UFunction
// whose reflected shape is MEASURED from the object dump: `_forceGamepad` (bool @0) and
// `ReturnValue` (EGameControllerType, 1 byte @1; 1 XBOX, 2 PS4, 3 PS5). UMG_KeyIcon's Set Key
// calls it to pick the prompt texture. For a native function the return value the CALLER
// receives is what lands in RESULT_DECL (UE4SS's own Lua post-hook writes there and its
// comment says so: "If this was a native UFunction then changing the return value here will
// have the desired effect"); the copy in the parameter frame is written too, so both places
// agree whichever one a reader picks up. The observed shape is logged ONCE, before the first
// write, so a wrong assumption is visible rather than silent — the Lua predecessor guessed
// the arity and logged every argument for the same reason.
// ---------------------------------------------------------------------------------------
unsigned long g_glyphCalls  = 0;
unsigned long g_glyphForced = 0;
bool          g_glyphShapeLogged = false;

void CbGlyphPre(UnrealScriptFunctionCallableContext&, void*)
{
    MaybeBindSubmixOnGameThread();
}

void CbGlyphPost(UnrealScriptFunctionCallableContext& context, void* customData)
{
    auto* hook = static_cast<HookInfo*>(customData);
    if (hook == nullptr) return;
    ++g_glyphCalls;

    uint8_t* result = static_cast<uint8_t*>(context.RESULT_DECL);
    uint8_t* locals = context.TheStack.Locals();
    const Field* ret = nullptr;
    for (const Field& f : hook->params)
        if (f.isReturn) { ret = &f; break; }

    const int  want        = sds::Rt().Cfg().glyphControllerType;
    const int  beforeRes   = result != nullptr ? *result : -1;
    const int  beforeLocal = (locals != nullptr && ret != nullptr) ? locals[ret->offset] : -1;

    if (!g_glyphShapeLogged)
    {
        g_glyphShapeLogged = true;
        bool forceGamepad = false;
        const bool gotForce = ReadBool(FindByName(hook->params, "_forceGamepad"), locals, forceGamepad);
        SDS_LOG_INFO("GetGameControllerType observed: RESULT_DECL=%p (value %d) Locals=%p "
                     "ReturnValue@%d size %d (value %d) _forceGamepad=%s -> Glyphs=%s(%d)",
                     static_cast<void*>(result), beforeRes, static_cast<void*>(locals),
                     ret != nullptr ? ret->offset : -1, ret != nullptr ? ret->size : -1,
                     beforeLocal, gotForce ? (forceGamepad ? "true" : "false") : "?",
                     sds::Rt().Cfg().GlyphName(), want);
        if (ret == nullptr || ret->size != 1)
            SDS_LOG_ERROR("GetGameControllerType: the return parameter is not a 1-byte enum as "
                          "measured; NOT overriding glyphs.");
    }
    if (want < 0 || ret == nullptr || ret->size != 1)
        return;
    // Only a GAMEPAD answer is rewritten (1 XBOX, 2 PS4, 3 PS5, 4 SwitchPro). KeyboardMouse (5)
    // and Unknown (0) are left alone: the point is "this Xbox-looking pad is a DualSense", not
    // "always draw PlayStation prompts" - a keyboard user must keep keyboard prompts.
    const int observed = result != nullptr ? beforeRes : beforeLocal;
    if (observed < 1 || observed > 4)
        return;

    if (result != nullptr)
        *result = static_cast<uint8_t>(want);
    if (locals != nullptr)
        locals[ret->offset] = static_cast<uint8_t>(want);
    ++g_glyphForced;
    if (g_glyphForced <= 3 || g_glyphForced % 1000 == 0)
        SDS_LOG_INFO("GetGameControllerType -> %d (was %d/%d) [%lu of %lu calls forced]", want,
                     beforeRes, beforeLocal, g_glyphForced, g_glyphCalls);
}

// ---------------------------------------------------------------------------------------
// The submix tap's ONE piece of UE4SS glue: resolve three objects and hand them over as raw
// pointers. Everything that then happens to them is in SubmixDiscovery/SubmixTap, which know
// nothing about UE4SS and are compiled and link-tested without it.
//
// ON THE GAME THREAD, ALWAYS. This is called from the top of every UFunction hook and from
// nowhere else, for the same reason as the two reads above: on_update runs on UE4SS's own
// event-loop jthread, and a UObject read there is an unsynchronised cross-thread read of
// state the engine mutates and the GC can move. The cost is that binding waits for the first
// hook to fire — which is exactly when haptics start mattering — and a session where no hook
// ever fires says so in the status line rather than looking like a silent submix.
// ---------------------------------------------------------------------------------------
std::chrono::steady_clock::time_point g_lastSubmixAttempt{};
int g_submixAttempts = 0;

// The scan walks tens of thousands of pointers ON THE GAME THREAD. A second between attempts is
// right while there is a real chance of success — the engine may still be starting its audio —
// but a session where it will never bind must not pay that forever, so it backs off.
constexpr int kSubmixFastAttempts   = 10;
constexpr int kSubmixFastIntervalMs = 1000;
constexpr int kSubmixSlowIntervalMs = 5000;

void MaybeBindSubmixOnGameThread()
{
    if (!sds::Rt().SubmixWantsBinding())
        return;
    const auto now = std::chrono::steady_clock::now();
    const int interval = g_submixAttempts < kSubmixFastAttempts ? kSubmixFastIntervalMs
                                                                : kSubmixSlowIntervalMs;
    if (g_lastSubmixAttempt.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastSubmixAttempt).count() < interval)
        return;
    g_lastSubmixAttempt = now;
    ++g_submixAttempts;

    const void* imageBase = nullptr;
    size_t      imageSize = 0;
    if (!sds::MainModuleRange(imageBase, imageSize))
    {
        SDS_LOG_ERROR("submix: the game executable's image range could not be read; the tap "
                      "cannot validate a vtable and will not run.");
        return;
    }

    UObject* world  = RC::Unreal::UObjectGlobals::FindFirstOf(STR("World"));
    UObject* engine = RC::Unreal::UObjectGlobals::FindFirstOf(STR("Engine"));

    // The submix, by the exact path measured in the box's own UE4SS object dump. The literal
    // "master" means "do not look one up" — a null submix is the engine's own shorthand for
    // the master submix (AudioMixerDevice.cpp:2350).
    UObject* submix = nullptr;
    bool     submixResolved = false;
    const std::string& path = sds::Rt().Cfg().submixPath;
    if (path == "master")
    {
        submixResolved = true;
    }
    else
    {
        submix = RC::Unreal::UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr,
                                                                       Widen(path));
        if (submix != nullptr)
        {
            const std::string full = Narrow(submix->GetFullName());
            // FMixerSubmix::ProcessAudio only invokes buffer listeners when the owning object
            // Casts to USoundSubmix (AudioMixerSubmix.cpp:1370) — an endpoint or soundfield
            // submix would register cleanly and then never call us, which is the exact
            // silent-null-result this spike must not produce.
            const bool isPlainSubmix = full.rfind("SoundSubmix ", 0) == 0;
            SDS_LOG_INFO("submix: resolved '%s' -> %s%s", path.c_str(), full.c_str(),
                         isPlainSubmix ? ""
                                       : "   <- NOT a plain SoundSubmix. UE 4.27 only calls "
                                         "buffer listeners for USoundSubmix, so this will "
                                         "register and then never fire.");
            submixResolved = true;
        }
    }

    // THE REROUTE's UObject half (docs/STRAY-DUALSENSE.md §14): the parent's OutputVolume goes
    // to 0 so nothing leaks into the speakers, and the master's ParentSubmix points at it.
    // Both are plain UPROPERTY writes through reflection; the engine only acts on them when
    // the runtime asks it to re-register the two submixes (RebuildSubmixLinks reads
    // ParentSubmix, InitInternal reads OutputVolume). Idempotent, so it runs every attempt
    // and logs once.
    UObject* rerouteMaster = nullptr;
    UObject* rerouteParent = nullptr;
    if (sds::Rt().Cfg().submixReroute)
    {
        const sds::Config& cfg = sds::Rt().Cfg();
        rerouteMaster = RC::Unreal::UObjectGlobals::StaticFindObject<UObject*>(
            nullptr, nullptr, Widen(cfg.submixRerouteMaster));
        rerouteParent = RC::Unreal::UObjectGlobals::StaticFindObject<UObject*>(
            nullptr, nullptr, Widen(cfg.submixRerouteParent));
        static bool rerouteLogged = false;
        bool ok = rerouteMaster != nullptr && rerouteParent != nullptr;
        if (ok)
        {
            FProperty* volProp    = rerouteParent->GetPropertyByNameInChain(STR("OutputVolume"));
            FProperty* parentProp = rerouteMaster->GetPropertyByNameInChain(STR("ParentSubmix"));
            float    volBefore    = -1.0f;
            UObject* parentBefore = nullptr;
            if (volProp != nullptr && volProp->IsA<RC::Unreal::FFloatProperty>())
            {
                auto* fp = static_cast<RC::Unreal::FFloatProperty*>(volProp);
                volBefore = fp->GetPropertyValueInContainer(rerouteParent);
                fp->SetPropertyValueInContainer(rerouteParent, 0.0f);
            }
            else
                ok = false;
            if (parentProp != nullptr && parentProp->IsA<RC::Unreal::FObjectProperty>())
            {
                auto* op = static_cast<RC::Unreal::FObjectProperty*>(parentProp);
                void* addr = reinterpret_cast<uint8_t*>(rerouteMaster) + op->GetOffset_ForInternal();
                parentBefore = op->GetObjectPropertyValue(addr);
                op->SetObjectPropertyValue(addr, rerouteParent);
            }
            else
                ok = false;
            if (!rerouteLogged)
            {
                rerouteLogged = true;
                SDS_LOG_INFO("submix: REROUTE UObject writes: '%s'.OutputVolume %.3f -> 0.0 "
                             "(prop %s), '%s'.ParentSubmix %s -> %s (prop %s)%s",
                             Narrow(rerouteParent->GetName()).c_str(),
                             static_cast<double>(volBefore), volProp != nullptr ? "found" : "MISSING",
                             Narrow(rerouteMaster->GetName()).c_str(),
                             parentBefore != nullptr ? Narrow(parentBefore->GetName()).c_str() : "null",
                             Narrow(rerouteParent->GetName()).c_str(),
                             parentProp != nullptr ? "found" : "MISSING",
                             ok ? "" : "   <- INCOMPLETE, the reroute will NOT be submitted");
            }
        }
        else if (!rerouteLogged)
        {
            rerouteLogged = true;
            SDS_LOG_WARN("submix: REROUTE objects not loaded yet (master=%p parent=%p); retrying "
                         "with the bind", static_cast<void*>(rerouteMaster),
                         static_cast<void*>(rerouteParent));
        }
        if (!ok)
        {
            rerouteMaster = nullptr;
            rerouteParent = nullptr;
        }
    }

    static bool announced = false;
    if (!announced)
    {
        announced = true;
        SDS_LOG_INFO("submix: binding from the game thread. exe=%p+0x%zX world=%p engine=%p "
                     "target='%s'", imageBase, imageSize, static_cast<void*>(world),
                     static_cast<void*>(engine), path.c_str());
    }

    sds::Rt().BindSubmixTap(world, engine, submix, submixResolved, rerouteMaster, rerouteParent,
                            imageBase, imageSize);
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
        ModDescription = STR("DualSense adaptive triggers, coil haptics and controller speaker for Stray");
        ModAuthors     = STR("stray-dlss");
        // ModIntendedSDKVersion deliberately left empty: UE4SS fills in the version this DLL
        // was BUILT against, which is the honest value.

        // The runtime brings up the pad, the HID mode writer, the workers and our own log. It
        // touches nothing Unreal, so it is safe this early and on whatever thread UE4SS
        // constructs us on.
        sds::Rt().Startup(reinterpret_cast<const void*>(&ReadPadVibrationEnabledOnGameThread));
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
    // what UE4SS itself does (LiveView.cpp), so it is the supported pattern.
    auto on_unreal_init() -> void override
    {
        SDS_LOG_INFO("on_unreal_init: the Unreal module is up; hook registration begins");
        RegisterAll();
    }

    // THIS IS NOT THE GAME THREAD. UE4SS fires it from its own event-loop jthread
    // (UE4SSProgram.cpp:431), ~200 Hz, unsynchronised with game frames. Nothing here may read
    // a UObject; everything here is our own state, guarded by its own mutexes and atomics.
    auto on_update() -> void override
    {
        sds::Rt().Tick();
        RegisterAll();          // self-limiting: returns immediately once everything is bound
        DrainLogMirror();
    }

  private:
    // Our logger runs on six threads and never calls UE4SS's Output itself. Lines are mirrored
    // here, on the thread UE4SS itself calls us on.
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

        hook.params = DescribeFields(hook.function, true, hook.shortName);

        try
        {
            // RegisterHook handles BOTH native and Blueprint UFunctions, and throws
            // std::runtime_error for anything it cannot classify.
            //
            // The POST callback must be a real callable, never a default-constructed
            // std::function: UObjectGlobals.cpp's GlobalScriptHookPost invokes
            // `Callable.CallablePost(...)` UNCONDITIONALLY, so an empty one would throw
            // std::bad_function_call inside the game's script VM.
            hook.ids = RC::Unreal::UObjectGlobals::RegisterHook(
                hook.function, hook.callback,
                hook.post ? hook.post
                          : RC::Unreal::UnrealScriptFunctionCallable(
                                [](UnrealScriptFunctionCallableContext&, void*) {}),
                &hook);
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
// Every hook runs on the game thread, so every hook is a chance to bind the submix tap. The
// call is a single atomic read once bound, and rate-limited to 1 Hz before that.
#define SDS_HOOK_PROLOGUE(varName)                                                              \
    MaybeBindSubmixOnGameThread();                                                               \
    auto* varName = static_cast<HookInfo*>(customData);                                          \
    if (varName == nullptr) return;                                                              \
    uint8_t* base = context.TheStack.Locals()

void CbTriggerActivated(UnrealScriptFunctionCallableContext& context, void* customData)
{
    SDS_HOOK_PROLOGUE(hook);
    bool    state = false;
    int64_t side  = -1;
    const bool gotState = ReadBool(FindByNameOrOrdinal(hook->params, "State", 0), base, state);
    const bool gotSide  = ReadInt(FindByNameOrOrdinal(hook->params, "Side", 1), base, side);
    if (!gotState || !gotSide)
    {
        // Loud: guessing here is how one trigger ends up hardened for the rest of the session.
        SDS_LOG_ERROR("SetPS5TriggerActivated: could not read State(%d)/Side(%d) from the "
                      "reflected parameters. NOT touching the triggers.",
                      gotState ? 1 : 0, gotSide ? 1 : 0);
        return;
    }
    // The authored effect is read on each ENGAGE (twice per scratch, rare) so it follows the
    // game if a surface ever specifies something else, and before the first transmit.
    if (state)
        ReadAuthoredTriggerEffectOnGameThread();
    sds::Rt().OnTriggerActivated(state, static_cast<int>(side));
}

void CbUseStarted(UnrealScriptFunctionCallableContext&, void*)
{
    MaybeBindSubmixOnGameThread();
    sds::Rt().OnUseStarted();
}

void CbAfterUseDone(UnrealScriptFunctionCallableContext&, void*)
{
    MaybeBindSubmixOnGameThread();
    sds::Rt().OnAfterUseDone();
}

// Both StartPS5Vibration shapes share this: the arguments are resolved by TYPE.
void CbStartVibration(UnrealScriptFunctionCallableContext& context, void* customData)
{
    SDS_HOOK_PROLOGUE(hook);
    ForcePS5HapticPathOnGameThread();        // PRE-hook: before the Blueprint's gate runs
    ReadPadVibrationEnabledOnGameThread();   // game thread: the only sound place for it
    const ResolvedArgs a = ResolveArgs(hook->params, base);
    SDS_LOG_INFO("%s args:%s", hook->shortName, a.description.c_str());
    if (a.soundFullName.empty())
    {
        SDS_LOG_ERROR("%s: no argument resolved to a SoundWave; nothing to play", hook->shortName);
        return;
    }
    sds::VibrationStart s;
    s.soundFullName     = a.soundFullName;
    s.componentFullName = a.componentFullName;
    s.level             = a.level;
    s.levelSeen         = a.levelSeen;
    s.fadeIn            = a.fadeIn;
    sds::Rt().OnStartVibration(s);
}

void CbStopVibration(UnrealScriptFunctionCallableContext& context, void* customData)
{
    SDS_HOOK_PROLOGUE(hook);
    ForcePS5HapticPathOnGameThread();
    const ResolvedArgs a = ResolveArgs(hook->params, base);
    sds::Rt().OnStopVibration(a.fadeOut);
}

void CbStopVibrationOnComponent(UnrealScriptFunctionCallableContext& context, void* customData)
{
    SDS_HOOK_PROLOGUE(hook);
    const ResolvedArgs a = ResolveArgs(hook->params, base);
    // ~700 calls a session: only the description of a HONOURED stop is worth a line, and the
    // runtime logs that. An unresolvable component can never match, so it is ignored there.
    sds::Rt().OnStopVibrationOnComponent(a.componentFullName, a.fadeOut);
}

void CbSetVibrationLevel(UnrealScriptFunctionCallableContext& context, void* customData)
{
    SDS_HOOK_PROLOGUE(hook);
    const ResolvedArgs a = ResolveArgs(hook->params, base);
    if (a.levelSeen)
        sds::Rt().OnSetVibrationLevel(a.level);
}

void CbStartControllerSound(UnrealScriptFunctionCallableContext& context, void* customData)
{
    SDS_HOOK_PROLOGUE(hook);
    ForcePS5HapticPathOnGameThread();
    const ResolvedArgs a = ResolveArgs(hook->params, base);
    SDS_LOG_INFO("%s args:%s", hook->shortName, a.description.c_str());
    if (a.soundFullName.empty())
    {
        SDS_LOG_ERROR("%s: no argument resolved to a SoundWave; nothing to play", hook->shortName);
        return;
    }
    sds::Rt().OnStartControllerSound(a.soundFullName, a.level, a.levelSeen, a.fadeIn);
}

void CbStopControllerSound(UnrealScriptFunctionCallableContext& context, void* customData)
{
    SDS_HOOK_PROLOGUE(hook);
    const ResolvedArgs a = ResolveArgs(hook->params, base);
    sds::Rt().OnStopControllerSound(a.fadeOut);
}

void CbSetControllerSoundLevel(UnrealScriptFunctionCallableContext& context, void* customData)
{
    SDS_HOOK_PROLOGUE(hook);
    const ResolvedArgs a = ResolveArgs(hook->params, base);
    if (a.levelSeen)
        sds::Rt().OnSetControllerSoundLevel(a.level);
}

#undef SDS_HOOK_PROLOGUE

RC::StringType Join(const wchar_t* prefix, const wchar_t* name)
{
    return RC::StringType(prefix) + RC::StringType(name);
}

void BuildHookTable()
{
    struct Row { const wchar_t* prefix; const wchar_t* name; const char* shortName;
                 RC::Unreal::UnrealScriptFunctionCallable cb;
                 RC::Unreal::UnrealScriptFunctionCallable post = nullptr; };
    const Row rows[] = {
        // Button glyphs: the POST hook rewrites the native return value (Glyphs=ps5).
        { kInput, L"GetGameControllerType", "GetGameControllerType", &CbGlyphPre, &CbGlyphPost },
        // Adaptive triggers (§13). Only the component's SetPS5TriggerActivated matters;
        // SetPS5TriggersState is never called (0 times) and is not hooked.
        { kComp, L"SetPS5TriggerActivated", "SetPS5TriggerActivated", &CbTriggerActivated },
        { kComp, L"_OnUseStarted",          "_OnUseStarted",          &CbUseStarted },
        { kComp, L"_OnAfterUseDone",        "_OnAfterUseDone",        &CbAfterUseDone },
        // Haptics: both start shapes, the global stop, the per-component stop, both levels.
        { kPc, L"StartPS5Vibration",                    "StartPS5Vibration",                    &CbStartVibration },
        { kPc, L"StartPS5VibrationOnAudioComponent",    "StartPS5VibrationOnAudioComponent",    &CbStartVibration },
        { kPc, L"StopPS5Vibration",                     "StopPS5Vibration",                     &CbStopVibration },
        { kPc, L"StopPS5VibrationOnAudioComponent",     "StopPS5VibrationOnAudioComponent",     &CbStopVibrationOnComponent },
        { kPc, L"SetPS5VibrationLevel",                 "SetPS5VibrationLevel",                 &CbSetVibrationLevel },
        { kPc, L"SetPS5VibrationLevelOnAudioComponent", "SetPS5VibrationLevelOnAudioComponent", &CbSetVibrationLevel },
        // Controller speaker.
        { kPc, L"StartPS5ControllerSound",                 "StartPS5ControllerSound",                 &CbStartControllerSound },
        { kPc, L"StartPS5ControllerSoundOnAudioComponent", "StartPS5ControllerSoundOnAudioComponent", &CbStartControllerSound },
        { kPc, L"StopPS5ControllerSound",                  "StopPS5ControllerSound",                  &CbStopControllerSound },
        { kPc, L"SetPS5ControllerSoundLevel",              "SetPS5ControllerSoundLevel",              &CbSetControllerSoundLevel },
    };

    g_hooks.reserve(std::size(rows));
    for (const Row& r : rows)
    {
        HookInfo info;
        info.path      = Join(r.prefix, r.name);
        info.shortName = r.shortName;
        info.callback  = r.cb;
        info.post      = r.post;
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
