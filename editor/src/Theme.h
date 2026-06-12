#pragma once

#include <imgui.h>

// Forge editor theme: warm neutral dark + single ember-orange accent.
namespace forge::ui {

inline constexpr ImVec4 kAccent{0.94f, 0.58f, 0.22f, 1.00f};
inline constexpr ImVec4 kAccentHover{1.00f, 0.66f, 0.30f, 1.00f};
inline constexpr ImVec4 kAccentActive{0.84f, 0.49f, 0.13f, 1.00f};
inline constexpr ImVec4 kAccentText{0.08f, 0.06f, 0.03f, 1.00f}; // dark text on accent fills

inline constexpr ImVec4 kDanger{0.70f, 0.22f, 0.20f, 1.00f};
inline constexpr ImVec4 kDangerHover{0.79f, 0.26f, 0.23f, 1.00f};
inline constexpr ImVec4 kDangerActive{0.60f, 0.19f, 0.17f, 1.00f};

inline void PushAccentButton()
{
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentActive);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccentText);
}
inline void PopAccentButton() { ImGui::PopStyleColor(4); }

inline void PushDangerButton()
{
    ImGui::PushStyleColor(ImGuiCol_Button, kDanger);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kDangerHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kDangerActive);
}
inline void PopDangerButton() { ImGui::PopStyleColor(3); }

inline void ApplyTheme()
{
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding = 6.0f;
    s.ChildRounding = 6.0f;
    s.FrameRounding = 4.0f;
    s.PopupRounding = 6.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;
    s.ScrollbarRounding = 12.0f;
    s.WindowPadding = {10, 10};
    s.FramePadding = {8, 5};
    s.ItemSpacing = {8, 6};
    s.ItemInnerSpacing = {6, 4};
    s.CellPadding = {4, 3};
    s.IndentSpacing = 14.0f;
    s.ScrollbarSize = 13.0f;
    s.GrabMinSize = 12.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.PopupBorderSize = 1.0f;
    s.SeparatorTextBorderSize = 2.0f;
    s.SeparatorTextPadding = {16, 4};
    s.SeparatorTextAlign = {0.0f, 0.5f};
    s.TabBarBorderSize = 2.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = {0.93f, 0.93f, 0.93f, 1.00f};
    c[ImGuiCol_TextDisabled] = {0.52f, 0.53f, 0.55f, 1.00f};
    c[ImGuiCol_WindowBg] = {0.106f, 0.110f, 0.125f, 1.00f};
    c[ImGuiCol_ChildBg] = {0, 0, 0, 0};
    c[ImGuiCol_PopupBg] = {0.133f, 0.137f, 0.161f, 0.98f};
    c[ImGuiCol_Border] = {0.20f, 0.21f, 0.235f, 0.60f};
    c[ImGuiCol_BorderShadow] = {0, 0, 0, 0};
    c[ImGuiCol_FrameBg] = {0.149f, 0.157f, 0.180f, 1.00f};
    c[ImGuiCol_FrameBgHovered] = {0.184f, 0.196f, 0.227f, 1.00f};
    c[ImGuiCol_FrameBgActive] = {0.22f, 0.235f, 0.27f, 1.00f};
    c[ImGuiCol_TitleBg] = {0.086f, 0.090f, 0.102f, 1.00f};
    c[ImGuiCol_TitleBgActive] = {0.122f, 0.125f, 0.145f, 1.00f};
    c[ImGuiCol_TitleBgCollapsed] = {0.086f, 0.090f, 0.102f, 0.75f};
    c[ImGuiCol_MenuBarBg] = {0.122f, 0.125f, 0.145f, 1.00f};
    c[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrab] = {0.227f, 0.239f, 0.27f, 1.00f};
    c[ImGuiCol_ScrollbarGrabHovered] = {0.29f, 0.306f, 0.345f, 1.00f};
    c[ImGuiCol_ScrollbarGrabActive] = kAccent;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccentHover;
    c[ImGuiCol_Button] = {0.18f, 0.19f, 0.22f, 1.00f};
    c[ImGuiCol_ButtonHovered] = {0.227f, 0.243f, 0.278f, 1.00f};
    c[ImGuiCol_ButtonActive] = {0.94f, 0.58f, 0.22f, 0.50f};
    c[ImGuiCol_Header] = {0.94f, 0.58f, 0.22f, 0.25f};
    c[ImGuiCol_HeaderHovered] = {0.94f, 0.58f, 0.22f, 0.35f};
    c[ImGuiCol_HeaderActive] = {0.94f, 0.58f, 0.22f, 0.45f};
    c[ImGuiCol_Separator] = {0.20f, 0.21f, 0.235f, 0.60f};
    c[ImGuiCol_SeparatorHovered] = {0.94f, 0.58f, 0.22f, 0.50f};
    c[ImGuiCol_SeparatorActive] = kAccent;
    c[ImGuiCol_ResizeGrip] = {0.94f, 0.58f, 0.22f, 0.20f};
    c[ImGuiCol_ResizeGripHovered] = {0.94f, 0.58f, 0.22f, 0.50f};
    c[ImGuiCol_ResizeGripActive] = {0.94f, 0.58f, 0.22f, 0.80f};
    c[ImGuiCol_Tab] = {0.122f, 0.125f, 0.145f, 1.00f};
    c[ImGuiCol_TabHovered] = {0.94f, 0.58f, 0.22f, 0.35f};
    c[ImGuiCol_TabActive] = {0.165f, 0.173f, 0.20f, 1.00f};
    c[ImGuiCol_TabUnfocused] = {0.094f, 0.098f, 0.114f, 1.00f};
    c[ImGuiCol_TabUnfocusedActive] = {0.133f, 0.137f, 0.161f, 1.00f};
    c[ImGuiCol_DockingPreview] = {0.94f, 0.58f, 0.22f, 0.40f};
    c[ImGuiCol_DockingEmptyBg] = {0.086f, 0.090f, 0.102f, 1.00f};
    c[ImGuiCol_TextSelectedBg] = {0.94f, 0.58f, 0.22f, 0.30f};
    c[ImGuiCol_DragDropTarget] = {0.94f, 0.58f, 0.22f, 0.90f};
    c[ImGuiCol_NavHighlight] = kAccent;
    c[ImGuiCol_PlotHistogram] = kAccent;
    c[ImGuiCol_PlotHistogramHovered] = kAccentHover;
}

} // namespace forge::ui
