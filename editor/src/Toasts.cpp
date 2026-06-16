#include "Toasts.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace forge {

namespace {
constexpr float kLifetime = 4.0f; // seconds fully shown + fading
constexpr float kFade = 0.6f;     // fade-out tail length
constexpr size_t kMaxToasts = 5;  // cap so a burst can't fill the screen

ImU32 AccentColor(ToastManager::Kind kind)
{
    switch (kind) {
    case ToastManager::Kind::Error: return IM_COL32(232, 96, 96, 255);
    case ToastManager::Kind::Success: return IM_COL32(120, 200, 140, 255);
    default: return IM_COL32(86, 156, 214, 255); // Info — matches the viewport blue
    }
}

const char* Title(ToastManager::Kind kind)
{
    switch (kind) {
    case ToastManager::Kind::Error: return "Error";
    case ToastManager::Kind::Success: return "Success";
    default: return "Info";
    }
}
} // namespace

void ToastManager::Push(Kind kind, std::string text)
{
    m_Toasts.push_back({kind, std::move(text), 0.0f});
    if (m_Toasts.size() > kMaxToasts)
        m_Toasts.erase(m_Toasts.begin()); // drop the oldest
}

void ToastManager::Draw()
{
    const float dt = ImGui::GetIO().DeltaTime;
    for (Toast& t : m_Toasts)
        t.age += dt;
    m_Toasts.erase(std::remove_if(m_Toasts.begin(), m_Toasts.end(),
                                  [](const Toast& t) { return t.age >= kLifetime; }),
                   m_Toasts.end());
    if (m_Toasts.empty())
        return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pad = 14.0f;
    float y = vp->WorkPos.y + pad; // stack downward from the top-right corner

    for (size_t i = 0; i < m_Toasts.size(); ++i) {
        const Toast& t = m_Toasts[i];
        float remain = kLifetime - t.age;
        float alpha = remain < kFade ? remain / kFade : 1.0f;
        ImU32 accent = AccentColor(t.kind);

        char id[32];
        std::snprintf(id, sizeof(id), "##toast%zu", i);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - pad, y), ImGuiCond_Always,
                                ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.88f * alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::Begin(id, nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoDocking);
        ImVec2 wp = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddRectFilled(wp, ImVec2(wp.x + 4.0f, wp.y + ImGui::GetWindowSize().y),
                                                  accent); // colored accent bar
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(accent), "%s", Title(t.kind));
        ImGui::TextUnformatted(t.text.c_str());
        y += ImGui::GetWindowSize().y + 8.0f;
        ImGui::End();
        ImGui::PopStyleVar();
    }
}

} // namespace forge
