#include "SubmixDiscovery.hpp"

#include "Log.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace sds {
namespace submix {
namespace {

// A DeviceId is an index into the audio device manager. A game has one, maybe a handful.
constexpr std::uint32_t kMaxPlausibleDeviceId = 64;
// GUObjectArray indices. Generous; the point is only to reject 0xDEADBEEF-shaped garbage.
constexpr std::int32_t kMaxPlausibleObjectIndex = 200 * 1000 * 1000;
// How many ClassPrivate hops to take looking for the UClass fixed point.
constexpr int kUObjectChainDepth = 6;

bool InImage(const void* p, const void* base, std::size_t size)
{
    const auto v = reinterpret_cast<std::uintptr_t>(p);
    const auto b = reinterpret_cast<std::uintptr_t>(base);
    return size != 0 && v >= b && v < b + size;
}

const void* ReadPtr(const void* at)
{
    const void* p = nullptr;
    std::memcpy(&p, at, sizeof(p));
    return p;
}

std::int32_t ReadI32(const void* at)
{
    std::int32_t v = 0;
    std::memcpy(&v, at, sizeof(v));
    return v;
}

// Does this look like a C++ vtable for a class compiled into the game executable?
bool LooksLikeVtable(const void* vtable, const void* base, std::size_t size)
{
    if (!InImage(vtable, base, size))
        return false;
    if (!Readable(vtable, sizeof(void*) * kVtableProbeSlots))
        return false;
    const void* const* slots = static_cast<const void* const*>(vtable);
    for (int i = 0; i < kVtableProbeSlots; ++i)
    {
        if (slots[i] == nullptr || !InImage(slots[i], base, size))
            return false;
    }
    return true;
}

// The largest readable window at `p`, halved down from `want`. Objects near the end of a heap
// block are shorter than the window we would like; refusing to look at them at all would be
// indistinguishable from finding nothing, which is the ambiguity this file exists to remove.
std::size_t ReadableWindow(const void* p, std::size_t want)
{
    while (want >= 0x100 && !Readable(p, want))
        want /= 2;
    return Readable(p, want) ? want : 0;
}

// FAudioDevice::SampleRate is an int32 (AudioDevice.h:1789) with a second copy in the
// FAudioPlatformSettings right after it. A live audio device therefore holds at least one —
// usually two — values from the standard set; a random UObject holds none.
//
// Searched as int32 AND as float: the stock header says int32, but this is a LICENSEE build
// and a float costs nothing to check. The OFFSET of the first hit is recorded, so a run that
// finds one measures where it lives instead of us deriving it from stock headers.
std::int32_t FindSampleRate(const void* obj, int& hits, std::size_t& firstAt, int& distinct)
{
    hits = 0;
    firstAt = 0;
    distinct = 0;
    std::int32_t seen[8] = {};
    int seenCount = 0;
    const std::size_t window = ReadableWindow(obj, kFingerprintBytes);
    if (window < sizeof(std::int32_t))
        return 0;
    const auto* bytes = static_cast<const unsigned char*>(obj);
    std::int32_t found = 0;
    for (std::size_t k = 0; k + sizeof(std::int32_t) <= window; k += 4)
    {
        const std::int32_t v = ReadI32(bytes + k);
        bool hit = IsStandardSampleRate(v);
        if (!hit)
        {
            float f = 0.0f;
            std::memcpy(&f, bytes + k, sizeof(f));
            if (f > 7000.0f && f < 400000.0f &&
                IsStandardSampleRate(static_cast<std::int32_t>(f)) &&
                static_cast<float>(static_cast<std::int32_t>(f)) == f)
            {
                hit = true;
            }
        }
        if (!hit)
            continue;
        ++hits;
        std::int32_t value = v;
        if (!IsStandardSampleRate(value))
        {
            float f = 0.0f;
            std::memcpy(&f, bytes + k, sizeof(f));
            value = static_cast<std::int32_t>(f);
        }
        bool novel = true;
        for (int i = 0; i < seenCount; ++i)
            if (seen[i] == value) { novel = false; break; }
        if (novel && seenCount < 8)
        {
            seen[seenCount++] = value;
            ++distinct;
        }
        if (found == 0)
        {
            found   = value;
            firstAt = k;
        }
    }
    return found;
}

struct ScanContext
{
    const void* imageBase = nullptr;
    std::size_t imageSize = 0;
    std::vector<DeviceCandidate>* out = nullptr;
    std::size_t maxCandidates = 0;
};

// Record one pointee, if it is new and looks like a C++ object of a class in the exe.
//
// POINTER-CENTRIC, not shape-centric. The first design looked for the whole
// `{TWeakObjectPtr; FAudioDevice*; FDeviceId}` triple, which measured 2026-09-03 found the
// wrong things and missed the right one: `UWorld::AudioDeviceHandle.Device` is NULL when the
// world uses the main audio device (so there is no triple to find), and the surrounding bytes
// prove nothing anyway. What identifies an FAudioDevice is the OBJECT, so that is what we
// look at; the handle's neighbours are recorded as corroboration only.
bool Consider(ScanContext& ctx, const void* owner, std::size_t k, bool fromWorld,
              std::int32_t ownerIndex, bool viaContainer, std::size_t containerOffset)
{
    if (ctx.out->size() >= ctx.maxCandidates)
        return false;
    const auto* bytes = static_cast<const unsigned char*>(owner);
    const void* p = ReadPtr(bytes + k);

    if (p == nullptr || (reinterpret_cast<std::uintptr_t>(p) & 7u) != 0)
        return false;
    if (InImage(p, ctx.imageBase, ctx.imageSize))    // FAudioDevice is heap-allocated
        return false;
    if (!Readable(p, sizeof(void*)))
        return false;
    const void* vtable = ReadPtr(p);
    if (!LooksLikeVtable(vtable, ctx.imageBase, ctx.imageSize))
        return false;

    for (const DeviceCandidate& c : *ctx.out)
        if (c.device == p)
            return false;

    DeviceCandidate cand;
    cand.device       = p;
    cand.vtable       = vtable;
    cand.offset       = k;
    cand.fromWorld    = fromWorld;
    cand.viaContainer    = viaContainer;
    cand.containerOffset = containerOffset;
    cand.isUObject    = LooksLikeUObject(p);
    cand.sampleRate   = FindSampleRate(p, cand.sampleRateHits, cand.sampleRateAt,
                                       cand.distinctRates);

    // Corroboration from the bytes AROUND the pointer, if the handle really is laid out as
    // {World; Device; DeviceId} with our pointer as Device:
    //   World.ObjectIndex at -8, DeviceId at +8, and in UEngine the manager at -16.
    if (!viaContainer)
    {
        if (k >= 8)
        {
            const std::int32_t objIndex = ReadI32(bytes + k - 8);
            cand.worldIndexMatch = fromWorld && ownerIndex > 0 && objIndex == ownerIndex;
        }
        if (Readable(bytes + k + 8, sizeof(std::int32_t)))
        {
            const std::uint32_t id = static_cast<std::uint32_t>(ReadI32(bytes + k + 8));
            if (id < kMaxPlausibleDeviceId)
                cand.id = id;
        }
        if (!fromWorld && k >= 16)
        {
            const void* mgr = ReadPtr(bytes + k - 16);
            cand.managerBefore = mgr != nullptr &&
                                 (reinterpret_cast<std::uintptr_t>(mgr) & 7u) == 0 &&
                                 !InImage(mgr, ctx.imageBase, ctx.imageSize) &&
                                 Readable(mgr, sizeof(void*));
        }
    }
    ctx.out->push_back(cand);
    return true;
}

// Every 8-aligned pointer in the object, and — one hop further — every pointer inside the
// plain heap blocks it points at. The second hop is how `FAudioDeviceManager` is followed
// WITHOUT knowing its layout: it has no vtable of its own, so it is invisible to the direct
// scan, but the devices it owns are not.
void ScanObject(ScanContext& ctx, const void* object, std::size_t scanBytes, bool fromWorld,
                std::int32_t ownerIndex, const char* what, bool verbose, bool secondHop)
{
    if (object == nullptr)
        return;
    const std::size_t window = ReadableWindow(object, scanBytes);
    if (window < 16)
    {
        SDS_LOG_WARN("submix discovery: %s at %p is not readable; skipped.", what, object);
        return;
    }
    // ALWAYS logged, not only when verbose: a window that quietly shrank is exactly how the
    // 2026-09-03 run missed UEngine::MainAudioDeviceHandle, and a silent shrink is
    // indistinguishable from "the thing is not there".
    if (verbose || window != scanBytes)
        SDS_LOG_INFO("submix discovery: scanning %s at %p over 0x%zX bytes%s", what, object,
                     window, window != scanBytes ? "  <- SHRUNK, the object ends sooner" : "");

    const std::size_t before = ctx.out->size();
    for (std::size_t k = 0; k + sizeof(void*) <= window; k += 8)
        Consider(ctx, object, k, fromWorld, ownerIndex, false, 0);
    const std::size_t direct = ctx.out->size() - before;

    if (!secondHop)
        return;

    // Second hop — THE FAudioDeviceManager PATH. `UEngine::AudioDeviceManager` (Engine.h:1732)
    // is a plain heap object with no vtable of its own, so the direct scan cannot see it; but
    // the FAudioDevices it owns are ordinary polymorphic objects, and its pointers to them look
    // like any other. Following one hop through every non-polymorphic heap block therefore
    // reaches the device through the manager WITHOUT needing to know the manager's layout — and
    // the container's own offset is recorded, so a hit names the manager's slot in UEngine.
    //
    // Bounded hard: containers are plain heap blocks and there can be many.
    constexpr std::size_t kMaxContainers   = 96;
    constexpr std::size_t kContainerWindow = 0x400;
    std::size_t containers = 0;
    const auto* bytes = static_cast<const unsigned char*>(object);
    for (std::size_t k = 0; k + sizeof(void*) <= window && containers < kMaxContainers; k += 8)
    {
        const void* p = ReadPtr(bytes + k);
        if (p == nullptr || (reinterpret_cast<std::uintptr_t>(p) & 7u) != 0)
            continue;
        if (InImage(p, ctx.imageBase, ctx.imageSize) || !Readable(p, sizeof(void*)))
            continue;
        // A container is a heap block that is NOT itself a polymorphic object — exactly what
        // FAudioDeviceManager is.
        if (LooksLikeVtable(ReadPtr(p), ctx.imageBase, ctx.imageSize))
            continue;
        const std::size_t inner = ReadableWindow(p, kContainerWindow);
        if (inner < 16)
            continue;
        ++containers;
        for (std::size_t j = 0; j + sizeof(void*) <= inner; j += 8)
            Consider(ctx, p, j, fromWorld, 0, true, k);
    }
    SDS_LOG_INFO("submix discovery: %s -> %zu direct candidate(s), %zu container(s) followed, "
                 "%zu total so far", what, direct, containers, ctx.out->size());
}

const char* SourceName(const DeviceCandidate& c)
{
    if (c.viaContainer) return c.fromWorld ? "world^" : "engin^";
    return c.fromWorld ? "UWorld" : "UEngin";
}

// The artefact that makes the NEXT run decisive when the sample-rate test is the thing that is
// wrong. The UObject rejection self-checks; this one cannot, so when nothing is accepted the
// raw words go in the log and the rate's offset becomes MEASURED rather than derived.
void DumpCandidate(const DeviceCandidate& c, int words)
{
    const std::size_t bytesWanted = static_cast<std::size_t>(words) * 4;
    const std::size_t window = ReadableWindow(c.device, bytesWanted);
    if (window < 4)
        return;
    SDS_LOG_INFO("  dump %s+0x%04zX device=%p (0x%zX bytes; look for BB80=48000 AC44=44100)",
                 SourceName(c), c.offset, c.device, window);
    const auto* b = static_cast<const unsigned char*>(c.device);
    char line[160];
    for (std::size_t k = 0; k + 4 <= window; k += 32)
    {
        int n = std::snprintf(line, sizeof(line), "    +0x%04zX ", k);
        for (std::size_t j = 0; j < 32 && k + j + 4 <= window; j += 4)
            n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "%08X ",
                               static_cast<unsigned>(ReadI32(b + k + j)));
        SDS_LOG_INFO("%s", line);
    }
}

} // namespace

bool Readable(const void* p, std::size_t bytes)
{
    if (p == nullptr || bytes == 0)
        return false;
    const auto* cur = static_cast<const unsigned char*>(p);
    const auto* end = cur + bytes;
    while (cur < end)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(cur, &mbi, sizeof(mbi)) != sizeof(mbi))
            return false;
        if (mbi.State != MEM_COMMIT)
            return false;
        const DWORD prot = mbi.Protect & 0xFFu;
        const bool readable = prot == PAGE_READONLY || prot == PAGE_READWRITE ||
                              prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READ ||
                              prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
        if (!readable || (mbi.Protect & PAGE_GUARD) != 0)
            return false;
        cur = static_cast<const unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

bool LooksLikeUObject(const void* p)
{
    // Walk ClassPrivate. For any UObject the chain ends at the `UClass` UClass, which is its
    // own class — so a pointer that repeats is the fixed point and proves the chain was real.
    // Works for Blueprint-generated classes too: they simply take one hop longer.
    const void* cur = p;
    for (int i = 0; i < kUObjectChainDepth; ++i)
    {
        if (!Readable(cur, kUObjectClassOffset + sizeof(void*)))
            return false;
        const void* cls = ReadPtr(static_cast<const unsigned char*>(cur) + kUObjectClassOffset);
        if (cls == nullptr || (reinterpret_cast<std::uintptr_t>(cls) & 7u) != 0)
            return false;
        if (cls == cur)
            return i > 0;     // the fixed point, reached through at least one real hop
        cur = cls;
    }
    return false;
}

DiscoveryResult FindAudioDevice(const DiscoveryInput& in)
{
    DiscoveryResult r;
    if (in.imageBase == nullptr || in.imageSize == 0)
    {
        r.refusal = "the game executable's image range is unknown";
        return r;
    }
    if (in.worldObject == nullptr && in.engineObject == nullptr)
    {
        r.refusal = "neither a UWorld nor a UEngine object was found";
        return r;
    }

    // SELF-CHECK. The UObject rejection is only as good as the +0x10 ClassPrivate assumption,
    // and objects we already KNOW are UObjects are free evidence for it. If they do not pass,
    // the offset is wrong for this build and the rejection is disabled rather than silently
    // discarding every candidate — including the right one.
    const bool worldIsU  = in.worldObject  != nullptr && LooksLikeUObject(in.worldObject);
    const bool engineIsU = in.engineObject != nullptr && LooksLikeUObject(in.engineObject);
    r.uobjectTestUsable = worldIsU || engineIsU;
    if (in.logCandidates)
    {
        if (r.uobjectTestUsable)
            SDS_LOG_INFO("submix discovery: UObject self-check PASSED (world=%d engine=%d), so "
                         "'is a UObject' is a usable rejection - FAudioDevice is not one.",
                         worldIsU ? 1 : 0, engineIsU ? 1 : 0);
        else
            SDS_LOG_WARN("submix discovery: UObject self-check FAILED on objects that ARE "
                         "UObjects, so ClassPrivate is not at +0x%zX in this build. The "
                         "UObject rejection is DISABLED; expect noisy candidates.",
                         kUObjectClassOffset);
    }

    // The UWorld's own GUObjectArray index, for the strongest corroboration available.
    std::int32_t worldIndex = 0;
    if (in.worldObject != nullptr &&
        Readable(in.worldObject, kUObjectInternalIndexOffset + sizeof(std::int32_t)))
    {
        worldIndex = ReadI32(static_cast<const unsigned char*>(in.worldObject) +
                             kUObjectInternalIndexOffset);
        if (worldIndex < 0 || worldIndex > kMaxPlausibleObjectIndex)
            worldIndex = 0;
    }

    ScanContext ctx;
    ctx.imageBase     = in.imageBase;
    ctx.imageSize     = in.imageSize;
    ctx.out           = &r.candidates;
    ctx.maxCandidates = in.maxCandidates;

    if (in.useEngine)
        ScanObject(ctx, in.engineObject, in.scanBytes, false, 0, "UEngine", in.logCandidates,
                   in.secondHop);
    if (in.useWorld)
        ScanObject(ctx, in.worldObject, in.scanBytes, true, worldIndex, "UWorld",
                   in.logCandidates, in.secondHop);

    const Choice choice = ChooseDevice(r.candidates, r.uobjectTestUsable);

    if (in.logCandidates)
    {
        SDS_LOG_INFO("submix discovery: %zu candidate object(s); UWorld InternalIndex=%d",
                     r.candidates.size(), worldIndex);
        int shown = 0;
        for (const DeviceCandidate& c : r.candidates)
        {
            // A UObject is noise here and there are hundreds of them; report the survivors and
            // anything non-UObject, and count the rest.
            if (c.isUObject && r.uobjectTestUsable && c.reject != nullptr)
                continue;
            ++shown;
            char via[64] = "";
            if (c.viaContainer)
                std::snprintf(via, sizeof(via), " via container at owner+0x%04zX",
                              c.containerOffset);
            SDS_LOG_INFO("  %s +0x%04zX device=%p vt=%p id=%u uobj=%d rate=%d(x%d @+0x%zX) "
                         "distinct=%d head=%d worldIdx=%d mgr=%d vtShared=%d -> %s%s",
                         SourceName(c), c.offset, c.device, c.vtable, c.id, c.isUObject ? 1 : 0,
                         c.sampleRate, c.sampleRateHits, c.sampleRateAt, c.distinctRates,
                         c.rateAtHead ? 1 : 0,
                         c.worldIndexMatch ? 1 : 0, c.managerBefore ? 1 : 0,
                         c.vtableShared ? 1 : 0,
                         c.reject != nullptr ? c.reject : "SURVIVES", via);
        }
        SDS_LOG_INFO("submix discovery: %d non-UObject candidate(s) shown, %zu UObject(s) "
                     "suppressed", shown, r.candidates.size() - static_cast<std::size_t>(shown));
    }

    if (choice.index < 0)
    {
        r.refusal = choice.refusal;
        // NOTHING WAS ACCEPTED, so the sample-rate test is the prime suspect - it is the only
        // one that cannot self-check. Dump the non-UObject candidates so the rate can be FOUND
        // and its offset measured, rather than assumed from a stock header.
        if (in.logCandidates && in.dumpWords > 0)
        {
            SDS_LOG_WARN("submix discovery: nothing accepted. Dumping the non-UObject "
                         "candidates - if one of these IS the FAudioDevice, its sample rate is "
                         "in here (48000 = 0x0000BB80, 44100 = 0x0000AC44) and its offset is "
                         "then MEASURED rather than derived.");
            int dumped = 0;
            for (const DeviceCandidate& c : r.candidates)
            {
                if (c.isUObject && r.uobjectTestUsable)
                    continue;
                if (dumped++ >= in.maxDumps)
                    break;
                DumpCandidate(c, in.dumpWords);
            }
        }
        return r;
    }

    const DeviceCandidate* chosen = &r.candidates[static_cast<std::size_t>(choice.index)];
    r.ok         = true;
    r.device     = chosen->device;
    r.vtable     = chosen->vtable;
    r.offset     = chosen->offset;
    r.fromWorld  = chosen->fromWorld;
    r.deviceId   = chosen->id;
    r.sampleRate = chosen->sampleRate;
    r.why        = choice.why;
    return r;
}

void LogVtable(const void* device, const void* imageBase, std::size_t imageSize)
{
    if (device == nullptr || !Readable(device, sizeof(void*)))
        return;
    const void* vtable = ReadPtr(device);
    if (!Readable(vtable, sizeof(void*) * kVtableProbeSlots))
        return;
    const void* const* slots = static_cast<const void* const*>(vtable);
    const auto base = reinterpret_cast<std::uintptr_t>(imageBase);

    SDS_LOG_INFO("submix discovery: FAudioDevice vtable at %p (exe+0x%llX), first %d slots as "
                 "RVAs. Slot 16 should be RegisterSubmixBufferListener and slot 17 its "
                 "Unregister; repeats are MSVC /OPT:ICF folding the base class's empty stubs "
                 "together, so a slot that shares its address with several others is NOT an "
                 "override.", vtable,
                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(vtable) - base),
                 kVtableProbeSlots);
    for (int i = 0; i < kVtableProbeSlots; ++i)
    {
        int repeats = 0;
        for (int j = 0; j < kVtableProbeSlots; ++j)
            if (slots[j] == slots[i]) ++repeats;
        const bool inImage = InImage(slots[i], imageBase, imageSize);
        SDS_LOG_INFO("  vtable[%2d] = exe+0x%08llX%s%s", i,
                     inImage ? static_cast<unsigned long long>(
                                   reinterpret_cast<std::uintptr_t>(slots[i]) - base)
                             : 0ull,
                     inImage ? "" : "  (OUTSIDE THE IMAGE)",
                     repeats > 1 ? "  (shared - likely a folded empty stub)" : "");
    }
}

bool CallRegisterSubmixBufferListener(const void* device, int slot, void* listener,
                                      void* submix, const void* imageBase,
                                      std::size_t imageSize, const char** whyNot)
{
    const char* dummy = nullptr;
    if (whyNot == nullptr)
        whyNot = &dummy;
    *whyNot = nullptr;

    if (device == nullptr)                       { *whyNot = "no device";            return false; }
    if (listener == nullptr)                     { *whyNot = "no listener";          return false; }
    if (slot < 0 || slot >= kVtableProbeSlots)   { *whyNot = "slot outside the validated range"; return false; }
    if (!Readable(device, sizeof(void*)))        { *whyNot = "the device is not readable"; return false; }

    const void* vtable = ReadPtr(device);
    if (!LooksLikeVtable(vtable, imageBase, imageSize))
    {
        *whyNot = "the device's vtable no longer validates";
        return false;
    }
    const void* const* slots = static_cast<const void* const*>(vtable);
    const void* fn = slots[slot];
    if (!InImage(fn, imageBase, imageSize))
    {
        *whyNot = "the slot does not point into the game executable";
        return false;
    }

    // MS x64 has a single calling convention, so a plain function pointer matches the member
    // function's ABI with `this` passed first.
    using Fn = void (*)(const void*, void*, void*);
    Fn call = nullptr;
    std::memcpy(&call, &fn, sizeof(call));
    call(device, listener, submix);
    return true;
}

} // namespace submix
} // namespace sds
