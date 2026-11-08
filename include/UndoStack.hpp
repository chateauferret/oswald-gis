#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <vector>

class UndoableCommand {
public:
    virtual ~UndoableCommand() = default;
    virtual void redo() = 0;
    virtual void undo() = 0;
};

class UndoStack {
public:
    void Execute(std::unique_ptr<UndoableCommand> command) {
        if (!command) {
            throw std::invalid_argument("Cannot execute an empty command.");
        }

        // Allocate history capacity before changing application state. The redo branch is
        // retained unless the new command succeeds.
        commands_.reserve(commands_.size() + 1);
        command->redo();
        commands_.erase(commands_.begin() + static_cast<std::ptrdiff_t>(cursor_),
                        commands_.end());
        commands_.push_back(std::move(command));
        ++cursor_;
    }

    bool Undo() {
        if (!CanUndo()) {
            return false;
        }
        commands_[cursor_ - 1]->undo();
        --cursor_;
        return true;
    }

    bool Redo() {
        if (!CanRedo()) {
            return false;
        }
        commands_[cursor_]->redo();
        ++cursor_;
        return true;
    }

    bool CanUndo() const { return cursor_ > 0; }
    bool CanRedo() const { return cursor_ < commands_.size(); }
    std::size_t Size() const { return commands_.size(); }

    void Clear() {
        commands_.clear();
        cursor_ = 0;
    }

private:
    std::vector<std::unique_ptr<UndoableCommand>> commands_;
    std::size_t cursor_ = 0;
};