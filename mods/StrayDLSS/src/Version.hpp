#pragma once

// Bumped by hand. It appears in the first line of stray-dlss-plugin.log, which is how a pasted
// log is matched to a build — this project's only feedback loop is a log coming back.
//
// 0.0.1: the attach-timing probe (plan Task 2).
// 0.1.0: the HOST (plan Stage 4). Hooks D3D12CreateDevice from start_mod(), installs the native
//        backend in drive mode on the real device, owns the present, and drives DLSS with no
//        ReShade in the process.
#define STRAY_DLSS_PLUGIN_VERSION_STRING "0.1.0"
