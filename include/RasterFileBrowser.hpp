#pragma once

#include <filesystem>
#include <vector>

class RasterFileBrowser {
public:
    struct Entry {
        std::filesystem::path path;
        bool isDirectory;
    };

    static bool IsSupportedFile(const std::filesystem::path& path);
    void Navigate(const std::filesystem::path& directory, bool allFiles = false);

    const std::filesystem::path& GetDirectory() const { return directory_; }
    const std::vector<Entry>& GetEntries() const { return entries_; }

private:
    std::filesystem::path directory_;
    std::vector<Entry> entries_;
};
