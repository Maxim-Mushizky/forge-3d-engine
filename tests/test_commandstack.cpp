#include "test_framework.h"

#include "CommandStack.h"

#include <memory>

// Batch bookkeeping for the scripted-undo path (#78/#162). GL-free: the stack
// only routes commands; no command here ever touches the scene.

namespace forge::test {

namespace {
struct NoopCommand : Command {
    void Undo(Scene&) override {}
    void Redo(Scene&) override {}
};
} // namespace

static void BatchCollectsPushes()
{
    CommandStack stack;
    stack.BeginBatch();
    stack.Push(std::make_unique<NoopCommand>());
    stack.Push(std::make_unique<NoopCommand>());
    CHECK(stack.UndoEntries().empty()); // routed into the batch, not the stack

    std::unique_ptr<CompositeCommand> batch = stack.EndBatch();
    CHECK(batch && !batch->Empty());
    CHECK(stack.UndoEntries().empty()); // EndBatch hands back, never pushes
}

static void StaleBatchRecovers()
{
    // A host failure skipped EndBatch. The next BeginBatch must not abort:
    // the stale batch's commands are already applied to the scene, so they
    // land as a normal undo entry and a fresh batch opens (#162).
    CommandStack stack;
    stack.BeginBatch();
    stack.Push(std::make_unique<NoopCommand>());

    stack.BeginBatch();
    CHECK(stack.UndoEntries().size() == 1); // stale batch became an undo entry

    stack.Push(std::make_unique<NoopCommand>());
    std::unique_ptr<CompositeCommand> batch = stack.EndBatch();
    CHECK(batch && !batch->Empty()); // new batch collected normally
    CHECK(stack.UndoEntries().size() == 1);
}

static void EmptyStaleBatchDropped()
{
    CommandStack stack;
    stack.BeginBatch();
    stack.BeginBatch(); // stale batch never collected anything -> no undo entry
    CHECK(stack.UndoEntries().empty());
    CHECK(stack.EndBatch() != nullptr); // the fresh batch is live
}

void RunCommandStackTests()
{
    BatchCollectsPushes();
    StaleBatchRecovers();
    EmptyStaleBatchDropped();
    std::printf("[ok] command stack tests\n");
}

} // namespace forge::test
