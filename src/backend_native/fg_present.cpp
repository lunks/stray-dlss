#include "backend_native/fg_present.hpp"

#include "backend_native/fg_reflex.hpp"
#include "backend_native/native_backend.hpp"
#include "backend_native/present_owner.hpp"
#include "backend_native/vtable_patch.hpp"
#include "backend_native/vtable_slots.hpp"
#include "log.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#define STRAY_RETURN_ADDRESS() _ReturnAddress()
#else
#define STRAY_RETURN_ADDRESS() __builtin_return_address(0)
#endif

using Microsoft::WRL::ComPtr;

namespace stray_dlss::native::fg {
namespace {

#define FG_HOOK_TRACE(name, self) \
	do { if (present::hook_trace()) \
		STRAY_LOG_INFO("[hook] %s self=%p thread=%lu own=%d", name, static_cast<void *>(self), static_cast<unsigned long>(::GetCurrentThreadId()), in_own_code() ? 1 : 0); } while (0)

struct ModuleRange
{
	std::uintptr_t begin = 0, end = 0;
};
// The image range of a module, from its own PE headers (no psapi dependency).
ModuleRange module_range(HMODULE m)
{
	ModuleRange r;
	const auto base = reinterpret_cast<const std::uint8_t *>(m);
	const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return r;
	const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return r;
	r.begin = reinterpret_cast<std::uintptr_t>(base);
	r.end = r.begin + nt->OptionalHeader.SizeOfImage;
	return r;
}

using PFN_GetBuffer = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, REFIID, void **);
using PFN_SetFullscreenState = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, BOOL, IDXGIOutput *);
using PFN_ResizeTarget = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, const DXGI_MODE_DESC *);
using PFN_SetColorSpace1 = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain3 *, DXGI_COLOR_SPACE_TYPE);
using PFN_Present = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);

constexpr unsigned kMaxReplacements = 8; // DXGI allows up to 16; UE4 asks for 3 (facts §32.4)
constexpr unsigned kListRing = 3;
constexpr std::uint32_t kBandRows = 120;   // the experiment's magenta band, top of the generated frame
constexpr std::uint32_t kCrop = 64;        // the validation crop, centred
constexpr unsigned kCropRing = 3;
constexpr std::uint32_t kValidateOkNeeded = 3; // consecutive ok verdicts before ngx output reaches the screen
constexpr std::uint64_t kLogEvery = 600;

std::uint64_t now_ns()
{
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct ListSlot
{
	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
	std::uint64_t pending = 0;
};

// One list ring + fence, used by one thread at a time (the worker's, or the game thread's in
// the inline pacing modes — never both: the inline modes run with no worker).
struct ListRing
{
	ListSlot slots[kListRing];
	ComPtr<ID3D12Fence> fence;
	std::uint64_t fence_value = 0;
	HANDLE event = nullptr;
	unsigned next = 0;

	bool create(ID3D12Device *device)
	{
		for (ListSlot &s : slots)
		{
			if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator))) ||
				FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator.Get(), nullptr, IID_PPV_ARGS(&s.list))))
				return false;
			s.list->Close();
		}
		if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
			return false;
		event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
		return event != nullptr;
	}
	void wait_all()
	{
		if (fence && fence_value != 0 && fence->GetCompletedValue() < fence_value && event != nullptr)
		{
			fence->SetEventOnCompletion(fence_value, event);
			::WaitForSingleObject(event, 2000);
		}
	}
	void destroy()
	{
		wait_all();
		for (ListSlot &s : slots)
		{
			s.list.Reset();
			s.allocator.Reset();
			s.pending = 0;
		}
		fence.Reset();
		fence_value = 0;
		if (event != nullptr)
		{
			::CloseHandle(event);
			event = nullptr;
		}
	}
	// An open list, its allocator retired by the fence.
	ID3D12GraphicsCommandList *begin()
	{
		ListSlot &s = slots[next];
		next = (next + 1) % kListRing;
		if (s.pending != 0 && fence->GetCompletedValue() < s.pending)
		{
			fence->SetEventOnCompletion(s.pending, event);
			::WaitForSingleObject(event, 2000);
		}
		if (FAILED(s.allocator->Reset()) || FAILED(s.list->Reset(s.allocator.Get(), nullptr)))
			return nullptr;
		return s.list.Get();
	}
	void submit(ID3D12GraphicsCommandList *list, ID3D12CommandQueue *queue)
	{
		list->Close();
		ID3D12CommandList *lists[] = { list };
		queue->ExecuteCommandLists(1, lists);
		queue->Signal(fence.Get(), ++fence_value);
		for (ListSlot &s : slots)
			if (s.list.Get() == list)
				s.pending = fence_value;
	}
};

struct CropSlot
{
	ComPtr<ID3D12Resource> generated_rb; // readback buffers
	ComPtr<ID3D12Resource> real_rb;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	std::uint64_t bytes = 0;
	std::uint64_t fence = 0; // the ring fence value that retires it; 0 = free
};

struct Chain
{
	IDXGISwapChain *sc = nullptr;  // non-owning
	IDXGISwapChain3 *sc3 = nullptr;
	ID3D12Device *device = nullptr;
	ID3D12CommandQueue *queue = nullptr;
	PFN_GetBuffer orig_get_buffer = nullptr;
	PFN_Present orig_present = nullptr;
	bool finalised = false;

	ComPtr<ID3D12Resource> replacement[kMaxReplacements];
	std::uint32_t replacement_count = 0;
	ComPtr<ID3D12Resource> generated[2];
	unsigned generated_next = 0;
	ComPtr<ID3D12DescriptorHeap> rtv_heap;
	D3D12_RESOURCE_DESC desc = {};
	bool armed = false;

	core::fg::GameIndexMirror mirror;
	core::fg::Epoch epoch;
	core::fg::Pacer pacer;
	core::fg::CropJudge judge;
	std::uint32_t ok_run = 0;
	bool validated = false;

	// This present's product (game thread only).
	ID3D12Resource *generated_this = nullptr; // non-owning into generated[]
	std::uint64_t generated_epoch = 0;
	bool generated_valid = false;

	ListRing ring;      // the present-side lists (worker or inline)
	// The crop copies ride the OWNER's present list (executed on `queue` before anything the
	// presenter submits); this fence, signalled from the game thread right after, retires them
	// without touching the ring the worker owns.
	ComPtr<ID3D12Fence> crop_fence;
	std::uint64_t crop_fence_value = 0;
	CropSlot crops[kCropRing];
	unsigned crop_next = 0;
	// The module that implements the swapchain (DXVK's dxgi.dll): a GetBuffer whose return
	// address lies inside it is DXGI's own internal call and must see the REAL buffer.
	std::uintptr_t impl_begin = 0, impl_end = 0;
};

// ---- the pacing trace ([STRAYDLSS] NgxFGTrace) ----
//
// One entry per game present: when the game called Present (the hook's entry), when the
// generated and the real present were issued and how long each orig Present call blocked, the
// delay the pacer asked for and whether the hold was cut short by the next game present. Dumped
// once, as the gap sequence the user feels, so a pacing fault names itself in one run.
struct TraceEntry
{
	std::uint64_t frame = 0;
	std::uint64_t t_hook = 0;      // present() entry on the game thread
	std::uint64_t t_gen0 = 0, t_gen1 = 0;   // around orig Present of the generated frame (0 = none)
	std::uint64_t t_real0 = 0, t_real1 = 0; // around orig Present of the real frame
	std::uint64_t delay_ns = 0;
	bool hurried = false;
	unsigned sync = 0, flags = 0;
};
constexpr unsigned kTraceMax = 512;
TraceEntry g_trace[kTraceMax];
std::atomic<unsigned> g_trace_count{ 0 };
std::atomic<bool> g_trace_armed{ false }, g_trace_dumped{ false };
std::uint64_t g_trace_t0 = 0;

void dump_trace()
{
	const unsigned n = g_trace_count.load();
	if (n == 0)
		return;
	STRAY_LOG_WARN("[fg-trace] %u presents. Columns: frame | hook +ms since previous hook | gen issued at +ms after hook [blocked ms] | real issued at +ms after hook [blocked ms] | pacer delay ms | hurried",
		n);
	std::uint64_t prev_hook = g_trace[0].t_hook;
	for (unsigned i = 0; i < n; ++i)
	{
		const TraceEntry &e = g_trace[i];
		STRAY_LOG_INFO("[fg-trace] %6llu | hook +%6.2f | gen %s+%6.2f [%5.2f] | real +%6.2f [%5.2f] | delay %5.2f | %s",
			static_cast<unsigned long long>(e.frame), (e.t_hook - prev_hook) / 1e6,
			e.t_gen0 != 0 ? "" : "(none) ", e.t_gen0 != 0 ? (e.t_gen0 - e.t_hook) / 1e6 : 0.0, e.t_gen0 != 0 ? (e.t_gen1 - e.t_gen0) / 1e6 : 0.0,
			e.t_real0 != 0 ? (e.t_real0 - e.t_hook) / 1e6 : 0.0, e.t_real0 != 0 ? (e.t_real1 - e.t_real0) / 1e6 : 0.0,
			e.delay_ns / 1e6, e.hurried ? "HURRIED" : "");
		prev_hook = e.t_hook;
	}
	// The gap sequence between consecutive ISSUED presents (what the display receives), in
	// order, 40 per line.
	char line[512];
	int pos = 0;
	std::uint64_t last = 0;
	unsigned k = 0;
	for (unsigned i = 0; i < n; ++i)
	{
		const std::uint64_t ts[2] = { g_trace[i].t_gen0, g_trace[i].t_real0 };
		for (std::uint64_t t : ts)
		{
			if (t == 0)
				continue;
			if (last != 0)
			{
				pos += std::snprintf(line + pos, sizeof(line) - static_cast<std::size_t>(pos), "%.1f ", (t - last) / 1e6);
				if (++k % 40 == 0 || pos > 440)
				{
					STRAY_LOG_INFO("[fg-trace] gaps: %s", line);
					pos = 0;
					line[0] = 0;
				}
			}
			last = t;
		}
	}
	if (pos > 0)
		STRAY_LOG_INFO("[fg-trace] gaps: %s", line);
}

// The work one game Present hands to the presenter.
struct Pair
{
	IDXGISwapChain *sc = nullptr;
	ID3D12Resource *generated = nullptr; // AddRef'd, may be null
	ID3D12Resource *real = nullptr;      // AddRef'd: the replacement the game rendered into
	std::uint64_t epoch = 0;
	std::uint64_t delay_ns = 0;
	unsigned sync = 0, flags = 0;
	PFN_Present orig_present = nullptr;
	std::uint64_t frame = 0;
	std::uint64_t t_hook = 0;
	int trace_slot = -1;
};

Config g_cfg;
Generator *g_generator = nullptr;
std::mutex g_mutex;               // chain state touched from the hooks (game thread) and the worker
Chain g_chain;                    // exactly one swapchain (Streamline supports exactly one too)
std::atomic<bool> g_have_chain{ false };

// worker
std::thread g_worker;
std::mutex g_wmutex;
std::condition_variable g_wcv;    // hook -> worker: a pair is pending / hurry / stop
std::condition_variable g_wdone;  // worker -> hook: the pair is done
bool g_pending = false, g_hurry = false, g_stop = false, g_busy = false;
Pair g_pair;
std::atomic<long> g_last_present_hr{ S_OK };

// stats
std::mutex g_smutex;
Stats g_stats;
core::fg::IntervalHistogram g_issued_hist;
std::uint64_t g_last_issued_ns = 0;
bool g_refusal_logged[static_cast<int>(core::fg::Refusal::count)] = {};
bool g_crop_verdict_logged[static_cast<int>(core::fg::CropVerdict::count)] = {};
char g_report[256] = "fg: off";

void count_refusal(core::fg::Refusal r, const char *detail)
{
	std::lock_guard<std::mutex> lock(g_smutex);
	++g_stats.refused[static_cast<int>(r)];
	if (!g_refusal_logged[static_cast<int>(r)])
	{
		g_refusal_logged[static_cast<int>(r)] = true;
		STRAY_LOG_WARN("fg: generated frame REFUSED (%s): %s. First occurrence only; the [fg] line carries the count.",
			core::fg::refusal_name(r), detail);
	}
}

void note_issued_present()
{
	const std::uint64_t t = now_ns();
	std::lock_guard<std::mutex> lock(g_smutex);
	++g_stats.presents_issued;
	if (g_last_issued_ns != 0 && t > g_last_issued_ns)
		g_issued_hist.add(t - g_last_issued_ns);
	g_last_issued_ns = t;
}

void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res;
	b.Transition.StateBefore = before;
	b.Transition.StateAfter = after;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	list->ResourceBarrier(1, &b);
}

// dst (in `dst_state`) <- src (in `src_state`), both restored. CopyResource: identical descs.
void record_copy(ID3D12GraphicsCommandList *list, ID3D12Resource *dst, D3D12_RESOURCE_STATES dst_state,
                 ID3D12Resource *src, D3D12_RESOURCE_STATES src_state)
{
	barrier(list, dst, dst_state, D3D12_RESOURCE_STATE_COPY_DEST);
	barrier(list, src, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	list->CopyResource(dst, src);
	barrier(list, src, D3D12_RESOURCE_STATE_COPY_SOURCE, src_state);
	barrier(list, dst, D3D12_RESOURCE_STATE_COPY_DEST, dst_state);
}

unsigned bytes_per_pixel(DXGI_FORMAT f)
{
	switch (f)
	{
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_UNORM: return 8;
	default: return 4; // R10G10B10A2, R8G8B8A8, B8G8R8A8 and their sRGB twins
	}
}

// Pixels whose colour channels are all zero, alpha ignored.
bool pixel_is_black(const std::uint8_t *p, DXGI_FORMAT f)
{
	if (bytes_per_pixel(f) == 8)
	{
		std::uint64_t v = 0;
		std::memcpy(&v, p, 8);
		return (v & 0x0000FFFFFFFFFFFFull) == 0;
	}
	std::uint32_t v = 0;
	std::memcpy(&v, p, 4);
	const std::uint32_t mask = f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R10G10B10A2_UINT ? 0x3FFFFFFFu : 0x00FFFFFFu;
	return (v & mask) == 0;
}

bool create_texture_like(ID3D12Device *device, const D3D12_RESOURCE_DESC &like, D3D12_RESOURCE_FLAGS extra_flags,
                         ComPtr<ID3D12Resource> &out, const wchar_t *name)
{
	D3D12_RESOURCE_DESC d = like;
	d.Flags = (like.Flags | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | extra_flags) & ~D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	d.MipLevels = 1;
	d.DepthOrArraySize = 1;
	d.SampleDesc.Count = 1;
	d.SampleDesc.Quality = 0;
	d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out));
	if (FAILED(hr))
	{
		STRAY_LOG_ERROR("fg: CreateCommittedResource(%ls %ux%u fmt %d flags 0x%x) failed 0x%08lx", name,
			static_cast<unsigned>(d.Width), d.Height, static_cast<int>(d.Format), static_cast<unsigned>(d.Flags), static_cast<unsigned long>(hr));
		return false;
	}
	out->SetName(name);
	return true;
}

// Caller holds g_mutex. Builds the replacements, the generated pair, the RTV heap and the crop
// readbacks from the real back buffer 0's desc. The game's GetBuffer(0) arrives right after
// creation / resize, when the object is fully built (facts §20/§32.4).
bool arm_locked(Chain &c)
{
	if (c.armed)
		return true;
	if (c.device == nullptr || c.orig_get_buffer == nullptr)
		return false;
	DXGI_SWAP_CHAIN_DESC sd = {};
	if (FAILED(c.sc->GetDesc(&sd)) || sd.BufferCount == 0 || sd.BufferCount > kMaxReplacements)
	{
		STRAY_LOG_ERROR("fg: GetDesc failed or BufferCount %u unsupported; FG stays unarmed", sd.BufferCount);
		return false;
	}
	ComPtr<ID3D12Resource> real0;
	if (FAILED(c.orig_get_buffer(c.sc, 0, IID_PPV_ARGS(&real0))) || !real0)
	{
		STRAY_LOG_ERROR("fg: the real GetBuffer(0) failed; FG stays unarmed");
		return false;
	}
	c.desc = real0->GetDesc();
	for (std::uint32_t i = 0; i < sd.BufferCount; ++i)
	{
		wchar_t name[48];
		std::swprintf(name, 48, L"stray-fg replacement %u", i);
		if (!create_texture_like(c.device, c.desc, D3D12_RESOURCE_FLAG_NONE, c.replacement[i], name))
			return false;
	}
	c.replacement_count = sd.BufferCount;
	for (unsigned i = 0; i < 2; ++i)
	{
		wchar_t name[48];
		std::swprintf(name, 48, L"stray-fg generated %u", i);
		if (!create_texture_like(c.device, c.desc, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, c.generated[i], name))
			return false;
	}
	if (!c.rtv_heap)
	{
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hd.NumDescriptors = 1;
		if (FAILED(c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&c.rtv_heap))))
			return false;
	}
	// The crop readbacks: one footprint (same for both textures), a readback buffer each.
	{
		D3D12_RESOURCE_DESC crop = c.desc;
		crop.Width = kCrop;
		crop.Height = kCrop;
		crop.MipLevels = 1;
		crop.DepthOrArraySize = 1;
		crop.Flags = D3D12_RESOURCE_FLAG_NONE;
		std::uint64_t bytes = 0;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
		c.device->GetCopyableFootprints(&crop, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
		for (CropSlot &s : c.crops)
		{
			s.footprint = fp;
			s.bytes = bytes;
			s.fence = 0;
			D3D12_HEAP_PROPERTIES heap = {};
			heap.Type = D3D12_HEAP_TYPE_READBACK;
			D3D12_RESOURCE_DESC bd = {};
			bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bd.Width = bytes;
			bd.Height = 1;
			bd.DepthOrArraySize = 1;
			bd.MipLevels = 1;
			bd.SampleDesc.Count = 1;
			bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			if (FAILED(c.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.generated_rb))) ||
				FAILED(c.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.real_rb))))
				return false;
		}
	}
	if (!c.crop_fence && FAILED(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.crop_fence))))
		return false;
	c.mirror.reset(sd.BufferCount);
	c.armed = true;
	{
		std::lock_guard<std::mutex> lock(g_smutex);
		g_stats.replacement_count = sd.BufferCount;
		g_stats.width = static_cast<std::uint32_t>(c.desc.Width);
		g_stats.height = c.desc.Height;
		g_stats.format = static_cast<unsigned>(c.desc.Format);
	}
	STRAY_LOG_WARN("fg: ARMED for swapchain %p: %u replacement back buffer(s) %ux%u fmt %d (real flags 0x%x), 2 generated, %u crop readbacks; "
		"the game renders into the replacements from now on and never touches the real ring",
		static_cast<void *>(c.sc), sd.BufferCount, static_cast<unsigned>(c.desc.Width), c.desc.Height, static_cast<int>(c.desc.Format),
		static_cast<unsigned>(c.desc.Flags), kCropRing);
	return true;
}

// Caller holds g_mutex. Drops the textures (the game released its references before the
// resize, facts §32.4; ours go here). The worker is already drained.
void disarm_locked(Chain &c)
{
	c.ring.wait_all();
	for (ComPtr<ID3D12Resource> &r : c.replacement)
		r.Reset();
	c.replacement_count = 0;
	for (ComPtr<ID3D12Resource> &g : c.generated)
		g.Reset();
	for (CropSlot &s : c.crops)
	{
		s.generated_rb.Reset();
		s.real_rb.Reset();
		s.fence = 0;
	}
	c.generated_this = nullptr;
	c.generated_valid = false;
	c.armed = false;
	c.validated = false;
	c.ok_run = 0;
	c.judge.reset();
	c.pacer.reset();
}

// ---- the experiment's generated frame: previous real frame + magenta band ----
bool record_experiment(Chain &c, ID3D12GraphicsCommandList *list, ID3D12Resource *out)
{
	const std::uint32_t n = c.mirror.count;
	ID3D12Resource *prev = c.replacement[(c.mirror.current() + n - 1) % n].Get();
	record_copy(list, out, D3D12_RESOURCE_STATE_COMMON, prev, D3D12_RESOURCE_STATE_COMMON);
	if (g_cfg.band)
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv_heap->GetCPUDescriptorHandleForHeapStart();
		c.device->CreateRenderTargetView(out, nullptr, rtv);
		barrier(list, out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
		const float magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
		D3D12_RECT band = { 0, 0, static_cast<LONG>(c.desc.Width), static_cast<LONG>(kBandRows < c.desc.Height ? kBandRows : c.desc.Height) };
		list->ClearRenderTargetView(rtv, magenta, 1, &band);
		barrier(list, out, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
	}
	return true;
}

// ---- the validation crop (ngx mode) ----
void record_crop(Chain &c, ID3D12GraphicsCommandList *list, ID3D12Resource *generated, ID3D12Resource *real)
{
	CropSlot &s = c.crops[c.crop_next];
	c.crop_next = (c.crop_next + 1) % kCropRing;
	if (s.fence != 0)
		return; // still in flight: skip this frame's crop rather than stall
	const LONG x0 = static_cast<LONG>(c.desc.Width / 2 - kCrop / 2), y0 = static_cast<LONG>(c.desc.Height / 2 - kCrop / 2);
	D3D12_BOX box = { static_cast<UINT>(x0), static_cast<UINT>(y0), 0, static_cast<UINT>(x0) + kCrop, static_cast<UINT>(y0) + kCrop, 1 };
	auto copy_one = [&](ID3D12Resource *src, ID3D12Resource *dst) {
		D3D12_TEXTURE_COPY_LOCATION d = {};
		d.pResource = dst;
		d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		d.PlacedFootprint = s.footprint;
		D3D12_TEXTURE_COPY_LOCATION sl = {};
		sl.pResource = src;
		sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		sl.SubresourceIndex = 0;
		barrier(list, src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
		list->CopyTextureRegion(&d, 0, 0, 0, &sl, &box);
		barrier(list, src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
	};
	copy_one(generated, s.generated_rb.Get());
	copy_one(real, s.real_rb.Get());
	s.fence = ~0ull; // marked in flight; the real fence value is stamped by the caller after submit
}

core::fg::CropStats reduce_crop(ID3D12Resource *rb, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &fp, std::uint64_t bytes, DXGI_FORMAT f)
{
	core::fg::CropStats st;
	void *p = nullptr;
	D3D12_RANGE range = { 0, static_cast<SIZE_T>(bytes) };
	if (FAILED(rb->Map(0, &range, &p)) || p == nullptr)
		return st;
	const unsigned bpp = bytes_per_pixel(f);
	std::uint64_t h = 1469598103934665603ull;
	for (std::uint32_t y = 0; y < kCrop; ++y)
	{
		const std::uint8_t *row = static_cast<const std::uint8_t *>(p) + static_cast<std::size_t>(y) * fp.Footprint.RowPitch;
		for (std::uint32_t x = 0; x < kCrop; ++x)
		{
			const std::uint8_t *px = row + static_cast<std::size_t>(x) * bpp;
			for (unsigned b = 0; b < bpp; ++b)
			{
				h ^= px[b];
				h *= 1099511628211ull;
			}
			++st.total;
			if (!pixel_is_black(px, f))
				++st.nonzero;
		}
	}
	D3D12_RANGE none = { 0, 0 };
	rb->Unmap(0, &none);
	st.hash = h;
	return st;
}

// Game thread, each present: judge the oldest retired crop. Caller holds g_mutex.
void drain_crops_locked(Chain &c)
{
	if (!c.crop_fence)
		return;
	const std::uint64_t done = c.crop_fence->GetCompletedValue();
	for (CropSlot &s : c.crops)
	{
		if (s.fence == 0 || s.fence == ~0ull || done < s.fence)
			continue;
		const core::fg::CropStats g = reduce_crop(s.generated_rb.Get(), s.footprint, s.bytes, c.desc.Format);
		const core::fg::CropStats r = reduce_crop(s.real_rb.Get(), s.footprint, s.bytes, c.desc.Format);
		s.fence = 0;
		const core::fg::CropVerdict v = c.judge.judge(g, r);
		{
			std::lock_guard<std::mutex> lock(g_smutex);
			switch (v)
			{
			case core::fg::CropVerdict::ok: ++g_stats.crop_ok; break;
			case core::fg::CropVerdict::black: ++g_stats.crop_black; break;
			case core::fg::CropVerdict::stale: ++g_stats.crop_stale; break;
			case core::fg::CropVerdict::identical: ++g_stats.crop_identical; break;
			case core::fg::CropVerdict::suspect: ++g_stats.crop_suspect; break;
			case core::fg::CropVerdict::dark: ++g_stats.crop_dark; break;
			default: break;
			}
		}
		const core::fg::CropWeight weight = core::fg::crop_weight(v);
		if (weight == core::fg::CropWeight::neutral)
		{
			c.ok_run = 0; // a first or suspect look neither validates nor revokes, but breaks a run
		}
		else if (weight == core::fg::CropWeight::good)
		{
			if (!c.validated && ++c.ok_run >= kValidateOkNeeded)
			{
				c.validated = true;
				std::lock_guard<std::mutex> lock(g_smutex);
				g_stats.validated = true;
				STRAY_LOG_WARN("fg: generated output VALIDATED (%u consecutive ok crops: generated nonzero %u/%u, real %u/%u) - generated frames reach the screen from now on",
					kValidateOkNeeded, g.nonzero, g.total, r.nonzero, r.total);
			}
		}
		else
		{
			c.ok_run = 0;
			if (c.validated)
			{
				c.validated = false;
				std::lock_guard<std::mutex> lock(g_smutex);
				g_stats.validated = false;
				STRAY_LOG_ERROR("fg: generated output REVOKED: crop verdict %s (generated nonzero %u/%u hash %016llx, real nonzero %u/%u hash %016llx); real frames only until it validates again",
					core::fg::crop_verdict_name(v), g.nonzero, g.total, static_cast<unsigned long long>(g.hash), r.nonzero, r.total, static_cast<unsigned long long>(r.hash));
			}
		}
		if (!g_crop_verdict_logged[static_cast<int>(v)])
		{
			g_crop_verdict_logged[static_cast<int>(v)] = true;
			STRAY_LOG_INFO("fg: first crop verdict %s: generated nonzero %u/%u hash %016llx | real nonzero %u/%u hash %016llx",
				core::fg::crop_verdict_name(v), g.nonzero, g.total, static_cast<unsigned long long>(g.hash), r.nonzero, r.total, static_cast<unsigned long long>(r.hash));
		}
	}
}

// ---- the present sequence, on the worker or inline ----

ID3D12Resource *current_real_buffer(Chain &c)
{
	if (c.sc3 == nullptr || c.orig_get_buffer == nullptr)
		return nullptr;
	const UINT idx = c.sc3->GetCurrentBackBufferIndex();
	ID3D12Resource *res = nullptr;
	if (FAILED(c.orig_get_buffer(c.sc, idx, IID_PPV_ARGS(&res))))
		return nullptr;
	return res; // one reference, released by the caller
}

// One present: copy `src` into the real current back buffer, execute, present. Returns the
// present's HRESULT, or E_FAIL when the copy could not be recorded.
HRESULT present_one(Chain &c, const Pair &p, ID3D12Resource *src)
{
	ID3D12Resource *real = current_real_buffer(c);
	if (real == nullptr)
		return E_FAIL;
	ID3D12GraphicsCommandList *list = c.ring.begin();
	if (list == nullptr)
	{
		real->Release();
		return E_FAIL;
	}
	record_copy(list, real, D3D12_RESOURCE_STATE_PRESENT, src, D3D12_RESOURCE_STATE_COMMON);
	c.ring.submit(list, c.queue);
	real->Release();
	const bool generated = src == p.generated;
	if (g_cfg.reflex != 0)
		reflex::marker(generated ? reflex::Marker::out_of_band_present_start : reflex::Marker::present_start, p.frame);
	const std::uint64_t t0 = now_ns();
	const HRESULT hr = p.orig_present(p.sc, p.sync, p.flags);
	const std::uint64_t t1 = now_ns();
	if (p.trace_slot >= 0)
	{
		TraceEntry &e = g_trace[static_cast<unsigned>(p.trace_slot)];
		if (generated) { e.t_gen0 = t0; e.t_gen1 = t1; }
		else { e.t_real0 = t0; e.t_real1 = t1; }
	}
	if (g_cfg.reflex != 0)
		reflex::marker(generated ? reflex::Marker::out_of_band_present_end : reflex::Marker::present_end, p.frame);
	note_issued_present();
	return hr;
}

// Waits `delay_ns` unless hurried (a new pair is waiting) or stopped. Worker-only.
bool paced_wait(std::uint64_t delay_ns)
{
	if (delay_ns == 0)
		return false;
	std::unique_lock<std::mutex> lock(g_wmutex);
	return g_wcv.wait_for(lock, std::chrono::nanoseconds(delay_ns), [] { return g_hurry || g_stop; });
}

void run_pair(Chain &c, Pair &p, bool on_worker)
{
	// Nothing armed under this epoch may present: a reconfiguration happened in between.
	bool live;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		live = c.armed && c.epoch.live(p.epoch) && c.queue != nullptr && c.orig_present != nullptr;
	}
	if (!live)
	{
		// The game still expects its Present to have happened; present the real frame if we
		// can, else pass the call through untouched by calling the original directly.
		if (p.orig_present != nullptr)
			g_last_present_hr.store(p.orig_present(p.sc, p.sync, p.flags));
	}
	else
	{
		if (p.generated != nullptr)
		{
			const HRESULT hr = present_one(c, p, p.generated);
			if (SUCCEEDED(hr))
			{
				std::lock_guard<std::mutex> lock(g_smutex);
				++g_stats.generated_presented;
			}
			bool hurried = false;
			if (on_worker)
				hurried = paced_wait(p.delay_ns);
			else if (p.delay_ns != 0)
				std::this_thread::sleep_for(std::chrono::nanoseconds(p.delay_ns));
			if (p.trace_slot >= 0)
				g_trace[static_cast<unsigned>(p.trace_slot)].hurried = hurried;
		}
		g_last_present_hr.store(present_one(c, p, p.real));
		if (p.trace_slot >= 0 && static_cast<unsigned>(p.trace_slot) + 1 == g_trace_count.load() &&
			g_trace_count.load() >= static_cast<unsigned>(g_cfg.trace) && !g_trace_dumped.exchange(true))
			dump_trace();
	}
	if (p.generated != nullptr)
		p.generated->Release();
	if (p.real != nullptr)
		p.real->Release();
	p.generated = p.real = nullptr;
}

void worker_main()
{
	for (;;)
	{
		Pair p;
		{
			std::unique_lock<std::mutex> lock(g_wmutex);
			g_wcv.wait(lock, [] { return g_pending || g_stop; });
			if (g_stop && !g_pending)
				return;
			p = g_pair;
			g_pending = false;
			g_hurry = false;
			g_busy = true;
		}
		run_pair(g_chain, p, /*on_worker=*/true);
		{
			std::lock_guard<std::mutex> lock(g_wmutex);
			g_busy = false;
		}
		g_wdone.notify_all();
	}
}

void start_worker()
{
	if (g_worker.joinable())
		return;
	g_stop = false;
	g_worker = std::thread(worker_main);
}

// Waits until the worker has nothing pending and nothing in flight.
void drain_worker()
{
	if (!g_worker.joinable())
		return;
	std::unique_lock<std::mutex> lock(g_wmutex);
	g_hurry = true;
	g_wcv.notify_all();
	g_wdone.wait(lock, [] { return !g_pending && !g_busy; });
	g_hurry = false;
}

void stop_worker()
{
	if (!g_worker.joinable())
		return;
	{
		std::lock_guard<std::mutex> lock(g_wmutex);
		g_stop = true;
		g_hurry = true;
	}
	g_wcv.notify_all();
	g_worker.join();
}

// ---- hooks on the swapchain vtable ----

HRESULT STDMETHODCALLTYPE hk_GetBuffer(IDXGISwapChain *self, UINT index, REFIID riid, void **out)
{
	FG_HOOK_TRACE("GetBuffer", self);
	auto orig = reinterpret_cast<PFN_GetBuffer>(original_for(self, slot::kSwapChain_GetBuffer));
	if (orig == nullptr)
		return E_FAIL;
	if (!g_cfg.enabled || in_own_code() || !g_have_chain.load() || self != g_chain.sc)
		return orig(self, index, riid, out);
	// DXGI calling its own GetBuffer (a re-entrant internal use) must get the real buffer.
	{
		const std::uintptr_t ret = reinterpret_cast<std::uintptr_t>(STRAY_RETURN_ADDRESS());
		if (g_chain.impl_begin != 0 && ret >= g_chain.impl_begin && ret < g_chain.impl_end)
			return orig(self, index, riid, out);
	}
	OwnCodeScope own;
	std::lock_guard<std::mutex> lock(g_mutex);
	Chain &c = g_chain;
	c.orig_get_buffer = orig;
	if (!arm_locked(c))
	{
		STRAY_LOG_ERROR("fg: could not arm at GetBuffer(%u); FG is OFF for this swapchain (the game gets the real buffer)", index);
		g_cfg.enabled = false;
		return orig(self, index, riid, out);
	}
	if (index >= c.replacement_count || out == nullptr)
		return orig(self, index, riid, out);
	const HRESULT hr = c.replacement[index]->QueryInterface(riid, out);
	static std::atomic<int> s_logged{ 0 };
	if (s_logged.fetch_add(1) < static_cast<int>(kMaxReplacements))
		STRAY_LOG_INFO("fg: GetBuffer(%u) -> replacement %p (hr 0x%08lx)", index, static_cast<void *>(c.replacement[index].Get()), static_cast<unsigned long>(hr));
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_SetFullscreenState(IDXGISwapChain *self, BOOL fullscreen, IDXGIOutput *target)
{
	FG_HOOK_TRACE("SetFullscreenState", self);
	auto orig = reinterpret_cast<PFN_SetFullscreenState>(original_for(self, slot::kSwapChain_SetFullscreenState));
	if (orig == nullptr)
		return E_FAIL;
	const bool ours = g_cfg.enabled && !in_own_code() && g_have_chain.load() && self == g_chain.sc;
	if (ours)
		before_reconfigure(self, fullscreen ? "SetFullscreenState(TRUE)" : "SetFullscreenState(FALSE)");
	const HRESULT hr = orig(self, fullscreen, target);
	if (ours)
		after_reconfigure(self, fullscreen ? "SetFullscreenState(TRUE)" : "SetFullscreenState(FALSE)", false, 0, hr);
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_ResizeTarget(IDXGISwapChain *self, const DXGI_MODE_DESC *mode)
{
	FG_HOOK_TRACE("ResizeTarget", self);
	auto orig = reinterpret_cast<PFN_ResizeTarget>(original_for(self, slot::kSwapChain_ResizeTarget));
	if (orig == nullptr)
		return E_FAIL;
	const bool ours = g_cfg.enabled && !in_own_code() && g_have_chain.load() && self == g_chain.sc;
	if (ours)
		before_reconfigure(self, "ResizeTarget");
	const HRESULT hr = orig(self, mode);
	if (ours)
		after_reconfigure(self, "ResizeTarget", false, 0, hr);
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_SetColorSpace1(IDXGISwapChain3 *self, DXGI_COLOR_SPACE_TYPE space)
{
	FG_HOOK_TRACE("SetColorSpace1", self);
	auto orig = reinterpret_cast<PFN_SetColorSpace1>(original_for(self, slot::kSwapChain3_SetColorSpace1));
	if (orig == nullptr)
		return E_FAIL;
	const HRESULT hr = orig(self, space);
	if (!in_own_code())
	{
		std::lock_guard<std::mutex> lock(g_smutex);
		g_stats.color_space = static_cast<unsigned>(space);
		STRAY_LOG_INFO("fg: the game set the swapchain colour space to %d (%s) hr 0x%08lx - decides DLSSG.ColorBuffersHDR",
			static_cast<int>(space),
			space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? "HDR10 / PQ / BT.2020" :
			space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ? "scRGB linear" :
			space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ? "sRGB" : "other",
			static_cast<unsigned long>(hr));
	}
	return hr;
}

} // namespace

// ---- public ----

void configure(const Config &cfg)
{
	// A pacing change on a live chain moves the presenter between the worker and the game
	// thread; drain first so no pair is in flight while the mode flips.
	if (g_have_chain.load() && g_cfg.pacing == Pacing::thread && cfg.pacing != Pacing::thread)
	{
		drain_worker();
		stop_worker();
	}
	g_cfg = cfg;
	if (g_have_chain.load() && cfg.pacing == Pacing::thread)
	{
		bool finalised;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			finalised = g_chain.finalised;
		}
		if (finalised)
			start_worker();
	}
	std::snprintf(g_report, sizeof(g_report), "fg: %s mode=%s pacing=%s wait=%d band=%d validate=%d",
		cfg.enabled ? "ENABLED" : "off", cfg.mode == Mode::experiment ? "experiment(present-twice, no NGX)" : "ngx",
		cfg.pacing == Pacing::thread ? "thread" : cfg.pacing == Pacing::sync ? "sync" : "none", cfg.wait_ms, cfg.band ? 1 : 0, cfg.validate);
	if (cfg.enabled)
		STRAY_LOG_WARN("%s", g_report);
}

const Config &config() { return g_cfg; }
bool enabled() { return g_cfg.enabled; }
void set_generator(Generator *g) { g_generator = g; }

void on_swapchain_recorded(IDXGISwapChain *sc, ID3D12Device *device)
{
	if (!g_cfg.enabled || sc == nullptr)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_have_chain.load())
	{
		if (g_chain.sc != sc)
			STRAY_LOG_ERROR("fg: a SECOND swapchain %p (first %p) - FG follows the first only; the second is presented untouched", static_cast<void *>(sc), static_cast<void *>(g_chain.sc));
		return;
	}
	g_chain = Chain{};
	g_chain.sc = sc;
	g_chain.device = device;
	{
		// Only the vtable pointer is read here (set at construction, safe mid-creation).
		void **vt = *reinterpret_cast<void ***>(sc);
		HMODULE m = nullptr;
		if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(vt[0]), &m) && m != nullptr)
		{
			const ModuleRange info = module_range(m);
			g_chain.impl_begin = info.begin;
			g_chain.impl_end = info.end;
		}
	}
	g_have_chain.store(true);
	unsigned n = 0;
	if (patch_slot(sc, slot::kSwapChain_GetBuffer, reinterpret_cast<void *>(&hk_GetBuffer), "IDXGISwapChain::GetBuffer") != nullptr) ++n;
	if (patch_slot(sc, slot::kSwapChain_SetFullscreenState, reinterpret_cast<void *>(&hk_SetFullscreenState), "IDXGISwapChain::SetFullscreenState") != nullptr) ++n;
	if (patch_slot(sc, slot::kSwapChain_ResizeTarget, reinterpret_cast<void *>(&hk_ResizeTarget), "IDXGISwapChain::ResizeTarget") != nullptr) ++n;
	STRAY_LOG_WARN("fg: swapchain %p recorded; %u slot(s) patched (GetBuffer/SetFullscreenState/ResizeTarget). The game's next GetBuffer arms the replacements.",
		static_cast<void *>(sc), n);
}

void on_swapchain_finalised(IDXGISwapChain *sc, IDXGISwapChain3 *sc3, ID3D12Device *device, ID3D12CommandQueue *queue)
{
	if (!g_cfg.enabled || !g_have_chain.load() || sc != g_chain.sc)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	Chain &c = g_chain;
	c.sc3 = sc3;
	c.device = device;
	c.queue = queue;
	c.orig_present = reinterpret_cast<PFN_Present>(original_for(sc, slot::kSwapChain_Present));
	if (sc3 != nullptr && patch_slot(sc, slot::kSwapChain3_SetColorSpace1, reinterpret_cast<void *>(&hk_SetColorSpace1), "IDXGISwapChain3::SetColorSpace1") != nullptr)
		STRAY_LOG_INFO("fg: SetColorSpace1 patched (observation only)");
	if (sc3 == nullptr || queue == nullptr || c.orig_present == nullptr)
	{
		STRAY_LOG_ERROR("fg: cannot finalise (SwapChain3=%p queue=%p origPresent=%p); FG is OFF", static_cast<void *>(sc3), static_cast<void *>(queue), reinterpret_cast<void *>(c.orig_present));
		g_cfg.enabled = false;
		return;
	}
	if (!c.ring.create(device))
	{
		STRAY_LOG_ERROR("fg: could not create the present list ring; FG is OFF");
		g_cfg.enabled = false;
		return;
	}
	c.finalised = true;
	if (g_cfg.reflex != 0)
		reflex::initialise(device, /*low_latency=*/true, /*boost=*/g_cfg.reflex >= 2);
	if (g_cfg.pacing == Pacing::thread)
		start_worker();
	STRAY_LOG_WARN("fg: finalised on queue %p; presenter=%s; armed=%d (GetBuffer %s)", static_cast<void *>(queue),
		g_cfg.pacing == Pacing::thread ? "worker thread" : "game thread", c.armed ? 1 : 0, c.armed ? "already seen" : "not yet seen - the game will be handed the REAL buffers until it asks");
}

void before_reconfigure(IDXGISwapChain *sc, const char *what)
{
	if (!g_have_chain.load() || sc != g_chain.sc)
		return;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_chain.epoch.begin_reconfigure();
	}
	drain_worker();
	std::lock_guard<std::mutex> lock(g_mutex);
	g_chain.ring.wait_all();
	STRAY_LOG_INFO("fg: %s begins: worker drained, GPU idle on our lists, epoch %llu suspended", what, static_cast<unsigned long long>(g_chain.epoch.value));
}

void after_reconfigure(IDXGISwapChain *sc, const char *what, bool drop_replacements, std::uint32_t new_count, long hr)
{
	if (!g_have_chain.load() || sc != g_chain.sc)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	Chain &c = g_chain;
	if (drop_replacements)
	{
		disarm_locked(c);
		if (new_count != 0)
			c.mirror.reset(new_count);
		else
			c.mirror.reset(c.mirror.count);
	}
	else
	{
		c.pacer.reset();
	}
	c.generated_valid = false;
	c.epoch.end_reconfigure();
	if (g_generator != nullptr)
		g_generator->on_reconfigure();
	{
		std::lock_guard<std::mutex> slock(g_smutex);
		++g_stats.reconfigures;
		g_stats.epoch = c.epoch.value;
		g_last_issued_ns = 0;
	}
	STRAY_LOG_WARN("fg: %s done (hr 0x%08lx): epoch %llu, replacements %s, mirror count %u", what, static_cast<unsigned long>(hr),
		static_cast<unsigned long long>(c.epoch.value), drop_replacements ? "DROPPED (the next GetBuffer re-arms)" : "kept", c.mirror.count);
}

ID3D12Resource *game_frame(IDXGISwapChain *sc)
{
	if (!g_cfg.enabled || !g_have_chain.load() || sc != g_chain.sc)
		return nullptr;
	std::lock_guard<std::mutex> lock(g_mutex);
	Chain &c = g_chain;
	if (!c.armed)
		return nullptr;
	return c.replacement[c.mirror.current()].Get();
}

bool record(IDXGISwapChain *sc, ID3D12GraphicsCommandList *list, std::uint64_t frame)
{
	if (!g_cfg.enabled || !g_have_chain.load() || sc != g_chain.sc || list == nullptr)
		return false;
	std::lock_guard<std::mutex> lock(g_mutex);
	Chain &c = g_chain;
	c.generated_valid = false;
	c.generated_this = nullptr;
	if (!c.finalised || !c.armed)
	{
		count_refusal(core::fg::Refusal::not_armed, "no replacement buffers yet (the game has not called GetBuffer since creation/resize)");
		return false;
	}
	if (c.epoch.suspended)
	{
		count_refusal(core::fg::Refusal::suspended, "a resize or fullscreen change is in progress");
		return false;
	}
	if (c.mirror.presents == 0)
	{
		count_refusal(core::fg::Refusal::no_previous_frame, "first present after creation/resize: nothing to interpolate from");
		return false;
	}
	ID3D12Resource *out = c.generated[c.generated_next].Get();
	c.generated_next ^= 1;
	ID3D12Resource *real_current = c.replacement[c.mirror.current()].Get();
	bool produced = false;
	const char *why = "generator declined";
	if (g_cfg.mode == Mode::experiment || g_generator == nullptr)
	{
		if (g_cfg.mode == Mode::ngx)
			why = "NgxFGMode=2 but no generator is installed (NGX unavailable)";
		else
			produced = record_experiment(c, list, out);
	}
	else
	{
		produced = g_generator->generate(list, real_current, out, static_cast<std::uint32_t>(c.desc.Width), c.desc.Height,
			static_cast<unsigned>(c.desc.Format), frame, &why);
	}
	if (!produced)
	{
		count_refusal(core::fg::Refusal::source_missing, why);
		return false;
	}
	if (g_cfg.mode == Mode::ngx && g_cfg.validate != 0)
		record_crop(c, list, out, real_current);
	c.generated_this = out;
	c.generated_epoch = c.epoch.value;
	c.generated_valid = true;
	return true;
}

bool present(const PresentArgs &args, long *hr_out)
{
	if (!g_cfg.enabled || !g_have_chain.load() || args.sc != g_chain.sc)
		return false;
	if (g_cfg.reflex != 0)
	{
		// Reflex's Sleep belongs at the start of the game's frame; inside its Present is the
		// closest point an injector reaches, and it is where Streamline's own DLSS-G puts it
		// when the host does not call it (the SL pacer). Measured, not assumed: the status of
		// every call is in the log and the [fg] line.
		reflex::marker(reflex::Marker::simulation_end, args.frame);
		reflex::sleep();
		reflex::marker(reflex::Marker::simulation_start, args.frame + 1);
	}
	Pair p;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		Chain &c = g_chain;
		if (!c.finalised || !c.armed)
			return false; // the game still holds the real buffers: present normally
		// Stamp the crop recorded this frame: the owner has just executed the list that carried
		// the copies on `queue`, so a signal from here retires them in order.
		for (CropSlot &s : c.crops)
			if (s.fence == ~0ull && c.crop_fence)
			{
				c.queue->Signal(c.crop_fence.Get(), ++c.crop_fence_value);
				s.fence = c.crop_fence_value;
			}
		drain_crops_locked(c);

		{
			std::lock_guard<std::mutex> slock(g_smutex);
			++g_stats.game_presents;
		}
		const std::uint64_t auto_delay = c.pacer.on_game_present(now_ns());
		std::uint64_t delay = auto_delay;
		if (g_cfg.wait_ms >= 0)
			delay = static_cast<std::uint64_t>(g_cfg.wait_ms) * 1'000'000ull;
		if (g_cfg.pacing == Pacing::none)
			delay = 0;

		core::fg::PresentState st;
		st.enabled = true;
		st.armed = c.armed;
		st.suspended = c.epoch.suspended;
		st.epoch = c.epoch.value;
		st.generated_epoch = c.generated_epoch;
		st.have_previous_frame = c.mirror.presents != 0;
		st.generated_valid = c.generated_valid;
		st.validated = g_cfg.mode == Mode::experiment || g_cfg.validate == 0 || c.validated;
		const core::fg::Refusal r = core::fg::decide(st);
		// record() already counted its own refusals; only the ones decided here are new.
		if (r == core::fg::Refusal::not_validated)
			count_refusal(r, "the generated output has not passed the crop gate yet (NgxFGValidate=0 skips it)");
		else if (r == core::fg::Refusal::stale_epoch)
			count_refusal(r, "the generated frame predates a reconfiguration");

		p.sc = args.sc;
		p.generated = r == core::fg::Refusal::none ? c.generated_this : nullptr;
		p.real = c.replacement[c.mirror.current()].Get();
		p.epoch = c.epoch.value;
		p.delay_ns = delay;
		p.sync = args.sync;
		p.flags = args.flags;
		p.orig_present = c.orig_present;
		p.frame = args.frame;
		p.t_hook = now_ns();
		if (g_cfg.trace > 0 && !g_trace_dumped.load())
		{
			if (p.generated != nullptr)
				g_trace_armed.store(true);
			if (g_trace_armed.load())
			{
				const unsigned slot = g_trace_count.load();
				const unsigned want = static_cast<unsigned>(g_cfg.trace) < kTraceMax ? static_cast<unsigned>(g_cfg.trace) : kTraceMax;
				if (slot < want)
				{
					g_trace[slot] = TraceEntry{};
					g_trace[slot].frame = args.frame;
					g_trace[slot].t_hook = p.t_hook;
					g_trace[slot].delay_ns = delay;
					g_trace[slot].sync = args.sync;
					g_trace[slot].flags = args.flags;
					p.trace_slot = static_cast<int>(slot);
					g_trace_count.store(slot + 1);
				}
			}
		}
		if (p.generated != nullptr)
			p.generated->AddRef();
		p.real->AddRef();
		c.mirror.on_present();
		c.generated_valid = false;
		c.generated_this = nullptr;
		{
			std::lock_guard<std::mutex> slock(g_smutex);
			g_stats.pacer_interval_ms = c.pacer.interval_ns / 1e6;
			g_stats.pacer_hitches = c.pacer.hitches;
			g_stats.epoch = c.epoch.value;
		}
	}
	if (g_cfg.pacing == Pacing::thread && g_worker.joinable())
	{
		std::unique_lock<std::mutex> lock(g_wmutex);
		if (g_pending || g_busy)
		{
			g_hurry = true;
			g_wcv.notify_all();
			{
				std::lock_guard<std::mutex> slock(g_smutex);
				++g_stats.worker_waits;
			}
			g_wdone.wait(lock, [] { return !g_pending && !g_busy; });
		}
		g_pair = p;
		g_pending = true;
		g_hurry = false;
		lock.unlock();
		g_wcv.notify_all();
		*hr_out = g_last_present_hr.load();
	}
	else
	{
		run_pair(g_chain, p, /*on_worker=*/false);
		*hr_out = g_last_present_hr.load();
	}
	if ((args.frame % kLogEvery) == 0 && args.frame != 0)
	{
		char when[32];
		std::snprintf(when, sizeof(when), "frame %llu", static_cast<unsigned long long>(args.frame));
		log_stats(when);
	}
	return true;
}

void uninstall()
{
	stop_worker();
	reflex::shutdown();
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_have_chain.load())
	{
		disarm_locked(g_chain);
		g_chain.ring.destroy();
		g_chain.rtv_heap.Reset();
		g_chain = Chain{};
		g_have_chain.store(false);
	}
}

Stats stats()
{
	std::lock_guard<std::mutex> lock(g_smutex);
	Stats s = g_stats;
	s.issued_p50_ms = g_issued_hist.percentile_ms(0.5);
	s.issued_p99_ms = g_issued_hist.percentile_ms(0.99);
	s.issued_second_peak_ms = g_issued_hist.second_peak_ms();
	return s;
}

const char *report() { return g_report; }

void log_stats(const char *when)
{
	const Stats s = stats();
	char refusals[256];
	int n = 0;
	for (int i = 1; i < static_cast<int>(core::fg::Refusal::count); ++i)
		if (s.refused[i] != 0)
			n += std::snprintf(refusals + n, sizeof(refusals) - static_cast<std::size_t>(n), " %s=%llu",
				core::fg::refusal_name(static_cast<core::fg::Refusal>(i)), static_cast<unsigned long long>(s.refused[i]));
	if (n == 0)
		std::snprintf(refusals, sizeof(refusals), " none");
	const reflex::Status rs = reflex::status();
	STRAY_LOG_INFO("[fg] %s: game presents=%llu issued=%llu generated=%llu (%.2fx) | refused:%s | pacer %.2f ms hitches=%llu | issued-interval p50=%u ms p99=%u ms %s | worker waits=%llu | epoch=%llu reconfigures=%llu | %ux%u fmt %u colourspace %d | crops ok=%llu identical=%llu black=%llu stale=%llu suspect=%llu dark=%llu validated=%d | reflex dll=%d init=%d sleepMode=%d(%d) sleeps=%llu(%d) markers=%llu(%d)",
		when, static_cast<unsigned long long>(s.game_presents), static_cast<unsigned long long>(s.presents_issued),
		static_cast<unsigned long long>(s.generated_presented),
		s.game_presents != 0 ? static_cast<double>(s.presents_issued) / static_cast<double>(s.game_presents) : 0.0,
		refusals, s.pacer_interval_ms, static_cast<unsigned long long>(s.pacer_hitches), s.issued_p50_ms, s.issued_p99_ms,
		s.issued_second_peak_ms >= 0 ? "BIMODAL (back-to-back presents)" : "unimodal",
		static_cast<unsigned long long>(s.worker_waits), static_cast<unsigned long long>(s.epoch), static_cast<unsigned long long>(s.reconfigures),
		s.width, s.height, s.format, static_cast<int>(s.color_space),
		static_cast<unsigned long long>(s.crop_ok), static_cast<unsigned long long>(s.crop_identical),
		static_cast<unsigned long long>(s.crop_black), static_cast<unsigned long long>(s.crop_stale), static_cast<unsigned long long>(s.crop_suspect), static_cast<unsigned long long>(s.crop_dark), s.validated ? 1 : 0,
		rs.dll_found ? 1 : 0, rs.initialised ? 1 : 0, rs.sleep_mode_set ? 1 : 0, rs.set_sleep_mode_status,
		static_cast<unsigned long long>(rs.sleeps), rs.last_sleep_status, static_cast<unsigned long long>(rs.markers), rs.last_marker_status);
}

} // namespace stray_dlss::native::fg
