#include "stdafx.hpp"
#include "common_fns.hpp"
#include "imgui_specific.hpp"

namespace error_modal_global_state
{
    static bool         g_open = false;
    static std::string  g_action = {};
    static std::string  g_failure = {};
}

void swan_popup_modals::open_error(char const *action, char const *failure, bool beautify_action, bool beautify_failure) noexcept
{
    using namespace error_modal_global_state;

    g_open = true;

    assert(action != nullptr);
    g_action = action;

    assert(failure != nullptr);
    assert(strlen(failure) > 0);
    g_failure = failure;

    if (beautify_action) {
        if (!g_action.empty()) {
            // capitalize first letter
            g_action.front() = (char)toupper(g_action.front());

            // ensure ends with period
            if (g_action.back() != '.') {
                g_action.push_back('.');
            }
        }
    }

    if (beautify_failure) {
        // capitalize first letter
        g_failure.front() = (char)toupper(g_failure.front());

        // ensure ends with period
        if (g_failure.back() != '.') {
            g_failure.push_back('.');
        }
    }
}

bool swan_popup_modals::is_open_error() noexcept
{
    using namespace error_modal_global_state;

    return g_open;
}

void swan_popup_modals::render_error() noexcept
{
    using namespace error_modal_global_state;

    if (g_open) {
        imgui::OpenPopup(swan_popup_modals::error);
    }
    if (!imgui::BeginPopupModal(swan_popup_modals::error, &g_open)) {
        return;
    }

    assert(!g_failure.empty());

    auto cleanup_and_close_popup = [&]() noexcept {
        g_open = false;
        g_action.clear();
        g_failure.clear();

        imgui::CloseCurrentPopup();
    };

    {
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        SCOPE_EXIT { imgui::PopTextWrapPos(); };

        if (g_action.empty()) {
            imgui::TextColored(red(), "%s", g_failure.c_str());
        }
        else {
            imgui::TextUnformatted("Failed:");
            imgui::TextColored(orange(), "%s", g_action.c_str());
            imgui::TextUnformatted("Reason:");
            imgui::TextColored(red(), "%s", g_failure.c_str());
        }
    }

    if (imgui::IsWindowFocused() && imgui::IsKeyPressed(ImGuiKey_Escape)) {
        cleanup_and_close_popup();
    }

    imgui::EndPopup();
}
