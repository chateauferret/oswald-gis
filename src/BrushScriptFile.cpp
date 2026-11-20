#include "BrushScriptFile.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <iomanip>
#include <limits>
#include <locale>
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

double requireNumber(const toml::table& table, std::string_view key,
                     std::string_view source) {
    const auto* node = table.get(key);
    if (node != nullptr) {
        if (const auto text = node->value<std::string>()) {
            double result = 0.0;
            const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), result);
            if (error == std::errc{} && end == text->data() + text->size()) {
                return result;
            }
        } else if (node->is_number()) {
            return *node->value<double>();
        }
    }
    throw std::runtime_error(describe(source) + ": \"" + std::string(key) +
                             "\" must be a number or numeric string.");
}

std::vector<ScriptParameter> parseParameters(const toml::table& table, std::string_view source) {
    const auto* node = table.get("parameter");
    if (node == nullptr) {
        return {};
    }
    const auto* array = node->as_array();
    if (array == nullptr) {
        throw std::runtime_error(describe(source) + ": \"parameter\" must be an array of tables.");
    }
    std::vector<ScriptParameter> parameters;
    for (const auto& entry : *array) {
        const auto* definition = entry.as_table();
        if (definition == nullptr) {
            throw std::runtime_error(describe(source) + ": each parameter must be a table.");
        }
        ScriptParameter parameter;
        parameter.name = requireString(*definition, "name", true, source);
        parameter.label = requireString(*definition, "label", false, source);
        parameter.units = requireString(*definition, "units", false, source);
        const auto context = std::string(source) + " parameter '" + parameter.name + "'";
        const auto type = requireString(*definition, "type", true, context);
        if (type == "float") {
            parameter.type = ScriptParameter::Type::Float;
        } else if (type == "integer") {
            parameter.type = ScriptParameter::Type::Integer;
        } else if (type == "choice") {
            parameter.type = ScriptParameter::Type::Choice;
        } else {
            throw std::runtime_error(describe(context) + ": type must be float, integer or choice.");
        }
        if (parameter.type == ScriptParameter::Type::Choice) {
            const auto* choices = (*definition)["choices"].as_array();
            if (choices == nullptr) {
                throw std::runtime_error(describe(context) + ": choices must be an array of strings.");
            }
            for (const auto& choice : *choices) {
                const auto value = choice.value<std::string>();
                if (!value) {
                    throw std::runtime_error(describe(context) + ": choices must contain strings.");
                }
                parameter.choices.push_back(*value);
            }
            for (const auto key : {"control", "min", "max", "scale"}) {
                if (definition->contains(key)) {
                    throw std::runtime_error(describe(context) + ": choice parameters cannot specify " + key + ".");
                }
            }
            parameter.defaultValue = requireString(*definition, "default", true, context);
        } else {
            const auto control = requireString(*definition, "control", true, context);
            if (control == "slider") {
                parameter.control = ScriptParameter::Control::Slider;
            } else if (control == "spin") {
                parameter.control = ScriptParameter::Control::Spin;
            } else if (control == "text") {
                parameter.control = ScriptParameter::Control::Text;
            } else {
                throw std::runtime_error(describe(context) + ": control must be slider, spin or text.");
            }
            parameter.minimum = requireNumber(*definition, "min", context);
            parameter.maximum = requireNumber(*definition, "max", context);
            parameter.defaultValue = requireNumber(*definition, "default", context);
            if (definition->contains("choices")) {
                throw std::runtime_error(describe(context) + ": only choice parameters can specify choices.");
            }
            if (definition->contains("scale")) {
                const auto scale = requireString(*definition, "scale", true, context);
                if (parameter.type != ScriptParameter::Type::Float ||
                    (scale != "linear" && scale != "log")) {
                    throw std::runtime_error(describe(context) + ": scale must be linear or log, for floats only.");
                }
                parameter.scale = scale == "log" ? ScriptParameter::Scale::Log : ScriptParameter::Scale::Linear;
            }
        }
        parameters.push_back(std::move(parameter));
    }
    try {
        ScriptedBrushTool::ValidateParameters(parameters);
    } catch (const std::invalid_argument& error) {
        throw std::runtime_error(describe(source) + ": " + error.what());
    }
    return parameters;
}

std::string numberString(double value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return tomlString(stream.str());
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
    if (table.contains("gpu_script")) {
        file.gpuScript = requireString(table, "gpu_script", true, source);
        if (file.gpuScript->empty() || file.gpuScript->size() > 64 * 1024) {
            throw std::runtime_error(describe(source) +
                                     ": gpu_script must contain between 1 byte and 64 KiB.");
        }
    }
    file.parameters = parseParameters(table, source);
    if (table.contains("application")) {
        const auto application = requireString(table, "application", true, source);
        if (application == "noise") {
            file.application = BrushApplication::Noise;
        } else if (application == "elevation") {
            file.application = BrushApplication::Elevation;
        } else if (application == "delta") {
            file.application = BrushApplication::Delta;
        } else {
            throw std::runtime_error(describe(source) + ": application must be noise, elevation or delta.");
        }
    }
    if (const auto* node = table.get("continuous")) {
        if (!node->is_boolean()) {
            throw std::runtime_error(describe(source) + ": continuous must be a boolean.");
        }
        file.continuous = *node->value<bool>();
    }
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
    ScriptedBrushTool::ValidateParameters(parameters);
    std::string text = "name=" + tomlString(name) + "\ndescription=" + tomlString(description);
    if (output) {
        text += *output == ScriptedBrushSettings::Output::Elevation ? "\noutput=\"elevation\""
                                                                   : "\noutput=\"normalized\"";
    }
    text += "\napplication=" + tomlString(application == BrushApplication::Noise ? "noise" :
                                          application == BrushApplication::Elevation ? "elevation" : "delta");
    text += continuous ? "\ncontinuous=true" : "\ncontinuous=false";
    text += "\nscript=" + tomlString(script) + "\n";
    if (gpuScript) {
        if (gpuScript->empty()) {
            throw std::invalid_argument("A GPU brush body cannot be empty.");
        }
        text += "gpu_script=" + tomlString(*gpuScript) + "\n";
    }
    for (const auto& parameter : parameters) {
        text += "\n[[parameter]]\nname=" + tomlString(parameter.name);
        if (!parameter.label.empty()) {
            text += "\nlabel=" + tomlString(parameter.label);
        }
        if (!parameter.units.empty()) {
            text += "\nunits=" + tomlString(parameter.units);
        }
        if (parameter.type == ScriptParameter::Type::Choice) {
            text += "\ntype=\"choice\"\nchoices=[";
            for (std::size_t i = 0; i < parameter.choices.size(); ++i) {
                text += (i == 0 ? "" : ", ") + tomlString(parameter.choices[i]);
            }
            text += "]\ndefault=" + tomlString(std::get<std::string>(parameter.defaultValue)) + "\n";
        } else {
            text += parameter.type == ScriptParameter::Type::Float ? "\ntype=\"float\"" : "\ntype=\"integer\"";
            text += "\ncontrol=" + tomlString(parameter.control == ScriptParameter::Control::Slider ?
                "slider" : parameter.control == ScriptParameter::Control::Spin ? "spin" : "text");
            text += "\nmin=" + numberString(parameter.minimum) +
                    "\nmax=" + numberString(parameter.maximum);
            if (parameter.type == ScriptParameter::Type::Float) {
                text += "\nscale=" + tomlString(parameter.scale == ScriptParameter::Scale::Log ? "log" : "linear");
            }
            text += "\ndefault=" + numberString(std::get<double>(parameter.defaultValue)) + "\n";
        }
    }
    return text;
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
