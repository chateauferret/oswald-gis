#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Raised when a script fails to compile or evaluate. Compile errors carry one
// diagnostic per parser error, with 1-based line and column numbers (0 when the
// position is unknown).
class ScriptError : public std::runtime_error {
public:
    struct Diagnostic {
        std::size_t line = 0;
        std::size_t column = 0;
        std::string message;
    };

    explicit ScriptError(const std::string& message, std::vector<Diagnostic> diagnostics = {});

    const std::vector<Diagnostic>& Diagnostics() const { return diagnostics_; }

private:
    std::vector<Diagnostic> diagnostics_;
};

class CompiledScript;

// Owns the exprtk symbol table shared by every script it compiles, together with
// the storage for engine-owned variables and the adaptors for bound C++
// functions. exprtk parses a script once into an optimized expression tree that
// is then evaluated repeatedly; there is no machine-code generation.
//
// Scripts may read and assign variables, declare locals with `var`, and use
// exprtk's control flow and maths functions; `return` statements are rejected.
// Every symbol must be defined before
// compiling a script that uses it; unknown symbols are compile errors. Symbol
// names are case-sensitive and cannot be redefined or removed.
//
// The built-in noise functions use the engine's noise seed, read at evaluation
// time, and return values in [-1, 1]:
//   simplex(x, y, z)                                     3D simplex noise
//   valuenoise(x, y, z)                                  quintic value noise
//   fbm(x, y, z, frequency, lacunarity, persistence, octaves)
//   billow(x, y, z, frequency, lacunarity, persistence, octaves)
//   valuefbm(x, y, z, frequency, lacunarity, persistence, octaves)
//   ridged(x, y, z, frequency, lacunarity, persistence, octaves, offset, gain)
// `octaves` must be an integer in [1, 16]. The constants pi, epsilon and inf are
// predefined by exprtk.
//
// Not thread-safe: compilation and evaluation mutate shared variables, so an
// engine and its scripts must be used from one thread at a time. The engine may
// be moved; compiled scripts keep its state alive. A moved-from engine throws
// std::logic_error when used.
class ScriptEngine {
public:
    using Function0 = std::function<double()>;
    using Function1 = std::function<double(double)>;
    using Function2 = std::function<double(double, double)>;
    using Function3 = std::function<double(double, double, double)>;
    using Function4 = std::function<double(double, double, double, double)>;
    // Receives the arguments of a call with any arity in [minimumArguments, maximumArguments].
    using VariadicFunction = std::function<double(std::span<const double>)>;

    static constexpr std::uint64_t DefaultMaxLoopIterations = 1'000'000;

    ScriptEngine();
    ~ScriptEngine();
    ScriptEngine(ScriptEngine&&) noexcept;
    ScriptEngine& operator=(ScriptEngine&&) noexcept;
    ScriptEngine(const ScriptEngine&) = delete;
    ScriptEngine& operator=(const ScriptEngine&) = delete;

    // Defines a variable stored by the engine. The returned reference remains
    // valid for the engine's lifetime and may be written between evaluations.
    double& DefineVariable(std::string_view name, double initialValue = 0.0);
    // Binds caller-owned storage. It must outlive the engine and every script
    // compiled by it.
    void BindVariable(std::string_view name, double& storage);
    void DefineConstant(std::string_view name, double value);

    // Calls with constant arguments are not folded at compile time, so functions
    // may depend on state that changes between evaluations.
    void DefineFunction(std::string_view name, Function0 function);
    void DefineFunction(std::string_view name, Function1 function);
    void DefineFunction(std::string_view name, Function2 function);
    void DefineFunction(std::string_view name, Function3 function);
    void DefineFunction(std::string_view name, Function4 function);
    void DefineFunction(std::string_view name, std::size_t minimumArguments,
                        std::size_t maximumArguments, VariadicFunction function);

    bool HasSymbol(std::string_view name) const;
    // Variable and constant access by name; throws std::invalid_argument for
    // unknown names, and when assigning to a constant.
    void SetVariable(std::string_view name, double value);
    double GetVariable(std::string_view name) const;

    void SetNoiseSeed(std::uint64_t seed);
    std::uint64_t GetNoiseSeed() const;

    // Applies to each loop entry of scripts compiled after the change.
    void SetMaxLoopIterations(std::uint64_t iterations);
    std::uint64_t GetMaxLoopIterations() const;

    // Throws ScriptError with diagnostics when compilation fails.
    CompiledScript Compile(std::string_view source) const;
    double Evaluate(std::string_view source) const;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

class CompiledScript {
public:
    CompiledScript(CompiledScript&&) noexcept;
    CompiledScript& operator=(CompiledScript&&) noexcept;
    CompiledScript(const CompiledScript&) = delete;
    CompiledScript& operator=(const CompiledScript&) = delete;
    ~CompiledScript();

    // Returns the value of the script's final statement. Exceptions thrown by
    // bound functions, noise argument errors and loop-limit violations are
    // reported as ScriptError.
    double Evaluate() const;
    const std::string& GetSource() const;

private:
    friend class ScriptEngine;
    struct State;
    explicit CompiledScript(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};
