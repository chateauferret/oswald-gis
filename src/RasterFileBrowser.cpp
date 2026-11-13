#include "RasterFileBrowser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <utility>

bool RasterFileBrowser::IsSupportedFile(const std::filesystem::path& path) {
    constexpr std::array<std::string_view, 15> extensions{
        ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".psd", ".gif",
        ".hdr", ".pic", ".ppm", ".pgm", ".pnm", ".jpe", ".tif", ".tiff"
    };
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) { return std::tolower(character); });
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

void RasterFileBrowser::Navigate(const std::filesystem::path& directory, bool allFiles) {
    auto absoluteDirectory = std::filesystem::canonical(directory);
    std::vector<Entry> entries;
    for (const auto& entry : std::filesystem::directory_iterator(absoluteDirectory)) {
        // A dangling symlink is not an available directory or raster.
        if (entry.is_symlink() && !std::filesystem::exists(entry.path())) {
            continue;
        }
        const bool isDirectory = entry.is_directory();
        if (isDirectory || (entry.is_regular_file() && (allFiles || IsSupportedFile(entry.path())))) {
            entries.push_back({entry.path(), isDirectory});
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
        if (left.isDirectory != right.isDirectory) {
            return left.isDirectory;
        }
        return left.path.filename() < right.path.filename();
    });
    directory_ = std::move(absoluteDirectory);
    entries_ = std::move(entries);
}
