#include "RasterFileBrowser.hpp"

#include <chrono>
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

struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("oswald-browser-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));

    TemporaryDirectory() { std::filesystem::create_directory(path); }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        if (error) {
            std::cerr << "Browser test cleanup failed: " << error.message() << '\n';
        }
    }
};

void testFormats() {
    for (const auto* extension : {".png", ".jpg", ".jpeg", ".jpe", ".bmp", ".tga", ".psd",
                                 ".gif", ".hdr", ".pic", ".ppm", ".pgm", ".pnm", ".PNG",
                                 ".JpEg", ".tif", ".tiff", ".TIF", ".TiFf"}) {
        check(RasterFileBrowser::IsSupportedFile(std::string("raster") + extension),
              "Supported raster extension was rejected.");
    }
    for (const auto* name : {"raster.tfw", "raster.aux.xml", "raster.txt", "raster",
                             "raster.png.txt"}) {
        check(!RasterFileBrowser::IsSupportedFile(name),
              "Unsupported raster extension was accepted.");
    }
}

void testNavigation() {
    TemporaryDirectory temporary;
    const auto child = temporary.path / "z folder";
    std::filesystem::create_directory(child);
    std::filesystem::create_directory(temporary.path / "a folder.png");
    for (const auto* name : {"b raster.PNG", "a raster.ppm", "ignored.xyz", "ignored.txt"}) {
        std::ofstream(temporary.path / name) << "fixture";
    }
    RasterFileBrowser browser;
    browser.Navigate(temporary.path);
    const auto root = std::filesystem::canonical(temporary.path);
    check(browser.GetDirectory() == root, "Browser did not resolve the directory.");
    const auto& entries = browser.GetEntries();
    check(entries.size() == 4, "Browser did not filter unsupported files.");
    check(entries[0].isDirectory && entries[1].isDirectory &&
          entries[0].path.filename() == "a folder.png" &&
          entries[1].path.filename() == "z folder", "Folders are not sorted first.");
    check(!entries[2].isDirectory && !entries[3].isDirectory &&
          entries[2].path.filename() == "a raster.ppm" &&
          entries[3].path.filename() == "b raster.PNG", "Raster files are not sorted.");
    browser.Navigate(root, true);
    check(browser.GetEntries().size() == 6 &&
          browser.GetEntries()[0].isDirectory && browser.GetEntries()[1].isDirectory,
          "Project browsing did not expose non-raster files or keep folders first.");
    browser.Navigate(root);

    bool failed = false;
    try {
        browser.Navigate(root / "missing");
    } catch (const std::filesystem::filesystem_error&) {
        failed = true;
    }
    check(failed, "Missing folder did not report an error.");
    check(browser.GetDirectory() == root && browser.GetEntries().size() == 4,
          "Failed navigation changed the current listing.");
    failed = false;
    try {
        browser.Navigate(root / "a raster.ppm");
    } catch (const std::filesystem::filesystem_error&) {
        failed = true;
    }
    check(failed, "Navigating to a file did not report an error.");

    browser.Navigate(child);
    check(browser.GetEntries().empty(), "Empty folder contains browser entries.");
    browser.Navigate(browser.GetDirectory().parent_path());
    check(browser.GetDirectory() == root, "Parent navigation failed.");
    std::ofstream(root / "new.jpg") << "fixture";
    browser.Navigate(browser.GetDirectory());
    check(browser.GetEntries().size() == 5, "Refresh did not pick up a new raster.");
#ifndef _WIN32
    std::filesystem::create_directory_symlink(child, root / "linked folder");
    std::filesystem::create_symlink(root / "new.jpg", root / "linked.jpg");
    std::filesystem::create_symlink(root / "missing", root / "broken.png");
    browser.Navigate(root);
    check(browser.GetEntries().size() == 7, "Browser mishandled symlinks.");
    browser.Navigate(root / "linked folder");
    check(browser.GetDirectory() == std::filesystem::canonical(child),
          "Browser could not navigate a directory symlink.");
#endif
}

} // namespace

int main() {
    try {
        testFormats();
        testNavigation();
        std::cout << "Raster file browser tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Raster file browser test failed: " << error.what() << '\n';
        return 1;
    }
}
