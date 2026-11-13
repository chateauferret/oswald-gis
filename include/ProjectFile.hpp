#pragma once

#include "SphericalGrid.hpp"

#include <filesystem>
#include <memory>

class ProjectFile {
public:
    static void Save(const std::filesystem::path& path, SphericalGrid& grid);
    static std::unique_ptr<SphericalGrid> Load(const std::filesystem::path& path);
    static void Install(SphericalGrid& target, SphericalGrid& loaded);
};
