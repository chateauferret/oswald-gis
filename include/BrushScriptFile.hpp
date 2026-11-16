#pragma once

#include "ScriptedBrushTool.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A brush script stored as a TOML file with string keys "name", "description", "output"
// ("normalized" or "elevation") and "script".
struct BrushScriptFile {
    std::string name;
    std::string description;
    std::string script;
    // Absent when the file has no "output" key; loading such a file keeps the current setting.
    std::optional<ScriptedBrushSettings::Output> output;

    // Throws std::runtime_error when the file cannot be read, is not valid TOML, or lacks a
    // non-empty string "name" and a string "script". "description" is optional; "output" is
    // optional but must be "normalized" (or "normalised") or "elevation" when present.
    static BrushScriptFile Load(const std::filesystem::path& path);
    // Throws std::invalid_argument for an empty name and std::runtime_error on I/O failure.
    void Save(const std::filesystem::path& path) const;
    std::string ToToml() const;
    static BrushScriptFile Parse(std::string_view text, std::string_view source = {});
};

// "<name>.toml", with characters that are unsafe in file names replaced by '_'.
std::filesystem::path BrushScriptFileName(std::string_view name);

struct BrushScriptLibraryEntry {
    std::filesystem::path path;
    BrushScriptFile file;
};

struct BrushScriptLibrary {
    std::vector<BrushScriptLibraryEntry> entries;
    // One message per .toml file that could not be loaded.
    std::vector<std::string> errors;

    // Loads every .toml file in directory, sorted by name (case-insensitive). A missing
    // directory yields an empty library.
    static BrushScriptLibrary Scan(const std::filesystem::path& directory);
};
