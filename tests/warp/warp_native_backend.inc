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
	return true;
}

bool test_registry_liveness(Gpu &gpu)
{
	std::printf("\n[test] resource registry: creation-time facts, GPU VA, and the destruction sentinel\n");
	namespace reg = stray_dlss::native::registry;
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

bool test_descriptor_shadow_copy_chain(Gpu &gpu)
{
	std::printf("\n[test] descriptor shadow: offline view -> copy into the online heap -> GPU handle -> entry\n");
	namespace sh = stray_dlss::native::shadow;
	namespace reg = stray_dlss::native::registry;
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
	const std::uint64_t unknown_before = sh::unknown_lookups();
	tex.Reset(); // destroyed -> sentinel -> registry::on_destroyed -> shadow::forget_resource
	check(!sh::lookup(cpu, got), "after the resource died the slot is forgotten");
	check(!sh::lookup(off0.ptr, got), "...the offline slot too");
	sh::count_unknown_lookup();
	check(sh::unknown_lookups() == unknown_before + 1, "unknown_lookups counts");
	std::printf("  slots=%llu heaps=%llu unknown_copies=%llu\n", static_cast<unsigned long long>(sh::stats().slots),
		static_cast<unsigned long long>(sh::stats().heaps), static_cast<unsigned long long>(sh::unknown_copies()));
	return true;
}

// A stream the way CD3DX12_PIPELINE_STATE_STREAM lays it out: type, payload at its own
// alignment, each subobject padded to pointer alignment.
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
	depth.Reset();
	gpu.list->SetDescriptorHeaps(1, heaps);
	gpu.list->SetComputeRootSignature(rs.Get());
	gpu.list->SetComputeRootDescriptorTable(0, gsrv);
	gpu.list->SetComputeRootConstantBufferView(1, view_cb->GetGPUVirtualAddress());
	gpu.list->SetComputeRootDescriptorTable(2, gbuf);
	icept::DispatchBindings after;
	native::backend().resolve_compute_bindings(ctx, after);
	check(find(after.srvs, 2) == nullptr && find(after.srvs, 4) == nullptr, "after the depth texture died, t2 and t4 are absent");
	check(find(after.srvs, 1) != nullptr && find(after.uavs, 0) != nullptr, "...and the others are still there");
	check(sh::unknown_lookups() == unknown_before + 2, "unknown_lookups grew by exactly 2");
	std::printf("  unknown_lookups=%llu\n", static_cast<unsigned long long>(sh::unknown_lookups()));
	native::log_stats("harness");
	stray_dlss::diff::log_summary("harness");
	std::printf("diff: disagreements=%llu\n", static_cast<unsigned long long>(sum.dispatches - sum.agree));
	// Leave the list clean for shutdown.
	if (!flush(gpu))
		return false;
	drain_validation(gpu, "native-hooks-teardown");
	stray_dlss::diff::set_enabled(false);
	return true;
}
