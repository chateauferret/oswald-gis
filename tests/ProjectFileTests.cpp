#include "ProjectFile.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Operation>
void expectFailure(Operation operation) {
    try {
        operation();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("Expected project operation to fail.");
}

struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("oswald-project-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryDirectory() { std::filesystem::create_directory(path); }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        if (error) {
            std::cerr << "Project test cleanup: " << error.message() << '\n';
        }
    }
};

std::vector<char> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void testProjects() {
    TemporaryDirectory temporary;
    const auto path = temporary.path / "roundtrip.oswald";
    SphericalGrid source({1000, 0.6, 123, 1});
    source.GetMetadata().description = "A project\nwith multiple layers and adaptive geometry.";
    source.GetMetadata().author = "Test author";
    source.SetSelectionWeights(std::vector<float>(1000, 0.75F));
    source.InstallSnapshot(source.GetSnapshot()->RefineSelectedRegion(5));
    const auto count = source.GetSnapshot()->GetVertices().size();
    std::vector<float> base(count);
    for (std::size_t i = 0; i < count; ++i) {
        base[i] = static_cast<float>(i) - 500.5F;
    }
    source.SetElevations(base);
    source.GetElevationLayer().SetDescription("Base relief");
    auto& offset = source.AddElevationLayer("Offset##1", "relief.map", "thermal.map");
    source.SetElevations(std::vector<float>(count, -200.25F));
    offset.SetBlendMode(LayerBlendMode::Add);
    offset.SetDescription("Signed offset");
    auto& hidden = source.AddElevationLayer("Hidden", "thermal.map", "grayscale.map");
    hidden.SetVisible(false);
    source.SetActiveLayer("Offset##1");
    ProjectFile::Save(path, source);
    check(source.GetMetadata().savedAt.size() == 20 &&
          source.GetMetadata().savedAt.back() == 'Z',
          "Successful save did not set a UTC date.");
    auto loaded = ProjectFile::Load(path);
    check(loaded->GetMetadata().description == source.GetMetadata().description &&
          loaded->GetMetadata().author == source.GetMetadata().author &&
          loaded->GetMetadata().savedAt == source.GetMetadata().savedAt,
          "Metadata did not survive a save/open round trip.");
    const auto before = source.GetSnapshot();
    const auto after = loaded->GetSnapshot();
    check(before->GetTriangleIndices() == after->GetTriangleIndices() &&
          before->HasSelection() == after->HasSelection() &&
          before->GetVertices().size() == after->GetVertices().size() &&
          after->GetOptions().seed == 123 &&
          after->GetOptions().pointCount == 1000 &&
          after->GetOptions().poissonVariability == 0.6 &&
          after->GetOptions().relaxationIterations == 1,
          "Project geometry or generation options changed on load.");
    for (std::size_t i = 0; i < count; ++i) {
        const auto& a = before->GetVertices()[i];
        const auto& b = after->GetVertices()[i];
        check(a.position == b.position && a.elevation == b.elevation &&
              a.selectionWeight == b.selectionWeight,
              "Project vertices or selection changed on load.");
        check(after->FindNearest(glm::dvec3(b.position)) == i,
              "Loaded spatial index does not match the saved point locations.");
    }
    check(loaded->GetLayers().size() == 3 &&
          loaded->GetActiveLayer().GetName() == "Offset##1",
          "Layer order or active layer was not restored.");
    for (std::size_t i = 0; i < source.GetLayers().size(); ++i) {
        const auto& a = *source.GetLayers()[i];
        const auto& b = *loaded->GetLayers()[i];
        check(a.GetName() == b.GetName() && a.GetDescription() == b.GetDescription() &&
              a.GetBlendMode() == b.GetBlendMode() && a.IsVisible() == b.IsVisible() &&
              a.GetColorMap() == b.GetColorMap() &&
              a.GetSelectionColorMap() == b.GetSelectionColorMap() &&
              std::equal(a.GetValues().begin(), a.GetValues().end(), b.GetValues().begin()),
              "Layer data or properties changed on load.");
    }
    const auto reducedBefore = before->CoarsenSelectedRegion(65534, 3);
    const auto reducedAfter = after->CoarsenSelectedRegion(65534, 3);
    check(reducedBefore && reducedAfter &&
          reducedBefore->GetTriangleIndices() == reducedAfter->GetTriangleIndices(),
          "Adaptive refinement provenance did not survive save/open.");
    SphericalGrid target({1000, 0.0, 11});
    const auto oldSnapshot = target.GetSnapshot();
    ProjectFile::Install(target, *loaded);
    check(target.GetSnapshot() == after && loaded->GetSnapshot() == oldSnapshot &&
          oldSnapshot->FindNearest(glm::dvec3(oldSnapshot->GetVertices()[10].position)) == 10,
          "Installing a project invalidated old readers or failed to replace the model.");

    source.DeactivateActiveLayer();
    ProjectFile::Save(path, source);
    auto inactive = ProjectFile::Load(path);
    check(!inactive->HasActiveLayer(), "Saving restored an unwanted active layer.");
    const auto validBytes = readBytes(path);
    const auto oldDate = source.GetMetadata().savedAt;
    source.GetMetadata().description.resize(1'048'577, 'x');
    expectFailure([&] { ProjectFile::Save(path, source); });
    check(readBytes(path) == validBytes && source.GetMetadata().savedAt == oldDate,
          "Failed save overwrote the project or changed its date.");
    source.GetMetadata().description = "Restored";
    expectFailure([&] { ProjectFile::Save(temporary.path / "missing" / "bad.oswald", source); });
    check(source.GetMetadata().savedAt == oldDate, "Failed output changed metadata date.");

    const auto corrupt = temporary.path / "corrupt.oswald";
    const auto reject = [&](const std::vector<char>& bytes) {
        std::ofstream output(corrupt, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        expectFailure([&] { ProjectFile::Load(corrupt); });
        check(target.GetSnapshot() == after, "Rejected file changed the current project.");
    };
    for (const std::size_t size : {0U, 7U, 12U, 80U}) {
        reject(std::vector<char>(validBytes.begin(), validBytes.begin() + size));
    }
    reject(std::vector<char>(validBytes.begin(), validBytes.end() - 1));
    auto bytes = validBytes;
    bytes[0] = 'X';
    reject(bytes);
    bytes = validBytes;
    bytes[8] = 2;
    reject(bytes);
    bytes = validBytes;
    bytes.push_back('X');
    reject(bytes);
    std::size_t cursor = 12;
    for (int i = 0; i < 3; ++i) {
        std::uint32_t length = 0;
        for (int byte = 0; byte < 4; ++byte) {
            length |= std::uint32_t(static_cast<unsigned char>(validBytes[cursor + byte])) << (8 * byte);
        }
        cursor += 4 + length;
    }
    const auto mutateU32 = [&](std::size_t position, std::uint32_t value) {
        bytes = validBytes;
        for (int byte = 0; byte < 4; ++byte) {
            bytes[position + byte] = static_cast<char>((value >> (byte * 8)) & 255);
        }
        reject(bytes);
    };
    mutateU32(cursor + 20, 1'000'001);
    mutateU32(cursor + 32, 0x7fc00000); // Non-finite first coordinate.
    const auto firstIndex = cursor + 32 + count * 24;
    mutateU32(firstIndex, static_cast<std::uint32_t>(count));
    const auto layersOffset = firstIndex + before->GetTriangleIndices().size() * 4;
    mutateU32(layersOffset + 4, 999);
    mutateU32(layersOffset + 8, 99);
    expectFailure([&] { ProjectFile::Load(temporary.path / "missing.oswald"); });
    for (const auto& entry : std::filesystem::directory_iterator(temporary.path)) {
        check(!entry.is_directory(), "Project save leaked a temporary output directory.");
    }
}

}

int main() {
    try {
        testProjects();
        std::cout << "Project file tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Project file test failed: " << error.what() << '\n';
        return 1;
    }
}
