#include "input/text_chat_dialog.h"

#include "input/text_chat_team.h"

#include <algorithm>
#include <cfloat>
#include <span>

#include <imgui.h>

namespace gta4::input {

using rex::system::xam::TextChatChannel;
using rex::system::xam::TextChatMessage;

TextChatDialog::TextChatDialog(rex::ui::ImGuiDrawer* drawer, gta4::ui::UiFonts fonts,
                               std::function<void(bool)> set_input_capture)
    : ImGuiDialog(drawer),
      fonts_(fonts),
      set_input_capture_(std::move(set_input_capture)) {}

TextChatDialog::~TextChatDialog() { Stop(); }

void TextChatDialog::AttachLive(
    rex::system::xam::LiveCompatibilityRuntime* live) {
  live_ = live;
  transport_ = live ? live->text_chat_transport() : nullptr;
}

void TextChatDialog::RequestOpen(TextChatChannel channel) {
  requested_channel_.store(channel == TextChatChannel::kTeam ? 2u : 1u,
                           std::memory_order_release);
  set_input_capture_(true);
  RequestRepaint();
}

void TextChatDialog::Stop() {
  requested_channel_.store(0, std::memory_order_release);
  FinishComposition();
  if (transport_) transport_->CloseTextChat();
  configured_session_id_ = 0;
  transport_ = nullptr;
  live_ = nullptr;
}

void TextChatDialog::SynchronizeSession() {
  const uint64_t session_id =
      live_ && live_->available() ? live_->active_session_id() : 0;
  if (!transport_ || !transport_->ready() || !session_id) {
    if (configured_session_id_ && transport_) transport_->CloseTextChat();
    configured_session_id_ = 0;
    return;
  }
  if (session_id != configured_session_id_) {
    if (transport_->Configure(session_id)) {
      configured_session_id_ = session_id;
      history_.clear();
      status_.clear();
    } else {
      configured_session_id_ = 0;
      status_ = "Text chat is unavailable";
    }
  }
}

void TextChatDialog::DrainReceived() {
  if (!transport_ || !configured_session_id_) return;
  auto messages = transport_->ReceiveMessages(kReceiveBatchMessages);
  for (auto& message : messages) {
    if (message.session_id != configured_session_id_) continue;
    if (history_.size() >= kHistoryMessages) history_.pop_front();
    history_.push_back(std::move(message));
  }
}

void TextChatDialog::FinishComposition() {
  composing_ = false;
  focus_input_ = false;
  input_.fill('\0');
  set_input_capture_(false);
}

bool TextChatDialog::SendComposition() {
  if (!transport_ || !configured_session_id_) {
    status_ = "Join an online session to use text chat";
    return false;
  }

  std::span<const uint64_t> targets;
  std::vector<uint64_t> team_targets;
  if (channel_ == TextChatChannel::kTeam) {
    auto resolved = FindTeamChatTargets(live_);
    if (!resolved || resolved->empty()) {
      status_ = "Team Chat has no valid teammates";
      return false;
    }
    team_targets = std::move(*resolved);
    targets = team_targets;
  }

  const std::string text(input_.data());
  const uint32_t sequence = next_sequence_;
  if (!transport_->Send(channel_, targets, sequence, text)) {
    status_ = "Text chat message was not accepted";
    return false;
  }
  ++next_sequence_;
  TextChatMessage local{
      .source_xuid = live_->identity().xuid,
      .session_id = configured_session_id_,
      .sequence = sequence,
      .channel = channel_,
      .player_name = live_->identity().player_name,
      .text = text,
  };
  if (history_.size() >= kHistoryMessages) history_.pop_front();
  history_.push_back(std::move(local));
  status_.clear();
  return true;
}

void TextChatDialog::OnDraw(ImGuiIO& io) {
  SynchronizeSession();
  DrainReceived();

  const uint32_t request = requested_channel_.exchange(0, std::memory_order_acq_rel);
  if (request) {
    channel_ = request == 2 ? TextChatChannel::kTeam : TextChatChannel::kAll;
    input_.fill('\0');
    io.ClearInputKeys();
    composing_ = true;
    focus_input_ = true;
  }

  if (history_.empty() && status_.empty() && !composing_) return;

  using namespace gta4::ui;
  // Bottom-left like the game's own chat feed: black panel, no frame, amber channel tags.
  ImGui::SetNextWindowPos(ImVec2(24.0f, io.DisplaySize.y - 24.0f), ImGuiCond_Always,
                          ImVec2(0.0f, 1.0f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(440.0f, 0.0f),
                                      ImVec2(std::max(440.0f, io.DisplaySize.x * 0.42f), FLT_MAX));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.62f));
  constexpr ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
  const bool open = ImGui::Begin("##gta4_text_chat", nullptr, flags);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  if (!open) {
    ImGui::End();
    return;
  }

  for (const auto& message : history_) {
    const bool team = message.channel == TextChatChannel::kTeam;
    ImGui::TextColored(team ? kAmber : kTextDim, team ? "TEAM" : "ALL");
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextColored(kText, "%s", message.player_name.c_str());
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "%s", message.text.c_str());
    ImGui::PopTextWrapPos();
  }
  if (!status_.empty()) {
    ImGui::TextColored(kAmber, "%s", status_.c_str());
  }

  if (composing_) {
    if (!history_.empty() || !status_.empty()) ImGui::Separator();
    {
      ScopedFont heading(fonts_.heading);
      ImGui::TextColored(kAmber, channel_ == TextChatChannel::kTeam ? "TEAM CHAT" : "ALL CHAT");
    }
    if (focus_input_) {
      ImGui::SetKeyboardFocusHere();
      focus_input_ = false;
    }
    ImGui::SetNextItemWidth(-1.0f);
    const bool submitted = ImGui::InputText(
        "##message", input_.data(), input_.size(),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (submitted && SendComposition()) {
      FinishComposition();
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      FinishComposition();
    }
    ImGui::TextColored(kTextMuted, "ENTER to send   ESC to close");
  }
  ImGui::End();
}

}  // namespace gta4::input
