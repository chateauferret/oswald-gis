#pragma once

#include <string>
#include <variant>
#include <vector>

using ScriptParameterValue = std::variant<double, std::string>;

struct ScriptParameter {
    enum class Type { Float, Integer, Choice };
    enum class Control { Slider, Spin, Text };
    enum class Scale { Linear, Log };

    std::string name;
    Type type = Type::Float;
    Control control = Control::Slider;
    Scale scale = Scale::Linear;
    double minimum = 0.0;
    double maximum = 1.0;
    std::vector<std::string> choices;
    ScriptParameterValue defaultValue = 0.0;
    std::string label;
    // Optional name of a Kilometres/Miles choice control. Stored values remain kilometres.
    std::string units;

    void Validate() const;
    void ValidateValue(const ScriptParameterValue& value) const;
    bool operator==(const ScriptParameter&) const = default;
};
