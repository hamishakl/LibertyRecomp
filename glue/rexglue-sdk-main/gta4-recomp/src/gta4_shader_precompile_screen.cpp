#include "gta4_shader_precompile_screen.h"

#include <algorithm>
#include <filesystem>

#include <fmt/format.h>
#include <imgui.h>

namespace gta4::ui {
namespace {

constexpr const char* kDinCondensedBold = "/System/Library/Fonts/Supplemental/DIN Condensed Bold.ttf";
// Baked large so the screen stays sharp up to 4K; drawn scaled to the display height.
constexpr float kTitlePixels = 112.0f;
constexpr float kBodyPixels = 48.0f;

ImFont* AddDin(ImFontAtlas* atlas, float pixels) {
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0};
  ImFontConfig config;
  config.OversampleH = 2;
  config.OversampleV = 1;
  return atlas->AddFontFromFileTTF(kDinCondensedBold, pixels, &config, ranges);
}

std::string Grouped(uint32_t value) {
  std::string digits = std::to_string(value), out;
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i && (digits.size() - i) % 3 == 0) out += ',';
    out += digits[i];
  }
  return out;
}

}  // namespace

LoadingScreenFonts LoadLoadingScreenFonts(ImFontAtlas* atlas) {
  std::error_code error;
  if (!atlas || !std::filesystem::exists(kDinCondensedBold, error)) return {};
  return {AddDin(atlas, kTitlePixels), AddDin(atlas, kBodyPixels)};
}

ShaderPrecompileScreen::ShaderPrecompileScreen(rex::ui::ImGuiDrawer* drawer,
                                               rex::system::IGraphicsSystem* graphics,
                                               LoadingScreenFonts fonts)
    : ImGuiDialog(drawer), graphics_(graphics), fonts_(fonts) {}

void ShaderPrecompileScreen::OnDraw(ImGuiIO& io) {
  // A warm cache finishes in milliseconds; only show the screen when there is real work.
  if (first_draw_time_ < 0.0) first_draw_time_ = ImGui::GetTime();
  if (ImGui::GetTime() - first_draw_time_ < 0.3) return;
  const auto progress = graphics_ ? graphics_->GetShaderPrecompileProgress()
                                  : rex::system::IGraphicsSystem::ShaderPrecompileProgress{};
  const float target = progress.total ? float(progress.completed) / float(progress.total) : 0.0f;
  // Ease toward the true value so the bar glides like the game's own loading bar.
  shown_fraction_ += (target - shown_fraction_) * std::clamp(io.DeltaTime * 8.0f, 0.0f, 1.0f);

  const ImVec2 size = io.DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(size);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs;
  if (ImGui::Begin("##gta4-shader-precompile", nullptr, kFlags)) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float unit = size.y / 1080.0f;
    const float left = size.x * 0.06f, right = size.x * 0.94f;
    ImFont* title_font = fonts_.title ? fonts_.title : ImGui::GetFont();
    ImFont* body_font = fonts_.body ? fonts_.body : ImGui::GetFont();
    const float title_size = 84.0f * unit, body_size = 34.0f * unit;

    const float bar_y = size.y * 0.88f;
    const float bar_height = std::max(3.0f, 5.0f * unit);
    const float title_y = bar_y - 150.0f * unit;
    draw->AddText(title_font, title_size, ImVec2(left, title_y), IM_COL32(255, 255, 255, 255),
                  "PREPARING LIBERTY CITY");
    const std::string detail =
        progress.total ? fmt::format("COMPILING SHADERS   {} / {}", Grouped(progress.completed),
                                     Grouped(progress.total))
                       : std::string("COMPILING SHADERS");
    draw->AddText(body_font, body_size, ImVec2(left, title_y + title_size + 6.0f * unit),
                  IM_COL32(170, 170, 170, 255), detail.c_str());
    const std::string percent = fmt::format("{}%", int(shown_fraction_ * 100.0f + 0.5f));
    const ImVec2 percent_extent = body_font->CalcTextSizeA(body_size, FLT_MAX, 0.0f, percent.c_str());
    draw->AddText(body_font, body_size, ImVec2(right - percent_extent.x, bar_y - body_size - 10.0f * unit),
                  IM_COL32(170, 170, 170, 255), percent.c_str());

    draw->AddRectFilled(ImVec2(left, bar_y), ImVec2(right, bar_y + bar_height), IM_COL32(255, 255, 255, 46));
    draw->AddRectFilled(ImVec2(left, bar_y), ImVec2(left + (right - left) * shown_fraction_, bar_y + bar_height),
                        IM_COL32(255, 255, 255, 255));
    draw->AddText(body_font, 24.0f * unit, ImVec2(left, bar_y + bar_height + 18.0f * unit),
                  IM_COL32(110, 110, 110, 255),
                  "Shaders recorded during play are compiled once here, so the game does not stutter later.");
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

}  // namespace gta4::ui
