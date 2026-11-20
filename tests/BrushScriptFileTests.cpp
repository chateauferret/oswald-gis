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

    BrushScriptFile gpu;
    gpu.name = "gpu body";
    gpu.script = "x";
    gpu.gpuScript = "return x;\n";
    const auto gpuParsed = BrushScriptFile::Parse(gpu.ToToml());
    check(gpuParsed.gpuScript == gpu.gpuScript,
          "Optional multiline gpu_script did not round-trip.");
    check(!BrushScriptFile::Parse("name='cpu only'\nscript='x'\n").gpuScript,
          "A CPU-only script unexpectedly acquired a GPU body.");
    check(throws([] {
        BrushScriptFile::Parse("name='empty gpu'\nscript='x'\ngpu_script=''\n");
    }), "Empty gpu_script was accepted.");
    check(throws([] {
        BrushScriptFile::Parse("name='bad gpu'\nscript='x'\ngpu_script=3\n");
    }), "Non-string gpu_script was accepted.");
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

void testParameters() {
    const auto file = BrushScriptFile::Parse(
        "name='parameters'\nscript=\"mode == 'one' ? amount + count : 0\"\n"
        "[[parameter]]\nname='amount'\ntype='float'\ncontrol='slider'\n"
        "min='0.1'\nmax='100'\nscale='log'\ndefault='4'\n"
        "[[parameter]]\nname='count'\ntype='integer'\ncontrol='spin'\n"
        "min=0\nmax=30\ndefault=5\n"
        "[[parameter]]\nname='mode'\ntype='choice'\nchoices=['one','two']\ndefault='one'\n"
        "[[parameter]]\nname='offset'\ntype='float'\ncontrol='text'\n"
        "min=-10.5\nmax=10.5\ndefault=0.25\n");
    check(file.parameters.size() == 4 &&
          file.parameters[0].scale == ScriptParameter::Scale::Log &&
          std::get<double>(file.parameters[0].defaultValue) == 4.0 &&
          file.parameters[1].type == ScriptParameter::Type::Integer &&
          file.parameters[1].control == ScriptParameter::Control::Spin &&
          file.parameters[2].choices == std::vector<std::string>{"one", "two"} &&
          std::get<std::string>(file.parameters[2].defaultValue) == "one" &&
          file.parameters[3].control == ScriptParameter::Control::Text &&
          file.parameters[3].scale == ScriptParameter::Scale::Linear,
          "Parameter definitions did not parse.");
    const auto parsed = BrushScriptFile::Parse(file.ToToml());
    check(parsed.parameters == file.parameters && parsed.script == file.script,
          "Parameter definitions/defaults did not round-trip.");
    check(BrushScriptFile::Parse("name='a'\nscript='1'").parameters.empty(),
          "Legacy script unexpectedly acquired parameters.");

    const std::string header = "name='bad'\nscript='1'\n";
    const std::string number = "[[parameter]]\nname='amount'\ntype='float'\ncontrol='slider'\n";
    for (const auto* invalid : {
             "min=0\nmax=1\ndefault=2", "min=1\nmax=1\ndefault=1",
             "min=2\nmax=1\ndefault=1", "min='no'\nmax=1\ndefault=0",
             "min=nan\nmax=1\ndefault=0", "min=0\nmax=inf\ndefault=0",
             "min=0\nmax=1.7e308\ndefault=0",
             "min=0\nmax=1\ndefault='nan'", "min=0\nmax=1",
             "max=1\ndefault=0", "min=0\nmax=1\ndefault=0\nscale='bad'",
             "min=0\nmax=1\ndefault=0\nscale='log'",
             "min=0\nmax=1\ndefault=0\nchoices=['one']"}) {
        check(throws([&] { BrushScriptFile::Parse(header + number + invalid); }),
              "Invalid numeric parameter accepted.");
    }
    const std::string integer = "[[parameter]]\nname='count'\ntype='integer'\ncontrol='text'\n";
    for (const auto* invalid : {"min=0\nmax=5\ndefault=1.5", "min=0.5\nmax=5\ndefault=1",
                               "min=0\nmax=2147483648\ndefault=1",
                               "min=0\nmax=5\ndefault=1\nscale='linear'"}) {
        check(throws([&] { BrushScriptFile::Parse(header + integer + invalid); }),
              "Invalid integer parameter accepted.");
    }
    const std::string choice = "[[parameter]]\nname='mode'\ntype='choice'\n";
    for (const auto* invalid : {"choices=[]\ndefault='one'", "choices=['one']\ndefault='two'",
                               "choices=['one','one']\ndefault='one'",
                               "choices=[1]\ndefault='one'", "default='one'",
                               "choices=['one']\ndefault=0",
                               "choices=['one']\ndefault='one'\ncontrol='text'"}) {
        check(throws([&] { BrushScriptFile::Parse(header + choice + invalid); }),
              "Invalid choice parameter accepted.");
    }
    for (const auto* name : {"x", "lat", "lower", "fbm", "sin", "pi", "while", "two words", "2x", ""}) {
        check(throws([&] {
            BrushScriptFile::Parse(header + "[[parameter]]\nname='" + name +
                                   "'\ntype='integer'\ncontrol='slider'\nmin=0\nmax=5\ndefault=1");
        }), "Invalid or reserved parameter name accepted.");
    }
    check(throws([&] { BrushScriptFile::Parse(header + number + "min=0\nmax=1\ndefault=0\n" +
                                             number + "min=0\nmax=1\ndefault=0"); }),
          "Duplicate parameter names accepted.");
    for (const auto* invalid : {"parameter=1", "parameter=[1]",
                               "[[parameter]]\nname='a'\ntype='unknown'",
                               "[[parameter]]\nname='a'\ntype='float'\ncontrol='unknown'"}) {
        check(throws([&] { BrushScriptFile::Parse(header + invalid); }),
              "Invalid parameter structure accepted.");
    }
}

void testBrushBehaviorMetadata() {
    const auto file = BrushScriptFile::Parse(
        "name='continuous'\napplication='delta'\ncontinuous=true\nscript='rate * elapsed'\n"
        "[[parameter]]\nname='Units'\ntype='choice'\nchoices=['Kilometres','Miles']\ndefault='Miles'\n"
        "[[parameter]]\nname='range_km'\nlabel='Blur range'\nunits='Units'\ntype='float'\n"
        "control='slider'\nmin=0.1\nmax=20000\ndefault=100\n");
    const auto parsed = BrushScriptFile::Parse(file.ToToml());
    check(parsed.application == BrushApplication::Delta && parsed.continuous &&
          parsed.parameters == file.parameters && parsed.parameters[1].label == "Blur range",
          "Brush metadata, parameter labels or distance display units did not round-trip.");
    check(BrushScriptFile::Parse("name='a'\nscript='1'").application == BrushApplication::Noise &&
          !BrushScriptFile::Parse("name='a'\nscript='1'").continuous,
          "Existing files no longer default to one-shot noise brushes.");
    for (const auto* invalid : {"application='bad'", "application=1", "continuous='true'", "continuous=1"}) {
        check(throws([&] { BrushScriptFile::Parse(std::string("name='a'\nscript='1'\n") + invalid); }),
              "Invalid brush behavior metadata accepted.");
    }
    check(throws([] {
        BrushScriptFile::Parse(
            "name='a'\nscript='1'\n[[parameter]]\nname='range_km'\nunits='missing'\n"
            "type='float'\ncontrol='slider'\nmin=0\nmax=1\ndefault=0");
    }), "Invalid parameter unit reference accepted.");
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
        testParameters();
        testBrushBehaviorMetadata();
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
