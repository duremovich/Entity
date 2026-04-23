#pragma once

#include "Command.hpp"

namespace entity {

// Command that can be reverted. CommandDispatcher transfers ownership of
// undoable commands onto an undo stack after successful execute(); undo() is
// expected to restore the state that existed before execute() ran.
//
// Undoable property commands capture their "previous" value on the first
// execute(). UI sites that pre-mutate the live state for responsive dragging
// must call the command's setPrevious*() before enqueueing — otherwise the
// auto-capture sees the post-drag value and undo becomes a no-op.
class UndoableCommand : public Command {
public:
    virtual bool undo(Engine& engine) = 0;

    // Default redo just re-runs execute(). Override if execute() captures
    // state that must be preserved on subsequent runs (most commands don't).
    virtual bool redo(Engine& engine) { return execute(engine); }
};

} // namespace entity
