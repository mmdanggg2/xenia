/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/imgui/imgui.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/window.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

std::atomic<int> xam_dialogs_shown_ = {0};

dword_result_t XamIsUIActive() {
  XELOGD("XamIsUIActive()");
  return xam_dialogs_shown_ > 0 ? 1 : 0;
}
DECLARE_XAM_EXPORT(XamIsUIActive, ExportTag::kImplemented);

class MessageBoxDialog : public xe::ui::ImGuiDialog {
 public:
  MessageBoxDialog(xe::ui::Window* window, std::wstring title,
                   std::wstring description, std::vector<std::wstring> buttons,
                   uint32_t default_button, uint32_t* out_chosen_button)
      : ImGuiDialog(window),
        title_(xe::to_string(title)),
        description_(xe::to_string(description)),
        buttons_(std::move(buttons)),
        default_button_(default_button),
        out_chosen_button_(out_chosen_button) {
    if (out_chosen_button) {
      *out_chosen_button = default_button;
    }
  }

  void OnDraw(ImGuiIO& io) override {
    bool first_draw = false;
    if (!has_opened_) {
      ImGui::OpenPopup(title_.c_str());
      has_opened_ = true;
      first_draw = true;
    }
    if (ImGui::BeginPopupModal(title_.c_str(), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("%s", description_.c_str());
      if (first_draw) {
        ImGui::SetKeyboardFocusHere();
      }
      for (size_t i = 0; i < buttons_.size(); ++i) {
        auto button_name = xe::to_string(buttons_[i]);
        if (ImGui::Button(button_name.c_str())) {
          if (out_chosen_button_) {
            *out_chosen_button_ = static_cast<uint32_t>(i);
          }
          ImGui::CloseCurrentPopup();
          Close();
        }
        ImGui::SameLine();
      }
      ImGui::Spacing();
      ImGui::Spacing();
      ImGui::EndPopup();
    } else {
      Close();
    }
  }

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string description_;
  std::vector<std::wstring> buttons_;
  uint32_t default_button_ = 0;
  uint32_t* out_chosen_button_ = nullptr;
};

// http://www.se7ensins.com/forums/threads/working-xshowmessageboxui.844116/?jdfwkey=sb0vm
dword_result_t XamShowMessageBoxUI(dword_t user_index, lpwstring_t title,
                                   lpwstring_t text, dword_t button_count,
                                   lpdword_t button_ptrs, dword_t active_button,
                                   dword_t flags, lpdword_t result_ptr,
                                   lpvoid_t overlapped_ptr) {
  std::vector<std::wstring> buttons;
  std::wstring all_buttons;
  for (uint32_t j = 0; j < button_count; ++j) {
    uint32_t button_ptr = button_ptrs[j];
    auto button = xe::load_and_swap<std::wstring>(
        kernel_state()->memory()->TranslateVirtual(button_ptr));
    all_buttons.append(button);
    if (j + 1 < button_count) {
      all_buttons.append(L" | ");
    }
    buttons.push_back(button);
  }

  XELOGD(
      "XamShowMessageBoxUI(%d, %.8X(%S), %.8X(%S), %d, %.8X(%S), %d, %X, %.8X, "
      "%.8X)",
      user_index, title, !title ? L"" : title.value().c_str(), text,
      text.value().c_str(), button_count, button_ptrs, all_buttons.c_str(),
      active_button, flags, result_ptr, overlapped_ptr);

  uint32_t chosen_button;
  if (FLAGS_headless) {
    // Auto-pick the focused button.
    chosen_button = active_button;
  } else {
    auto display_window = kernel_state()->emulator()->display_window();
    xe::threading::Fence fence;
    display_window->loop()->PostSynchronous([&]() {
      // TODO(benvanik): setup icon states.
      switch (flags & 0xF) {
        case 0:
          // config.pszMainIcon = nullptr;
          break;
        case 1:
          // config.pszMainIcon = TD_ERROR_ICON;
          break;
        case 2:
          // config.pszMainIcon = TD_WARNING_ICON;
          break;
        case 3:
          // config.pszMainIcon = TD_INFORMATION_ICON;
          break;
      }
      (new MessageBoxDialog(display_window, !title ? L"" : title.value(),
                            text.value(), buttons, active_button,
                            &chosen_button))
          ->Then(&fence);
    });
    ++xam_dialogs_shown_;
    fence.Wait();
    --xam_dialogs_shown_;
  }
  *result_ptr = chosen_button;

  kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT(XamShowMessageBoxUI, ExportTag::kImplemented);

class KeyboardInputDialog : public xe::ui::ImGuiDialog {
 public:
  KeyboardInputDialog(xe::ui::Window* window, std::wstring title,
                      std::wstring description, std::wstring default_text,
                      std::wstring* out_text, size_t max_length)
      : ImGuiDialog(window),
        title_(xe::to_string(title)),
        description_(xe::to_string(description)),
        default_text_(xe::to_string(default_text)),
        out_text_(out_text),
        max_length_(max_length) {
    if (out_text_) {
      *out_text_ = default_text;
    }
    text_buffer_.resize(max_length);
    std::strncpy(text_buffer_.data(), default_text_.c_str(),
                 std::min(text_buffer_.size() - 1, default_text_.size()));
  }

  void OnDraw(ImGuiIO& io) override {
    bool first_draw = false;
    if (!has_opened_) {
      ImGui::OpenPopup(title_.c_str());
      has_opened_ = true;
      first_draw = true;
    }
    if (ImGui::BeginPopupModal(title_.c_str(), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextWrapped("%s", description_.c_str());
      if (first_draw) {
        ImGui::SetKeyboardFocusHere();
      }
      if (ImGui::InputText("##body", text_buffer_.data(), text_buffer_.size(),
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (out_text_) {
          *out_text_ = xe::to_wstring(text_buffer_.data());
        }
        ImGui::CloseCurrentPopup();
        Close();
      }
      if (ImGui::Button("OK")) {
        if (out_text_) {
          *out_text_ = xe::to_wstring(text_buffer_.data());
        }
        ImGui::CloseCurrentPopup();
        Close();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
        Close();
      }
      ImGui::Spacing();
      ImGui::EndPopup();
    } else {
      Close();
    }
  }

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string description_;
  std::string default_text_;
  std::wstring* out_text_ = nullptr;
  std::vector<char> text_buffer_;
  size_t max_length_ = 0;
};

// http://www.se7ensins.com/forums/threads/release-how-to-use-xshowkeyboardui-release.906568/
dword_result_t XamShowKeyboardUI(dword_t user_index, dword_t flags,
                                 lpwstring_t default_text, lpwstring_t title,
                                 lpwstring_t description, lpwstring_t buffer,
                                 dword_t buffer_length,
                                 pointer_t<XAM_OVERLAPPED> overlapped) {
  if (!buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (FLAGS_headless) {
    // Redirect default_text back into the buffer.
    std::memset(buffer, 0, buffer_length * 2);
    if (default_text) {
      xe::store_and_swap<std::wstring>(buffer, default_text.value());
    }

    if (overlapped) {
      kernel_state()->CompleteOverlappedImmediate(overlapped, X_ERROR_SUCCESS);
      return X_ERROR_IO_PENDING;
    } else {
      return X_ERROR_SUCCESS;
    }
  }

  std::wstring out_text;

  auto display_window = kernel_state()->emulator()->display_window();
  xe::threading::Fence fence;
  display_window->loop()->PostSynchronous([&]() {
    (new KeyboardInputDialog(display_window, title ? title.value() : L"",
                             description ? description.value() : L"",
                             default_text ? default_text.value() : L"",
                             &out_text, buffer_length))
        ->Then(&fence);
  });
  ++xam_dialogs_shown_;
  fence.Wait();
  --xam_dialogs_shown_;

  // Zero the output buffer.
  std::memset(buffer, 0, buffer_length * 2);

  // Truncate the string.
  out_text = out_text.substr(0, buffer_length - 1);
  xe::store_and_swap<std::wstring>(buffer, out_text);

  if (overlapped) {
    kernel_state()->CompleteOverlappedImmediate(overlapped, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  } else {
    return X_ERROR_SUCCESS;
  }
}
DECLARE_XAM_EXPORT(XamShowKeyboardUI, ExportTag::kImplemented);

dword_result_t XamShowDeviceSelectorUI(dword_t user_index, dword_t content_type,
                                       dword_t content_flags,
                                       qword_t total_requested,
                                       lpdword_t device_id_ptr,
                                       pointer_t<XAM_OVERLAPPED> overlapped) {
  // NOTE: 0xF00D0000 magic from xam_content.cc
  switch (content_type) {
    case 1:  // save game
      *device_id_ptr = 0xF00D0000 | 0x0001;
      break;
    case 2:  // marketplace
      *device_id_ptr = 0xF00D0000 | 0x0002;
      break;
    case 3:  // title/publisher update?
      *device_id_ptr = 0xF00D0000 | 0x0003;
      break;
    default:
      assert_unhandled_case(content_type);
      *device_id_ptr = 0xF00D0000 | 0x0001;
      break;
  }

  if (overlapped) {
    kernel_state()->CompleteOverlappedImmediate(overlapped, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  } else {
    return X_ERROR_SUCCESS;
  }
}
DECLARE_XAM_EXPORT(XamShowDeviceSelectorUI, ExportTag::kImplemented);

dword_result_t XamShowDirtyDiscErrorUI(dword_t user_index) {
  XELOGD("XamShowDirtyDiscErrorUI(%d)", user_index);

  if (FLAGS_headless) {
    assert_always();
    exit(1);
    return X_ERROR_SUCCESS;
  }

  auto display_window = kernel_state()->emulator()->display_window();
  xe::threading::Fence fence;
  display_window->loop()->PostSynchronous([&]() {
    xe::ui::ImGuiDialog::ShowMessageBox(
        display_window, "Disc Read Error",
        "There's been an issue reading content from the game disc.\nThis is "
        "likely caused by bad or unimplemented file IO calls.")
        ->Then(&fence);
  });
  ++xam_dialogs_shown_;
  fence.Wait();
  --xam_dialogs_shown_;

  // This is death, and should never return.
  // TODO(benvanik): cleaner exit.
  exit(1);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT(XamShowDirtyDiscErrorUI, ExportTag::kImplemented);

void RegisterUIExports(xe::cpu::ExportResolver* export_resolver,
                       KernelState* kernel_state) {}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
