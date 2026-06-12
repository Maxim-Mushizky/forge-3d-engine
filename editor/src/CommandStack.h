#pragma once

#include <forge/scene/Scene.h>

#include <memory>
#include <vector>

namespace forge {

// Undo/redo via whole-entity snapshots. Entities are small value types
// (the mesh is a shared_ptr), so copying is cheap and commands stay trivial.
class Command {
public:
    virtual ~Command() = default;
    virtual void Undo(Scene& scene) = 0;
    virtual void Redo(Scene& scene) = 0;
};

class AddEntityCommand : public Command {
public:
    explicit AddEntityCommand(Entity snapshot) : m_Entity(std::move(snapshot)) {}
    void Undo(Scene& scene) override { scene.Remove(m_Entity.id); }
    void Redo(Scene& scene) override { scene.Insert(m_Entity); }

private:
    Entity m_Entity;
};

class DeleteEntityCommand : public Command {
public:
    explicit DeleteEntityCommand(Entity snapshot) : m_Entity(std::move(snapshot)) {}
    void Undo(Scene& scene) override { scene.Insert(m_Entity); }
    void Redo(Scene& scene) override { scene.Remove(m_Entity.id); }

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

private:
    std::vector<std::unique_ptr<Command>> m_Commands;
};

class CommandStack {
public:
    // The action has already been applied; Push only records it.
    void Push(std::unique_ptr<Command> command)
    {
        m_UndoStack.push_back(std::move(command));
        m_RedoStack.clear();
    }

    bool Undo(Scene& scene)
    {
        if (m_UndoStack.empty())
            return false;
        m_UndoStack.back()->Undo(scene);
        m_RedoStack.push_back(std::move(m_UndoStack.back()));
        m_UndoStack.pop_back();
        return true;
    }

    bool Redo(Scene& scene)
    {
        if (m_RedoStack.empty())
            return false;
        m_RedoStack.back()->Redo(scene);
        m_UndoStack.push_back(std::move(m_RedoStack.back()));
        m_RedoStack.pop_back();
        return true;
    }

private:
    std::vector<std::unique_ptr<Command>> m_UndoStack;
    std::vector<std::unique_ptr<Command>> m_RedoStack;
};

} // namespace forge
