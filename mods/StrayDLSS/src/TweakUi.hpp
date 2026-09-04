// The in-game tuning tab's drawing half. ImGui only; the model, the setters and the ini save
// are in TweakState.hpp, which is deliberately ImGui-free so the fast CI lane still covers it.
//
// WHERE THIS APPEARS, and it is not what "overlay" usually means. UE4SS's debug GUI at the
// pinned commit (68caddcf) always renders into its OWN OS window: DebuggingGUI::setup ends with
// m_os_backend->create_window(), and the [Debug] RenderMode setting chooses only WHICH THREAD
// pumps that window's loop (ExternalThread's jthread, or a UEngine::Tick /
// UGameViewportClient::Tick post-hook calling gui_render_thread_tick). There is no swapchain or
// Present hook for ImGui anywhere in that tree, so there is no in-game overlay to render into.
// HARD, read out of UE4SS/src/GUI/GUI.cpp and UE4SS/src/UE4SSProgram.cpp at that commit.
#pragma once

namespace RC {
class CppUserModBase;
}

namespace stray_dlss::plugin {

// The register_tab callback. UE4SS's GUITab::RenderFunctionType is a plain
// `void (*)(CppUserModBase*)` at this commit — not a std::function — so this is a free
// function, and the `instance` argument is unused because every value the tab shows lives in
// the application's own modules rather than in the mod object.
void RenderTweakTab(RC::CppUserModBase *instance);

} // namespace stray_dlss::plugin
