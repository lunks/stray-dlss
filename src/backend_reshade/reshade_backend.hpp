// The ReShade add-on as an icept::Backend: ReShade's trackers (reshade_bindings.{hpp,cpp})
// behind the seam, plus the translation of ReShade's handles into CommandContext /
// PresentContext for the framework-free application.
#pragma once

#include "intercept/backend.hpp"

#include "reshade_all.hpp"

namespace stray_dlss::rsb {

class ReshadeBackend final : public icept::Backend
{
public:
	const char *name() const override { return "reshade"; }
	bool resolve_compute_bindings(const icept::CommandContext &ctx, icept::DispatchBindings &out) override;
	void describe_view(icept::DescriptorId view, std::uint32_t reg, std::vector<BoundTexture> &out) override;
	bool describe_resource(icept::ResourceId res, icept::ResourceInfo &out) override;
	bool resource_from_view(icept::DescriptorId view, icept::ResourceId &out) override;
	bool read_buffer(const icept::BufferRange &range, std::uint64_t bytes, void *out) override;
	bool is_resource_live(icept::ResourceId res) override;
	void restore_game_compute_state(const icept::CommandContext &ctx) override;
	void present_barrier(const icept::PresentContext &ctx, icept::ResourceId res,
	                     std::uint32_t before, std::uint32_t after) override;
	void dump_tracker_state(const icept::CommandContext &ctx, const char *why) override;
};

ReshadeBackend &backend();

// The one device this add-on serves (init_device / destroy_device).
void set_device(reshade::api::device *device);
reshade::api::device *device();

// ReShade's handles -> the seam's contexts. Built on the fly: three virtual calls, no map,
// no lifetime to get wrong. `backend_cookie` is the reshade::api::command_list* itself.
icept::CommandContext context_for(reshade::api::command_list *cmd_list);
icept::PresentContext present_context_for(reshade::api::command_queue *queue,
                                          reshade::api::swapchain *swapchain, std::uint64_t frame);

} // namespace stray_dlss::rsb
