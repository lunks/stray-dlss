#include "TweakUi.hpp"

#include "Host.hpp"
#include "TweakState.hpp"
#include "Version.hpp"

#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace stray_dlss::plugin {
namespace {

using tweak::Group;
using tweak::Kind;
using tweak::Knob;

// The result of the last save, kept so the button reports rather than only logging - whoever
// presses it is very likely looking at a screen and not at stray-dlss-plugin.log.
std::string g_save_message;
bool g_save_ok = false;

void help_marker(const Knob &k)
{
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::BeginItemTooltip())
	{
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
		ImGui::TextUnformatted(k.help);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}

// The history-reset tag, drawn IN THE ROW rather than only in the tooltip. A tooltip is opt-in,
// and the failure this guards against — reading the whole screen changing as the knob's own
// effect — happens to someone who did not think to hover.
void reset_tag()
{
	ImGui::SameLine();
	ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f), "[resets NR history]");
}

void draw_knob(const Knob &k)
{
	ImGui::PushID(k.ini_key);
	const float before = tweak::value_of(k);
	float after = before;

	if (k.read_only)
		ImGui::BeginDisabled();

	switch (k.kind)
	{
	case Kind::boolean:
	{
		bool b = before != 0.0f;
		if (ImGui::Checkbox(k.label, &b))
			after = b ? 1.0f : 0.0f;
		break;
	}
	case Kind::integer:
	{
		int i = static_cast<int>(std::lround(before));
		if (ImGui::SliderInt(k.label, &i, static_cast<int>(k.min_value), static_cast<int>(k.max_value)))
			after = static_cast<float>(i);
		break;
	}
	case Kind::combo:
	{
		int i = static_cast<int>(std::lround(before));
		if (i < 0) i = 0;
		if (i >= k.combo_count) i = k.combo_count - 1;
		if (ImGui::Combo(k.label, &i, k.combo_items, k.combo_count))
			after = static_cast<float>(i);
		break;
	}
	case Kind::real:
	{
		float f = before;
		if (ImGui::SliderFloat(k.label, &f, k.min_value, k.max_value, "%.3f"))
			after = f;
		break;
	}
	}

	if (k.read_only)
		ImGui::EndDisabled();

	if (k.resets_nr_history)
		reset_tag();
	if (k.read_only)
	{
		ImGui::SameLine();
		ImGui::TextDisabled("[read-only: create-time]");
	}
	help_marker(k);

	if (after != before)
		tweak::set_value(k, after);

	ImGui::PopID();
}

void draw_group(Group g)
{
	int n = 0;
	const Knob *ks = tweak::knobs(g, n);
	if (ks == nullptr || n == 0)
		return;
	ImGui::SeparatorText(tweak::group_name(g));
	for (int i = 0; i < n; ++i)
		draw_knob(ks[i]);
}

void draw_status()
{
	char line[512];
	tweak::format_sr_status(line, sizeof(line));
	ImGui::TextUnformatted(line);
	tweak::format_nr_status(line, sizeof(line));
	ImGui::TextUnformatted(line);
	tweak::format_fg_status(line, sizeof(line));
	ImGui::TextUnformatted(line);
}

} // namespace

void RenderTweakTab(RC::CppUserModBase * /*instance*/)
{
	ImGui::TextDisabled("StrayDLSS %s", STRAY_DLSS_PLUGIN_VERSION_STRING);
	ImGui::TextWrapped(
		"Every control here takes effect on the NEXT FRAME - the values are re-sent to the NGX "
		"parameter block on every evaluate, so nothing is recreated and nothing is relaunched. "
		"Saving is explicit: nothing is written to StrayDLSS.ini until you press the button.");

	ImGui::SeparatorText("Status");
	draw_status();

	draw_group(Group::nr);
	draw_group(Group::sr);
	draw_group(Group::fg);
	draw_group(Group::diagnostics);

	ImGui::SeparatorText("Persistence");
	const std::string &path = IniPath();
	ImGui::TextDisabled("%s", path.empty() ? "<no StrayDLSS.ini was found at startup>" : path.c_str());
	if (ImGui::Button("Save to ini"))
	{
		std::string error;
		g_save_ok = tweak::save_to_ini(path, error);
		g_save_message = g_save_ok ? "saved; comments and unknown keys preserved" : error;
	}
	ImGui::SameLine();
	if (ImGui::Button("Reload from ini"))
	{
		// The file is re-read by Host::Tick on its own mtime check; this only re-APPLIES what
		// the config source currently holds, which is what makes the button honest about doing
		// nothing when the file has not changed.
		const int moved = tweak::apply_from_config();
		g_save_ok = true;
		char msg[96];
		std::snprintf(msg, sizeof(msg), "%d value(s) re-applied from the ini", moved);
		g_save_message = msg;
	}
	if (!g_save_message.empty())
	{
		ImGui::SameLine();
		ImGui::TextColored(g_save_ok ? ImVec4(0.45f, 0.92f, 0.47f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
			"%s", g_save_message.c_str());
	}
	ImGui::TextWrapped(
		"The save rewrites only the values of the keys above, in place. Comments, blank lines, "
		"key order and every other [STRAYDLSS] key are preserved byte for byte - which matters "
		"because this file is how the box is driven and its comments carry the reasoning for "
		"each knob.");
}

} // namespace stray_dlss::plugin
