#pragma once

#include <rex/system/interfaces/graphics.h>
#include <rex/ui/imgui_dialog.h>

struct ImFont;
struct ImFontAtlas;

namespace gta4::ui {

// Fonts for the launch-time loading screen. Loaded from macOS' DIN Condensed Bold (the closest
// shipped match to GTA IV's DIN 1451 UI face); null when unavailable, which falls back to ImGui's
// default font.
struct LoadingScreenFonts {
  ImFont* title = nullptr;
  ImFont* body = nullptr;
};
LoadingScreenFonts LoadLoadingScreenFonts(ImFontAtlas* atlas);

// Full-screen GTA IV-style loading screen shown while the GPU plugin precompiles recorded shader
// pipelines and the title's main thread is held. Self-owned like every ImGuiDialog; call Finish().
class ShaderPrecompileScreen final : public rex::ui::ImGuiDialog {
 public:
  ShaderPrecompileScreen(rex::ui::ImGuiDrawer* drawer, rex::system::IGraphicsSystem* graphics,
                         LoadingScreenFonts fonts);
  void Finish() { Close(); }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  rex::system::IGraphicsSystem* graphics_;
  LoadingScreenFonts fonts_;
  float shown_fraction_ = 0.0f;
  double first_draw_time_ = -1.0;
};

}  // namespace gta4::ui
