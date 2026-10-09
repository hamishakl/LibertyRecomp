#include "gta4_ui_theme.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace gta4::ui {
namespace {

constexpr const char* kDinAlternateBold = "/System/Library/Fonts/Supplemental/DIN Alternate Bold.ttf";
constexpr const char* kDinCondensedBold = "/System/Library/Fonts/Supplemental/DIN Condensed Bold.ttf";
// ImGui lays out in logical pixels and the drawer scales to the physical framebuffer, so glyphs are
// rasterised at 2x and drawn at half scale to stay sharp on retina displays.
constexpr float kBakeScale = 2.0f;

ImFont* AddFont(ImFontAtlas* atlas, const char* path, float logical_pixels) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) return nullptr;
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0};
  ImFontConfig config;
  config.OversampleH = 2;
  config.OversampleV = 1;
  ImFont* font = atlas->AddFontFromFileTTF(path, logical_pixels * kBakeScale, &config, ranges);
  if (font) font->Scale = 1.0f / kBakeScale;
  return font;
}

ImVec4 White(float alpha) { return ImVec4(1.0f, 1.0f, 1.0f, alpha); }
ImVec4 Amber(float alpha) { return ImVec4(kAmber.x, kAmber.y, kAmber.z, alpha); }

}  // namespace

UiFonts LoadUiFonts(ImFontAtlas* atlas) {
  UiFonts fonts;
  if (!atlas) return fonts;
  fonts.body = AddFont(atlas, kDinAlternateBold, 17.0f);
  fonts.heading = AddFont(atlas, kDinCondensedBold, 26.0f);
  fonts.title = AddFont(atlas, kDinCondensedBold, 56.0f);
  if (fonts.body) ImGui::GetIO().FontDefault = fonts.body;
  return fonts;
}

void ApplyTheme(ImGuiStyle& style) {
  style.WindowRounding = style.ChildRounding = style.FrameRounding = style.PopupRounding = 0.0f;
  style.ScrollbarRounding = style.GrabRounding = style.TabRounding = 0.0f;
  style.WindowBorderSize = style.ChildBorderSize = style.PopupBorderSize = 1.0f;
  style.FrameBorderSize = 1.0f;
  style.WindowPadding = ImVec2(18.0f, 16.0f);
  style.FramePadding = ImVec2(12.0f, 7.0f);
  style.ItemSpacing = ImVec2(10.0f, 8.0f);
  style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
  style.ScrollbarSize = 10.0f;
  style.GrabMinSize = 12.0f;
  style.WindowTitleAlign = ImVec2(0.0f, 0.5f);

  ImVec4* c = style.Colors;
  c[ImGuiCol_Text] = kText;
  c[ImGuiCol_TextDisabled] = kTextDim;
  c[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.90f);
  c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  c[ImGuiCol_PopupBg] = ImVec4(0.04f, 0.04f, 0.04f, 0.97f);
  c[ImGuiCol_Border] = kHairline;
  c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  c[ImGuiCol_FrameBg] = White(0.07f);
  c[ImGuiCol_FrameBgHovered] = White(0.12f);
  c[ImGuiCol_FrameBgActive] = White(0.16f);
  c[ImGuiCol_TitleBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
  c[ImGuiCol_TitleBgActive] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
  c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0f, 0.0f, 0.0f, 0.8f);
  c[ImGuiCol_MenuBarBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  c[ImGuiCol_ScrollbarGrab] = White(0.25f);
  c[ImGuiCol_ScrollbarGrabHovered] = White(0.40f);
  c[ImGuiCol_ScrollbarGrabActive] = kAmber;
  c[ImGuiCol_CheckMark] = kAmber;
  c[ImGuiCol_SliderGrab] = White(0.85f);
  c[ImGuiCol_SliderGrabActive] = kAmber;
  c[ImGuiCol_Button] = White(0.10f);
  c[ImGuiCol_ButtonHovered] = White(0.22f);
  c[ImGuiCol_ButtonActive] = Amber(0.85f);
  c[ImGuiCol_Header] = White(0.12f);
  c[ImGuiCol_HeaderHovered] = White(0.22f);
  c[ImGuiCol_HeaderActive] = Amber(0.85f);
  c[ImGuiCol_Separator] = kHairline;
  c[ImGuiCol_SeparatorHovered] = White(0.40f);
  c[ImGuiCol_SeparatorActive] = kAmber;
  c[ImGuiCol_ResizeGrip] = White(0.12f);
  c[ImGuiCol_ResizeGripHovered] = White(0.30f);
  c[ImGuiCol_ResizeGripActive] = kAmber;
  c[ImGuiCol_Tab] = White(0.06f);
  c[ImGuiCol_TabHovered] = White(0.20f);
  c[ImGuiCol_TabActive] = White(0.16f);
  c[ImGuiCol_TabUnfocused] = White(0.04f);
  c[ImGuiCol_TabUnfocusedActive] = White(0.10f);
  c[ImGuiCol_PlotLines] = White(1.0f);
  c[ImGuiCol_PlotLinesHovered] = kAmberBright;
  c[ImGuiCol_PlotHistogram] = kAmber;
  c[ImGuiCol_PlotHistogramHovered] = kAmberBright;
  c[ImGuiCol_TableHeaderBg] = White(0.08f);
  c[ImGuiCol_TableBorderStrong] = kHairline;
  c[ImGuiCol_TableBorderLight] = White(0.08f);
  c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  c[ImGuiCol_TableRowBgAlt] = White(0.03f);
  c[ImGuiCol_TextSelectedBg] = Amber(0.35f);
  c[ImGuiCol_DragDropTarget] = kAmber;
  c[ImGuiCol_NavHighlight] = kAmber;
  c[ImGuiCol_NavWindowingHighlight] = White(0.70f);
  c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.50f);
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.60f);
}

bool PrimaryButton(const char* label, const ImVec2& size) {
  ImGui::PushStyleColor(ImGuiCol_Button, kAmber);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAmberBright);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, White(0.95f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  const bool pressed = ImGui::Button(label, size);
  ImGui::PopStyleColor(5);
  return pressed;
}

bool SecondaryButton(const char* label, const ImVec2& size) {
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, White(0.15f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, Amber(0.85f));
  ImGui::PushStyleColor(ImGuiCol_Border, White(0.55f));
  const bool pressed = ImGui::Button(label, size);
  ImGui::PopStyleColor(4);
  return pressed;
}

void ProgressLine(float fraction, float height) {
  fraction = std::clamp(fraction, 0.0f, 1.0f);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(255, 255, 255, 46));
  if (fraction > 0.0f)
    draw->AddRectFilled(origin, ImVec2(origin.x + width * fraction, origin.y + height),
                        ImGui::GetColorU32(kAmber));
  ImGui::Dummy(ImVec2(width, height));
}

}  // namespace gta4::ui
