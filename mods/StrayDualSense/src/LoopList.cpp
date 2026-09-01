#include "LoopList.hpp"

namespace sds {

void LoopList::Parse(const std::string& text)
{
    m_names.clear();
    m_loaded = true;

    size_t pos = 0;
    while (pos <= text.size())
    {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;

        const size_t b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos) continue;
        const size_t e = line.find_last_not_of(" \t\r");
        line = line.substr(b, e - b + 1);
        if (line.empty() || line[0] == '#') continue;
        m_names.insert(line);
    }
}

} // namespace sds
