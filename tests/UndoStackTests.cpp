#include "UndoStack.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ValueCommand final : public UndoableCommand {
public:
    ValueCommand(int& value, int next) : value_(value), previous_(value), next_(next) {}

    void redo() override { value_ = next_; }
    void undo() override { value_ = previous_; }

private:
    int& value_;
    int previous_;
    int next_;
};

class FailingCommand final : public UndoableCommand {
public:
    void redo() override { throw std::runtime_error("expected command failure"); }
    void undo() override {}
};

void testHistory() {
    int value = 0;
    UndoStack stack;
    check(!stack.Undo() && !stack.Redo(), "Empty history changed state.");
    stack.Execute(std::make_unique<ValueCommand>(value, 1));
    stack.Execute(std::make_unique<ValueCommand>(value, 2));
    check(value == 2 && stack.Size() == 2 && stack.CanUndo() && !stack.CanRedo(),
          "Commands were not applied and recorded.");
    check(stack.Undo() && value == 1, "Undo did not restore the previous state.");
    check(stack.Undo() && value == 0 && !stack.CanUndo(), "Undo did not reach the initial state.");
    check(stack.Redo() && value == 1, "Redo did not reapply the first command.");
    check(stack.Redo() && value == 2 && !stack.CanRedo(), "Redo did not reach the latest state.");

    check(stack.Undo(), "Could not prepare to replace the redo branch.");
    stack.Execute(std::make_unique<ValueCommand>(value, 3));
    check(value == 3 && stack.Size() == 2 && !stack.CanRedo(),
          "A new command did not discard the obsolete redo branch.");
    stack.Clear();
    check(stack.Size() == 0 && !stack.CanUndo() && !stack.CanRedo(),
          "Clearing history retained commands.");
}

void testFailedCommandsPreserveHistory() {
    int value = 0;
    UndoStack stack;
    stack.Execute(std::make_unique<ValueCommand>(value, 1));
    stack.Undo();
    try {
        stack.Execute(std::make_unique<FailingCommand>());
        throw std::runtime_error("Expected a failing command to throw.");
    } catch (const std::runtime_error& error) {
        check(std::string_view(error.what()) == "expected command failure",
              "Unexpected command failure.");
    }
    check(value == 0 && stack.Size() == 1 && stack.CanRedo(),
          "Failed execution changed state or destroyed redo history.");
    check(stack.Redo() && value == 1, "Redo history was unusable after failed execution.");
}

} // namespace

int main() {
    try {
        testHistory();
        testFailedCommandsPreserveHistory();
        std::cout << "Undo stack tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Undo stack test failed: " << error.what() << '\n';
        return 1;
    }
}