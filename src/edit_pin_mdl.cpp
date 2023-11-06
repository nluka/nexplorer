#include "imgui/imgui.h"
#include "common.hpp"
#include "imgui_specific.hpp"

namespace imgui = ImGui;

static bool s_edit_pin_open = false;
static pinned_path *s_edit_pin = nullptr;

char const *swan_id_edit_pin_popup_modal() noexcept
{
    return "Edit Pin";
}

void swan_open_popup_modal_edit_pin(pinned_path *pin) noexcept
{
    s_edit_pin_open = true;

    assert(pin != nullptr);
    s_edit_pin = pin;
}

bool swan_is_popup_modal_open_edit_pin() noexcept
{
    return s_edit_pin_open;
}

void swan_render_popup_modal_edit_pin() noexcept
{
    if (s_edit_pin_open) {
        imgui::OpenPopup(swan_id_edit_pin_popup_modal());
    }
    if (!imgui::BeginPopupModal(swan_id_edit_pin_popup_modal(), nullptr)) {
        return;
    }

    assert(s_edit_pin != nullptr);

    static char label_input[pinned_path::LABEL_MAX_LEN + 1] = {};
    static swan_path_t path_input = {};
    static ImVec4 color_input = dir_color();
    static std::string err_msg = {};

    auto cleanup_and_close_popup = [&]() {
        s_edit_pin_open = false;
        s_edit_pin = nullptr;

        init_empty_cstr(label_input);
        init_empty_cstr(path_input.data());
        err_msg.clear();

        imgui::CloseCurrentPopup();
    };

    imgui::ColorEdit4("Edit Color##pin", &color_input.x, ImGuiColorEditFlags_NoAlpha|ImGuiColorEditFlags_NoInputs|ImGuiColorEditFlags_NoLabel);
    imgui::SameLine();
    imgui::TextColored(color_input, "Color");

    imgui::Spacing();

    if (imgui::IsWindowAppearing() && !imgui::IsAnyItemActive() && !imgui::IsMouseClicked(0)) {
        // set initial focus on label input
        imgui::SetKeyboardFocusHere(0);

        // init input fields
        color_input = s_edit_pin->color;
        path_input = s_edit_pin->path;
        strncpy(label_input, s_edit_pin->label.c_str(), lengthof(label_input));
    }
    {
        [[maybe_unused]] imgui_scoped_avail_width width(imgui::CalcTextSize(" 00/64").x);

        if (imgui::InputTextWithHint("##pin_label", "Label...", label_input, lengthof(label_input))) {
            err_msg.clear();
        }
    }
    imgui::SameLine();
    imgui::Text("%zu/%zu", strlen(label_input), pinned_path::LABEL_MAX_LEN);

    imgui::Spacing();

    {
        [[maybe_unused]] imgui_scoped_avail_width width = {};

        if (imgui::InputTextWithHint("##pin_path", "Path...", path_input.data(), path_input.size(),
                                     ImGuiInputTextFlags_CallbackCharFilter, filter_chars_callback, (void *)windows_illegal_path_chars()))
        {
            err_msg.clear();
        }
    }

    imgui::Spacing();
    imgui::Spacing();

    if (imgui::Button("Save##pin") && !strempty(path_input.data()) && !strempty(label_input)) {
        swan_path_t path = path_squish_adjacent_separators(path_input);
        path_force_separator(path, get_explorer_options().dir_separator_utf8());

        s_edit_pin->color = color_input;
        s_edit_pin->label = label_input;
        s_edit_pin->path = path;

        bool success = save_pins_to_disk();
        debug_log("save_pins_to_disk: %d", success);

        cleanup_and_close_popup();
    }

    imgui::SameLine();

    if (imgui::Button("Cancel##pin")) {
        cleanup_and_close_popup();
    }

    if (!err_msg.empty()) {
        imgui::Spacing();
        imgui::TextColored(red(), "Error: %s", err_msg.c_str());
    }

    if (imgui::IsWindowFocused() && imgui::IsKeyPressed(ImGuiKey_Escape)) {
        cleanup_and_close_popup();
    }

    imgui::EndPopup();
}
