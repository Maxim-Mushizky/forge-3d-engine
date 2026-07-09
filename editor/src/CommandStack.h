#pragma once

#include <forge/anim/Pose.h>
#include <forge/anim/SkinApply.h>
#include <forge/core/Log.h>
#include <forge/scene/Scene.h>

#include <memory>
#include <utility>
#include <vector>

namespace forge {

// Undo/redo via whole-entity snapshots. Entities are small value types
// (the mesh is a shared_ptr), so copying is cheap and commands stay trivial.
class Command {
public:
    virtual ~Command() = default;
    virtual void Undo(Scene& scene) = 0;
    virtual void Redo(Scene& scene) = 0;
    // Short label for the undo-history panel (#23). Coarse per command type —
    // commands don't carry the originating action's semantics.
    virtual const char* Name() const { return "Edit"; }
};

class AddEntityCommand : public Command {
public:
    explicit AddEntityCommand(Entity snapshot) : m_Entity(std::move(snapshot)) {}
    void Undo(Scene& scene) override { scene.Remove(m_Entity.id); }
    void Redo(Scene& scene) override { scene.Insert(m_Entity); }
    const char* Name() const override { return "Add object"; }

private:
    Entity m_Entity;
};

class DeleteEntityCommand : public Command {
public:
    explicit DeleteEntityCommand(Entity snapshot) : m_Entity(std::move(snapshot)) {}
    void Undo(Scene& scene) override { scene.Insert(m_Entity); }
    void Redo(Scene& scene) override { scene.Remove(m_Entity.id); }
    const char* Name() const override { return "Delete object"; }

private:
    Entity m_Entity;
};

// Covers transform, material, and rename edits uniformly.
class EditEntityCommand : public Command {
public:
    EditEntityCommand(Entity before, Entity after)
        : m_Before(std::move(before)), m_After(std::move(after)) {}
    void Undo(Scene& scene) override { scene.Replace(m_Before); }
    void Redo(Scene& scene) override { scene.Replace(m_After); }
    const char* Name() const override { return "Transform / edit"; }

private:
    Entity m_Before, m_After;
};

// One sculpt stroke: sparse vertex diff (positions + normals are both in Vertex,
// so undo/redo just writes vertices back — no normal recompute needed).
// Entity snapshots can't cover this: they share the mesh pointer.
class SculptStrokeCommand : public Command {
public:
    SculptStrokeCommand(UUID entity, std::vector<uint32_t> indices, std::vector<Vertex> before,
                        std::vector<Vertex> after)
        : m_Entity(entity), m_Indices(std::move(indices)), m_Before(std::move(before)),
          m_After(std::move(after))
    {
    }

    void Undo(Scene& scene) override { Apply(scene, m_Before); }
    void Redo(Scene& scene) override { Apply(scene, m_After); }
    const char* Name() const override { return "Edit vertices"; }

private:
    void Apply(Scene& scene, const std::vector<Vertex>& values)
    {
        Entity* e = scene.Find(m_Entity);
        if (!e || !e->mesh)
            return;
        auto& verts = e->mesh->MutableVertices();
        for (size_t i = 0; i < m_Indices.size(); ++i)
            if (m_Indices[i] < verts.size())
                verts[m_Indices[i]] = values[i];
        e->mesh->RecomputeBounds();
        e->mesh->UploadVertices();
    }

    UUID m_Entity;
    std::vector<uint32_t> m_Indices;
    std::vector<Vertex> m_Before, m_After;
};

// One set_pose edit: stores just the before/after Pose (a few hundred bytes), never
// a mesh clone — the whole point of the Pose model. Undo/redo re-applies the stored
// pose and re-skins from bind (ApplyPose never compounds). Entity snapshots can't
// cover this: they'd share the mesh pointer and wouldn't re-deform.
class SetPoseCommand : public Command {
public:
    SetPoseCommand(UUID entity, Pose before, Pose after)
        : m_Entity(entity), m_Before(std::move(before)), m_After(std::move(after))
    {
    }

    void Undo(Scene& scene) override { Apply(scene, m_Before); }
    void Redo(Scene& scene) override { Apply(scene, m_After); }
    const char* Name() const override { return "Pose"; }

private:
    void Apply(Scene& scene, const Pose& pose)
    {
        Entity* e = scene.Find(m_Entity);
        if (!e || !e->mesh || !e->skeleton)
            return;
        e->pose = pose;
        ApplyPose(*e->mesh, *e->skeleton, e->pose);
    }

    UUID m_Entity;
    Pose m_Before, m_After;
};

// Topology ops (mirror, subdivide, boolean, extrude) replace the whole mesh.
// O(1): the COW discipline means nobody mutates a mesh that undo history holds,
// so keeping both shared_ptrs is safe and cheap.
class MeshSwapCommand : public Command {
public:
    MeshSwapCommand(UUID entity, std::shared_ptr<Mesh> before, std::shared_ptr<Mesh> after)
        : m_Entity(entity), m_Before(std::move(before)), m_After(std::move(after))
    {
    }

    void Undo(Scene& scene) override { Apply(scene, m_Before); }
    void Redo(Scene& scene) override { Apply(scene, m_After); }
    const char* Name() const override { return "Mesh op"; }

private:
    void Apply(Scene& scene, const std::shared_ptr<Mesh>& mesh)
    {
        if (Entity* e = scene.Find(m_Entity))
            e->mesh = mesh;
    }

    UUID m_Entity;
    std::shared_ptr<Mesh> m_Before, m_After;
};

// Groups several commands into one undo step (e.g. multi-part model import).
class CompositeCommand : public Command {
public:
    void Add(std::unique_ptr<Command> command) { m_Commands.push_back(std::move(command)); }
    bool Empty() const { return m_Commands.empty(); }

    void Undo(Scene& scene) override
    {
        for (auto it = m_Commands.rbegin(); it != m_Commands.rend(); ++it)
            (*it)->Undo(scene);
    }
    void Redo(Scene& scene) override
    {
        for (auto& c : m_Commands)
            c->Redo(scene);
    }
    const char* Name() const override { return "Batch"; }

private:
    std::vector<std::unique_ptr<Command>> m_Commands;
};

class CommandStack {
public:
    // The action has already been applied; Push only records it. While a
    // scripted batch (#78) is open, commands collect into it instead so a
    // whole agent script lands as a single undo entry.
    void Push(std::unique_ptr<Command> command)
    {
        if (m_Batch) {
            m_Batch->Add(std::move(command));
            return;
        }
        m_UndoStack.push_back(std::move(command));
        m_RedoStack.clear();
        ++m_Revision;
    }

    // Scripted batches (#78). Begin routes subsequent Pushes into a composite;
    // End hands it back — the caller Pushes it on success, or Undoes and drops
    // it so a failed script rolls back atomically.
    void BeginBatch()
    {
        if (m_Batch) {
            // The previous batch owner never reached EndBatch (a failure
            // escaped the script path). Its commands are already applied to
            // the scene, so land them as a normal undo entry — dropping them
            // would break undo; aborting would kill the editor (#162).
            FORGE_ERROR("CommandStack: batch already open — closing the stale one");
            std::unique_ptr<CompositeCommand> stale = std::move(m_Batch);
            if (!stale->Empty())
                Push(std::move(stale));
        }
        m_Batch = std::make_unique<CompositeCommand>();
    }
    std::unique_ptr<CompositeCommand> EndBatch() { return std::move(m_Batch); }

    bool Undo(Scene& scene)
    {
        if (m_UndoStack.empty())
            return false;
        m_UndoStack.back()->Undo(scene);
        m_RedoStack.push_back(std::move(m_UndoStack.back()));
        m_UndoStack.pop_back();
        ++m_Revision;
        return true;
    }

    bool Redo(Scene& scene)
    {
        if (m_RedoStack.empty())
            return false;
        m_RedoStack.back()->Redo(scene);
        m_UndoStack.push_back(std::move(m_RedoStack.back()));
        m_RedoStack.pop_back();
        ++m_Revision;
        return true;
    }

    void Clear()
    {
        m_UndoStack.clear();
        m_RedoStack.clear();
        ++m_Revision;
    }

    // Monotonic edit counter: comparing against the value at last save gives
    // dirty tracking without hooking every call site. (Undoing back to the
    // saved state still reads as dirty — a harmless false positive.)
    uint64_t Revision() const { return m_Revision; }

    // For the undo-history panel (#23): applied steps oldest->newest (back = the
    // current state) and the pending redo steps (back = the next to redo).
    const std::vector<std::unique_ptr<Command>>& UndoEntries() const { return m_UndoStack; }
    const std::vector<std::unique_ptr<Command>>& RedoEntries() const { return m_RedoStack; }

private:
    std::vector<std::unique_ptr<Command>> m_UndoStack;
    std::vector<std::unique_ptr<Command>> m_RedoStack;
    std::unique_ptr<CompositeCommand> m_Batch; // non-null while a script batch is open
    uint64_t m_Revision = 0;
};

} // namespace forge
