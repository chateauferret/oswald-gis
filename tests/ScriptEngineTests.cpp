#include "Noise.hpp"
#include "ScriptEngine.hpp"

#include <cmath>
#include <iostream>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool near(double a, double b) { return std::abs(a - b) <= 1.0e-12; }

template <typename Exception, typename Action>
bool throws(Action action) {
    try {
        action();
    } catch (const Exception&) {
        return true;
    }
    return false;
}

void testExpressionsAndConstants() {
    ScriptEngine engine;
    check(near(engine.Evaluate("1 + 2 * 3"), 7.0), "Arithmetic evaluated incorrectly.");
    check(near(engine.Evaluate("pi"), std::numbers::pi), "pi is not predefined.");
    check(near(engine.Evaluate("var a := 2; var b := a ^ 3; clamp(0, b - 3, 4)"), 4.0),
          "Locals, statements or built-in functions failed.");
    check(near(engine.Evaluate("if (2 > 1) 5; else 6;"), 5.0), "Conditionals failed.");

    engine.DefineConstant("seaLevel", 12.5);
    check(near(engine.Evaluate("seaLevel * 2"), 25.0), "Constant was not bound.");
    check(near(engine.GetVariable("seaLevel"), 12.5), "Constant was not readable.");
    check(throws<std::invalid_argument>([&] { engine.SetVariable("seaLevel", 1.0); }),
          "Constant assignment from C++ was accepted.");
    check(throws<ScriptError>([&] { engine.Compile("seaLevel := 3"); }),
          "Constant assignment in a script compiled.");
}

void testVariables() {
    ScriptEngine engine;
    double& x = engine.DefineVariable("x", 2.0);
    const auto script = engine.Compile("x * 10");
    check(near(script.Evaluate(), 20.0), "Engine variable initial value was not used.");
    x = 3.0;
    check(near(script.Evaluate(), 30.0), "Engine variable reference was not live.");
    engine.SetVariable("x", 4.0);
    check(near(x, 4.0) && near(script.Evaluate(), 40.0), "SetVariable failed.");

    for (int i = 0; i < 100; ++i) {
        engine.DefineVariable("v" + std::to_string(i));
    }
    x = 5.0;
    check(near(script.Evaluate(), 50.0), "Variable storage moved after more definitions.");

    double elevation = 100.0;
    engine.BindVariable("elevation", elevation);
    engine.Evaluate("elevation := elevation + x");
    check(near(elevation, 105.0), "Script assignment to bound storage failed.");
    check(near(engine.GetVariable("elevation"), 105.0), "GetVariable missed bound storage.");

    engine.DefineVariable("X", 1.0);
    check(near(engine.Evaluate("X + x"), 6.0), "Symbol names are not case-sensitive.");

    check(engine.HasSymbol("x") && engine.HasSymbol("simplex") && !engine.HasSymbol("y"),
          "HasSymbol reported incorrectly.");
    check(throws<std::invalid_argument>([&] { engine.GetVariable("y"); }),
          "Unknown variable read was accepted.");
    check(throws<std::invalid_argument>([&] { engine.DefineVariable("x"); }),
          "Duplicate variable was accepted.");
    check(throws<std::invalid_argument>([&] { engine.DefineVariable("sin"); }),
          "Reserved function name was accepted.");
    check(throws<std::invalid_argument>([&] { engine.DefineVariable("while"); }),
          "Reserved keyword was accepted.");
    check(throws<std::invalid_argument>([&] { engine.DefineVariable("2x"); }),
          "Invalid identifier was accepted.");
    check(throws<std::invalid_argument>([&] { engine.DefineConstant("fbm", 1.0); }),
          "Built-in noise name was redefined.");

    auto& mode = engine.DefineStringVariable("mode", "one");
    const auto choiceScript = engine.Compile("mode == 'one' ? 10 : 20");
    check(choiceScript.Evaluate() == 10.0 && engine.HasSymbol("mode"),
          "String parameter was not defined.");
    for (int i = 0; i < 100; ++i) {
        engine.DefineStringVariable("s" + std::to_string(i));
    }
    mode = "two";
    check(choiceScript.Evaluate() == 20.0, "String parameter storage moved or did not update.");
    check(throws<std::invalid_argument>([&] { engine.DefineVariable("mode"); }) &&
          throws<std::invalid_argument>([&] { engine.DefineStringVariable("x"); }) &&
          throws<std::invalid_argument>([&] { engine.DefineStringVariable("fbm"); }),
          "String variables did not enforce symbol uniqueness.");
}

void testFunctions() {
    ScriptEngine engine;
    int calls = 0;
    engine.DefineFunction("counter", ScriptEngine::Function0([&] { return ++calls; }));
    engine.DefineFunction("twice", ScriptEngine::Function1([](double a) { return 2.0 * a; }));
    engine.DefineFunction("sub", ScriptEngine::Function2([](double a, double b) { return a - b; }));
    engine.DefineFunction("mad", ScriptEngine::Function3(
        [](double a, double b, double c) { return a * b + c; }));
    engine.DefineFunction("sum4", ScriptEngine::Function4(
        [](double a, double b, double c, double d) { return a + b + c + d; }));
    engine.DefineFunction("total", 1, 6, [](std::span<const double> values) {
        double result = 0.0;
        for (const double value : values) result += value;
        return result;
    });

    check(near(engine.Evaluate("twice(sub(10, 4)) + mad(2, 3, 1) + sum4(1, 2, 3, 4)"), 29.0),
          "Fixed-arity functions evaluated incorrectly.");
    check(near(engine.Evaluate("total(1) + total(1, 2, 3, 4, 5, 6)"), 22.0),
          "Variadic function evaluated incorrectly.");

    const auto counted = engine.Compile("counter()");
    counted.Evaluate();
    counted.Evaluate();
    check(calls == 2, "Stateful function was constant-folded or not called.");

    check(throws<ScriptError>([&] { engine.Compile("sub(1)"); }), "Too few arguments compiled.");
    check(throws<ScriptError>([&] { engine.Compile("total(1,2,3,4,5,6,7)"); }),
          "Too many variadic arguments compiled.");
    check(throws<std::invalid_argument>([&] {
              engine.DefineFunction("twice", ScriptEngine::Function1([](double a) { return a; }));
          }),
          "Duplicate function was accepted.");
    check(throws<std::invalid_argument>([&] {
              engine.DefineFunction("empty", ScriptEngine::Function1());
          }),
          "Empty function was accepted.");
    check(throws<std::invalid_argument>([&] {
              engine.DefineFunction("bad", 3, 2, [](std::span<const double>) { return 0.0; });
          }),
          "Invalid variadic argument range was accepted.");

    engine.DefineFunction("fail", ScriptEngine::Function1([](double) -> double {
        throw std::domain_error("bad input");
    }));
    bool wrapped = false;
    try {
        engine.Evaluate("fail(1)");
    } catch (const ScriptError& error) {
        wrapped = std::string(error.what()).find("bad input") != std::string::npos;
    }
    check(wrapped, "Function exception was not reported as a ScriptError.");
}

void testNoiseFunctions() {
    ScriptEngine engine;
    double& x = engine.DefineVariable("x");
    double& y = engine.DefineVariable("y");
    double& z = engine.DefineVariable("z");
    const auto simplex = engine.Compile("simplex(x, y, z)");
    const auto fbm = engine.Compile("fbm(x, y, z, 4, 2, 0.5, 5)");
    const auto billow = engine.Compile("billow(x, y, z, 4, 2, 0.5, 5)");
    const auto value = engine.Compile("valuenoise(x, y, z)");
    const auto valueFbm = engine.Compile("valuefbm(x, y, z, 4, 2, 0.5, 5)");
    const auto ridged = engine.Compile("ridged(x, y, z, 4, 2, 0.5, 5)");
    const auto warpX = engine.Compile("warp_x(x, y, z, 4, 1.25, 2, 0.5, 3)");
    const auto warpY = engine.Compile("warp_y(x, y, z, 4, 1.25, 2, 0.5, 3)");
    const auto warpZ = engine.Compile("warp_z(x, y, z, 4, 1.25, 2, 0.5, 3)");
    const noise::FractalParameters fractal{4.0, 2.0, 0.5, 5};
    const noise::DomainWarpParameters warp{4.0, 1.25, 2.0, 0.5, 3};

    for (const std::uint64_t seed : {0ULL, 42ULL, 0xfedcba9876543210ULL}) {
        engine.SetNoiseSeed(seed);
        check(engine.GetNoiseSeed() == seed, "Noise seed was not stored.");
        for (int i = 0; i < 50; ++i) {
            x = std::sin(i * 0.37) * 1.7;
            y = std::cos(i * 0.21) * 2.3;
            z = i * 0.113 - 2.0;
            const glm::dvec3 p(x, y, z);
            check(simplex.Evaluate() == noise::Simplex(p, seed), "simplex() differs from brush noise.");
            check(fbm.Evaluate() == noise::SimplexFbm(p, seed, fractal), "fbm() differs.");
            check(billow.Evaluate() == 2.0 * std::abs(noise::SimplexFbm(p, seed, fractal)) - 1.0,
                  "billow() differs.");
            check(value.Evaluate() == noise::Value(p, seed),
                  "valuenoise() differs.");
            check(valueFbm.Evaluate() ==
                      noise::ValueFbm(p, seed, fractal),
                  "valuefbm() differs.");
            check(ridged.Evaluate() == noise::Ridged(p, seed, fractal),
                  "ridged() differs.");
            const auto warped = noise::DomainWarp(p, seed, warp);
            check(warpX.Evaluate() == warped.x && warpY.Evaluate() == warped.y &&
                  warpZ.Evaluate() == warped.z,
                  "Script domain-warp functions differ from the shared noise implementation.");
            for (const auto* script : {&simplex, &fbm, &billow, &value, &valueFbm, &ridged}) {
                const double result = script->Evaluate();
                check(std::isfinite(result) && result >= -1.0 && result <= 1.0,
                      "Noise function left [-1, 1].");
            }
        }
    }

    x = 0.3;
    y = 0.7;
    z = 0.1;
    engine.SetNoiseSeed(1);
    const double first = simplex.Evaluate();
    engine.SetNoiseSeed(2);
    check(simplex.Evaluate() != first, "Seed changes did not reach compiled scripts.");
    engine.SetNoiseSeed(1);
    check(simplex.Evaluate() == first, "Noise is not deterministic for a seed.");
    check(near(engine.Evaluate("simplex(0.3, 0.7, 0.1)"), first),
          "Constant noise arguments were folded with a stale seed.");

    check(throws<ScriptError>([&] { engine.Evaluate("fbm(x, y, z, 4, 2, 0.5, 2.5)"); }),
          "Fractional octave count was accepted.");
    check(throws<ScriptError>([&] { engine.Evaluate("fbm(x, y, z, 4, 2, 0.5, 17)"); }),
          "Excessive octave count was accepted.");
    check(throws<ScriptError>([&] { engine.Evaluate("fbm(x, y, z, 1/0, 2, 0.5, 3)"); }),
          "Infinite frequency was accepted.");
    check(throws<ScriptError>([&] {
              engine.Evaluate("warp_x(x, y, z, 4, 1.25, 2, 0.5, 2.5)");
          }), "Fractional warp octave count was accepted.");
    check(throws<ScriptError>([&] {
              engine.Evaluate("warp_z(x, y, z, 4, 11, 2, 0.5, 3)");
          }), "Out-of-range warp strength was accepted.");
    check(throws<ScriptError>([&] { engine.Compile("simplex(x, y)"); }),
          "Noise call with missing arguments compiled.");
    check(throws<ScriptError>([&] { engine.Compile("warp_x(x, y, z, 4, 1, 2, 0.5)"); }),
          "Domain-warp call with missing arguments compiled.");
    check(throws<ScriptError>([&] { engine.Compile("ridged(x, y, z, 4, 2, 0.5, 5, 1, 2)"); }),
          "Removed ridge offset/gain arguments were silently accepted.");
}

void testCompileErrors() {
    ScriptEngine engine;
    engine.DefineVariable("x");
    try {
        engine.Compile("var a := 1;\nx + unknownThing");
        throw std::runtime_error("Unknown symbol compiled.");
    } catch (const ScriptError& error) {
        check(!error.Diagnostics().empty(), "Compile error had no diagnostics.");
        const auto& diagnostic = error.Diagnostics().front();
        check(diagnostic.line == 2 && diagnostic.column == 5,
              "Compile error position is incorrect.");
        check(diagnostic.message.find("unknownThing") != std::string::npos,
              "Compile error does not name the unknown symbol.");
        check(std::string(error.what()).find("line 2") != std::string::npos,
              "Compile error summary omits the line.");
    }
    check(throws<ScriptError>([&] { engine.Compile("x + * 2"); }), "Syntax error compiled.");
    check(throws<ScriptError>([&] { engine.Compile("return [x];"); }),
          "Return statement compiled.");
    check(near(engine.Evaluate("'one' == 'two'"), 0.0), "String comparison failed.");
    check(throws<ScriptError>([&] { engine.Compile("println('text')"); }),
          "Console I/O was enabled.");
    check(throws<ScriptError>([&] { engine.Compile("open('file')"); }),
          "File I/O was enabled.");
}

void testLoopLimit() {
    ScriptEngine engine;
    check(engine.GetMaxLoopIterations() == ScriptEngine::DefaultMaxLoopIterations,
          "Default loop limit is wrong.");
    engine.SetMaxLoopIterations(100);
    check(near(engine.Evaluate("var s := 0; for (var i := 0; i < 100; i += 1) { s += i; }; s"),
               4950.0),
          "Loop within the limit failed.");
    check(near(engine.Evaluate(
                   "var s := 0; for (var j := 0; j < 3; j += 1) {"
                   " for (var i := 0; i < 100; i += 1) { s += 1; } }; s"),
               300.0),
          "Loop limit was not applied per loop entry.");
    const auto endlessWhile = engine.Compile("var i := 0; while (i >= 0) { i += 1; }; i");
    check(throws<ScriptError>([&] { endlessWhile.Evaluate(); }),
          "Infinite while loop was not stopped.");
    const auto endlessRepeat = engine.Compile("var k := 0; repeat k += 1; until (k < 0); k");
    check(throws<ScriptError>([&] { endlessRepeat.Evaluate(); }),
          "Infinite repeat loop was not stopped.");
    check(throws<std::invalid_argument>([&] { engine.SetMaxLoopIterations(0); }),
          "Zero loop limit was accepted.");
}

void testLifetimes() {
    std::optional<CompiledScript> script;
    {
        ScriptEngine engine;
        engine.DefineVariable("x", 3.0);
        ScriptEngine moved = std::move(engine);
        check(throws<std::logic_error>([&] { engine.Evaluate("1"); }),
              "Moved-from engine was usable.");
        script.emplace(moved.Compile("x * x"));
    }
    check(near(script->Evaluate(), 9.0), "Script did not keep its engine state alive.");
    check(script->GetSource() == "x * x", "Script source was not retained.");
}

} // namespace

int main() {
    try {
        testExpressionsAndConstants();
        testVariables();
        testFunctions();
        testNoiseFunctions();
        testCompileErrors();
        testLoopLimit();
        testLifetimes();
        std::cout << "Script engine tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Script engine test failed: " << error.what() << '\n';
        return 1;
    }
}
