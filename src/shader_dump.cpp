#include "shader_dump.hpp"

#include "log.hpp"

#include <imgui.h>
#include <reshade.hpp>

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace stray_dlss::shader_dump {
namespace {

constexpr const char *kConfigSection = "STRAYDLSS";
constexpr const char *kOutputDir = "stray-dlss-shaders";

std::mutex g_mutex;
bool g_enabled = false;
std::unordered_set<std::uint64_t> g_dumped;
std::unordered_map<std::uint64_t, std::uint32_t> g_dispatch_counts;
std::FILE *g_manifest = nullptr;

} // namespace

void initialise()
{
	std::lock_guard<std::mutex> lock(g_mutex);

	bool enable = false;
	reshade::get_config_value(nullptr, kConfigSection, "DumpShaders", enable);
	g_enabled = enable;

	if (!g_enabled)
	{
		STRAY_LOG_INFO("Shader dumping is off. Set [%s] DumpShaders=1 in ReShade.ini to enable.",
			kConfigSection);
		return;
	}

	if (!CreateDirectoryA(kOutputDir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
	{
		STRAY_LOG_ERROR("Could not create '%s' (error %lu); shader dumping disabled.",
			kOutputDir, GetLastError());
		g_enabled = false;
		return;
	}

	char path[MAX_PATH];
	std::snprintf(path, sizeof(path), "%s\\manifest.txt", kOutputDir);
	if (fopen_s(&g_manifest, path, "w") != 0)
		g_manifest = nullptr;

	if (g_manifest != nullptr)
	{
		std::fprintf(g_manifest, "# stray-dlss compute shader dump\n");
		std::fprintf(g_manifest, "# hash                 bytes    dispatched  file\n");
		std::fflush(g_manifest);
	}

	STRAY_LOG_INFO("Shader dumping ENABLED, writing to '%s\\'", kOutputDir);
}

bool enabled()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_enabled;
}

void dump_compute_shader(std::uint64_t hash, const void *code, std::size_t code_size)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_enabled || code == nullptr || code_size == 0)
		return;

	if (!g_dumped.insert(hash).second)
		return; // already written

	char path[MAX_PATH];
	std::snprintf(path, sizeof(path), "%s\\cs_%016llx.dxbc", kOutputDir,
		static_cast<unsigned long long>(hash));

	std::FILE *f = nullptr;
	if (fopen_s(&f, path, "wb") != 0 || f == nullptr)
	{
		STRAY_LOG_WARN("Could not write %s", path);
		return;
	}

	std::fwrite(code, 1, code_size, f);
	std::fclose(f);

	if (g_manifest != nullptr)
	{
		std::fprintf(g_manifest, "0x%016llx  %8zu  %10u  cs_%016llx.dxbc\n",
			static_cast<unsigned long long>(hash), code_size,
			g_dispatch_counts[hash],
			static_cast<unsigned long long>(hash));
		std::fflush(g_manifest);
	}
}

void note_dispatch(std::uint64_t hash, std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_enabled)
		return;

	// Only the first few dispatches of each shader are interesting; after that the counts
	// just grow. Recording the dispatch dimensions lets the offline analysis match a shader
	// against ceil(viewrect / 8) without the game telling us the view size.
	const std::uint32_t count = ++g_dispatch_counts[hash];
	if (count > 3 || g_manifest == nullptr)
		return;

	std::fprintf(g_manifest, "  dispatch 0x%016llx  %ux%ux%u\n",
		static_cast<unsigned long long>(hash), x, y, z);
	std::fflush(g_manifest);
}

void finish()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_manifest != nullptr)
	{
		std::fprintf(g_manifest, "# %zu distinct compute shaders dumped\n", g_dumped.size());
		std::fclose(g_manifest);
		g_manifest = nullptr;
	}
}

} // namespace stray_dlss::shader_dump
