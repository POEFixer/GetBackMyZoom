// A text the patcher's worker hands on: a key of lang/en.json and its {name}
// arguments. The drawing thread words it in the program language
// (GetBackMyZoom.cpp); the log lines and the copied report word it in English
// (English(), over the English compiled in). STL only: the patcher and the
// tests never include PluginSDK.h.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace Gbmz {

// A key kept for later lookup: the key itself. Spelled like PluginI18n's
// I18n::Key, so tools/plugin_i18n/gen.py --gate checks every such literal
// against lang/en.json.
constexpr const char* Key(const char* key) { return key; }

struct LocText {
    using Args = std::vector<std::pair<std::string, std::string>>;

    std::string key;   // empty: no text
    Args args;         // {name} -> value; values are never translated

    LocText() = default;
    LocText(const char* k) : key(k ? k : "") {}   // NOLINT: a key converts on purpose
    LocText(const char* k, Args a) : key(k ? k : ""), args(std::move(a)) {}

    bool Empty() const { return key.empty(); }
    void Clear() {
        key.clear();
        args.clear();
    }
    bool operator==(const LocText&) const = default;
};

// `text` with every {name} of `args` replaced; a placeholder without an
// argument stays as it is, and a value is never scanned again.
inline std::string FillPlaceholders(const char* text, const LocText::Args& args) {
    std::string out;
    if (!text) return out;
    for (const char* p = text; *p;) {
        if (*p == '{') {
            const char* close = p + 1;
            while (*close && *close != '}') ++close;
            if (*close == '}') {
                const std::string name(p + 1, close);
                const std::string* value = nullptr;
                for (const auto& arg : args)
                    if (arg.first == name) {
                        value = &arg.second;
                        break;
                    }
                if (value) {
                    out += *value;
                    p = close + 1;
                    continue;
                }
            }
        }
        out.push_back(*p++);
    }
    return out;
}

// The English text of `t` (lang/en.json, compiled in), for log lines and the
// copied report; the key itself when the table has none. Defined beside the
// generated English table (GetBackMyZoom.cpp). Any thread.
std::string English(const LocText& t);

} // namespace Gbmz
