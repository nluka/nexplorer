#include "stdafx.hpp"
#include "data_types.hpp"
#include "common_fns.hpp"
#include "imgui_specific.hpp"
#include "path.hpp"

#undef min
#undef max

static u64 const MAX_RECENT_FILES = 100;
static circular_buffer<recent_file> s_recent_files = circular_buffer<recent_file>(MAX_RECENT_FILES);
static std::mutex s_recent_files_mutex = {};

circular_buffer<recent_file> &global_state::recent_files() noexcept { return s_recent_files; }

HRESULT progress_sink::FinishOperations(HRESULT)
{
    print_debug_msg("FinishOperations");

    if (this->contains_delete_operations) {
        {
            std::scoped_lock lock(s_recent_files_mutex);

            s_recent_files.erase(std::remove_if(s_recent_files.begin(), s_recent_files.end(), [](recent_file &recent_file) {
                wchar_t recent_file_path_utf16[MAX_PATH];

                s32 written = utf8_to_utf16(recent_file.path.data(), recent_file_path_utf16, lengthof(recent_file_path_utf16));

                if (written == 0) {
                    // TODO: error
                    return false;
                } else {
                    return !PathFileExistsW(recent_file_path_utf16);
                }
            }));
        }

        global_state::save_recent_files_to_disk();
    }

    return S_OK;
}

u64 global_state::find_recent_file_idx(char const *search_path) noexcept
{
    auto &recent_files = global_state::recent_files();

    std::scoped_lock lock(s_recent_files_mutex);

    for (u64 i = 0; i < s_recent_files.size(); ++i) {
        auto const &recent_file = recent_files[i];
        if (path_loosely_same(recent_file.path.data(), search_path)) {
            return i;
        }
    }

    return u64(-1);
}

void global_state::move_recent_file_idx_to_front(u64 recent_file_idx, char const *new_action) noexcept
{
    auto &recent_files = global_state::recent_files();

    std::scoped_lock lock(s_recent_files_mutex);

    auto temp = recent_files[recent_file_idx];
    temp.action_time = current_time_system();
    if (new_action) {
        temp.action.clear();
        temp.action = new_action;
    }

    recent_files.erase(recent_files.begin() + recent_file_idx);
    recent_files.push_front(temp);
}

void global_state::add_recent_file(char const *action, char const *full_file_path) noexcept
{
    auto &recent_files = global_state::recent_files();
    swan_path_t path = path_create(full_file_path);

    std::scoped_lock lock(s_recent_files_mutex);
    recent_files.push_front({ action, current_time_system(), path });
}

void global_state::remove_recent_file(u64 recent_file_idx) noexcept
{
    auto &recent_files = global_state::recent_files();

    std::scoped_lock lock(s_recent_files_mutex);

    recent_files.erase(recent_files.begin() + recent_file_idx);
}

bool global_state::save_recent_files_to_disk() noexcept
try {
    std::filesystem::path full_path = global_state::execution_path() / "data\\recent_files.txt";

    std::ofstream iss(full_path);

    if (!iss) {
        return false;
    }

    auto const &recent_files = global_state::recent_files();

    std::scoped_lock lock(s_recent_files_mutex);

    for (auto const &file : recent_files) {
        auto time_t = std::chrono::system_clock::to_time_t(file.action_time);
        std::tm tm = *std::localtime(&time_t);

        iss << file.action.size() << ' '
            << file.action.c_str() << ' '
            << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << ' '
            << path_length(file.path) << ' '
            << file.path.data() << '\n';
    }

    print_debug_msg("SUCCESS global_state::save_recent_files_to_disk");
    return true;
}
catch (...) {
    print_debug_msg("FAILED global_state::save_recent_files_to_disk");
    return false;
}

std::pair<bool, u64> global_state::load_recent_files_from_disk() noexcept
try {
    std::filesystem::path full_path = global_state::execution_path() / "data\\recent_files.txt";

    std::ifstream in(full_path);

    if (!in) {
        return { false, 0 };
    }

    auto &recent_files = global_state::recent_files();

    std::scoped_lock lock(s_recent_files_mutex);

    recent_files.clear();

    std::string line = {};
    line.reserve(global_state::page_size() - 1);

    u64 num_loaded_successfully = 0;

    while (std::getline(in, line)) {
        std::istringstream iss(line);

        u64 stored_action_len = 0;
        u64 stored_path_len = 0;
        swan_path_t stored_path = {};

        iss >> stored_action_len;
        iss.ignore(1);

        char buffer[recent_file::ACTION_MAX_LEN + 1] = {};
        iss.read(buffer, std::min(stored_action_len, lengthof(buffer) - 1));
        iss.ignore(1);

        std::tm tm = {};
        iss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        system_time_point_t stored_time = std::chrono::system_clock::from_time_t(std::mktime(&tm));

        iss.ignore(1);
        iss >> (u64 &)stored_path_len;

        iss.ignore(1);
        iss.read(stored_path.data(), stored_path_len);

        recent_files.push_back();
        recent_files.back().action = buffer;
        recent_files.back().action_time = stored_time;
        recent_files.back().path = stored_path;

        ++num_loaded_successfully;

        line.clear();
    }

    print_debug_msg("SUCCESS global_state::load_recent_files_from_disk, loaded %zu files", num_loaded_successfully);
    return { true, num_loaded_successfully };
}
catch (...) {
    print_debug_msg("FAILED global_state::load_recent_files_from_disk");
    return { false, 0 };
}

void swan_windows::render_recent_files(bool &open) noexcept
{
    if (!imgui::Begin(swan_windows::get_name(swan_windows::recent_files), &open)) {
        imgui::End();
        return;
    }

    if (imgui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
        global_state::save_focused_window(swan_windows::recent_files);
    }

    auto &recent_files = global_state::recent_files();
    system_time_point_t current_time = current_time_system();
    u64 remove_idx = u64(-1);
    u64 move_to_front_idx = u64(-1);

    if (imgui::BeginTable("recent_files", 4, ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_BordersV|ImGuiTableFlags_Hideable|ImGuiTableFlags_Reorderable|ImGuiTableFlags_Resizable)) {
        imgui::TableSetupColumn("#");
        imgui::TableSetupColumn("When");
        imgui::TableSetupColumn("File Name");
        imgui::TableSetupColumn("Full Path");
        imgui::TableHeadersRow();

        std::scoped_lock lock(s_recent_files_mutex);

        u64 n = 0;
        for (auto &file : recent_files) {
            char *file_name = get_file_name(file.path.data());
            char const *full_path = file.path.data();
            auto directory = get_everything_minus_file_name(full_path);
            swan_path_t file_directory = path_create(directory.data(), directory.size());

            bool left_clicked = false;
            bool right_clicked = false;

            imgui::TableNextColumn();
            imgui::Text("%zu", ++n);

            imgui::TableNextColumn();
            {
                auto when = compute_when_str(file.action_time, current_time);
                imgui::Text("%s %s", file.action.c_str(), when.data());
            }

            imgui::TableNextColumn();
            {
                file_name_extension_splitter splitter(file_name);
                imgui::TextColored(get_color(basic_dirent::kind::file), get_icon_for_extension(splitter.ext));
            }
            imgui::SameLine();
            {
                char buffer[2048];
                (void) snprintf(buffer, lengthof(buffer), "%s##recent_file_%zu", file_name, n);
                imgui::Selectable(buffer, &left_clicked, ImGuiSelectableFlags_SpanAllColumns);
                right_clicked = imgui::IsItemClicked(ImGuiMouseButton_Right);
            }

            imgui::TableNextColumn();
            {
                char buffer[2048];
                (void) snprintf(buffer, lengthof(buffer), "%s##recent_file_%zu", full_path, n);
                imgui::Selectable(buffer, &left_clicked, ImGuiSelectableFlags_SpanAllColumns);
                right_clicked = imgui::IsItemClicked(ImGuiMouseButton_Right);
            }

            if (left_clicked) {
                print_debug_msg("left clicked recent file #%zu [%s]", n, full_path);

                auto res = open_file(file_name, file_directory.data());

                if (res.success) {
                    move_to_front_idx = n-1;
                } else {
                    swan_popup_modals::open_error(make_str("Open file [%s].", full_path).c_str(), res.error_or_utf8_path.c_str());
                    remove_idx = n-1;
                }
            }
            if (right_clicked) {
                auto &expl = global_state::explorers()[0];

                wchar_t file_path_utf16[MAX_PATH];
                s32 written = utf8_to_utf16(full_path, file_path_utf16, lengthof(file_path_utf16));

                if (written == 0) {
                    // TODO: handle error
                }
                else {
                    auto file_exists = PathFileExistsW(file_path_utf16);

                    if (!file_exists) {
                        swan_popup_modals::open_error(make_str("Open file location [%s].", full_path).c_str(), "File not found.");
                        remove_idx = n-1;
                    }
                    else {
                        expl.deselect_all_cwd_entries();
                        swan_path_t to_select = path_create(file_name);
                        expl.entries_to_select.push_back(to_select);

                        bool file_directory_exists = expl.update_cwd_entries(full_refresh, file_directory.data());

                        if (!file_directory_exists) {
                            swan_popup_modals::open_error(make_str("Open file location [%s].", full_path).c_str(),
                                                          make_str("Directory [%s] not found.", file_directory.data()).c_str());
                            remove_idx = n-1;
                        }
                        else {
                            expl.cwd = path_create(file_directory.data());
                            expl.push_history_item(expl.cwd);
                            (void) expl.save_to_disk();
                            expl.scroll_to_nth_selected_entry_next_frame = 0;
                            global_state::settings().show.explorer_0 = true;
                            imgui::SetWindowFocus(expl.name);
                        }
                    }
                }
            }
        }
        imgui::EndTable();
    }

    if (!recent_files.empty()) {
        if (imgui::Button("Clear##recent_files")) {
            std::scoped_lock lock(s_recent_files_mutex);
            recent_files.clear();
            (void) global_state::save_recent_files_to_disk();
        }
    }

    if (remove_idx != u64(-1)) {
        (void) global_state::remove_recent_file(remove_idx);
        (void) global_state::save_recent_files_to_disk();
    }
    if (move_to_front_idx != u64(-1)) {
        global_state::move_recent_file_idx_to_front(move_to_front_idx, "Opened");
        (void) global_state::save_recent_files_to_disk();
    }

    imgui::End();
}
