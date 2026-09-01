#pragma once

// Bumped by hand. It appears in the first line of stray-dlss-plugin.log, which is how a pasted
// log is matched to a build — this project's only feedback loop is a log coming back.
//
// 0.0.1: the attach-timing probe (plan Task 2). It hooks nothing the game renders with; it
//        measures WHEN start_mod() runs relative to the game's D3D12CreateDevice, and what an
//        early export hook and a throwaway device actually see with ReShade in the process.
#define STRAY_DLSS_PLUGIN_VERSION_STRING "0.0.1-probe"
