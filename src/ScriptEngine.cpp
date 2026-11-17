#include "ScriptEngine.hpp"

#include "Noise.hpp"
#include "BrushTool.hpp"

// Allow string-valued parameters, but no file or console I/O.
#define exprtk_disable_caseinsensitivity
#define exprtk_disable_rtl_io
#define exprtk_disable_rtl_io_file
#define exprtk_disable_rtl_vecops
#include <exprtk.hpp>

#include <cmath>
#include <deque>
#include <unordered_set>
#include <utility>

namespace {

using SymbolTable = exprtk::symbol_table<double>;
using Expression = exprtk::expression<double>;
using Parser = exprtk::parser<double>;

class FunctionAdapter final : public exprtk::ivararg_function<double> {
public:
    FunctionAdapter(std::size_t minimumArguments, std::size_t maximumArguments,
                    ScriptEngine::VariadicFunction function)
        : function_(std::move(function)) {
        if (minimumArguments == 0) {
            exprtk::enable_zero_parameters(*this);
        }
        exprtk::set_min_num_args(*this, minimumArguments);
        exprtk::set_max_num_args(*this, maximumArguments);
    }

    double operator()(const std::vector<double>& arguments) override {
        return function_(std::span<const double>(arguments));
    }

private:
    ScriptEngine::VariadicFunction function_;
};

class LoopGuard final : public exprtk::loop_runtime_check {
public:
    LoopGuard() {
        loop_set = e_all_loops;
        max_loop_iterations = ScriptEngine::DefaultMaxLoopIterations;
    }

    void handle_runtime_violation(const violation_context&) override {
        throw ScriptError("A script loop exceeded its iteration limit.");
    }
};

int octaveCount(double value, const char* function) {
    if (!std::isfinite(value) || value < 1.0 || value > 16.0 || value != std::floor(value)) {
        throw ScriptError(std::string(function) + ": octaves must be an integer in [1, 16].");
    }
    return static_cast<int>(value);
}

noise::FractalParameters fractalArguments(std::span<const double> arguments,
                                          const char* function) {
    return {arguments[3], arguments[4], arguments[5], octaveCount(arguments[6], function)};
}

} // namespace

ScriptError::ScriptError(const std::string& message, std::vector<Diagnostic> diagnostics)
    : std::runtime_error(message), diagnostics_(std::move(diagnostics)) {}

struct ScriptEngine::Impl {
    SymbolTable symbols;
    std::deque<double> ownedVariables;
    std::deque<std::string> ownedStrings;
    std::deque<FunctionAdapter> functions;
    // exprtk's symbol_exists() does not search variadic functions.
    std::unordered_set<std::string> functionNames;
    LoopGuard loopGuard;
    std::uint64_t noiseSeed = 0;

    bool defined(const std::string& name) const {
        return functionNames.contains(name) || symbols.symbol_exists(name, false);
    }

    void checkName(const std::string& name) const {
        if (defined(name) || symbols.symbol_exists(name)) {
            throw std::invalid_argument("Script symbol '" + name +
                                        "' is reserved or already defined.");
        }
    }

    [[noreturn]] static void invalidName(const std::string& name) {
        throw std::invalid_argument("'" + name + "' is not a valid script identifier.");
    }

    void addFunction(std::string_view name, std::size_t minimumArguments,
                     std::size_t maximumArguments, VariadicFunction function) {
        const std::string key(name);
        checkName(key);
        if (!function) {
            throw std::invalid_argument("Script function '" + key + "' is empty.");
        }
        if (minimumArguments > maximumArguments) {
            throw std::invalid_argument("Script function '" + key +
                                        "' has an invalid argument range.");
        }
        auto& adapter = functions.emplace_back(minimumArguments, maximumArguments,
                                               std::move(function));
        if (!symbols.add_function(key, adapter)) {
            functions.pop_back();
            invalidName(key);
        }
        functionNames.insert(key);
    }

    exprtk::details::variable_node<double>& variable(std::string_view name) const {
        auto* node = symbols.get_variable(std::string(name));
        if (node == nullptr) {
            throw std::invalid_argument("Unknown script variable '" + std::string(name) + "'.");
        }
        return *node;
    }

    void addNoiseFunctions() {
        auto spectral = std::make_shared<PowerLawNoiseBrushTool>();
        addFunction("spectral", 6, 6, [this, spectral](std::span<const double> a) {
            if (!std::isfinite(a[5]) || a[5] != std::floor(a[5]) || a[5] < 8 || a[5] > 128 ||
                !std::isfinite(a[3]) || a[3] <= 0 || a[3] > 1e6 ||
                !std::isfinite(a[4]) || a[4] < -4 || a[4] > 8) {
                throw ScriptError("spectral() needs frequency in (0, 1e6], exponent in [-4, 8], "
                                  "and a power-of-two resolution in [8, 128].");
            }
            PowerLawNoiseSettings settings;
            settings.seed = noiseSeed;
            settings.frequency = static_cast<float>(a[3]);
            settings.spectralExponent = static_cast<float>(a[4]);
            settings.gridResolution = static_cast<int>(a[5]);
            return spectral->Sample({a[0], a[1], a[2]}, settings);
        });
        addFunction("simplex", 3, 3, [this](std::span<const double> a) {
            return noise::Simplex({a[0], a[1], a[2]}, noiseSeed);
        });
        addFunction("valuenoise", 3, 3, [this](std::span<const double> a) {
            return noise::Value({a[0], a[1], a[2]}, noiseSeed);
        });
        addFunction("fbm", 7, 7, [this](std::span<const double> a) {
            return noise::SimplexFbm({a[0], a[1], a[2]}, noiseSeed, fractalArguments(a, "fbm"));
        });
        addFunction("billow", 7, 7, [this](std::span<const double> a) {
            const double value =
                noise::SimplexFbm({a[0], a[1], a[2]}, noiseSeed, fractalArguments(a, "billow"));
            return 2.0 * std::abs(value) - 1.0;
        });
        addFunction("valuefbm", 7, 7, [this](std::span<const double> a) {
            return noise::ValueFbm({a[0], a[1], a[2]}, noiseSeed, fractalArguments(a, "valuefbm"));
        });
        addFunction("ridged", 7, 7, [this](std::span<const double> a) {
            return noise::Ridged({a[0], a[1], a[2]}, noiseSeed, fractalArguments(a, "ridged"));
        });
    }
};

struct CompiledScript::State {
    // Declared first so the engine outlives the expression's references into it.
    std::shared_ptr<ScriptEngine::Impl> engine;
    std::string source;
    Expression expression;
};

namespace {

ScriptEngine::Impl& checked(const std::shared_ptr<ScriptEngine::Impl>& impl) {
    if (!impl) {
        throw std::logic_error("ScriptEngine has been moved from.");
    }
    return *impl;
}

} // namespace

ScriptEngine::ScriptEngine() : impl_(std::make_shared<Impl>()) {
    impl_->symbols.add_constants();
    impl_->addNoiseFunctions();
}

ScriptEngine::~ScriptEngine() = default;
ScriptEngine::ScriptEngine(ScriptEngine&&) noexcept = default;
ScriptEngine& ScriptEngine::operator=(ScriptEngine&&) noexcept = default;

double& ScriptEngine::DefineVariable(std::string_view name, double initialValue) {
    auto& impl = checked(impl_);
    const std::string key(name);
    impl.checkName(key);
    auto& storage = impl.ownedVariables.emplace_back(initialValue);
    if (!impl.symbols.add_variable(key, storage)) {
        impl.ownedVariables.pop_back();
        Impl::invalidName(key);
    }
    return storage;
}

void ScriptEngine::BindVariable(std::string_view name, double& storage) {
    auto& impl = checked(impl_);
    const std::string key(name);
    impl.checkName(key);
    if (!impl.symbols.add_variable(key, storage)) {
        Impl::invalidName(key);
    }
}

std::string& ScriptEngine::DefineStringVariable(std::string_view name, std::string initialValue) {
    auto& impl = checked(impl_);
    const std::string key(name);
    impl.checkName(key);
    auto& storage = impl.ownedStrings.emplace_back(std::move(initialValue));
    if (!impl.symbols.add_stringvar(key, storage)) {
        impl.ownedStrings.pop_back();
        Impl::invalidName(key);
    }
    return storage;
}

void ScriptEngine::DefineConstant(std::string_view name, double value) {
    auto& impl = checked(impl_);
    const std::string key(name);
    impl.checkName(key);
    if (!impl.symbols.add_constant(key, value)) {
        Impl::invalidName(key);
    }
}

void ScriptEngine::DefineFunction(std::string_view name, Function0 function) {
    if (!function) throw std::invalid_argument("Script function is empty.");
    checked(impl_).addFunction(name, 0, 0, [f = std::move(function)](std::span<const double>) {
        return f();
    });
}

void ScriptEngine::DefineFunction(std::string_view name, Function1 function) {
    if (!function) throw std::invalid_argument("Script function is empty.");
    checked(impl_).addFunction(name, 1, 1, [f = std::move(function)](std::span<const double> a) {
        return f(a[0]);
    });
}

void ScriptEngine::DefineFunction(std::string_view name, Function2 function) {
    if (!function) throw std::invalid_argument("Script function is empty.");
    checked(impl_).addFunction(name, 2, 2, [f = std::move(function)](std::span<const double> a) {
        return f(a[0], a[1]);
    });
}

void ScriptEngine::DefineFunction(std::string_view name, Function3 function) {
    if (!function) throw std::invalid_argument("Script function is empty.");
    checked(impl_).addFunction(name, 3, 3, [f = std::move(function)](std::span<const double> a) {
        return f(a[0], a[1], a[2]);
    });
}

void ScriptEngine::DefineFunction(std::string_view name, Function4 function) {
    if (!function) throw std::invalid_argument("Script function is empty.");
    checked(impl_).addFunction(name, 4, 4, [f = std::move(function)](std::span<const double> a) {
        return f(a[0], a[1], a[2], a[3]);
    });
}

void ScriptEngine::DefineFunction(std::string_view name, std::size_t minimumArguments,
                                  std::size_t maximumArguments, VariadicFunction function) {
    checked(impl_).addFunction(name, minimumArguments, maximumArguments, std::move(function));
}

bool ScriptEngine::HasSymbol(std::string_view name) const {
    return checked(impl_).defined(std::string(name));
}

void ScriptEngine::SetVariable(std::string_view name, double value) {
    auto& impl = checked(impl_);
    if (impl.symbols.is_constant_node(std::string(name))) {
        throw std::invalid_argument("Script constant '" + std::string(name) +
                                    "' cannot be assigned.");
    }
    impl.variable(name).ref() = value;
}

double ScriptEngine::GetVariable(std::string_view name) const {
    return checked(impl_).variable(name).value();
}

void ScriptEngine::SetNoiseSeed(std::uint64_t seed) { checked(impl_).noiseSeed = seed; }

std::uint64_t ScriptEngine::GetNoiseSeed() const { return checked(impl_).noiseSeed; }

void ScriptEngine::SetMaxLoopIterations(std::uint64_t iterations) {
    if (iterations == 0) {
        throw std::invalid_argument("The script loop iteration limit must be positive.");
    }
    checked(impl_).loopGuard.max_loop_iterations = iterations;
}

std::uint64_t ScriptEngine::GetMaxLoopIterations() const {
    return checked(impl_).loopGuard.max_loop_iterations;
}

CompiledScript ScriptEngine::Compile(std::string_view source) const {
    auto& impl = checked(impl_);
    auto state = std::make_unique<CompiledScript::State>();
    state->engine = impl_;
    state->source = std::string(source);
    state->expression.register_symbol_table(impl.symbols);

    Parser parser;
    parser.settings().disable_control_structure(Parser::settings_t::e_ctrl_return);
    parser.register_loop_runtime_check(impl.loopGuard);
    if (!parser.compile(state->source, state->expression)) {
        std::vector<ScriptError::Diagnostic> diagnostics;
        for (std::size_t i = 0; i < parser.error_count(); ++i) {
            auto error = parser.get_error(i);
            ScriptError::Diagnostic diagnostic;
            diagnostic.message = error.diagnostic;
            if (exprtk::parser_error::update_error(error, state->source)) {
                diagnostic.line = error.line_no + 1;
                diagnostic.column = error.column_no + 1;
            }
            diagnostics.push_back(std::move(diagnostic));
        }
        std::string message = "Script compilation failed";
        if (!diagnostics.empty()) {
            const auto& first = diagnostics.front();
            if (first.line != 0) {
                message += " at line " + std::to_string(first.line) + ", column " +
                           std::to_string(first.column);
            }
            message += ": " + first.message;
        }
        throw ScriptError(message, std::move(diagnostics));
    }
    return CompiledScript(std::move(state));
}

double ScriptEngine::Evaluate(std::string_view source) const {
    return Compile(source).Evaluate();
}

CompiledScript::CompiledScript(std::unique_ptr<State> state) : state_(std::move(state)) {}
CompiledScript::CompiledScript(CompiledScript&&) noexcept = default;
CompiledScript& CompiledScript::operator=(CompiledScript&&) noexcept = default;
CompiledScript::~CompiledScript() = default;

double CompiledScript::Evaluate() const {
    if (!state_) {
        throw std::logic_error("CompiledScript has been moved from.");
    }
    try {
        return state_->expression.value();
    } catch (const ScriptError&) {
        throw;
    } catch (const std::exception& error) {
        throw ScriptError(std::string("Script evaluation failed: ") + error.what());
    }
}

const std::string& CompiledScript::GetSource() const {
    if (!state_) {
        throw std::logic_error("CompiledScript has been moved from.");
    }
    return state_->source;
}
