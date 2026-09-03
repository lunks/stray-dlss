#include "SubmixDiscovery.hpp"

#include "Log.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <vector>

namespace sds {
namespace submix {
namespace {

// A DeviceId is an index into the audio device manager. A game has one, maybe a handful.
constexpr std::uint32_t kMaxPlausibleDeviceId = 64;
// GUObjectArray indices. Generous; the point is only to reject 0xDEADBEEF-shaped garbage.
constexpr std::int32_t kMaxPlausibleObjectIndex = 200 * 1000 * 1000;

struct Candidate
{
    const void*   device = nullptr;
    std::size_t   offset = 0;
    std::uint32_t id     = 0;
};

bool InImage(const void* p, const void* base, std::size_t size)
{
    const auto v = reinterpret_cast<std::uintptr_t>(p);
    const auto b = reinterpret_cast<std::uintptr_t>(base);
    return size != 0 && v >= b && v < b + size;
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

void ScanObject(const void* object, std::size_t scanBytes, const void* base, std::size_t size,
                std::vector<Candidate>& out, const char* what)
{
    if (object == nullptr || scanBytes < 24)
        return;
    if (!Readable(object, scanBytes))
    {
        // Objects near the end of a heap block can be shorter than the window; walk it down
        // rather than giving up, because refusing to scan is indistinguishable from finding
        // nothing and that is the ambiguity this whole file exists to remove.
        while (scanBytes >= 0x200 && !Readable(object, scanBytes))
            scanBytes /= 2;
        if (!Readable(object, scanBytes))
        {
            SDS_LOG_WARN("submix discovery: %s at %p is not readable at all; skipped.",
                         what, object);
            return;
        }
        SDS_LOG_INFO("submix discovery: %s readable window shrunk to 0x%zX bytes",
                     what, scanBytes);
    }

    const auto* bytes = static_cast<const unsigned char*>(object);
    for (std::size_t k = 0; k + 24 <= scanBytes; k += 8)
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
        // The device itself must NOT be inside the image: FAudioDevice is heap-allocated.
        if (InImage(device, base, size))
            continue;
        if (!Readable(device, sizeof(void*)))
            continue;

        const void* vtable = nullptr;
        std::memcpy(&vtable, device, sizeof(vtable));
        if (!LooksLikeVtable(vtable, base, size))
            continue;

        bool seen = false;
        for (const Candidate& c : out)
            if (c.device == device) { seen = true; break; }
        if (!seen)
            out.push_back(Candidate{ device, k, id });
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

    std::vector<Candidate> world, engine;
    ScanObject(in.worldObject,  in.scanBytes, in.imageBase, in.imageSize, world,  "UWorld");
    ScanObject(in.engineObject, in.scanBytes, in.imageBase, in.imageSize, engine, "UEngine");
    r.worldMatches  = static_cast<int>(world.size());
    r.engineMatches = static_cast<int>(engine.size());

    SDS_LOG_INFO("submix discovery: FAudioDeviceHandle-shaped candidates - UWorld %p -> %d, "
                 "UEngine %p -> %d", in.worldObject, r.worldMatches, in.engineObject,
                 r.engineMatches);
    for (const Candidate& c : world)
        SDS_LOG_INFO("  UWorld  +0x%04zX device=%p id=%u", c.offset, c.device, c.id);
    for (const Candidate& c : engine)
        SDS_LOG_INFO("  UEngine +0x%04zX device=%p id=%u", c.offset, c.device, c.id);

    // The decisive check: one device pointer, found independently in two different objects at
    // two different offsets.
    for (const Candidate& a : world)
    {
        for (const Candidate& b : engine)
        {
            if (a.device != b.device)
                continue;
            if (r.device != nullptr && r.device != a.device)
            {
                r.refusal = "two different devices matched in BOTH objects - ambiguous";
                r.device  = nullptr;
                return r;
            }
            r.device       = a.device;
            r.worldOffset  = a.offset;
            r.engineOffset = b.offset;
            r.deviceId     = a.id;
        }
    }

    if (r.device == nullptr && !in.requireBoth)
    {
        // Single-source fallback: only acceptable when exactly one candidate exists, and it is
        // said loudly, because the cross-check is the whole reason to trust this at all.
        if (world.size() == 1 && engine.empty())
        {
            r.device      = world[0].device;
            r.worldOffset = world[0].offset;
            r.deviceId    = world[0].id;
            SDS_LOG_WARN("submix discovery: accepting the UWorld candidate WITHOUT the UEngine "
                         "cross-check (SubmixDeviceSource is not 'both'). This is the weakest "
                         "configuration; suspect it first if anything misbehaves.");
        }
        else if (engine.size() == 1 && world.empty())
        {
            r.device       = engine[0].device;
            r.engineOffset = engine[0].offset;
            r.deviceId     = engine[0].id;
            SDS_LOG_WARN("submix discovery: accepting the UEngine candidate WITHOUT the UWorld "
                         "cross-check. This is the weakest configuration.");
        }
    }

    if (r.device == nullptr)
    {
        r.refusal = "no FAudioDevice pointer appeared in both the UWorld and the UEngine "
                    "object; refusing to guess";
        return r;
    }

    std::memcpy(&r.vtable, r.device, sizeof(r.vtable));
    r.ok = true;
    return r;
}

void LogVtable(const void* device, const void* imageBase, std::size_t imageSize)
{
    if (device == nullptr || !Readable(device, sizeof(void*)))
        return;
    const void* vtable = nullptr;
    std::memcpy(&vtable, device, sizeof(vtable));
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

    const void* vtable = nullptr;
    std::memcpy(&vtable, device, sizeof(vtable));
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
