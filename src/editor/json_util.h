// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <string>

#include "json/json.hpp"

namespace pg {

using OJson = nlohmann::ordered_json;

// Pretty JSON that stays readable for big data files: objects one key per line, but any
// array or object whose one-line form fits in `width` characters stays on one line.
std::string json_pretty(const OJson& j, int width = 110);

}  // namespace pg
