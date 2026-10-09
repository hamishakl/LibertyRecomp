#include "gta4_install_dialog.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

#include <SDL3/SDL_dialog.h>
#include <imgui.h>

#include <rex/filesystem.h>

namespace gta4::install {
namespace {

struct PickerRequest {
  std::function<void(std::filesystem::path, std::string)> complete;
};

void SDLCALL OnPathPicked(void* userdata, const char* const* file_list, int) {
  std::unique_ptr<PickerRequest> request(static_cast<PickerRequest*>(userdata));
  if (!request || !request->complete) {
    return;
  }
  if (!file_list) {
    request->complete({}, SDL_GetError());
    return;
  }
  if (!file_list[0]) {
    request->complete({}, {});
    return;
  }
  request->complete(rex::to_path(file_list[0]), {});
}

constexpr SDL_DialogFileFilter kGameFilters[] = {
    {"Xbox 360 disc image", "iso"},
    {"All supported files", "*"},
};

constexpr SDL_DialogFileFilter kUpdateFilters[] = {
    {"Xbox title update", "xexp"},
    {"Xbox content package", "*"},
};

constexpr SDL_DialogFileFilter kDlcFilters[] = {
    {"Xbox content package", "*"},
};

std::string HumanBytes(uint64_t bytes) {
  char buffer[32];
  if (bytes >= 1ull << 30) std::snprintf(buffer, sizeof buffer, "%.2f GB", double(bytes) / double(1ull << 30));
  else if (bytes >= 1ull << 20) std::snprintf(buffer, sizeof buffer, "%.1f MB", double(bytes) / double(1ull << 20));
  else std::snprintf(buffer, sizeof buffer, "%llu KB", static_cast<unsigned long long>(bytes >> 10));
  return buffer;
}

constexpr ImVec2 kPrimarySize(200.0f, 44.0f);
constexpr ImVec2 kSecondarySize(150.0f, 44.0f);

}  // namespace

InstallDialog::InstallDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path install_root,
                             bool dlc_only, gta4::ui::UiFonts fonts, CompleteCallback complete,
                             CancelCallback cancel)
    : ImGuiDialog(drawer),
      install_root_(std::move(install_root)),
      dlc_only_(dlc_only),
      fonts_(fonts),
      complete_(std::move(complete)),
      cancel_(std::move(cancel)),
      picker_state_(std::make_shared<PickerState>()) {}

void InstallDialog::OnClose() {
  picker_state_->inspection_worker.Stop();
  progress_.cancel_requested = true;
  if (install_thread_.joinable()) {
    install_thread_.join();
  }
}

void InstallDialog::AssignPickedPath(PickerTarget target, std::filesystem::path path) {
  std::lock_guard lock(picker_state_->mutex);
  switch (target) {
    case PickerTarget::kGame:
      picker_state_->game = std::move(path);
      if (picker_state_->game.empty()) {
        picker_state_->inspection_worker.Clear();
      } else {
        picker_state_->inspection_worker.Request(picker_state_->game);
      }
      break;
    case PickerTarget::kUpdate:
      picker_state_->update = std::move(path);
      break;
    case PickerTarget::kTlad:
      picker_state_->tlad = std::move(path);
      break;
    case PickerTarget::kTbogt:
      picker_state_->tbogt = std::move(path);
      break;
  }
}

std::filesystem::path InstallDialog::PathFor(PickerTarget target) const {
  std::lock_guard lock(picker_state_->mutex);
  switch (target) {
    case PickerTarget::kGame:
      return picker_state_->game;
    case PickerTarget::kUpdate:
      return picker_state_->update;
    case PickerTarget::kTlad:
      return picker_state_->tlad;
    case PickerTarget::kTbogt:
      return picker_state_->tbogt;
  }
  return {};
}

void InstallDialog::ShowFilePicker(PickerTarget target) {
  auto weak_state = std::weak_ptr<PickerState>(picker_state_);
  auto* request =
      new PickerRequest{[weak_state, target](std::filesystem::path path, std::string error) {
        auto state = weak_state.lock();
        if (!state) {
          return;
        }
        std::lock_guard lock(state->mutex);
        if (!error.empty()) {
          state->error = std::move(error);
          return;
        }
        if (path.empty()) {
          return;
        }
        state->error.clear();
        switch (target) {
          case PickerTarget::kGame:
            state->game = std::move(path);
            state->inspection_worker.Request(state->game);
            break;
          case PickerTarget::kUpdate:
            state->update = std::move(path);
            break;
          case PickerTarget::kTlad:
            state->tlad = std::move(path);
            break;
          case PickerTarget::kTbogt:
            state->tbogt = std::move(path);
            break;
        }
      }};

  const SDL_DialogFileFilter* filters = kDlcFilters;
  int filter_count = static_cast<int>(std::size(kDlcFilters));
  if (target == PickerTarget::kGame) {
    filters = kGameFilters;
    filter_count = static_cast<int>(std::size(kGameFilters));
  } else if (target == PickerTarget::kUpdate) {
    filters = kUpdateFilters;
    filter_count = static_cast<int>(std::size(kUpdateFilters));
  }
  SDL_ShowOpenFileDialog(OnPathPicked, request, nullptr, filters, filter_count, nullptr, false);
}

void InstallDialog::ShowFolderPicker(PickerTarget target) {
  auto weak_state = std::weak_ptr<PickerState>(picker_state_);
  auto* request =
      new PickerRequest{[weak_state, target](std::filesystem::path path, std::string error) {
        auto state = weak_state.lock();
        if (!state) {
          return;
        }
        std::lock_guard lock(state->mutex);
        if (!error.empty()) {
          state->error = std::move(error);
          return;
        }
        if (path.empty()) {
          return;
        }
        state->error.clear();
        switch (target) {
          case PickerTarget::kGame:
            state->game = std::move(path);
            state->inspection_worker.Request(state->game);
            break;
          case PickerTarget::kUpdate:
            state->update = std::move(path);
            break;
          case PickerTarget::kTlad:
            state->tlad = std::move(path);
            break;
          case PickerTarget::kTbogt:
            state->tbogt = std::move(path);
            break;
        }
      }};
  SDL_ShowOpenFolderDialog(OnPathPicked, request, nullptr, nullptr, false);
}

void InstallDialog::DrawSourceRow(const char* label, PickerTarget target,
                                  const std::filesystem::path& value, bool required) {
  using namespace gta4::ui;
  ImGui::PushID(label);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 12.0f));
  if (ImGui::BeginChild("row", ImVec2(0.0f, 0.0f),
                        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders |
                            ImGuiChildFlags_AlwaysUseWindowPadding)) {
    {
      ScopedFont heading(fonts_.heading);
      ImGui::TextUnformatted(label);
      ImGui::SameLine(0.0f, 14.0f);
      ImGui::TextColored(required ? kAmber : kTextMuted, required ? "REQUIRED" : "OPTIONAL");
    }
    if (value.empty()) {
      ImGui::TextColored(kTextMuted, "Not selected");
    } else {
      ImGui::PushStyleColor(ImGuiCol_Text, kText);
      ImGui::TextWrapped("%s", value.string().c_str());
      ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    if (SecondaryButton("SELECT FILE")) ShowFilePicker(target);
    ImGui::SameLine();
    if (SecondaryButton("SELECT FOLDER")) ShowFolderPicker(target);
    if (!value.empty()) {
      ImGui::SameLine();
      if (SecondaryButton("CLEAR")) AssignPickedPath(target, {});
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  ImGui::PopID();
}

void InstallDialog::DrawHeader(const char* kicker, const char* title) {
  using namespace gta4::ui;
  {
    ScopedFont heading(fonts_.heading);
    ImGui::TextColored(kAmber, "%s", kicker);
  }
  {
    ScopedFont big(fonts_.title);
    ImGui::TextUnformatted(title);
  }
  // The loading screen's hairline under the title.
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;
  ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(origin.x, origin.y + 4.0f),
                                            ImVec2(origin.x + width, origin.y + 6.0f),
                                            IM_COL32(255, 255, 255, 230));
  ImGui::Dummy(ImVec2(width, 24.0f));
}

void InstallDialog::StartInstall() {
  if (install_thread_.joinable()) {
    install_thread_.join();
  }

  Selection selection;
  {
    std::lock_guard lock(picker_state_->mutex);
    if (!dlc_only_) {
      const GameSourceInspectionSnapshot inspection = picker_state_->inspection_worker.Snapshot();
      if (!inspection.result || !inspection.result->supported() || picker_state_->update.empty()) {
        return;
      }
      selection.game_source = picker_state_->game;
      selection.update_source = picker_state_->update;
    }
    if (!picker_state_->tlad.empty()) {
      selection.dlc_sources.push_back({Episode::kTlad, picker_state_->tlad});
    }
    if (!picker_state_->tbogt.empty()) {
      selection.dlc_sources.push_back({Episode::kTbogt, picker_state_->tbogt});
    }
    if (dlc_only_ && selection.dlc_sources.empty()) {
      return;
    }
  }

  progress_.copied_bytes = 0;
  progress_.total_bytes = 0;
  progress_.cancel_requested = false;
  result_ = {};
  install_done_ = false;
  state_ = State::kInstalling;
  install_thread_ = std::thread([this, selection = std::move(selection)]() {
    result_ = Install(selection, install_root_, progress_);
    install_done_.store(true, std::memory_order_release);
  });
}

void InstallDialog::FinishInstallIfNeeded() {
  if (state_ != State::kInstalling || !install_done_.load(std::memory_order_acquire)) {
    return;
  }
  if (install_thread_.joinable()) {
    install_thread_.join();
  }
  state_ = result_.success ? State::kInstalled : State::kFailed;
}

void InstallDialog::DrawBaseInspection() {
  using namespace gta4::ui;
  const GameSourceInspectionSnapshot snapshot = picker_state_->inspection_worker.Snapshot();
  if (snapshot.checking) {
    ImGui::TextColored(kAmber, "CHECKING SOURCE...");
    return;
  }
  if (!snapshot.result) {
    return;
  }
  const bool supported = snapshot.result->supported();
  const std::string summary = FormatGameSourceInspection(*snapshot.result);
  ImGui::PushStyleColor(ImGuiCol_Text, supported ? kText : kDanger);
  ImGui::TextWrapped("%s", summary.c_str());
  ImGui::PopStyleColor();
  const std::string diagnostics = FormatGameSourceDiagnostics(*snapshot.result);
  if (!diagnostics.empty()) {
    ImGui::TextColored(kTextMuted, "%s", diagnostics.c_str());
  }
}

void InstallDialog::OnDraw(ImGuiIO& io) {
  using namespace gta4::ui;
  FinishInstallIfNeeded();

  if (completion_frames_ >= 0) {
    if (completion_frames_ == 0) {
      completion_frames_ = -1;
      auto complete = std::move(complete_);
      Close();
      if (complete) {
        complete();
      }
      return;
    }
    --completion_frames_;
  }

  // Full-screen black like the game's own loading screens, content in a left column.
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
  constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoBringToFrontOnFocus;
  const bool open = ImGui::Begin("Liberty Recompiled Setup##installer", nullptr, kWindowFlags);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  if (!open) {
    ImGui::End();
    return;
  }

  const float margin_x = io.DisplaySize.x * 0.07f;
  const float margin_y = io.DisplaySize.y * 0.09f;
  const float column = std::min(io.DisplaySize.x * 0.62f, 980.0f);
  ImGui::SetCursorPos(ImVec2(margin_x, margin_y));
  ImGui::BeginChild("##column", ImVec2(column, io.DisplaySize.y - margin_y - 24.0f), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground);

  if (state_ == State::kSelecting || state_ == State::kFailed) {
    DrawHeader("LIBERTY RECOMPILED", dlc_only_ ? "INSTALL EPISODES" : "INSTALL GTA IV");
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    if (!dlc_only_) {
      ImGui::TextWrapped(
          "Select your legally obtained PAL (NZ/AU/EU) Xbox 360 GTA IV source and Title Update 5. "
          "The update may be an STFS package or a raw default.xexp.");
    } else {
      ImGui::TextWrapped("Add either or both episodes to the existing GTA IV installation.");
    }
    ImGui::PopStyleColor();
    ImGui::Spacing();

    if (!dlc_only_) {
      DrawSourceRow("BASE GAME", PickerTarget::kGame, PathFor(PickerTarget::kGame), true);
      DrawBaseInspection();
      ImGui::Spacing();
      DrawSourceRow("TITLE UPDATE 5", PickerTarget::kUpdate, PathFor(PickerTarget::kUpdate), true);
      ImGui::Spacing();
    }
    DrawSourceRow("THE LOST AND DAMNED", PickerTarget::kTlad, PathFor(PickerTarget::kTlad), false);
    ImGui::Spacing();
    DrawSourceRow("THE BALLAD OF GAY TONY", PickerTarget::kTbogt, PathFor(PickerTarget::kTbogt), false);
    ImGui::Spacing();
    ImGui::TextColored(kTextMuted, "Install directory  %s", install_root_.string().c_str());

    std::string picker_error;
    {
      std::lock_guard lock(picker_state_->mutex);
      picker_error = picker_state_->error;
    }
    if (!picker_error.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
      ImGui::TextWrapped("File picker error: %s", picker_error.c_str());
      ImGui::PopStyleColor();
    }
    if (state_ == State::kFailed && !result_.error.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
      ImGui::TextWrapped("Installation failed: %s", result_.error.c_str());
      ImGui::PopStyleColor();
    }

    bool has_supported_game = dlc_only_;
    bool has_update = dlc_only_;
    bool has_dlc = false;
    {
      std::lock_guard lock(picker_state_->mutex);
      const GameSourceInspectionSnapshot inspection = picker_state_->inspection_worker.Snapshot();
      has_supported_game = dlc_only_ || (inspection.result && inspection.result->supported());
      has_update = dlc_only_ || !picker_state_->update.empty();
      has_dlc = !picker_state_->tlad.empty() || !picker_state_->tbogt.empty();
    }
    const bool may_install = has_supported_game && has_update && (!dlc_only_ || has_dlc);
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    {
      ScopedFont heading(fonts_.heading);
      ImGui::BeginDisabled(!may_install);
      if (PrimaryButton(state_ == State::kFailed ? "RETRY INSTALL" : "INSTALL", kPrimarySize)) {
        StartInstall();
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (SecondaryButton("CANCEL", kSecondarySize)) {
        auto cancel = std::move(cancel_);
        Close();
        if (cancel) {
          cancel();
        }
      }
    }
  } else if (state_ == State::kInstalling) {
    DrawHeader("LIBERTY RECOMPILED", "INSTALLING");
    const uint64_t copied = progress_.copied_bytes.load(std::memory_order_relaxed);
    const uint64_t total = progress_.total_bytes.load(std::memory_order_relaxed);
    const float fraction =
        total == 0 ? 0.0f
                   : std::clamp(static_cast<float>(copied) / static_cast<float>(total), 0.0f, 1.0f);
    ImGui::TextColored(kTextDim, "Validating, extracting and publishing the installation.");
    ImGui::Dummy(ImVec2(0.0f, 18.0f));
    {
      ScopedFont heading(fonts_.heading);
      const std::string left = "COPYING FILES   " + HumanBytes(copied) + " / " + HumanBytes(total);
      char percent[16];
      std::snprintf(percent, sizeof percent, "%d%%", int(fraction * 100.0f + 0.5f));
      ImGui::TextUnformatted(left.c_str());
      ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() -
                      ImGui::CalcTextSize(percent).x);
      ImGui::TextColored(kTextDim, "%s", percent);
    }
    ProgressLine(fraction);
    ImGui::Dummy(ImVec2(0.0f, 18.0f));
    {
      ScopedFont heading(fonts_.heading);
      if (SecondaryButton("CANCEL INSTALL", kSecondarySize)) {
        progress_.cancel_requested = true;
      }
    }
  } else {
    DrawHeader("LIBERTY RECOMPILED", "READY TO PLAY");
    ImGui::TextColored(kTextDim,
                       "Installation and integrity validation completed successfully.");
    ImGui::Dummy(ImVec2(0.0f, 18.0f));
    {
      ScopedFont heading(fonts_.heading);
      if (completion_frames_ < 0 && PrimaryButton("START GAME", kPrimarySize)) {
        completion_frames_ = 1;
      }
    }
  }

  ImGui::EndChild();
  ImGui::End();
}

}  // namespace gta4::install
