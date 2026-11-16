#include "BrushScriptFile.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Function>
bool throws(Function function) {
    try {
        function();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

void testParseExampleFormat() {
    const auto file = BrushScriptFile::Parse(
        "name=\"fbm\"\ndescription=\"fractal Brownian motion\"\n"
        "script=\"fbm(x, y, z, 4, 2, 0.5, 6)\"");
    check(file.name == "fbm" && file.description == "fractal Brownian motion" &&
          file.script == "fbm(x, y, z, 4, 2, 0.5, 6)", "Example brush script did not parse.");
    const auto noDescription = BrushScriptFile::Parse("name='a'\nscript='1'\nextra=3\n");
    check(noDescription.description.empty() && noDescription.script == "1",
          "Optional description or unknown keys were not tolerated.");
}

void testRejectsInvalidFiles() {
    check(throws([] { BrushScriptFile::Parse("name=\"a\"\n"); }), "Missing script accepted.");
    check(throws([] { BrushScriptFile::Parse("script=\"1\"\n"); }), "Missing name accepted.");
    check(throws([] { BrushScriptFile::Parse("name=\"\"\nscript=\"1\"\n"); }),
          "Empty name accepted.");
    check(throws([] { BrushScriptFile::Parse("name=3\nscript=\"1\"\n"); }),
          "Non-string name accepted.");
    check(throws([] { BrushScriptFile::Parse("name=\"a\nscript=\"1\"\n"); }),
          "Malformed TOML accepted.");
    try {
        BrushScriptFile::Parse("name=\"a\"\nscript=\n", "bad.toml");
        check(false, "Missing value accepted.");
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        check(message.find("bad.toml") != std::string::npos &&
              message.find("line 2") != std::string::npos,
              "Parse errors did not name the file and line.");
    }
}

void testRoundTrip() {
    const BrushScriptFile cases[] = {
        {"fbm", "fractal Brownian motion", "fbm(x, y, z, 4, 2, 0.5, 6)"},
        {"multi", "Two\nlines", "var h := fbm(x, y, z, 4, 2, 0.5, 6);\n\tclamp(-1, h * 2, 1)\n"},
        {"quotes \"and\" 'apostrophes'", "back\\slash", "if (x > 0) 'a'; else 1;\nx\r\n"},
        {"unicode \xC3\xA9", "control \x01 char", "''' tricky \"\"\" \n"},
    };
    for (const auto& original : cases) {
        const auto parsed = BrushScriptFile::Parse(original.ToToml());
        check(parsed.name == original.name && parsed.description == original.description &&
              parsed.script == original.script, "Brush script did not round-trip.");
    }
    const BrushScriptFile multiline{"m", "", "a\nb"};
    check(multiline.ToToml().find("'''") != std::string::npos,
          "Multi-line scripts were not written readably.");
}

void testOutputMode() {
    using Output = ScriptedBrushSettings::Output;
    check(!BrushScriptFile::Parse("name='a'\nscript='1'\n").output.has_value(),
          "A file without an output key reported an output mode.");
    check(BrushScriptFile::Parse("name='a'\noutput='elevation'\nscript='1'\n").output ==
              Output::Elevation, "Elevation output did not parse.");
    check(BrushScriptFile::Parse("name='a'\noutput='Normalised'\nscript='1'\n").output ==
              Output::Normalized, "Normalised output did not parse.");
    check(throws([] { BrushScriptFile::Parse("name='a'\noutput='raw'\nscript='1'\n"); }),
          "Unknown output mode accepted.");
    check(throws([] { BrushScriptFile::Parse("name='a'\noutput=1\nscript='1'\n"); }),
          "Non-string output mode accepted.");
    for (const auto mode : {Output::Normalized, Output::Elevation}) {
        const BrushScriptFile original{"m", "d", "1", mode};
        check(BrushScriptFile::Parse(original.ToToml()).output == mode,
              "Output mode did not round-trip.");
    }
    check(BrushScriptFile{"m", "d", "1"}.ToToml().find("output") == std::string::npos,
          "An unset output mode was written.");
}

void testFileNames() {
    check(BrushScriptFileName("fbm") == "fbm.toml", "Simple name changed.");
    check(BrushScriptFileName("my ridges/2") == "my_ridges_2.toml", "Unsafe name not sanitised.");
    check(BrushScriptFileName("..") == "script.toml", "Dot name not replaced.");
    check(BrushScriptFileName("") == "script.toml", "Empty name not replaced.");
}

void testSaveLoadAndScan() {
    const auto directory = std::filesystem::temp_directory_path() / "oswald_brush_script_tests";
    std::filesystem::remove_all(directory);
    check(BrushScriptLibrary::Scan(directory).entries.empty(), "Missing directory not empty.");
    std::filesystem::create_directories(directory);

    const BrushScriptFile zeta{"Zeta", "last", "1"};
    const BrushScriptFile alpha{"alpha", "first", "fbm(x, y, z, 4, 2, 0.5, 6)\n+ 0.1"};
    zeta.Save(directory / BrushScriptFileName(zeta.name));
    alpha.Save(directory / BrushScriptFileName(alpha.name));
    const auto loaded = BrushScriptFile::Load(directory / "alpha.toml");
    check(loaded.script == alpha.script, "Saved script did not load.");
    check(!std::filesystem::exists(directory / "alpha.toml.tmp"), "Temporary file left behind.");

    alpha.Save(directory / "alpha.toml");
    check(BrushScriptFile::Load(directory / "alpha.toml").name == "alpha", "Overwrite failed.");

    writeText(directory / "broken.toml", "name = \n");
    writeText(directory / "notes.txt", "ignored");
    const auto library = BrushScriptLibrary::Scan(directory);
    check(library.entries.size() == 2 && library.entries[0].file.name == "alpha" &&
          library.entries[1].file.name == "Zeta", "Library was not loaded in name order.");
    check(library.errors.size() == 1 &&
          library.errors[0].find("broken.toml") != std::string::npos,
          "Broken library file was not reported.");

    check(throws([&] { BrushScriptFile{"", "", "1"}.Save(directory / "x.toml"); }),
          "Unnamed script was saved.");
    check(throws([&] { BrushScriptFile::Load(directory / "missing.toml"); }),
          "Missing file loaded.");
    check(throws([&] { alpha.Save(directory / "no-such-dir" / "a.toml"); }),
          "Save to a missing directory succeeded.");
    std::filesystem::remove_all(directory);
}

void testShippedLibrary() {
    const auto library = BrushScriptLibrary::Scan(OSWALD_BRUSH_SCRIPT_DIR);
    check(library.errors.empty() && !library.entries.empty(),
          "Shipped brush scripts did not all load.");
}

} // namespace

int main() {
    try {
        testParseExampleFormat();
        testRejectsInvalidFiles();
        testRoundTrip();
        testOutputMode();
        testFileNames();
        testSaveLoadAndScan();
        testShippedLibrary();
        std::cout << "Brush script file tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Brush script file test failed: " << error.what() << '\n';
        return 1;
    }
}
