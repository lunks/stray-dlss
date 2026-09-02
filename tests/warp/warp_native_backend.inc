// ---- the native backend on WARP (plan Tasks 9-12, 15) ----
//
// Included by warp_mv_resolve.cpp after its helpers (Gpu, create_buffer, build_probe, unwrap,
// running_under_reshade, check, HR). Installs the native hooks on the harness's REAL device —
// under the real-ReShade lane that is the unwrapped original, so the hooks sit beneath
// ReShade's proxies exactly as they do in the game — and then exercises them through the
// PUBLIC D3D12 API only. Runs last: the hooks are never restored, by design.

#include "app/diff_observer.hpp"
#include "backend_native/d3d12_hooks.hpp"
#include "backend_native/descriptor_shadow.hpp"
#include "backend_native/native_backend.hpp"
#include "backend_native/resource_registry.hpp"
#include "backend_native/root_shadow.hpp"
#include "backend_native/vtable_patch.hpp"
#include "backend_native/vtable_slots.hpp"
#include "core/ue4_view.hpp"
#include "core/view_params.hpp"

namespace {

using PFN_GetDesc = D3D12_RESOURCE_DESC *(STDMETHODCALLTYPE *)(ID3D12Resource *, D3D12_RESOURCE_DESC *);
PFN_GetDesc g_orig_getdesc = nullptr;
std::atomic<int> g_getdesc_calls{ 0 };

// ID3D12Resource::GetDesc returns a struct by value; on the COM ABI that is a hidden pointer
// argument, which is what the C Vtbl declares and what this signature spells out.
D3D12_RESOURCE_DESC *STDMETHODCALLTYPE hooked_getdesc(ID3D12Resource *self, D3D12_RESOURCE_DESC *out)
{
	++g_getdesc_calls;
	return g_orig_getdesc(self, out);
}

ID3D12Device *real_device(Gpu &gpu)
{
	if (!running_under_reshade())
		return gpu.device.Get();
	IUnknown *orig = unwrap(gpu.device.Get());
	if (orig == nullptr)
		return gpu.device.Get();
	ID3D12Device *dev = nullptr;
	orig->QueryInterface(IID_PPV_ARGS(&dev));
	orig->Release();
	if (dev != nullptr)
		dev->Release(); // the device outlives the harness; a borrowed pointer is enough
	return dev != nullptr ? dev : gpu.device.Get();
}

} // namespace

bool test_vtable_patch_roundtrip(Gpu &gpu)
{
	std::printf("\n[test] vtable_patch installs once and chains to the original\n");
	ComPtr<ID3D12Resource> a = create_buffer(gpu, 256), b = create_buffer(gpu, 256);
	if (!a || !b)
	{
		fail("could not create buffers");
		return false;
	}
	g_orig_getdesc = reinterpret_cast<PFN_GetDesc>(stray_dlss::native::patch_slot(a.Get(),
		stray_dlss::native::slot::kResource_GetDesc, reinterpret_cast<void *>(&hooked_getdesc), "ID3D12Resource::GetDesc"));
	void *again = stray_dlss::native::patch_slot(a.Get(), stray_dlss::native::slot::kResource_GetDesc,
		reinterpret_cast<void *>(&hooked_getdesc), "ID3D12Resource::GetDesc");
	const D3D12_RESOURCE_DESC da = a->GetDesc(), db = b->GetDesc();
	std::printf("  calls=%d idempotent=%d desc_ok=%d shared_vtable_reached_b=%d\n",
		g_getdesc_calls.load(), again == reinterpret_cast<void *>(g_orig_getdesc),
		da.Width == 256 && db.Width == 256, g_getdesc_calls.load() == 2);
	check(g_orig_getdesc != nullptr, "patch_slot returned the original");
	check(again == reinterpret_cast<void *>(g_orig_getdesc), "a second install returns the stored original");
	check(da.Width == 256 && db.Width == 256, "the chained original still answers");
	check(g_getdesc_calls.load() >= 1, "the hook fired");

	// original_for resolves the original PER-VTABLE from the object, exactly what the present
	// owner's hooks now use instead of a shadow global (facts §20). It must equal what patch_slot
	// returned for a's slot, and — the whole point — return nullptr for a slot never patched.
	void *by_lookup = stray_dlss::native::original_for(a.Get(), stray_dlss::native::slot::kResource_GetDesc);
	void *unpatched = stray_dlss::native::original_for(a.Get(), 11); // ID3D12Resource::GetGPUVirtualAddress, never patched
	std::printf("  original_for(GetDesc)=%p (want %p) original_for(unpatched)=%p (want null)\n",
		by_lookup, reinterpret_cast<void *>(g_orig_getdesc), unpatched);
	check(by_lookup == reinterpret_cast<void *>(g_orig_getdesc), "original_for returns the chained original for a patched slot");
	check(unpatched == nullptr, "original_for returns null for a slot that was never patched");
	return true;
}

bool test_registry_liveness(Gpu &gpu)
{
	std::printf("\n[test] resource registry: creation-time facts, GPU VA, and the destruction sentinel\n");
	namespace reg = stray_dlss::native::registry;
	check(reg::arm(), "the sentinel page allocates");
	stray_dlss::icept::ResourceId id = 0;
	{
		ComPtr<ID3D12Resource> buf = create_buffer(gpu, 256, D3D12_HEAP_TYPE_UPLOAD);
		if (!buf)
		{
			fail("could not create a buffer");
			return false;
		}
		id = reinterpret_cast<stray_dlss::icept::ResourceId>(buf.Get());
		reg::note_created(buf.Get());
		reg::note_created(buf.Get()); // idempotent
		check(reg::is_live(id), "registered buffer is live");
		stray_dlss::icept::ResourceInfo info;
		check(reg::describe(id, info) && info.is_buffer && info.buffer_size == 256 && info.upload_heap,
			"describe reports buffer, 256 bytes, upload heap");
		stray_dlss::icept::BufferRange br;
		check(reg::buffer_for_va(buf->GetGPUVirtualAddress() + 16, br) && br.buffer == id && br.offset == 16,
			"buffer_for_va maps VA+16 to (buffer, 16)");
		check(!reg::buffer_for_va(buf->GetGPUVirtualAddress() + 256, br), "one past the end is not in the buffer");
		std::printf("  registered=%llu live=%llu sentinel_failures=%llu\n",
			static_cast<unsigned long long>(reg::stats().registered), static_cast<unsigned long long>(reg::stats().live),
			static_cast<unsigned long long>(reg::stats().sentinel_failures));
	} // the last reference goes here: the sentinel fires
	check(!reg::is_live(id), "released buffer is NOT live, with no explicit destroy call");
	check(reg::stats().sentinel_failures == 0, "the sentinel attached");
	return true;
}

// A module handle for an address, or null when no module maps it — how the harness proves the
// sentinel page is NOT inside this image (the thing an add-on reload unmaps).
HMODULE module_containing(const void *addr)
{
	HMODULE h = nullptr;
	if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(addr), &h))
		return nullptr;
	return h;
}

// The §15 hazard, closed: a sentinel attached in one life of the DLL must be harmless after
// that life detaches, because the resource it rides on can die later — after ReShade has
// unloaded the image (docs/STRAY-RENDERING-FACTS.md §14/§15). The vtable lives outside the
// image and detach() turns it into inert stubs; the runtime's Release then reaches code that
// is still mapped and does nothing. A second life re-arms and re-registers cleanly.
bool test_registry_sentinel_survives_detach(Gpu &gpu)
{
	std::printf("\n[test] resource registry: sentinels outlive a detach without reaching this image\n");
	namespace reg = stray_dlss::native::registry;
	namespace sh = stray_dlss::native::shadow;
	check(reg::arm(), "life 1 armed");
	const void *vtbl = reg::sentinel_vtable_for_test();
	check(vtbl != nullptr, "the sentinel vtable exists");
	const HMODULE page_module = module_containing(vtbl);
	const HMODULE our_module = module_containing(reinterpret_cast<const void *>(&test_registry_sentinel_survives_detach));
	std::printf("  vtable page %p is in module %p; this code is in module %p\n", vtbl,
		static_cast<void *>(page_module), static_cast<void *>(our_module));
	check(page_module == nullptr, "the sentinel vtable page is mapped by NO module (VirtualAlloc, not the image)");
	// Before detach the table points INTO this image (the real Release runs our code).
	const void *const *live_entries = static_cast<const void *const *>(vtbl);
	check(module_containing(live_entries[2]) == our_module, "before detach, Release points into this image");

	// Life 1: a buffer that will outlive the detach, and one registered only to be counted.
	ComPtr<ID3D12Resource> survivor = create_buffer(gpu, 256, D3D12_HEAP_TYPE_UPLOAD);
	ComPtr<ID3D12Resource> doomed = create_buffer(gpu, 256, D3D12_HEAP_TYPE_UPLOAD);
	if (!survivor || !doomed)
	{
		fail("could not create buffers");
		return false;
	}
	const auto id_of = [](ID3D12Resource *r) { return reinterpret_cast<stray_dlss::icept::ResourceId>(r); };
	reg::note_created(survivor.Get());
	reg::note_created(doomed.Get());
	check(reg::is_live(id_of(survivor.Get())) && reg::is_live(id_of(doomed.Get())), "both registered live in life 1");
	const reg::Stats before = reg::stats();

	// The host detaches (ReShade unloading the add-on across the device recreate).
	reg::detach();
	check(!reg::is_live(id_of(survivor.Get())), "after detach the registry knows nothing");
	check(reg::stats().orphaned >= 2, "the two sentinels were orphaned");
	check(module_containing(live_entries[2]) == nullptr, "after detach, Release points OUTSIDE every module (the inert stub)");
	check(module_containing(live_entries[1]) == nullptr && module_containing(live_entries[0]) == nullptr,
		"...and so do AddRef and QueryInterface");

	// The resource dies while the image is "gone": the runtime calls the orphaned sentinel's
	// Release. If that reached this image's code, on_destroyed would count it.
	doomed.Reset();
	check(reg::stats().destroyed == before.destroyed, "an orphaned sentinel's Release reaches NO callback");
	std::printf("  the doomed buffer died after detach: destroyed=%llu (unchanged), no fault\n",
		static_cast<unsigned long long>(reg::stats().destroyed));

	// Life 2: re-arm, and re-register the survivor. SetPrivateDataInterface REPLACES life 1's
	// sentinel (the runtime releases it into the stub) and attaches a live one.
	reg::set_destroy_listener(&sh::forget_resource);
	check(reg::arm(), "life 2 armed");
	check(reg::sentinel_vtable_for_test() != vtbl, "life 2 has its own vtable page; life 1's is left inert");
	reg::note_created(survivor.Get());
	check(reg::is_live(id_of(survivor.Get())), "the survivor is live again in life 2");
	const std::uint64_t destroyed_before = reg::stats().destroyed;
	survivor.Reset();
	check(reg::stats().destroyed == destroyed_before + 1, "the survivor's death fires life 2's sentinel, exactly once");
	check(!reg::is_live(id_of(nullptr)) , "null is never live");
	std::printf("  registered=%llu destroyed=%llu orphaned=%llu unarmed=%llu\n",
		static_cast<unsigned long long>(reg::stats().registered), static_cast<unsigned long long>(reg::stats().destroyed),
		static_cast<unsigned long long>(reg::stats().orphaned), static_cast<unsigned long long>(reg::stats().unarmed));
	return true;
}

// The GPU-VA map resolves by RANGE: a root CBV into the interior of UE4's multi-MB upload ring,
// and placed buffers that ALIAS one another on a heap (a small one inside a big one's span),
// where a lookup past the inner one's end must still find the outer one.
bool test_registry_va_ranges_ring_and_placed(Gpu &gpu)
{
	std::printf("\n[test] resource registry: GPU VA -> buffer by range (ring interior, aliased placed buffers)\n");
	namespace reg = stray_dlss::native::registry;
	reg::arm();
	const auto id_of = [](ID3D12Resource *r) { return reinterpret_cast<stray_dlss::icept::ResourceId>(r); };
	// An 8 MB upload "ring", addressed 4.7 MB in — run F's measured offsets.
	ComPtr<ID3D12Resource> ring = create_buffer(gpu, 8u << 20, D3D12_HEAP_TYPE_UPLOAD);
	if (!ring)
	{
		fail("could not create the ring");
		return false;
	}
	reg::note_created(ring.Get());
	stray_dlss::icept::BufferRange br;
	check(reg::buffer_for_va(ring->GetGPUVirtualAddress() + 4706816, br) && br.buffer == id_of(ring.Get()) && br.offset == 4706816,
		"an interior address of the ring resolves to (ring, 4706816)");
	check(reg::buffer_for_va(ring->GetGPUVirtualAddress() + (8u << 20) - 1, br) && br.offset == (8u << 20) - 1, "...and its last byte");
	check(!reg::buffer_for_va(ring->GetGPUVirtualAddress() + (8u << 20), br), "one past the ring's end is nothing");

	// A heap with an outer placed buffer and an inner one ALIASING its middle. (Not `big`/`small`: rpcndr.h defines `small` as a macro.)
	D3D12_HEAP_DESC hd = {};
	hd.SizeInBytes = 1u << 20;
	hd.Properties.Type = D3D12_HEAP_TYPE_UPLOAD;
	hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
	ComPtr<ID3D12Heap> heap;
	HR(gpu.device->CreateHeap(&hd, IID_PPV_ARGS(&heap)));
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
	bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ComPtr<ID3D12Resource> outer, inner;
	bd.Width = 512u << 10;
	HR(gpu.device->CreatePlacedResource(heap.Get(), 0, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&outer)));
	bd.Width = 4096;
	HR(gpu.device->CreatePlacedResource(heap.Get(), 65536, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&inner)));
	reg::note_created(outer.Get());
	reg::note_created(inner.Get());
	const std::uint64_t big_va = outer->GetGPUVirtualAddress(), small_va = inner->GetGPUVirtualAddress();
	std::printf("  outer %llx +%u  inner %llx +%u  (inner - outer = %lld)\n", static_cast<unsigned long long>(big_va), 512u << 10,
		static_cast<unsigned long long>(small_va), 4096u, static_cast<long long>(small_va - big_va));
	check(small_va > big_va && small_va < big_va + (512u << 10), "the inner placed buffer lies INSIDE the outer one's VA range");
	check(reg::buffer_for_va(small_va + 100, br) && br.buffer == id_of(inner.Get()) && br.offset == 100, "inside the inner one: the inner one (innermost)");
	check(reg::buffer_for_va(big_va + 100, br) && br.buffer == id_of(outer.Get()) && br.offset == 100, "before the inner one: the outer one");
	check(reg::buffer_for_va(big_va + 200000, br) && br.buffer == id_of(outer.Get()) && br.offset == 200000,
		"PAST the inner one's end: still the outer one (the range walk, not the nearest start)");
	inner.Reset(); // the sentinel erases its VA range too
	check(reg::buffer_for_va(small_va + 100, br) && br.buffer == id_of(outer.Get()) && br.offset == small_va + 100 - big_va,
		"after the inner one dies, its addresses belong to the outer one again");
	return true;
}

bool test_descriptor_shadow_copy_chain(Gpu &gpu)
{
	std::printf("\n[test] descriptor shadow (DEBUG mode): offline view -> copy into the online heap -> GPU handle -> entry\n");
	namespace sh = stray_dlss::native::shadow;
	namespace reg = stray_dlss::native::registry;
	sh::set_mode(sh::Mode::debug); // this test asserts provenance (seq / via_copy / src_slot)
	sh::clear_for_test();
	reg::set_destroy_listener(&sh::forget_resource);

	D3D12_HEAP_PROPERTIES def = {};
	def.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = 64; td.Height = 64; td.DepthOrArraySize = 1; td.MipLevels = 1;
	td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.SampleDesc.Count = 1;
	ComPtr<ID3D12Resource> tex;
	HR(gpu.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&tex)));
	reg::note_created(tex.Get());

	D3D12_DESCRIPTOR_HEAP_DESC hd = {};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	hd.NumDescriptors = 8;
	ComPtr<ID3D12DescriptorHeap> offline, online;
	HR(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&offline)));
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	HR(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&online)));
	const UINT inc = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	sd.Texture2D.MipLevels = 1;
	const D3D12_CPU_DESCRIPTOR_HANDLE off0 = offline->GetCPUDescriptorHandleForHeapStart();
	gpu.device->CreateShaderResourceView(tex.Get(), &sd, off0);
	// Driving the shadow's API directly here (the hooks are installed by a later test).
	// Under ReShade the handles the harness holds are PROXY handles; the shadow keys by whatever
	// it is given consistently, so the chain still closes.
	sh::ViewEntry e;
	e.kind = sh::ViewKind::srv;
	e.resource = reinterpret_cast<stray_dlss::icept::ResourceId>(tex.Get());
	e.format = stray_dlss::TexFormat::r16g16b16a16_float;
	e.width = 64; e.height = 64;
	sh::note_view(off0.ptr, e);

	D3D12_CPU_DESCRIPTOR_HANDLE on5 = online->GetCPUDescriptorHandleForHeapStart();
	on5.ptr += 5 * inc;
	gpu.device->CopyDescriptorsSimple(1, on5, off0, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	sh::note_copy(on5.ptr, off0.ptr);
	sh::note_heap_bound(online.Get());

	const D3D12_GPU_DESCRIPTOR_HANDLE gpu5 = { online->GetGPUDescriptorHandleForHeapStart().ptr + 5 * inc };
	stray_dlss::icept::DescriptorId cpu = 0;
	check(sh::gpu_to_cpu(gpu5.ptr, cpu) && cpu == on5.ptr, "GPU handle of online[5] maps back to its CPU handle");
	sh::ViewEntry got;
	check(sh::lookup(cpu, got) && got.resource == e.resource && got.format == e.format && got.width == 64,
		"the copied slot holds the SRV entry");
	check(got.via_copy && !got.is_null, "the copied slot is marked as written by a copy");
	sh::ViewEntry src_entry;
	check(sh::lookup(off0.ptr, src_entry) && !src_entry.via_copy && src_entry.seq < got.seq,
		"the source slot was written by a view creation, earlier in the write sequence");

	// A NULL view is a KNOWN empty slot, and copying it propagates that knowledge: the resolver
	// must see "nothing bound here" rather than "unknown" (UE4 fills unbound registers from
	// null descriptors; run F counted 3.6M such copies as unknown).
	D3D12_CPU_DESCRIPTOR_HANDLE off1 = off0;
	off1.ptr += inc;
	const std::uint64_t unknown_copies_before = sh::unknown_copies();
	sh::note_null_view(off1.ptr);
	D3D12_CPU_DESCRIPTOR_HANDLE on6 = on5;
	on6.ptr += inc;
	sh::note_copy(on6.ptr, off1.ptr);
	sh::ViewEntry null_entry;
	check(sh::lookup(on6.ptr, null_entry) && null_entry.is_null && null_entry.resource == 0 && null_entry.via_copy,
		"a copy of a null view is a known-null slot, not an unknown one");
	check(sh::unknown_copies() == unknown_copies_before, "copying a null view is not an unknown copy");

	const std::uint64_t unknown_before = sh::unknown_lookups();
	tex.Reset(); // destroyed -> sentinel -> registry::on_destroyed -> shadow::forget_resource
	check(sh::lookup(cpu, got) && got.dead && got.resource == e.resource, "after the resource died the slot is a TOMBSTONE naming it");
	check(got.via_copy && got.src_slot == off0.ptr, "the copied slot remembers its source");
	check(sh::lookup(off0.ptr, got) && got.dead && !got.via_copy, "...the offline slot is a tombstone too, written by a view creation");
	check(sh::lookup(on6.ptr, null_entry) && null_entry.is_null, "...but the null slot stays known-null");
	sh::count_unknown_lookup();
	check(sh::unknown_lookups() == unknown_before + 1, "unknown_lookups counts");
	std::printf("  slots=%llu heaps=%llu unknown_copies=%llu\n", static_cast<unsigned long long>(sh::stats().slots),
		static_cast<unsigned long long>(sh::stats().heaps), static_cast<unsigned long long>(sh::unknown_copies()));
	return true;
}

// A stream the way CD3DX12_PIPELINE_STATE_STREAM lays it out: type, payload at its own
// alignment, each subobject padded to pointer alignment.
bool test_descriptor_shadow_fast(Gpu &gpu)
{
	std::printf("\n[test] descriptor shadow (FAST mode): flat lock-free arrays - write, copy, ranges, gpu_to_cpu, liveness\n");
	namespace sh = stray_dlss::native::shadow;
	namespace reg = stray_dlss::native::registry;
	sh::set_mode(sh::Mode::fast);
	sh::clear_for_test();
	reg::arm();

	D3D12_HEAP_PROPERTIES def = {};
	def.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = 64; td.Height = 64; td.DepthOrArraySize = 1; td.MipLevels = 1;
	td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.SampleDesc.Count = 1;
	ComPtr<ID3D12Resource> tex;
	HR(gpu.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&tex)));
	reg::note_created(tex.Get());

	D3D12_DESCRIPTOR_HEAP_DESC hd = {};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	hd.NumDescriptors = 8;
	ComPtr<ID3D12DescriptorHeap> offline, online;
	HR(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&offline)));
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	HR(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&online)));
	// Fast mode learns a heap's flat array at creation; drive it directly (the hooks are a later test).
	sh::note_heap_created(offline.Get());
	sh::note_heap_created(online.Get());
	const UINT inc = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	sd.Texture2D.MipLevels = 1;
	const D3D12_CPU_DESCRIPTOR_HANDLE off0 = offline->GetCPUDescriptorHandleForHeapStart();
	gpu.device->CreateShaderResourceView(tex.Get(), &sd, off0);
	sh::ViewEntry e;
	e.kind = sh::ViewKind::srv;
	e.resource = reinterpret_cast<stray_dlss::icept::ResourceId>(tex.Get());
	e.dxgi_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	sh::note_view(off0.ptr, e);

	sh::ViewEntry got;
	check(sh::lookup(off0.ptr, got) && got.resource == e.resource && got.kind == sh::ViewKind::srv,
		"fast: the written slot holds the SRV entry, resource and kind");
	check(got.width == 64 && got.height == 64 && !got.dead,
		"fast: dimensions are reconstructed from the registry, and the live resource is not dead");

	// Copy into the online heap; the GPU handle maps back and the entry follows.
	D3D12_CPU_DESCRIPTOR_HANDLE on5 = online->GetCPUDescriptorHandleForHeapStart();
	on5.ptr += 5 * inc;
	gpu.device->CopyDescriptorsSimple(1, on5, off0, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	sh::note_copy(on5.ptr, off0.ptr);
	stray_dlss::icept::DescriptorId cpu = 0;
	const D3D12_GPU_DESCRIPTOR_HANDLE gpu5 = { online->GetGPUDescriptorHandleForHeapStart().ptr + 5 * inc };
	check(sh::gpu_to_cpu(gpu5.ptr, cpu) && cpu == on5.ptr, "fast: GPU handle of online[5] maps back to its CPU handle");
	check(sh::lookup(cpu, got) && got.resource == e.resource && got.width == 64, "fast: the copied slot holds the SRV entry");

	// A range copy of several descriptors in one call.
	D3D12_CPU_DESCRIPTOR_HANDLE off_run = off0; // off0..off0+2 -> online[0..2]
	sh::ViewEntry e1 = e; sh::note_view(off_run.ptr + inc, e1);
	D3D12_CPU_DESCRIPTOR_HANDLE on0 = online->GetCPUDescriptorHandleForHeapStart();
	sh::note_copy_range(on0.ptr, off0.ptr, 2, inc);
	check(sh::lookup(on0.ptr, got) && got.resource == e.resource, "fast: note_copy_range propagates the first of the run");
	check(sh::lookup(on0.ptr + inc, got) && got.resource == e.resource, "fast: note_copy_range propagates the second of the run");

	// A null view is a KNOWN empty slot and copying it carries that.
	D3D12_CPU_DESCRIPTOR_HANDLE off1 = off0; off1.ptr += 3 * inc;
	sh::note_null_view(off1.ptr);
	D3D12_CPU_DESCRIPTOR_HANDLE on6 = on5; on6.ptr += inc;
	sh::note_copy(on6.ptr, off1.ptr);
	check(sh::lookup(on6.ptr, got) && got.is_null && got.resource == 0, "fast: a copy of a null view is a known-null slot");

	// A slot never written is UNKNOWN (lookup fails), distinct from null.
	D3D12_CPU_DESCRIPTOR_HANDLE on7 = on6; on7.ptr += inc;
	check(!sh::lookup(on7.ptr, got), "fast: an untouched slot reads as unknown, not as a stale entry");

	// Liveness by generation: when the resource dies its slots read as tombstones naming it.
	tex.Reset();
	check(sh::lookup(cpu, got) && got.dead && got.resource == e.resource, "fast: after the resource died the slot is a tombstone naming it");

	std::printf("  fast slots_written=%llu heaps=%llu\n",
		static_cast<unsigned long long>(sh::stats().slots), static_cast<unsigned long long>(sh::stats().heaps));
	sh::set_mode(sh::Mode::debug); // leave the harness back on debug for the remaining tests
	sh::clear_for_test();
	return true;
}

bool test_native_stream_walk()
{
	std::printf("\n[test] pipeline-state stream walk finds the CS and stops honestly\n");
	using stray_dlss::native::hooks::find_cs_in_stream;
	unsigned char blob[64] = { 1, 2, 3 };
	struct alignas(void *) SubRs { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; ID3D12RootSignature *v; };
	struct alignas(void *) SubBc { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_SHADER_BYTECODE v; };
	struct alignas(void *) SubFlags { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_PIPELINE_STATE_FLAGS v; };
	struct alignas(void *) SubMask { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; UINT v; };
	struct Stream { SubRs rs; SubMask node; SubFlags flags; SubBc cs; } s = {};
	s.rs.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
	s.node.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK;
	s.flags.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS;
	s.cs.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS;
	s.cs.v.pShaderBytecode = blob;
	s.cs.v.BytecodeLength = sizeof(blob);
	std::size_t len = 0;
	const char *why = nullptr;
	const void *cs = find_cs_in_stream(&s, sizeof(s), len, &why);
	check(cs == blob && len == sizeof(blob), "CS found after three other subobjects");
	// A graphics stream: no CS, no complaint.
	struct Gfx { SubRs rs; SubBc vs; SubBc ps; } g = {};
	g.rs.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
	g.vs.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS;
	g.ps.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS;
	why = nullptr;
	check(find_cs_in_stream(&g, sizeof(g), len, &why) == nullptr && why == nullptr, "graphics stream: no CS, no stop reason");
	// Truncated: the CS subobject cut short.
	why = nullptr;
	check(find_cs_in_stream(&s, sizeof(s) - 8, len, &why) == nullptr && why != nullptr, "truncated stream is reported");
	std::printf("  stop reason for the truncated stream: %s\n", why ? why : "(none)");
	// An unknown subobject type stops the walk rather than guessing a size.
	struct Unk { SubMask junk; SubBc cs; } u = {};
	u.junk.t = static_cast<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE>(0x7fff);
	u.cs.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS;
	why = nullptr;
	check(find_cs_in_stream(&u, sizeof(u), len, &why) == nullptr && why != nullptr, "unknown subobject type stops the walk");
	return true;
}

// The whole chain through the PUBLIC API with the hooks installed: a UE4-shaped compute root
// signature, the measured TAA bindings (depth + stencil as two views of ONE typeless
// resource, velocity, colour, history, eye adaptation, a root CBV carrying a View prefix
// whose row 135 validates itself), CopyDescriptors into the online heap, the binds, and
// then the native resolve — and a Dispatch through the hooked slot with the oracle's answer
// published, so the differential machinery validates itself before it sees the game.
bool test_native_hooks_ue4_shaped_frame(Gpu &gpu)
{
	std::printf("\n[test] native hooks: a UE4-shaped frame resolves to the measured registers\n");
	namespace icept = stray_dlss::icept;
	namespace native = stray_dlss::native;
	namespace sh = stray_dlss::native::shadow;

	ID3D12Device *dev = real_device(gpu);
	std::printf("  installing on %s device %p\n", running_under_reshade() ? "the UNWRAPPED" : "the", static_cast<void *>(dev));
	if (!native::install(dev, native::Mode::observe))
	{
		fail("native::install failed");
		return false;
	}
	std::printf("  %s\n", native::attach_report());
	stray_dlss::diff::set_enabled(true);

	// --- root signature: [0] table SRV t0-t5, [1] root CBV b1, [2] table UAV u0, [3] constants b0 x4 ---
	D3D12_DESCRIPTOR_RANGE srv_range = {};
	srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srv_range.NumDescriptors = 6;
	srv_range.BaseShaderRegister = 0;
	srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	D3D12_DESCRIPTOR_RANGE uav_range = {};
	uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	uav_range.NumDescriptors = 1;
	uav_range.BaseShaderRegister = 0;
	uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	D3D12_ROOT_PARAMETER params[4] = {};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[0].DescriptorTable.NumDescriptorRanges = 1;
	params[0].DescriptorTable.pDescriptorRanges = &srv_range;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[1].Descriptor.ShaderRegister = 1;
	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[2].DescriptorTable.NumDescriptorRanges = 1;
	params[2].DescriptorTable.pDescriptorRanges = &uav_range;
	params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[3].Constants.ShaderRegister = 0;
	params[3].Constants.Num32BitValues = 4;
	D3D12_ROOT_SIGNATURE_DESC rd = {};
	rd.NumParameters = 4;
	rd.pParameters = params;
	ComPtr<ID3DBlob> blob, err;
	HR(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err));
	ComPtr<ID3D12RootSignature> rs;
	HR(gpu.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
	check(native::hooks::root_signature_count() >= 1, "CreateRootSignature was seen and deserialized");

	// A PSO for it: the probe shader reads b0 constants, b1, t0 and u0 — a subset of this layout.
	D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
	pd.pRootSignature = rs.Get();
	pd.CS.pShaderBytecode = g_warp_probe_dxbc;
	pd.CS.BytecodeLength = sizeof(g_warp_probe_dxbc);
	ComPtr<ID3D12PipelineState> pso;
	HR(gpu.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));
	const std::uint64_t pso_hash = native::pipeline_hash(pso.Get());
	check(pso_hash != 0 && pso_hash == stray_dlss::fnv1a64(g_warp_probe_dxbc, sizeof(g_warp_probe_dxbc)),
		"CreateComputePipelineState hashed the caller's bytecode");

	// --- the textures, at the measured formats ---
	D3D12_HEAP_PROPERTIES def = {};
	def.Type = D3D12_HEAP_TYPE_DEFAULT;
	const auto make_tex = [&](UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource> &out) {
		D3D12_RESOURCE_DESC d = {};
		d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
		d.Format = f; d.SampleDesc.Count = 1; d.Flags = flags;
		return SUCCEEDED(gpu.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&out)));
	};
	ComPtr<ID3D12Resource> eye, colour, depth, velocity, history, output, out_buf;
	if (!make_tex(1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, eye) ||
		!make_tex(320, 180, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, colour) ||
		!make_tex(320, 180, DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, depth) ||
		!make_tex(320, 180, DXGI_FORMAT_R16G16B16A16_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, velocity) ||
		!make_tex(320, 180, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, history) ||
		!make_tex(640, 360, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, output))
	{
		fail("could not create the frame's textures");
		return false;
	}
	const auto id_of = [](ID3D12Resource *r) { return reinterpret_cast<icept::ResourceId>(r); };
	check(native::registry::is_live(id_of(depth.Get())), "CreateCommittedResource registered the depth texture");

	// --- the View CB: an upload buffer whose 2448-byte prefix carries a row-135 that validates itself ---
	ComPtr<ID3D12Resource> view_cb = create_buffer(gpu, 4096, D3D12_HEAP_TYPE_UPLOAD);
	{
		unsigned char *m = nullptr;
		D3D12_RANGE none = { 0, 0 };
		HR(view_cb->Map(0, &none, reinterpret_cast<void **>(&m)));
		std::memset(m, 0, 4096);
		float row135[4] = { 0.0f, 0.452f, 1.0f / 0.452f, 0.0f };
		const int msaa = 1;
		std::memcpy(&row135[0], &msaa, 4); // int32 NumSceneColorMSAASamples reinterpreted
		std::memcpy(m + 135 * 16, row135, sizeof(row135));
		view_cb->Unmap(0, nullptr);
	}

	// --- descriptors: OFFLINE heap (as UE4 does), then CopyDescriptors into the ONLINE heap ---
	D3D12_DESCRIPTOR_HEAP_DESC hd = {};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	hd.NumDescriptors = 16;
	ComPtr<ID3D12DescriptorHeap> offline, online;
	HR(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&offline)));
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	HR(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&online)));
	const UINT inc = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const auto off = [&](UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE h = offline->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * inc; return h; };
	const auto on = [&](UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE h = online->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * inc; return h; };

	D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	sd.Texture2D.MipLevels = 1;
	// t0 eye, t1 colour, t2 depth, t3 velocity, t4 stencil (same resource as t2), t5 history
	sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; gpu.device->CreateShaderResourceView(eye.Get(), &sd, off(0));
	sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; gpu.device->CreateShaderResourceView(colour.Get(), &sd, off(1));
	sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; gpu.device->CreateShaderResourceView(depth.Get(), &sd, off(2));
	sd.Format = DXGI_FORMAT_R16G16B16A16_UNORM; gpu.device->CreateShaderResourceView(velocity.Get(), &sd, off(3));
	sd.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT; sd.Texture2D.PlaneSlice = 1; gpu.device->CreateShaderResourceView(depth.Get(), &sd, off(4));
	sd.Texture2D.PlaneSlice = 0;
	sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; gpu.device->CreateShaderResourceView(history.Get(), &sd, off(5));
	D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
	ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	gpu.device->CreateUnorderedAccessView(output.Get(), nullptr, &ud, off(6));
	// The probe shader writes a STRUCTURED BUFFER at u0 (GPU-based validation checks the
	// UAV dimension against the shader), so the dispatch phase below rebinds u0 to this one;
	// the resolve phase keeps the texture UAV, which is what the TAA pass binds.
	{
		D3D12_HEAP_PROPERTIES dh = {};
		dh.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
		bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		HR(gpu.device->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &bd,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out_buf)));
		D3D12_UNORDERED_ACCESS_VIEW_DESC bu = {};
		bu.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		bu.Format = DXGI_FORMAT_UNKNOWN;
		bu.Buffer.NumElements = 4;
		bu.Buffer.StructureByteStride = 16;
		gpu.device->CreateUnorderedAccessView(out_buf.Get(), nullptr, &bu, off(7));
	}

	// UE4 copies its offline descriptors into the online heap in one call per table.
	gpu.device->CopyDescriptorsSimple(6, on(0), off(0), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE dst = on(8), src = off(6);
		const UINT one = 1;
		gpu.device->CopyDescriptors(1, &dst, &one, 1, &src, &one, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}
	gpu.device->CopyDescriptorsSimple(1, on(9), off(7), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// --- bind, exactly as a game would ---
	ID3D12DescriptorHeap *heaps[] = { online.Get() };
	gpu.list->SetDescriptorHeaps(1, heaps);
	gpu.list->SetComputeRootSignature(rs.Get());
	D3D12_GPU_DESCRIPTOR_HANDLE gsrv = online->GetGPUDescriptorHandleForHeapStart();
	D3D12_GPU_DESCRIPTOR_HANDLE guav = gsrv;
	guav.ptr += 8 * inc;
	gpu.list->SetComputeRootDescriptorTable(0, gsrv);
	gpu.list->SetComputeRootConstantBufferView(1, view_cb->GetGPUVirtualAddress());
	gpu.list->SetComputeRootDescriptorTable(2, guav);
	const UINT consts[4] = { 0x2222u, 0, 0, 0 };
	gpu.list->SetComputeRoot32BitConstants(3, 4, consts, 0);
	gpu.list->SetPipelineState(pso.Get());

	// Cumulative across the harness (an earlier test counts one on purpose), so the check is
	// that a fully shadowed frame adds NONE.
	const std::uint64_t unknown_at_start = sh::unknown_lookups();

	// --- the native resolve ---
	icept::CommandContext ctx;
	ctx.native = reinterpret_cast<ID3D12GraphicsCommandList *>(
		running_under_reshade() ? [&]() -> void * {
			IUnknown *o = unwrap(gpu.list.Get());
			void *p = o; if (o) o->Release(); return p; }() : static_cast<void *>(gpu.list.Get()));
	ctx.device = dev;
	icept::DispatchBindings b;
	const bool resolved = native::backend().resolve_compute_bindings(ctx, b);
	check(resolved, "resolve_compute_bindings found the bindings");
	const auto find = [](const std::vector<stray_dlss::BoundTexture> &v, std::uint32_t slot) -> const stray_dlss::BoundTexture * {
		for (const auto &t : v) if (t.slot == slot) return &t;
		return nullptr;
	};
	const auto *t0 = find(b.srvs, 0), *t1 = find(b.srvs, 1), *t2 = find(b.srvs, 2), *t3 = find(b.srvs, 3), *t4 = find(b.srvs, 4), *t5 = find(b.srvs, 5);
	const auto *u0 = find(b.uavs, 0);
	check(b.srvs.size() == 6 && t0 && t1 && t2 && t3 && t4 && t5, "six SRVs, t0..t5");
	check(t2 && t4 && t2->resource == t4->resource && t2->resource == id_of(depth.Get()), "t2 and t4 are the SAME resource (depth+stencil)");
	check(t2 && t2->format == stray_dlss::TexFormat::r32_float_x8x24_typeless, "t2 is the depth view format");
	check(t4 && t4->format == stray_dlss::TexFormat::x32_typeless_g8x24_uint, "t4 is the stencil view format");
	check(t0 && t0->width == 1 && t0->height == 1 && t0->format == stray_dlss::TexFormat::r32g32b32a32_float, "t0 is the 1x1 eye adaptation");
	check(t3 && t3->format == stray_dlss::TexFormat::r16g16b16a16_unorm && t3->width == 320, "t3 is velocity");
	check(u0 && u0->resource == id_of(output.Get()) && u0->width == 640 && u0->height == 360, "u0 is the output at its own size");
	check(b.constant_buffers.size() == 1 && b.constant_buffers[0].first == 1 && b.constant_buffers[0].second.buffer == id_of(view_cb.Get()) &&
		b.constant_buffers[0].second.offset == 0, "the root CBV at parameter 1 maps to the upload buffer");
	{
		// Under ReShade the harness holds a PROXY heap; the shadow reports the real one beneath.
		ID3D12DescriptorHeap *real_online = online.Get();
		if (running_under_reshade())
		{
			if (IUnknown *o = unwrap(online.Get()))
			{
				real_online = reinterpret_cast<ID3D12DescriptorHeap *>(o);
				o->Release();
			}
		}
		check(b.heap_count == 1 && b.heaps[0] == real_online, "the online heap is reported");
	}
	{
		unsigned char copy[stray_dlss::ue4::kViewPrefixBytes];
		stray_dlss::ue4::ViewParams vp{};
		const bool read = b.constant_buffers.size() == 1 &&
			native::backend().read_buffer(b.constant_buffers[0].second, sizeof(copy), copy) &&
			stray_dlss::ue4::parse_view_params(copy, sizeof(copy), vp);
		check(read, "read_buffer returned the View prefix and it parsed");
		if (read)
		{
			const float prod = vp.pre_exposure * vp.one_over_pre_exposure;
			std::printf("  row 135: pre_exposure=%f one_over=%f product=%f\n", vp.pre_exposure, vp.one_over_pre_exposure, prod);
			check(std::fabs(prod - 1.0f) < 1e-5f, "row 135 validates itself: PreExposure * OneOverPreExposure == 1");
		}
	}
	const std::uint64_t unknown_before = sh::unknown_lookups();
	check(unknown_before == unknown_at_start, "no unknown lookups on a fully shadowed frame");

	// --- the differential machinery, end to end: the oracle's answer published, the hooked
	//     Dispatch consumes it and compares against a fresh native resolve ---
	// u0 -> the structured buffer the probe shader expects, so the Dispatch is legal.
	D3D12_GPU_DESCRIPTOR_HANDLE gbuf = gsrv;
	gbuf.ptr += 9 * inc;
	gpu.list->SetComputeRootDescriptorTable(2, gbuf);
	icept::DispatchBindings expected;
	check(native::backend().resolve_compute_bindings(ctx, expected) && find(expected.uavs, 0) != nullptr &&
		find(expected.uavs, 0)->resource == id_of(out_buf.Get()) && find(expected.uavs, 0)->width == 0,
		"rebinding the UAV table is seen: u0 is now the buffer (no extent)");
	stray_dlss::diff::publish_expected(ctx.native, pso_hash, 40, 23, expected);
	gpu.list->Dispatch(1, 1, 1);
	const stray_dlss::diff::Summary sum = stray_dlss::diff::summary();
	std::printf("  diff: dispatches=%llu agree=%llu disagreements=%llu\n",
		static_cast<unsigned long long>(sum.dispatches), static_cast<unsigned long long>(sum.agree),
		static_cast<unsigned long long>(sum.dispatches - sum.agree));
	check(sum.dispatches >= 1 && sum.dispatches == sum.agree, "the hooked Dispatch consumed the expectation and agreed");
	// A deliberately wrong oracle must be caught, or the observer proves nothing.
	expected.srvs[2].format = stray_dlss::TexFormat::r11g11b10_float;
	stray_dlss::diff::publish_expected(ctx.native, pso_hash, 40, 23, expected);
	gpu.list->Dispatch(1, 1, 1);
	const stray_dlss::diff::Summary sum2 = stray_dlss::diff::summary();
	check(sum2.mismatch == sum.mismatch + 1, "a wrong oracle format on t2 is one mismatch");
	if (!flush(gpu))
		return false;
	check(drain_validation(gpu, "native-hooks") == 0, "no D3D12 validation errors with the hooks installed");

	// --- the liveness contract through the whole chain ---
	ID3D12Resource *const depth_ptr_for_test = depth.Get(); // the identity, kept after the object is gone
	depth.Reset();
	gpu.list->SetDescriptorHeaps(1, heaps);
	gpu.list->SetComputeRootSignature(rs.Get());
	gpu.list->SetComputeRootDescriptorTable(0, gsrv);
	gpu.list->SetComputeRootConstantBufferView(1, view_cb->GetGPUVirtualAddress());
	gpu.list->SetComputeRootDescriptorTable(2, gbuf);
	const std::uint64_t dead_before = sh::dead_lookups();
	icept::DispatchBindings after;
	native::backend().resolve_compute_bindings(ctx, after);
	check(find(after.srvs, 2) == nullptr && find(after.srvs, 4) == nullptr, "after the depth texture died, t2 and t4 are absent");
	check(find(after.srvs, 1) != nullptr && find(after.uavs, 0) != nullptr, "...and the others are still there");
	check(sh::unknown_lookups() == unknown_before, "...as TOMBSTONES, not unknowns: unknown_lookups did not grow");
	check(sh::dead_lookups() == dead_before + 2, "dead_lookups grew by exactly 2");
	{
		int tomb = 0;
		for (const auto &u : after.unresolved)
			if (u.kind == 't' && (u.reg == 2 || u.reg == 4) && u.reason == 2 && u.dead_resource == id_of(depth_ptr_for_test))
				++tomb;
		check(tomb == 2, "the resolve reports both slots as tombstones naming the dead depth texture");
	}
	std::printf("  unknown_lookups=%llu dead_lookups=%llu\n", static_cast<unsigned long long>(sh::unknown_lookups()),
		static_cast<unsigned long long>(sh::dead_lookups()));
	// Setting the SAME root signature again changes nothing: the root CBV survives in the
	// shadow (vkd3d-proton early-outs on the same object; ReShade's tracker keeps its args).
	gpu.list->SetComputeRootSignature(rs.Get());
	icept::DispatchBindings same_rs;
	native::backend().resolve_compute_bindings(ctx, same_rs);
	check(same_rs.constant_buffers.size() == 1 && find(same_rs.srvs, 1) != nullptr,
		"re-setting the same root signature keeps the root CBV and the tables in the shadow");
	// A DIFFERENT one does invalidate: this signature is not the same object. (Declared at
	// function scope: the list references it until it is discarded below, and the debug
	// layer objects to closing a list whose root signature is already gone — #921.)
	ComPtr<ID3D12RootSignature> other;
	{
		D3D12_ROOT_SIGNATURE_DESC empty = {};
		ComPtr<ID3DBlob> eb, ee;
		HR(D3D12SerializeRootSignature(&empty, D3D_ROOT_SIGNATURE_VERSION_1, &eb, &ee));
		HR(gpu.device->CreateRootSignature(0, eb->GetBufferPointer(), eb->GetBufferSize(), IID_PPV_ARGS(&other)));
		gpu.list->SetComputeRootSignature(other.Get());
		icept::DispatchBindings changed;
		native::backend().resolve_compute_bindings(ctx, changed);
		check(changed.constant_buffers.empty() && changed.srvs.empty(), "a different root signature invalidates every root argument");
		gpu.list->SetComputeRootSignature(rs.Get()); // back, for a clean close
	}
	std::printf("  unknown_lookups=%llu\n", static_cast<unsigned long long>(sh::unknown_lookups()));
	native::log_stats("harness");
	stray_dlss::diff::log_summary("harness");
	std::printf("diff: disagreements=%llu\n", static_cast<unsigned long long>(sum.dispatches - sum.agree));
	// Leave the list clean — DISCARDED, not executed: it binds a table holding views of the
	// depth texture destroyed above, and executing that removed the device (see discard()).
	if (!discard(gpu))
		return false;
	drain_validation(gpu, "native-hooks-teardown");
	stray_dlss::diff::set_enabled(false);
	return true;
}

// ---- NativeMode=drive (plan Stage 3) ----
//
// The native backend as THE driver: its hooks deliver the seam's events to a sink, and the
// Dispatch hook does not forward the game's call when the sink says so. This sink behaves as
// the application does at the TAA site — records our motion-vector resolve onto the game's
// list (clobbering root signature, PSO, heaps and root arguments), puts the game's compute
// state back through the NATIVE restore (the root shadow's replay, root CBVs and 32-bit
// constants included), and then either lets the game's dispatch run or suppresses it.
namespace {

struct DriveTestSink final : stray_dlss::icept::Sink
{
	Gpu *gpu = nullptr;
	GameResources *game = nullptr;
	const stray_dlss::ue4::ViewParams *view = nullptr;
	bool clobber = false;
	bool suppress = false;
	int dispatches = 0, pipelines = 0, binds = 0, resets = 0;
	bool restore_ran = false;

	void on_device(::ID3D12Device *, bool) override {}
	void on_pipeline(std::uint64_t, const void *, std::size_t, stray_dlss::icept::ShaderKind, bool) override { ++pipelines; }
	void on_command_list(const stray_dlss::icept::CommandContext &, bool) override {}
	void on_command_list_reset(const stray_dlss::icept::CommandContext &) override { ++resets; }
	void on_bind_pipeline(const stray_dlss::icept::CommandContext &, std::uint64_t) override { ++binds; }
	bool on_dispatch(const stray_dlss::icept::CommandContext &ctx, std::uint32_t, std::uint32_t, std::uint32_t) override
	{
		++dispatches;
		if (clobber)
		{
			// Exactly the application's discipline: everything we record is OUR state and
			// must not enter the shadow (assessment §8.3).
			stray_dlss::native::OwnCodeScope own;
			const auto view_copy = *view;
			stray_dlss::mv::ResolveInputs in;
			in.depth_resource = reinterpret_cast<std::uint64_t>(game->depth.Get());
			in.velocity_resource = reinterpret_cast<std::uint64_t>(game->velocity.Get());
			in.render_width = 1920;
			in.render_height = 1080;
			in.view = &view_copy;
			stray_dlss::mv::record(ctx.native, in, 2);
			stray_dlss::native::backend().restore_game_compute_state(ctx);
			restore_ran = true;
		}
		return suppress;
	}
	void on_render_targets(const stray_dlss::icept::CommandContext &, std::uint32_t, const stray_dlss::icept::DescriptorId *, stray_dlss::icept::DescriptorId) override {}
	void on_draw(const stray_dlss::icept::CommandContext &, std::uint32_t) override {}
	void on_copy(const stray_dlss::icept::CommandContext &, stray_dlss::icept::ResourceId, stray_dlss::icept::ResourceId) override {}
	void on_execute(const stray_dlss::icept::CommandContext &) override {}
	void on_swapchain(const stray_dlss::icept::ResourceId *, std::uint32_t, bool) override {}
	std::uint64_t next_frame() override { return 0; }
	void on_present(const stray_dlss::icept::PresentContext &) override {}
};

} // namespace

// Runs LAST: it re-installs the hooks in drive mode, which is where they stay.
bool test_drive_mode_restore_is_complete(Gpu &gpu)
{
	std::printf("\n[test] NativeMode=drive: the hooks deliver, the native restore is complete, the Dispatch hook suppresses\n");
	namespace native = stray_dlss::native;
	ID3D12Device *dev = real_device(gpu);

	static DriveTestSink sink;
	sink = DriveTestSink{};
	sink.gpu = &gpu;
	{
		const HRESULT removed = dev->GetDeviceRemovedReason();
		std::printf("  device removed reason on entry: 0x%08lx\n", static_cast<unsigned long>(removed));
		check(removed == S_OK, "the device is still alive on entry (an earlier test did not remove it)");
	}
	native::uninstall(); // the observer life ends (hooks restored, sentinels made inert)
	native::set_sink(&sink);
	if (!native::install(dev, native::Mode::drive))
	{
		fail("native::install(drive) failed");
		return false;
	}
	std::printf("  %s\n", native::attach_report());
	check(native::mode() == native::Mode::drive, "the backend is in drive mode");

	// Pipeline creation is delivered through the creation hook (the application hashes it).
	const int pipelines_before = sink.pipelines;
	Probe p;
	if (!build_probe(gpu, p))
		return false;
	check(sink.pipelines == pipelines_before + 1, "CreateComputePipelineState delivered on_pipeline exactly once");
	check(native::pipeline_hash(p.pso.Get()) == stray_dlss::fnv1a64(g_warp_probe_dxbc, sizeof(g_warp_probe_dxbc)),
		"...and the shadow hashed the same bytecode");

	GameResources game;
	if (!create_game_resources(gpu, game, 1920, 1080))
		return false;
	if (!stray_dlss::mv::initialise(dev, 1920, 1080))
		return false;
	const auto view = make_view(1920, 1080);
	sink.game = &game;
	sink.view = &view;
	drain_validation(gpu, "drive-setup");

	// GOLDEN: the game's dispatch, delivered to the sink, let through untouched.
	UINT golden[4] = {};
	const int binds_before = sink.binds;
	bind_probe_state(gpu, p, 0x1111u);
	std::printf("  binds before=%d after=%d\n", binds_before, sink.binds);
	check(sink.binds == binds_before + 1, "SetPipelineState delivered on_bind_pipeline exactly once");
	gpu.list->Dispatch(1, 1, 1);
	check(sink.dispatches == 1, "Dispatch delivered on_dispatch");
	if (!read_probe(gpu, p, golden))
		return false;
	std::printf("  golden   = %08X %08X %08X %08X\n", golden[0], golden[1], golden[2], golden[3]);
	check(sink.resets >= 1, "the list's Reset (inside flush) delivered on_command_list_reset");

	// CLOBBER + NATIVE RESTORE inside on_dispatch, then the game's dispatch runs: the output
	// must be the golden one, or the restore lost a root argument.
	sink.clobber = true;
	bind_probe_state(gpu, p, 0x1111u);
	gpu.list->Dispatch(1, 1, 1);
	check(sink.restore_ran, "the sink clobbered and restored through the native backend");
	UINT restored[4] = {};
	if (!read_probe(gpu, p, restored))
		return false;
	std::printf("  restored = %08X %08X %08X %08X\n", restored[0], restored[1], restored[2], restored[3]);
	check(memcmp(golden, restored, sizeof(golden)) == 0,
		"the game's dispatch produces identical output after our clobber and the NATIVE restore");
	check(drain_validation(gpu, "drive-restore") == 0, "no validation errors across the drive-mode clobber and restore");

	// SUPPRESSION: the sink says true, the game's Dispatch is never forwarded. Prove it with
	// a marker the probe writes: after a suppressed dispatch the output is unchanged; after
	// an allowed one with the same marker it changes.
	sink.clobber = false;
	sink.suppress = true;
	bind_probe_state(gpu, p, 0x2222u);
	gpu.list->Dispatch(1, 1, 1);
	UINT suppressed[4] = {};
	if (!read_probe(gpu, p, suppressed))
		return false;
	check(memcmp(golden, suppressed, sizeof(golden)) == 0, "a suppressed dispatch leaves the output untouched (the original was NOT forwarded)");
	sink.suppress = false;
	bind_probe_state(gpu, p, 0x2222u);
	gpu.list->Dispatch(1, 1, 1);
	UINT allowed[4] = {};
	if (!read_probe(gpu, p, allowed))
		return false;
	std::printf("  suppressed= %08X ... allowed= %08X (golden %08X)\n", suppressed[0], allowed[0], golden[0]);
	check(memcmp(golden, allowed, sizeof(golden)) != 0, "...and the same marker, let through, changes it (the control)");
	const native::Stats st = native::stats();
	std::printf("  drive: dispatches delivered=%llu suppressed=%llu pipelines=%llu\n",
		static_cast<unsigned long long>(st.drive_dispatches), static_cast<unsigned long long>(st.drive_suppressed),
		static_cast<unsigned long long>(st.drive_pipelines));
	check(st.drive_dispatches == 4 && st.drive_suppressed == 1, "the drive counters agree: 4 delivered, 1 suppressed");
	check(drain_validation(gpu, "drive-suppress") == 0, "no validation errors across the suppression");
	native::log_stats("harness-drive");
	native::set_sink(nullptr);
	return true;
}
