// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "editor/json_util.h"

namespace pg {

namespace {
void emit(const OJson& j, int indent, int width, std::string& out) {
    std::string flat = j.dump();
    if (!j.is_structured() || int(flat.size()) + indent <= width || j.empty()) {
        out += flat;
        return;
    }
    std::string pad(size_t(indent + 1), ' ');
    std::string close(size_t(indent), ' ');
    if (j.is_array()) {
        // Arrays of short scalars or short arrays: pack several per line.
        bool packable = true;
        for (const OJson& e : j)
            if (e.is_object() || int(e.dump().size()) > width / 3) packable = false;
        out += "[\n";
        if (packable) {
            std::string line = pad;
            bool first = true;
            for (size_t i = 0; i < j.size(); ++i) {
                std::string s = j[i].dump() + (i + 1 < j.size() ? "," : "");
                if (!first && int(line.size() + s.size() + 1) > width) {
                    out += line + "\n";
                    line = pad;
                    first = true;
                }
                line += (first ? "" : " ") + s;
                first = false;
            }
            out += line + "\n";
        } else {
            for (size_t i = 0; i < j.size(); ++i) {
                out += pad;
                emit(j[i], indent + 1, width, out);
                out += i + 1 < j.size() ? ",\n" : "\n";
            }
        }
        out += close + "]";
        return;
    }
    out += "{\n";
    size_t i = 0;
    for (auto it = j.begin(); it != j.end(); ++it, ++i) {
        std::string key = OJson(it.key()).dump();
        out += pad + key + ": ";
        emit(it.value(), indent + 1, width, out);
        out += i + 1 < j.size() ? ",\n" : "\n";
    }
    out += close + "}";
}
}  // namespace

std::string json_pretty(const OJson& j, int width) {
    std::string out;
    emit(j, 0, width, out);
    return out;
}

}  // namespace pg
