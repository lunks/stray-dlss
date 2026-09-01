// The one place ReShade's headers are included.
//
// reshade_overlay.hpp only defines its inline ImGui:: wrappers when IMGUI_VERSION_NUM is
// already defined, so imgui.h MUST come first. Get that order wrong in any single header and
// the whole translation unit silently falls back to imgui.h's own declarations, which nothing
// defines — surfacing much later as unresolved externals for ImGui::Text and friends.
//
// Every file in this project includes THIS header, never <reshade.hpp> directly, so the order
// cannot be got wrong. (docs/RESEARCH.md §2.8)
#pragma once

#include <imgui.h>
#include <reshade.hpp>
