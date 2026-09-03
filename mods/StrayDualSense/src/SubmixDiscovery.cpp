#include "SubmixDiscovery.hpp"

#include "Log.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

// FAudioDevice::SampleRate is an int32 (AudioDevice.h:1789) and FAudioPlatformSettings right
// after it carries another copy, so a live audio device holds at least one — usually two —
// int32s from this set. A random UObject holds none.
std::int32_t FindSampleRate(const void* obj, int& hits)
{
    hits = 0;
    const std::size_t window = ReadableWindow(obj, kFingerprintBytes);
    if (window < sizeof(std::int32_t))
        return 0;
    const auto* bytes = static_cast<const unsigned char*>(obj);
    std::int32_t found = 0;
    for (std::size_t k = 0; k + sizeof(std::int32_t) <= window; k += 4)
    {
        std::int32_t v = 0;
        std::memcpy(&v, bytes + k, sizeof(v));
        if (!IsStandardSampleRate(v))
            continue;
        ++hits;
        if (found == 0)
            found = v;
    }
    return found;
}

void ScanObject(const void* object, std::size_t scanBytes, const void* base, std::size_t size,
                bool fromWorld, std::int32_t ownerIndex,
                std::vector<DeviceCandidate>& out, const char* what, bool verbose)
{
    if (object == nullptr)
        return;
    const std::size_t window = ReadableWindow(object, scanBytes);
    if (window < 24)
    {
        if (verbose)
            SDS_LOG_WARN("submix discovery: %s at %p is not readable; skipped.", what, object);
        return;
    }
    if (window != scanBytes && verbose)
        SDS_LOG_INFO("submix discovery: %s readable window shrunk to 0x%zX bytes", what, window);

    const auto* bytes = static_cast<const unsigned char*>(object);
    for (std::size_t k = 0; k + 24 <= window; k += 8)
    {
        std::int32_t  objIndex = 0, serial = 0;
        const void*   device   = nullptr;
        std::uint32_t id       = 0;
        std::memcpy(&objIndex, bytes + k,      sizeof(objIndex));
        std::memcpy(&serial,   bytes + k + 4,  sizeof(serial));
        std::memcpy(&device,   bytes + k + 8,  sizeof(device));
        std::memcpy(&id,       bytes + k + 16, sizeof(id));

        if (device == nullptr || (reinterpret_cast<std::uintptr_t>(device) & 7u) != 0)
            continue;
        if (id >= kMaxPlausibleDeviceId)
            continue;
        if (objIndex < 0 || objIndex > kMaxPlausibleObjectIndex || serial < 0)
            continue;
        // FAudioDevice is heap-allocated; a pointer into the image is not one.
        if (InImage(device, base, size))
            continue;
        if (!Readable(device, sizeof(void*)))
            continue;

        const void* vtable = ReadPtr(device);
        if (!LooksLikeVtable(vtable, base, size))
            continue;

        bool seen = false;
        for (const DeviceCandidate& c : out)
            if (c.device == device) { seen = true; break; }
        if (seen)
            continue;

        DeviceCandidate cand;
        cand.device    = device;
        cand.vtable    = vtable;
        cand.offset    = k;
        cand.id        = id;
        cand.fromWorld = fromWorld;
        cand.isUObject = LooksLikeUObject(device);
        cand.sampleRate = FindSampleRate(device, cand.sampleRateHits);
        // The handle's weak pointer names the world that asked for the device, so for a
        // candidate found INSIDE that world the index must be the world's own.
        cand.worldIndexMatch = fromWorld && ownerIndex > 0 && objIndex == ownerIndex;
        // UEngine::MainAudioDeviceHandle is preceded by FAudioDeviceManager* (Engine.h:1732).
        if (!fromWorld && k >= 8)
        {
            const void* mgr = ReadPtr(bytes + k - 8);
            cand.managerBefore = mgr != nullptr &&
                                 (reinterpret_cast<std::uintptr_t>(mgr) & 7u) == 0 &&
                                 !InImage(mgr, base, size) && Readable(mgr, sizeof(void*));
        }
        out.push_back(cand);
    }
}

const char* SourceName(const DeviceCandidate& c) { return c.fromWorld ? "UWorld " : "UEngine"; }

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

    // The UWorld's own GUObjectArray index, for the strongest signal we have.
    std::int32_t worldIndex = 0;
    if (in.worldObject != nullptr &&
        Readable(in.worldObject, kUObjectInternalIndexOffset + sizeof(std::int32_t)))
    {
        std::memcpy(&worldIndex,
                    static_cast<const unsigned char*>(in.worldObject) + kUObjectInternalIndexOffset,
                    sizeof(worldIndex));
        if (worldIndex < 0 || worldIndex > kMaxPlausibleObjectIndex)
            worldIndex = 0;
    }

    if (in.useWorld)
        ScanObject(in.worldObject, in.scanBytes, in.imageBase, in.imageSize, true, worldIndex,
                   r.candidates, "UWorld", in.logCandidates);
    if (in.useEngine)
        ScanObject(in.engineObject, in.scanBytes, in.imageBase, in.imageSize, false, 0,
                   r.candidates, "UEngine", in.logCandidates);

    const Choice choice = ChooseDevice(r.candidates, r.uobjectTestUsable);

    if (in.logCandidates)
    {
        SDS_LOG_INFO("submix discovery: %zu handle-shaped candidate(s); UWorld InternalIndex=%d",
                     r.candidates.size(), worldIndex);
        for (const DeviceCandidate& c : r.candidates)
            SDS_LOG_INFO("  %s +0x%04zX device=%p vt=%p id=%u uobj=%d rate=%d(x%d) "
                         "worldIdx=%d mgr=%d vtShared=%d -> %s",
                         SourceName(c), c.offset, c.device, c.vtable, c.id, c.isUObject ? 1 : 0,
                         c.sampleRate, c.sampleRateHits, c.worldIndexMatch ? 1 : 0,
                         c.managerBefore ? 1 : 0, c.vtableShared ? 1 : 0,
                         c.reject != nullptr ? c.reject : "SURVIVES");
    }

    if (choice.index < 0)
    {
        r.refusal = choice.refusal;
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
