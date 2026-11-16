#include "BrushScriptFile.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace {

bool isControl(unsigned char ch) {
    return ch < 0x20 || ch == 0x7F;
}

// Multi-line text is written as a TOML multi-line literal string so scripts stay readable
// in the file. Literal strings cannot escape anything, so text containing quotes or other
// control characters falls back to an escaped basic string.
bool canWriteMultilineLiteral(std::string_view value) {
    if (value.find('\n') == std::string_view::npos || value.find('\'') != std::string_view::npos) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](char ch) {
        const auto byte = static_cast<unsigned char>(ch);
        return isControl(byte) && byte != '\n' && byte != '\t';
    });
}

std::string tomlString(std::string_view value) {
    if (canWriteMultilineLiteral(value)) {
        // A newline straight after the opening delimiter is trimmed by TOML readers.
        return "'''\n" + std::string(value) + "'''";
    }
    std::string result = "\"";
    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\t': result += "\\t"; break;
        case '\r': result += "\\r"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        default:
            if (isControl(byte)) {
                constexpr char digits[] = "0123456789ABCDEF";
                result += "\\u00";
                result += digits[byte >> 4];
                result += digits[byte & 0x0F];
            } else {
                result += ch;
            }
        }
    }
    return result + "\"";
}

std::string describe(std::string_view source) {
    return source.empty() ? std::string("Brush script") : "Brush script " + std::string(source);
}

std::string requireString(const toml::table& table, std::string_view key, bool required,
                          std::string_view source) {
    const auto* node = table.get(key);
    if (node == nullptr) {
        if (required) {
            throw std::runtime_error(describe(source) + " has no \"" + std::string(key) + "\".");
        }
        return {};
    }
    const auto value = node->value<std::string>();
    if (!value) {
        throw std::runtime_error(describe(source) + ": \"" + std::string(key) +
                                 "\" must be a string.");
    }
    return *value;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::optional<ScriptedBrushSettings::Output> parseOutput(const toml::table& table,
                                                         std::string_view source) {
    if (table.get("output") == nullptr) {
        return std::nullopt;
    }
    const auto value = lowercase(requireString(table, "output", true, source));
    if (value == "normalized" || value == "normalised") {
        return ScriptedBrushSettings::Output::Normalized;
    }
    if (value == "elevation") {
        return ScriptedBrushSettings::Output::Elevation;
    }
    throw std::runtime_error(describe(source) +
                             ": \"output\" must be \"normalized\" or \"elevation\".");
}

} // namespace

BrushScriptFile BrushScriptFile::Parse(std::string_view text, std::string_view source) {
    toml::table table;
    try {
        table = toml::parse(text, source);
    } catch (const toml::parse_error& error) {
        std::ostringstream message;
        message << describe(source) << ", line " << error.source().begin.line << ": "
                << error.description();
        throw std::runtime_error(message.str());
    }
    BrushScriptFile file;
    file.name = requireString(table, "name", true, source);
    file.description = requireString(table, "description", false, source);
    file.output = parseOutput(table, source);
    file.script = requireString(table, "script", true, source);
    if (file.name.empty()) {
        throw std::runtime_error(describe(source) + " has an empty name.");
    }
    return file;
}

BrushScriptFile BrushScriptFile::Load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open brush script " + path.string() + ".");
    }
    std::ostringstream text;
    text << input.rdbuf();
    if (input.bad()) {
        throw std::runtime_error("Cannot read brush script " + path.string() + ".");
    }
    return Parse(text.str(), path.string());
}

std::string BrushScriptFile::ToToml() const {
    std::string text = "name=" + tomlString(name) + "\ndescription=" + tomlString(description);
    if (output) {
        text += *output == ScriptedBrushSettings::Output::Elevation ? "\noutput=\"elevation\""
                                                                   : "\noutput=\"normalized\"";
    }
    return text + "\nscript=" + tomlString(script) + "\n";
}

void BrushScriptFile::Save(const std::filesystem::path& path) const {
    if (name.empty()) {
        throw std::invalid_argument("A brush script needs a name.");
    }
    const auto text = ToToml();
    // Write beside the target and rename, so a failed write never truncates an existing file.
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Cannot write brush script " + path.string() + ".");
        }
        output << text;
        output.close();
        if (!output) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw std::runtime_error("Cannot write brush script " + path.string() + ".");
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("Cannot save brush script " + path.string() + ": " +
                                 error.message());
    }
}

std::filesystem::path BrushScriptFileName(std::string_view name) {
    std::string stem;
    for (const char ch : name) {
        const auto byte = static_cast<unsigned char>(ch);
        stem += std::isalnum(byte) != 0 || ch == '-' || ch == '_' || ch == '.' ? ch : '_';
    }
    stem.erase(0, stem.find_first_not_of('.'));
    if (stem.empty()) {
        stem = "script";
    }
    return stem + ".toml";
}

BrushScriptLibrary BrushScriptLibrary::Scan(const std::filesystem::path& directory) {
    BrushScriptLibrary library;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        return library;
    }
    std::filesystem::directory_iterator iterator(directory, error);
    if (error) {
        library.errors.push_back("Cannot read " + directory.string() + ": " + error.message());
        return library;
    }
    for (const auto& entry : iterator) {
        std::error_code entryError;
        if (!entry.is_regular_file(entryError) ||
            lowercase(entry.path().extension().string()) != ".toml") {
            continue;
        }
        try {
            library.entries.push_back({entry.path(), BrushScriptFile::Load(entry.path())});
        } catch (const std::exception& loadError) {
            library.errors.emplace_back(loadError.what());
        }
    }
    std::sort(library.entries.begin(), library.entries.end(), [](const auto& a, const auto& b) {
        const auto left = lowercase(a.file.name);
        const auto right = lowercase(b.file.name);
        return left != right ? left < right : a.path < b.path;
    });
    return library;
}
