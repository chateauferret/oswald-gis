#pragma once

#include <cstddef>
#include <cstdint>
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
        ++revision_;
    }

    bool Undo() {
        if (!CanUndo()) {
            return false;
        }
        commands_[cursor_ - 1]->undo();
        --cursor_;
        ++revision_;
        return true;
    }

    bool Redo() {
        if (!CanRedo()) {
            return false;
        }
        commands_[cursor_]->redo();
        ++cursor_;
        ++revision_;
        return true;
    }

    bool CanUndo() const { return cursor_ > 0; }
    bool CanRedo() const { return cursor_ < commands_.size(); }
    std::size_t Size() const { return commands_.size(); }
    std::size_t UndoCount() const { return cursor_; }
    std::uint64_t Revision() const { return revision_; }

    void Clear() {
        commands_.clear();
        cursor_ = 0;
        // Callers also clear history when replacing the document or grid.
        ++revision_;
    }

private:
    std::vector<std::unique_ptr<UndoableCommand>> commands_;
    std::size_t cursor_ = 0;
    std::uint64_t revision_ = 0;
};