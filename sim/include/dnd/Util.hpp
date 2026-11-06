#pragma once
#include <sstream>
#include <string>

namespace dnd {

// Concatenate anything streamable into a std::string.
template <class... Ts>
std::string Cat(const Ts&... parts) {
    std::ostringstream o;
    (o << ... << parts);
    return o.str();
}

}  // namespace dnd
