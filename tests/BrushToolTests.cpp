#include "BrushTool.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<float> elevationsOf(const SphericalGridSnapshot& grid) {
    std::vector<float> elevations;
    elevations.reserve(grid.GetVertices().size());
    for (const auto& vertex : grid.GetVertices()) {
        elevations.push_back(vertex.elevation);
    }
    return elevations;
}

void testOperationsAndClamping() {
    SphericalGrid model({1000, 0.0, 17});
    const auto& grid = *model.GetSnapshot();
    ElevationBrushTool brush;
    auto values = elevationsOf(grid);
    BrushSettings settings;
    settings.radius = 20020.0;
    settings.falloffPercent = 0.0;
    settings.value = 120.0F;

    settings.operation = BrushOperation::Replace;
    brush.Apply(grid, {0, 0, 1}, values, settings, 1.0);
    check(values[0] == 120.0F, "Replace did not write the target elevation.");

    settings.operation = BrushOperation::Minimum;
    settings.value = -100.0F;
    brush.Apply(grid, {0, 0, 1}, values, settings, 1.0);
    check(values[0] == -100.0F, "Minimum did not select the lower elevation.");

    settings.operation = BrushOperation::Maximum;
    settings.value = 32767.0F;
    brush.Apply(grid, {0, 0, 1}, values, settings, 1.0);
    check(values[0] == 32767.0F, "Maximum did not select the higher elevation.");
}

void testIntensityAndGeodesicFalloff() {
    SphericalGrid model({1000, 0.0, 19});
    const auto& grid = *model.GetSnapshot();
    const auto& vertices = grid.GetVertices();
    const glm::dvec3 center(vertices[0].position);
    const glm::dvec3 neighbor(vertices[1].position);
    const double neighborAngle = std::atan2(glm::length(glm::cross(center, neighbor)),
                                             glm::dot(center, neighbor));
    const double neighborDistance = neighborAngle * 6371.0;
    ElevationBrushTool brush;
    auto values = elevationsOf(grid);
    BrushSettings settings;
    settings.operation = BrushOperation::Replace;
    settings.radius = neighborDistance * 1.5;
    settings.falloffPercent = 50.0;
    settings.intensityPercent = 50.0;
    settings.value = 100.0F;
    const auto changed = brush.Apply(grid, center, values, settings, 0.25);
    check(values[0] == 100.0F, "Intensity changed the immediate Replace operation.");
    const float expectedNeighbor = 100.0F * (2.0F / 3.0F);
    check(std::abs(values[1] - expectedNeighbor) < 1e-3F,
          "Geodesic outer falloff was not linearly tapered.");
    check(std::find(changed.begin(), changed.end(), 0) != changed.end() &&
          std::find(changed.begin(), changed.end(), 1) != changed.end(),
          "Brush did not report changed grid vertices.");

    values = elevationsOf(grid);
    settings.operation = BrushOperation::Maximum;
    settings.value = 100.0F;
    settings.intensityPercent = 0.0;
    brush.Apply(grid, center, values, settings, 0.0);
    check(values[0] == 100.0F, "Maximum did not apply its full result in one application.");
    check(std::abs(values[1] - expectedNeighbor) < 1e-3F,
          "One-shot Maximum did not respect linear falloff.");
    check(!brush.IsContinuous(), "Elevation brush should apply once per stroke.");
    check(RaiseLowerBrushTool().IsContinuous(), "Raise/Lower brush should be continuous.");
}

void testRaiseLowerBrush() {
    SphericalGrid model({1000, 0.0, 20});
    const auto& grid = *model.GetSnapshot();
    const auto center = glm::normalize(glm::dvec3(grid.GetVertices()[0].position));
    const auto neighbor = glm::normalize(glm::dvec3(grid.GetVertices()[1].position));
    const double distance = std::atan2(glm::length(glm::cross(center, neighbor)),
                                      glm::dot(center, neighbor)) * 6371.0;
    RaiseLowerBrushTool brush;
    BrushSettings settings;
    settings.radius = distance * 1.5;
    settings.falloffPercent = 50.0;
    settings.intensityPercent = 50.0;
    settings.value = std::numeric_limits<float>::quiet_NaN();
    auto values = elevationsOf(grid);
    check(settings.raiseLowerRate == 8192.0, "Default Raise/Lower speed is incorrect.");
    brush.Apply(grid, center, values, settings, 0.25);
    check(std::abs(values[0] - 1024.0F) < 1e-3F,
          "Raise speed depends on a value rather than intensity and time.");
    check(std::abs(values[1] - 1024.0F * (2.0F / 3.0F)) < 1e-2F,
          "Raise/Lower brush did not respect linear geodesic falloff.");
    settings.raiseLowerRate = 100.0;
    values = elevationsOf(grid);
    brush.Apply(grid, center, values, settings, 0.25);
    check(std::abs(values[0] - 12.5F) < 1e-4F, "Configured Raise/Lower speed was ignored.");
    settings.intensityPercent = 0.0;
    check(brush.Apply(grid, center, values, settings, 1.0).empty(),
          "Zero-intensity Raise/Lower brush changed elevations.");
    settings.intensityPercent = 100.0;
    check(brush.Apply(grid, center, values, settings, 0.0).empty(),
          "Zero elapsed time changed elevations.");

    settings.raiseLowerRate = 8192.0;
    settings.intensityPercent = 25.0;
    settings.falloffPercent = 0.0;
    for (const auto operation : {RaiseLowerOperation::Raise, RaiseLowerOperation::Lower}) {
        settings.raiseLowerOperation = operation;
        const float direction = operation == RaiseLowerOperation::Raise ? 1.0F : -1.0F;
        for (const int frames : {1, 30, 120}) {
            values = elevationsOf(grid);
            for (int frame = 0; frame < frames; ++frame) {
                brush.Apply(grid, center, values, settings, 1.0 / frames);
            }
            check(std::abs(values[0] - direction * 2048.0F) < 0.01F,
                  "Raise/Lower speed depends on frame rate.");
        }
        settings.intensityPercent = 100.0;
        values = elevationsOf(grid);
        brush.Apply(grid, center, values, settings, 4.0);
        check(values[0] == direction * 32767.0F,
              "Raise/Lower brush did not clamp at the elevation limit after four seconds.");
        check(brush.Apply(grid, center, values, settings, 1.0).empty(),
              "Raise/Lower brush changed already-clamped elevations.");
        settings.intensityPercent = 25.0;
    }
}

void testOuterRadiusProfiles() {
    SphericalGrid model({1000, 0.0, 21});
    const auto& grid = *model.GetSnapshot();
    const auto& vertices = grid.GetVertices();
    const glm::dvec3 center = glm::normalize(glm::dvec3(vertices[0].position));
    ElevationBrushTool brush;
    for (const auto unit : {BrushDistanceUnit::Kilometres, BrushDistanceUnit::Miles}) {
        const double planetRadius = unit == BrushDistanceUnit::Kilometres
                                        ? 6371.0 : 3958.7613;
        for (const double percent : {0.0, 25.0, 50.0, 100.0}) {
            BrushSettings settings;
            settings.distanceUnit = unit;
            settings.radius = planetRadius * 0.5;
            settings.falloffPercent = percent;
            settings.intensityPercent = 50.0;
            settings.value = 100.0F;
            auto values = elevationsOf(grid);
            const auto changed = brush.Apply(grid, center, values, settings, 1.0);
            std::vector<std::size_t> expectedChanged;
            bool testedCore = false;
            bool testedTaper = false;
            bool testedOutside = false;
            for (std::size_t i = 0; i < vertices.size(); ++i) {
                const auto point = glm::normalize(glm::dvec3(vertices[i].position));
                const double angle = std::atan2(glm::length(glm::cross(center, point)),
                                                glm::dot(center, point));
                const double fraction = angle * planetRadius / settings.radius;
                double weight = 0.0;
                if (fraction > 1.0) {
                    testedOutside = true;
                } else if (fraction <= 1.0 - percent / 100.0) {
                    weight = 1.0;
                    testedCore = true;
                } else {
                    weight = (1.0 - fraction) / (percent / 100.0);
                    testedTaper = true;
                }
                const float expected = static_cast<float>(100.0 * weight);
                check(std::isfinite(values[i]) && std::abs(values[i] - expected) < 1e-3F,
                      "Brush did not match the fixed-outer-radius profile.");
                if (expected > 0.0F) {
                    expectedChanged.push_back(i);
                }
            }
            check(changed == expectedChanged, "Brush changed vertices outside its profile.");
            check(testedCore && testedOutside, "Profile test missed the core or outer boundary.");
            check(testedTaper == (percent > 0.0),
                  "Only the cylinder profile should have no tapered region.");
        }
    }
}

void testSimplexNoiseBrush() {
    SphericalGrid model({1000, 0.0, 29});
    const auto& grid = *model.GetSnapshot();
    SimplexNoiseBrushTool brush;
    BrushSettings settings;
    settings.radius = 20020.0;
    settings.falloffPercent = 0.0;
    auto values = elevationsOf(grid);
    auto changed = brush.Apply(grid, {0, 0, 1}, values, settings, 0.0);
    check(changed.size() == values.size(), "Simplex noise did not cover the brush area.");
    check(std::any_of(values.begin(), values.end(),
                      [](float value) { return std::abs(value) > 1.0F; }),
          "Simplex noise produced no variation.");
    auto repeatedValues = elevationsOf(grid);
    const auto repeat = brush.Apply(grid, {0, 0, 1}, repeatedValues, settings, 0.0);
    check(repeat == changed, "Simplex noise output changed between identical applications.");

    brush.GetSettings().billowed = true;
    auto billowed = elevationsOf(grid);
    brush.Apply(grid, {0, 0, 1}, billowed, settings, 0.0);
    check(std::all_of(billowed.begin(), billowed.end(), [](float value) {
              return value >= -32767.0F && value <= 32767.0F;
          }), "Billowed noise exceeded the elevation range.");
    check(billowed != values, "Billowed noise did not alter the noise profile.");

    brush.GetSettings().octaves = 0;
    bool invalidRejected = false;
    try {
        brush.Apply(grid, {0, 0, 1}, values, settings, 0.0);
    } catch (const std::invalid_argument&) {
        invalidRejected = true;
    }
    check(invalidRejected, "Simplex noise accepted an invalid octave count.");
}

void testSimplexNoiseSeed() {
    SphericalGrid model({1000, 0.0, 39});
    const auto& grid = *model.GetSnapshot();
    SimplexNoiseBrushTool brush;
    BrushSettings settings;
    settings.radius = 20020.0;
    settings.falloffPercent = 0.0;

    std::vector<float> defaultSeedValues(grid.GetVertices().size(), 0.0F);
    brush.Apply(grid, {0, 0, 1}, defaultSeedValues, settings, 0.0);
    auto repeatedValues = std::vector<float>(grid.GetVertices().size(), 0.0F);
    brush.Apply(grid, {0, 0, 1}, repeatedValues, settings, 0.0);
    check(repeatedValues == defaultSeedValues,
          "Simplex noise changed between applications with the same seed.");

    brush.GetSettings().seed = std::numeric_limits<std::uint64_t>::max();
    auto maxSeedValues = std::vector<float>(grid.GetVertices().size(), 0.0F);
    brush.Apply(grid, {0, 0, 1}, maxSeedValues, settings, 0.0);
    check(maxSeedValues != defaultSeedValues,
          "Changing the full-width noise seed did not change the noise.");
}

void testSimplexNoiseOperations() {
    SphericalGrid model({1000, 0.0, 43});
    const auto& grid = *model.GetSnapshot();
    SimplexNoiseBrushTool brush;
    BrushSettings settings;
    settings.radius = 20020.0;
    settings.falloffPercent = 0.0;
    const std::vector<float> initial(grid.GetVertices().size(), 0.0F);
    auto noiseValues = initial;
    brush.Apply(grid, {0, 0, 1}, noiseValues, settings, 0.0);

    constexpr float startingValue = 3000.0F;
    for (const auto operation : {
             SimplexNoiseSettings::Operation::Replace,
             SimplexNoiseSettings::Operation::Add,
             SimplexNoiseSettings::Operation::Subtract,
             SimplexNoiseSettings::Operation::Minimum,
             SimplexNoiseSettings::Operation::Maximum}) {
        brush.GetSettings().operation = operation;
        std::vector<float> values(grid.GetVertices().size(), startingValue);
        brush.Apply(grid, {0, 0, 1}, values, settings, 0.0);
        for (std::size_t i = 0; i < values.size(); ++i) {
            float expected = noiseValues[i];
            switch (operation) {
            case SimplexNoiseSettings::Operation::Replace:
                break;
            case SimplexNoiseSettings::Operation::Add:
                expected = startingValue + noiseValues[i];
                break;
            case SimplexNoiseSettings::Operation::Subtract:
                expected = startingValue - noiseValues[i];
                break;
            case SimplexNoiseSettings::Operation::Minimum:
                expected = std::min(startingValue, noiseValues[i]);
                break;
            case SimplexNoiseSettings::Operation::Maximum:
                expected = std::max(startingValue, noiseValues[i]);
                break;
            }
            expected = std::clamp(expected, -32767.0F, 32767.0F);
            check(std::abs(values[i] - expected) < 0.01F,
                  "Simplex noise operation produced an unexpected elevation.");
        }
    }

    brush.GetSettings().operation = static_cast<SimplexNoiseSettings::Operation>(99);
    auto invalidValues = initial;
    bool invalidRejected = false;
    try {
        brush.Apply(grid, {0, 0, 1}, invalidValues, settings, 0.0);
    } catch (const std::invalid_argument&) {
        invalidRejected = true;
    }
    check(invalidRejected, "Simplex noise accepted an invalid operation.");
}

void testSimplexNoiseValueRange() {
    SphericalGrid model({1000, 0.0, 47});
    const auto& grid = *model.GetSnapshot();
    SimplexNoiseBrushTool brush;
    BrushSettings settings;
    settings.radius = 20020.0;
    settings.falloffPercent = 0.0;

    std::vector<float> defaultRange(grid.GetVertices().size(), 0.0F);
    brush.Apply(grid, {0, 0, 1}, defaultRange, settings, 0.0);
    brush.GetSettings().rangeBetween = 100.0F;
    brush.GetSettings().rangeAnd = -300.0F;
    std::vector<float> scaled(grid.GetVertices().size(), 0.0F);
    brush.Apply(grid, {0, 0, 1}, scaled, settings, 0.0);
    for (std::size_t i = 0; i < scaled.size(); ++i) {
        const float normalized = (defaultRange[i] / 32767.0F + 1.0F) * 0.5F;
        const float expected = -300.0F + normalized * 400.0F;
        check(std::abs(scaled[i] - expected) < 0.01F,
              "Simplex noise did not scale into the ordered Between/And range.");
    }

    brush.GetSettings().rangeBetween = 250.0F;
    brush.GetSettings().rangeAnd = 250.0F;
    std::vector<float> constantRange(grid.GetVertices().size(), 0.0F);
    brush.Apply(grid, {0, 0, 1}, constantRange, settings, 0.0);
    check(std::all_of(constantRange.begin(), constantRange.end(),
                      [](float value) { return value == 250.0F; }),
          "Equal Simplex noise range bounds did not produce a constant target.");

    brush.GetSettings().rangeBetween = -32768.0F;
    bool invalidRangeRejected = false;
    try {
        brush.Apply(grid, {0, 0, 1}, constantRange, settings, 0.0);
    } catch (const std::invalid_argument&) {
        invalidRangeRejected = true;
    }
    check(invalidRangeRejected, "Simplex noise accepted an out-of-range noise bound.");
}

void testPhase18NoiseTools() {
    SphericalGrid model({1000, 0.0, 53});
    const auto& grid = *model.GetSnapshot();
    BrushSettings brushSettings;
    brushSettings.radius = 20020.0;
    brushSettings.falloffPercent = 0.0;

    ValueNoiseBrushTool value;
    auto& valueSettings = value.GetSettings();
    valueSettings.seed = 13;
    valueSettings.frequency = 5.0F;
    valueSettings.octaves = 3;
    valueSettings.rangeBetween = -1200.0F;
    valueSettings.rangeAnd = 1800.0F;
    std::vector<float> valueField(grid.GetVertices().size(), 0.0F);
    const auto valueChanged = value.Apply(grid, {0, 0, 1}, valueField, brushSettings, 0.0);
    check(valueChanged.size() == valueField.size() &&
          *std::min_element(valueField.begin(), valueField.end()) <
              *std::max_element(valueField.begin(), valueField.end()),
          "Value noise did not generate a varying field across the full brush.");
    auto repeatedValue = std::vector<float>(grid.GetVertices().size(), 0.0F);
    value.Apply(grid, {0, 0, 1}, repeatedValue, brushSettings, 0.0);
    check(repeatedValue == valueField, "Value noise was not deterministic for a fixed seed.");
    valueSettings.seed++;
    auto changedSeed = std::vector<float>(grid.GetVertices().size(), 0.0F);
    value.Apply(grid, {0, 0, 1}, changedSeed, brushSettings, 0.0);
    check(changedSeed != valueField, "Value noise ignored its seed.");
    valueSettings.amplitude = 0.0F;
    auto zeroAmplitude = std::vector<float>(grid.GetVertices().size(), 0.0F);
        value.Apply(grid, {0, 0, 1}, zeroAmplitude, brushSettings, 0.0);
        check(std::all_of(zeroAmplitude.begin(), zeroAmplitude.end(),
                    [](float height) { return height == 300.0F; }),
            "Zero value-noise amplitude did not map to the output range midpoint.");
    valueSettings.amplitude = 1.0F;
    valueSettings.interpolation = static_cast<ValueNoiseSettings::Interpolation>(99);
    bool invalidInterpolationRejected = false;
    try {
        value.Apply(grid, {0, 0, 1}, zeroAmplitude, brushSettings, 0.0);
    } catch (const std::invalid_argument&) {
        invalidInterpolationRejected = true;
    }
    check(invalidInterpolationRejected, "Value noise accepted an invalid interpolation mode.");

    RidgeNoiseBrushTool ridge;
    auto& ridgeSettings = ridge.GetSettings();
    ridgeSettings.seed = 17;
    ridgeSettings.frequency = 6.0F;
    ridgeSettings.octaves = 4;
    ridgeSettings.persistence = 0.55F;
    ridgeSettings.ridgeOffset = 1.1F;
    ridgeSettings.gainExponent = 1.4F;
    std::vector<float> ridgeField(grid.GetVertices().size(), 0.0F);
    ridge.Apply(grid, {0, 0, 1}, ridgeField, brushSettings, 0.0);
    check(*std::min_element(ridgeField.begin(), ridgeField.end()) <
              *std::max_element(ridgeField.begin(), ridgeField.end()) &&
          std::all_of(ridgeField.begin(), ridgeField.end(), [](float height) {
              return height >= -32767.0F && height <= 32767.0F;
          }), "Ridged multifractal noise did not produce bounded relief.");
    auto repeatedRidge = std::vector<float>(grid.GetVertices().size(), 0.0F);
    ridge.Apply(grid, {0, 0, 1}, repeatedRidge, brushSettings, 0.0);
    check(repeatedRidge == ridgeField, "Ridge noise was not deterministic for a fixed seed.");
    ridgeSettings.gainExponent = 0.0F;
    bool invalidGainRejected = false;
    try {
        ridge.Apply(grid, {0, 0, 1}, repeatedRidge, brushSettings, 0.0);
    } catch (const std::invalid_argument&) {
        invalidGainRejected = true;
    }
    check(invalidGainRejected, "Ridge noise accepted an invalid gain exponent.");

    PowerLawNoiseBrushTool spectral;
    auto& spectralSettings = spectral.GetSettings();
    spectralSettings.seed = 29;
    spectralSettings.gridResolution = 16;
    spectralSettings.frequency = 3.0F;
    spectralSettings.spectralExponent = 2.0F;
    std::vector<float> spectralField(grid.GetVertices().size(), 0.0F);
    spectral.Apply(grid, {0, 0, 1}, spectralField, brushSettings, 0.0);
    check(*std::min_element(spectralField.begin(), spectralField.end()) <
              *std::max_element(spectralField.begin(), spectralField.end()) &&
          std::all_of(spectralField.begin(), spectralField.end(), [](float height) {
              return std::isfinite(height) && height >= -32767.0F && height <= 32767.0F;
          }), "Power-law spectral noise did not generate bounded spatial variation.");
    auto repeatedSpectral = std::vector<float>(grid.GetVertices().size(), 0.0F);
    spectral.Apply(grid, {0, 0, 1}, repeatedSpectral, brushSettings, 0.0);
    check(repeatedSpectral == spectralField,
          "Power-law spectral noise was not deterministic for a fixed seed.");
    spectralSettings.spectralExponent = 4.0F;
    auto changedSpectrum = std::vector<float>(grid.GetVertices().size(), 0.0F);
    spectral.Apply(grid, {0, 0, 1}, changedSpectrum, brushSettings, 0.0);
    check(changedSpectrum != spectralField,
          "Changing the spectral exponent did not alter the generated spectrum.");
    spectralSettings.gridResolution = 12;
    bool invalidResolutionRejected = false;
    try {
        spectral.Apply(grid, {0, 0, 1}, changedSpectrum, brushSettings, 0.0);
    } catch (const std::invalid_argument&) {
        invalidResolutionRejected = true;
    }
    check(invalidResolutionRejected, "Spectral noise accepted a non-power-of-two grid.");

    const auto checkModifiers = [&](BrushTool& tool, NoiseSettings& parameters) {
        std::vector<float> unmodified(grid.GetVertices().size(), 0.0F);
        tool.Apply(grid, {0, 0, 1}, unmodified, brushSettings, 0.0);
        parameters.domainWarpEnabled = true;
        parameters.warpFrequency = 4.0F;
        parameters.warpStrength = 5.0F;
        parameters.warpOctaves = 1;
        std::vector<float> warped(grid.GetVertices().size(), 0.0F);
        tool.Apply(grid, {0, 0, 1}, warped, brushSettings, 0.0);
        double warpDifference = 0.0;
        for (std::size_t i = 0; i < warped.size(); ++i) {
            warpDifference += std::abs(warped[i] - unmodified[i]);
        }
        warpDifference /= warped.size();
        check(warpDifference > 100.0,
              (std::string("Domain warp change was too small: ") +
               std::to_string(warpDifference)).c_str());

          parameters.warpOctaves = 5;
          std::vector<float> multiOctaveWarped(grid.GetVertices().size(), 0.0F);
          tool.Apply(grid, {0, 0, 1}, multiOctaveWarped, brushSettings, 0.0);
          check(multiOctaveWarped != warped,
              "Increasing domain-warp octaves did not change the terrain pattern.");

        std::vector<float> repeat(grid.GetVertices().size(), 0.0F);
        tool.Apply(grid, {0, 0, 1}, repeat, brushSettings, 0.0);
          check(repeat == multiOctaveWarped, "Domain-warped noise was not deterministic.");
    };
    SimplexNoiseBrushTool simplex;
    ValueNoiseBrushTool modifiedValue;
    RidgeNoiseBrushTool modifiedRidge;
    PowerLawNoiseBrushTool modifiedSpectral;
    modifiedSpectral.GetSettings().gridResolution = 8;
    checkModifiers(simplex, simplex.GetSettings());
    checkModifiers(modifiedValue, modifiedValue.GetSettings());
    checkModifiers(modifiedRidge, modifiedRidge.GetSettings());
    checkModifiers(modifiedSpectral, modifiedSpectral.GetSettings());
}

void testFullRangeBlendStaysInRange() {
    SphericalGrid model({2000, 0.0, 29});
    const auto& grid = *model.GetSnapshot();
    BrushSettings brushSettings;
    brushSettings.radius = 20020.0;
    brushSettings.falloffPercent = 100.0;
    NoiseSettings parameters;
    parameters.rangeBetween = -32767.0F;
    parameters.rangeAnd = 32767.0F;
    for (const double sample : {0.0, 1.0, 0.999999, 1e-7}) {
        for (const float start : {-32767.0F, 32767.0F, -32766.99F, 1.0e-3F}) {
            std::vector<float> elevations(grid.GetVertices().size(), start);
            ApplyNoiseSampler(grid, {0, 0, 1}, elevations, brushSettings, 0.0, parameters,
                              [sample](const NoiseSample&) { return sample; });
            check(std::all_of(elevations.begin(), elevations.end(), [](float height) {
                      return std::isfinite(height) && height >= -32767.0F &&
                             height <= 32767.0F;
                  }), "Full-range noise blending escaped the valid elevation range.");
            SphericalGrid target({2000, 0.0, 29});
            target.SetElevations(elevations);
        }
    }
}

void testGaussianBlurBrush() {
    SphericalGrid model({1000, 0.0, 31});
    const auto& grid = *model.GetSnapshot();
    GaussianBlurBrushTool brush;
    BrushSettings settings;
    settings.radius = 20020.0;
    settings.falloffPercent = 0.0;
    brush.GetSettings().range = 20000.0;
    brush.GetSettings().sigma = 5000.0F;
    auto values = elevationsOf(grid);
    values[0] = 1000.0F;
    const auto changed = brush.Apply(grid, {0, 0, 1}, values, settings, 0.0);
    check(!changed.empty() && values[0] < 1000.0F && values[0] > 0.0F,
          "Gaussian blur did not reduce the isolated peak.");
    check(std::any_of(values.begin() + 1, values.end(),
                      [](float value) { return value > 0.0F; }),
          "Gaussian blur did not spread the isolated peak.");

    auto constant = elevationsOf(grid);
    std::fill(constant.begin(), constant.end(), 123.5F);
    check(brush.Apply(grid, {0, 0, 1}, constant, settings, 0.0).empty(),
          "Gaussian blur changed a constant field.");
    check(std::all_of(constant.begin(), constant.end(),
                      [](float value) { return value == 123.5F; }),
          "Gaussian blur did not preserve a constant field.");
}

void testClampBrush() {
    SphericalGrid model({1000, 0.0, 37});
    const auto& grid = *model.GetSnapshot();
    ClampBrushTool brush;
    BrushSettings settings;
    settings.radius = 20020.0;
    settings.falloffPercent = 0.0;
    auto values = elevationsOf(grid);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = i % 2 == 0 ? 500.0F : -500.0F;
    }
    brush.GetSettings().minimum = -100.0F;
    brush.GetSettings().maximum = 100.0F;
    const auto changed = brush.Apply(grid, {0, 0, 1}, values, settings, 0.0);
    check(changed.size() == values.size(), "Clamp did not cover the brush area.");
    check(std::all_of(values.begin(), values.end(), [](float value) {
              return value == -100.0F || value == 100.0F;
          }), "Clamp did not replace values outside its bounds.");

    brush.GetSettings().minimum = 200.0F;
    bool invalidRejected = false;
    try {
        brush.Apply(grid, {0, 0, 1}, values, settings, 0.0);
    } catch (const std::invalid_argument&) {
        invalidRejected = true;
    }
    check(invalidRejected, "Clamp accepted a minimum greater than its maximum.");
}

void testAdditionalBrushSelectionWeights() {
    SphericalGrid model({1000, 0.0, 41});
    const auto& base = *model.GetSnapshot();
    const std::size_t selectedIndex = 0;
    std::vector<float> selection(base.GetVertices().size(), 0.0F);
    selection[selectedIndex] = 0.5F;
    std::vector<float> source(base.GetVertices().size(), 500.0F);
    SphericalGridSnapshot selectedGrid(base, source, selection, true);
    BrushSettings settings;
    settings.radius = 20020.0;
    settings.falloffPercent = 0.0;

    ClampBrushTool clamp;
    clamp.GetSettings().maximum = 100.0F;
    auto clamped = source;
    clamp.Apply(selectedGrid, {0, 0, 1}, clamped, settings, 0.0);
    check(clamped[selectedIndex] == 300.0F &&
          std::all_of(clamped.begin() + 1, clamped.end(),
                      [](float value) { return value == 500.0F; }),
          "Clamp did not multiply its effect by the per-vertex selection weight.");

    SimplexNoiseBrushTool noise;
    auto noised = source;
    noise.Apply(selectedGrid, {0, 0, 1}, noised, settings, 0.0);
    check(noised[selectedIndex] != source[selectedIndex] &&
          std::all_of(noised.begin() + 1, noised.end(),
                      [](float value) { return value == 500.0F; }),
          "Simplex noise did not multiply its effect by the per-vertex selection weight.");

    std::fill(source.begin(), source.end(), 0.0F);
    source[selectedIndex] = 1000.0F;
    GaussianBlurBrushTool blur;
    blur.GetSettings().range = 20000.0;
    blur.GetSettings().sigma = 5000.0F;
    auto blurred = source;
    blur.Apply(selectedGrid, {0, 0, 1}, blurred, settings, 0.0);
    check(blurred[selectedIndex] > 500.0F && blurred[selectedIndex] < 1000.0F &&
          std::all_of(blurred.begin() + 1, blurred.end(),
                      [](float value) { return value == 0.0F; }),
          "Gaussian blur did not multiply its effect by the per-vertex selection weight.");
}

void testValidation() {
    SphericalGrid model({1000, 0.0, 23});
    const auto& grid = *model.GetSnapshot();
    ElevationBrushTool brush;
    auto values = elevationsOf(grid);
    BrushSettings settings;
    auto invalid = [&](const char* message) {
        try {
            brush.Apply(grid, {0, 0, 1}, values, settings, 1.0);
        } catch (const std::invalid_argument&) {
            return;
        }
        throw std::runtime_error(message);
    };
    settings.radius = 0.0;
    invalid("Zero brush radius was accepted.");
    settings.radius = 1.0;
    settings.falloffPercent = 101.0;
    invalid("Out-of-range falloff was accepted.");
    settings.falloffPercent = 0.0;
    settings.intensityPercent = -1.0;
    brush.Apply(grid, {0, 0, 1}, values, settings, 1.0);
    settings.intensityPercent = 100.0;
    settings.value = 32768.0F;
    invalid("Out-of-range elevation value was accepted.");
    settings.value = 0.0F;
    values.pop_back();
    invalid("A mismatched elevation buffer was accepted.");

    values = elevationsOf(grid);
    RaiseLowerBrushTool raiseLower;
    settings.intensityPercent = -1.0;
    bool intensityRejected = false;
    try {
        raiseLower.Apply(grid, {0, 0, 1}, values, settings, 1.0);
    } catch (const std::invalid_argument&) {
        intensityRejected = true;
    }
    check(intensityRejected, "Negative Raise/Lower intensity was accepted.");
    settings.intensityPercent = 100.0;
    for (const double rate : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN()}) {
        settings.raiseLowerRate = rate;
        bool rejected = false;
        try {
            raiseLower.Apply(grid, {0, 0, 1}, values, settings, 1.0);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "Invalid Raise/Lower speed was accepted.");
    }
}

class TestBrush final : public BrushTool {
public:
    std::string_view GetName() const override { return "Test Brush"; }
    bool IsContinuous() const override { return false; }
    std::vector<std::size_t> Apply(const SphericalGridSnapshot&, glm::dvec3,
                                   std::vector<float>&, const BrushSettings&,
                                   double) const override {
        return {};
    }
};

void testToolManager() {
    BrushToolManager manager;
    check(manager.GetSelectedTool().GetName() == "Elevation Brush",
          "Manager did not select its default elevation brush.");
        check(manager.GetTools().size() == 8, "Manager did not register all built-in brush tools.");
        check(manager.SelectTool("Value Noise") && manager.SelectTool("Ridge Noise") &&
            manager.SelectTool("Power-Law Spectral Noise"),
            "Manager could not select the Phase 18 noise tools.");
    manager.GetSettings().value = 1234.0F;
    manager.GetSettings().operation = BrushOperation::Maximum;
    check(manager.SelectTool("Raise/Lower Brush") &&
          dynamic_cast<RaiseLowerBrushTool*>(&manager.GetSelectedTool()) != nullptr,
          "Manager did not select the Raise/Lower brush.");
    manager.GetSettings().raiseLowerOperation = RaiseLowerOperation::Lower;
    check(manager.SelectTool("Elevation Brush") &&
          manager.GetSettings().value == 1234.0F &&
          manager.GetSettings().operation == BrushOperation::Maximum &&
          manager.GetSettings().raiseLowerOperation == RaiseLowerOperation::Lower,
          "Switching brushes did not preserve their settings.");
    manager.RegisterTool(std::make_unique<TestBrush>());
    check(manager.SelectTool("Test Brush") && manager.GetSelectedTool().GetName() == "Test Brush",
          "Manager could not select a registered tool.");
    check(!manager.SelectTool("Missing Brush"), "Manager selected an unregistered tool.");
    bool duplicateRejected = false;
    try {
        manager.RegisterTool(std::make_unique<TestBrush>());
    } catch (const std::invalid_argument&) {
        duplicateRejected = true;
    }
    check(duplicateRejected, "Manager accepted duplicate tool names.");
}

} // namespace

int main() {
    try {
        testOperationsAndClamping();
        testIntensityAndGeodesicFalloff();
        testRaiseLowerBrush();
        testOuterRadiusProfiles();
        testSimplexNoiseBrush();
        testSimplexNoiseSeed();
        testSimplexNoiseOperations();
        testSimplexNoiseValueRange();
        testPhase18NoiseTools();
        testFullRangeBlendStaysInRange();
        testGaussianBlurBrush();
        testClampBrush();
        testAdditionalBrushSelectionWeights();
        testValidation();
        testToolManager();
        std::cout << "Brush tool tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Brush tool test failed: " << error.what() << '\n';
        return 1;
    }
}
