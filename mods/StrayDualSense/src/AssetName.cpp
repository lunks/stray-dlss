#include "AssetName.hpp"

namespace sds {

std::string ShortAssetName(const std::string& fullName)
{
    const auto isIdent = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_';
    };

    // The Lua mod matched "([%w_]+)%.[%w_]+$" first: the identifier AFTER the final dot.
    const std::string::size_type dot = fullName.find_last_of('.');
    const std::string::size_type begin = (dot == std::string::npos) ? 0 : dot + 1;

    // Then trim to the trailing identifier run, which also covers a bare object name and
    // strips trailing punctuation.
    std::string::size_type end = fullName.size();
    while (end > begin && !isIdent(fullName[end - 1]))
        --end;
    std::string::size_type start = end;
    while (start > begin && isIdent(fullName[start - 1]))
        --start;
    if (start >= end)
        return {};
    return fullName.substr(start, end - start);
}

} // namespace sds
