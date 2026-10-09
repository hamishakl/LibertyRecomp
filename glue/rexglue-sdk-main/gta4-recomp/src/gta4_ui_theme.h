#pragma once

#include <imgui.h>

struct ImFontAtlas;

namespace gta4::ui {

// GTA IV-style look shared by every ImGui surface (installer, text chat, overlays): black
// translucent panels, white DIN type, one amber accent, no rounding, 1 px hairlines.
inline constexpr ImVec4 kAmber{0.97f, 0.69f, 0.20f, 1.0f};
inline constexpr ImVec4 kAmberBright{1.00f, 0.80f, 0.38f, 1.0f};
inline constexpr ImVec4 kText{0.95f, 0.95f, 0.95f, 1.0f};
inline constexpr ImVec4 kTextDim{0.66f, 0.66f, 0.66f, 1.0f};
inline constexpr ImVec4 kTextMuted{0.42f, 0.42f, 0.42f, 1.0f};
inline constexpr ImVec4 kDanger{1.00f, 0.36f, 0.33f, 1.0f};
inline constexpr ImVec4 kHairline{1.0f, 1.0f, 1.0f, 0.18f};
inline constexpr ImVec4 kPanel{1.0f, 1.0f, 1.0f, 0.06f};

struct UiFonts {
  ImFont* body = nullptr;     // ~17 px logical, DIN Alternate Bold: paragraphs, paths, input
  ImFont* heading = nullptr;  // ~26 px logical, DIN Condensed Bold: section labels, buttons
  ImFont* title = nullptr;    // ~56 px logical, DIN Condensed Bold: screen titles
};

// Loads the macOS system DIN faces (the closest shipped match to GTA IV's DIN 1451 UI face),
// baked at 2x so they stay sharp on retina, and makes `body` the ImGui default font. A missing
// face stays null and callers fall back to whatever font is current.
UiFonts LoadUiFonts(ImFontAtlas* atlas);

// Palette and metrics for the current ImGui style. Runs after the drawer's defaults.
void ApplyTheme(ImGuiStyle& style);

// Pushes a font for the scope when it exists; a null font is a no-op.
struct ScopedFont {
  explicit ScopedFont(ImFont* font) : pushed(font != nullptr) {
    if (pushed) ImGui::PushFont(font);
  }
  ~ScopedFont() {
    if (pushed) ImGui::PopFont();
  }
  ScopedFont(const ScopedFont&) = delete;
  ScopedFont& operator=(const ScopedFont&) = delete;
  bool pushed;
};

// Amber-filled button with black text: the one primary action on a screen.
bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
// Hairline-outlined button for secondary actions.
bool SecondaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
// Thin progress bar in the loading-screen style: white track, amber fill.
void ProgressLine(float fraction, float height = 6.0f);

}  // namespace gta4::ui
