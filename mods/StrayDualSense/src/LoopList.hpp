// StrayDualSense — which assets loop.
//
// The CALLER never decides. UE4 serialises `bLooping` only when it is TRUE, so a SoundWave's
// name table carrying it IS the game's own flag; the asset tooling emits that as a text file,
// one asset name per line (`haptic_loops.txt`: 22 of 63 VIBE assets, docs/STRAY-DUALSENSE.md
// §12). Looping everything made a 0.24 s bump buzz forever.
//
// Pure and portable; the file read happens elsewhere.
#pragma once

#include <string>
#include <unordered_set>

namespace sds {

class LoopList
{
public:
    // Parse the file's text: one name per line, tolerant of CRLF, trailing spaces and blank
    // lines. Lines starting with '#' are comments. Replaces any previous contents.
    void Parse(const std::string& text);

    bool   Contains(const std::string& name) const { return m_names.count(name) != 0; }
    size_t Count() const { return m_names.size(); }
    bool   Loaded() const { return m_loaded; }

private:
    std::unordered_set<std::string> m_names;
    bool m_loaded = false;
};

} // namespace sds
