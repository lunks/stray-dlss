#include "intercept/backend.hpp"

#include "log.hpp"

namespace stray_dlss::icept {
namespace {

// Refuses everything. Says so once.
class NullBackend final : public Backend
{
public:
	const char *name() const override { return "null"; }
	bool resolve_compute_bindings(const CommandContext &, DispatchBindings &) override { return refuse(); }
	bool resolve_graphics_srvs(const CommandContext &, std::vector<BoundTexture> &) override { return refuse(); }
	void describe_view(DescriptorId, std::uint32_t, std::vector<BoundTexture> &) override { refuse(); }
	bool describe_resource(ResourceId, ResourceInfo &) override { return refuse(); }
	bool resource_from_view(DescriptorId, ResourceId &) override { return refuse(); }
	bool read_buffer(const BufferRange &, std::uint64_t, void *) override { return refuse(); }
	bool is_resource_live(ResourceId) override { return refuse(); }
	void restore_game_compute_state(const CommandContext &) override { refuse(); }
	void present_barrier(const PresentContext &, ResourceId, std::uint32_t, std::uint32_t) override { refuse(); }
	void dump_tracker_state(const CommandContext &, const char *) override { refuse(); }

private:
	bool refuse()
	{
		if (!m_said)
		{
			m_said = true;
			STRAY_LOG_ERROR("intercept: NO BACKEND INSTALLED - every query answers 'no'. "
				"set_backend() was never called by the host.");
		}
		return false;
	}
	bool m_said = false;
};

NullBackend g_null;
Backend *g_backend = &g_null;

} // namespace

Backend *backend() { return g_backend; }

void set_backend(Backend *b) { g_backend = b != nullptr ? b : &g_null; }

} // namespace stray_dlss::icept
