#pragma once
// Single include point for the JSON library so the rest of the code does not
// depend on the concrete header path.
#include <nlohmann/json.hpp>

namespace gtamm {
using Json = nlohmann::json;
}
