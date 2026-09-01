// StrayDualSense — asset-name derivation.
//
// Pure, portable, and split out from Runtime.cpp for exactly that reason: it is the only piece
// of game-facing logic that can be proven in CI on a machine with no Windows and no game.
#pragma once

#include <string>

namespace sds {

// "SoundWave /Game/Sound/SFX/controllers/Vibrations/CatPurr2_VIBE.CatPurr2_VIBE"
//   -> "CatPurr2_VIBE"
//
// Reproduces the working Lua mod's shortName(): the identifier after the final '.', or the
// trailing identifier run when there is no '.'. Returns "" when nothing usable is present —
// callers must treat that as an error, not as a name.
std::string ShortAssetName(const std::string& fullName);

} // namespace sds
