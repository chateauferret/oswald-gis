#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

class AtomicFile {
public:
    explicit AtomicFile(const std::filesystem::path& destination)
        : destination_(std::filesystem::absolute(destination)) {
        if (destination.empty() || destination.filename().empty() ||
            std::filesystem::is_directory(destination_)) {
            throw std::invalid_argument("Choose a destination file, not a directory.");
        }
        static std::atomic<unsigned long> sequence{0};
        for (int attempt = 0; attempt < 20; ++attempt) {
            directory_ = destination_.parent_path() /
                (".oswald-write-" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(sequence.fetch_add(1)));
            if (std::filesystem::create_directory(directory_)) {
                return;
            }
        }
        throw std::runtime_error("Cannot create a temporary output directory.");
    }

    ~AtomicFile() {
        std::error_code error;
        std::filesystem::remove(GetPath(), error);
        if (error) {
            std::cerr << "Temporary output cleanup failed: " << error.message() << '\n';
        }
        std::filesystem::remove(directory_, error);
        if (error) {
            std::cerr << "Temporary directory cleanup failed: " << error.message() << '\n';
        }
    }

    AtomicFile(const AtomicFile&) = delete;
    AtomicFile& operator=(const AtomicFile&) = delete;
    std::filesystem::path GetPath() const { return directory_ / "output"; }
    void Commit() { std::filesystem::rename(GetPath(), destination_); }

private:
    std::filesystem::path destination_;
    std::filesystem::path directory_;
};
