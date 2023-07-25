#ifndef SWAN_EXPLORER_WINDOW_CPP
#define SWAN_EXPLORER_WINDOW_CPP

#include <regex>
#include <fstream>

#include <windows.h>
#include <shlwapi.h>
#include <shlguid.h>
#include <shobjidl.h>
#include <shlobj_core.h>
#include <timezoneapi.h>
#include <datetimeapi.h>

#include "imgui/imgui.h"

#include "primitives.hpp"
#include "on_scope_exit.hpp"
#include "common.hpp"
#include "scoped_timer.hpp"
#include "util.hpp"
#include "path.hpp"

#if defined(NDEBUG)
#   define MAX_EXPLORER_WD_HISTORY 100
#else
#   define MAX_EXPLORER_WD_HISTORY 5 // something small for easier debugging
#endif

using namespace swan;

struct paste_payload
{
    struct item
    {
        u64 size;
        basic_dir_ent::kind type;
        path_t path;
    };

    char const *window_name = nullptr;
    std::vector<item> items = {};
    // false indicates a cut, true indicates a copy
    bool keep_src = {};
};

static IShellLinkA *s_shell_link = nullptr;
static IPersistFile *s_persist_file_interface = nullptr;
static paste_payload s_paste_payload = {};

static
std::pair<i32, std::array<char, 64>> filetime_to_string(FILETIME *time) noexcept(true)
{
    std::array<char, 64> buffer = {};
    DWORD flags = FDTF_LONGDATE | FDTF_LONGTIME | FDTF_RELATIVE | FDTF_LTRDATE;
    i32 length = SHFormatDateTimeA(time, &flags, buffer.data(), (u32)buffer.size());

    // for some reason, SHFormatDateTimeA will pad parts of the string with ASCII 63 (?)
    // when using LONGDATE or LONGTIME, so we will simply convert them to spaces
    std::replace(buffer.begin(), buffer.end(), '?', ' ');

    return { length, buffer };
}

enum cwd_entries_table_col_id : ImGuiID
{
    cwd_entries_table_col_number,
    cwd_entries_table_col_id,
    cwd_entries_table_col_path,
    cwd_entries_table_col_type,
    cwd_entries_table_col_size_pretty,
    cwd_entries_table_col_size_bytes,
    // cwd_entries_table_col_creation_time,
    cwd_entries_table_col_last_write_time,
    cwd_entries_table_col_count
};

static
void sort_cwd_entries(explorer_window &expl, ImGuiTableSortSpecs *sort_specs)
{
    assert(sort_specs != nullptr);

    scoped_timer<timer_unit::MICROSECONDS> sort_timer(&expl.sort_us);

    // needs to return true when left < right
    auto compare = [&](explorer_window::dir_ent const &left, explorer_window::dir_ent const &right) {
        bool left_lt_right = false;

        for (i32 i = 0; i < sort_specs->SpecsCount; ++i) {
            auto const &sort_spec = sort_specs->Specs[i];

            // comparing with this variable using == will handle the sort direction
            bool direction_flipper = sort_spec.SortDirection == ImGuiSortDirection_Ascending ? false : true;

            switch (sort_spec.ColumnUserID) {
                default:
                case cwd_entries_table_col_id: {
                    left_lt_right = (left.basic.id < right.basic.id) == direction_flipper;
                    break;
                }
                case cwd_entries_table_col_path: {
                    left_lt_right = (lstrcmpiA(left.basic.path.data(), right.basic.path.data()) < 0) == direction_flipper;
                    break;
                }
                case cwd_entries_table_col_type: {
                    auto compute_precedence = [](explorer_window::dir_ent const &ent) -> u32 {
                        // lower items (and thus higher values) have greater precedence
                        enum class precedence : u32
                        {
                            everything_else,
                            symlink,
                            directory,
                        };

                        if      (ent.basic.is_directory()) return (u32)precedence::directory;
                        else if (ent.basic.is_symlink())   return (u32)precedence::symlink;
                        else                               return (u32)precedence::everything_else;
                    };

                    u32 left_precedence = compute_precedence(left);
                    u32 right_precedence = compute_precedence(right);

                    left_lt_right = (left_precedence > right_precedence) == direction_flipper;
                    break;
                }
                case cwd_entries_table_col_size_pretty:
                case cwd_entries_table_col_size_bytes: {
                    left_lt_right = (left.basic.size < right.basic.size) == direction_flipper;
                    break;
                }
                // case cwd_entries_table_col_creation_time: {
                //     i32 cmp = CompareFileTime(&left.creation_time_raw, &right.creation_time_raw);
                //     left_lt_right = (cmp <= 0) == direction_flipper;
                //     break;
                // }
                case cwd_entries_table_col_last_write_time: {
                    i32 cmp = CompareFileTime(&left.basic.last_write_time_raw, &right.basic.last_write_time_raw);
                    left_lt_right = (cmp <= 0) == direction_flipper;
                    break;
                }
            }
        }

        return left_lt_right;
    };

    std::sort(expl.cwd_entries.begin(), expl.cwd_entries.end(), compare);
}

bool update_cwd_entries(
    u8 actions,
    explorer_window *expl_ptr,
    std::string_view parent_dir,
    explorer_options const &opts,
    std::source_location sloc)
{
    debug_log("[%s] update_cwd_entries() called from [%s:%d]",
        expl_ptr->name, get_just_file_name(sloc.file_name()), sloc.line());

    scoped_timer<timer_unit::MICROSECONDS> function_timer(&expl_ptr->update_cwd_entries_total_us);

    IM_ASSERT(expl_ptr != nullptr);

    bool parent_dir_exists = false;

    char dir_sep = opts.dir_separator();

    explorer_window &expl = *expl_ptr;
    expl.needs_initial_sort = true;
    expl.update_cwd_entries_total_us = 0;
    expl.update_cwd_entries_searchpath_setup_us = 0;
    expl.update_cwd_entries_filesystem_us = 0;
    expl.update_cwd_entries_filter_us = 0;
    expl.update_cwd_entries_regex_ctor_us = 0;

    if (actions & query_filesystem) {
        static std::vector<path_t> selected_entries = {};
        selected_entries.clear();

        for (auto const &dir_ent : expl.cwd_entries) {
            if (dir_ent.is_selected) {
                selected_entries.push_back(dir_ent.basic.path);
            }
        }

        expl.cwd_entries.clear();

        // this seems inefficient but was measured to be faster than the "efficient" way,
        // or maybe both are so fast that it doesn't matter...
        while (parent_dir.ends_with(' ')) {
            parent_dir = std::string_view(parent_dir.data(), parent_dir.size() - 1);
        }

        if (parent_dir != "") {
            static std::string search_path{};
            {
                scoped_timer<timer_unit::MICROSECONDS> search_path_timer(&expl.update_cwd_entries_searchpath_setup_us);

                search_path.reserve(parent_dir.size() + 2);
                search_path = parent_dir;

                if (!search_path.ends_with(dir_sep)) {
                    search_path += dir_sep;
                }
                search_path += '*';
            }

            debug_log("[%s] querying filesystem, search_path = [%s]", expl.name, search_path.c_str());

            scoped_timer<timer_unit::MICROSECONDS> filesystem_timer(&expl.update_cwd_entries_filesystem_us);

            WIN32_FIND_DATAA find_data;
            HANDLE find_handle = FindFirstFileA(search_path.data(), &find_data);

            auto find_handle_cleanup_routine = make_on_scope_exit([&find_handle] { FindClose(find_handle); });

            if (find_handle == INVALID_HANDLE_VALUE) {
                debug_log("[%s] find_handle == INVALID_HANDLE_VALUE", expl.name);
                parent_dir_exists = false;
                return parent_dir_exists;
            } else {
                parent_dir_exists = true;
            }

            u32 id = 0;

            do {
                explorer_window::dir_ent entry = {};
                entry.basic.id = id;
                std::strncpy(entry.basic.path.data(), find_data.cFileName, entry.basic.path.size());
                entry.basic.size = two_u32_to_one_u64(find_data.nFileSizeLow, find_data.nFileSizeHigh);
                entry.basic.creation_time_raw = find_data.ftCreationTime;
                entry.basic.last_write_time_raw = find_data.ftLastWriteTime;

                if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    entry.basic.type = basic_dir_ent::kind::directory;
                }
                else if (path_ends_with(entry.basic.path, ".lnk")) {
                    entry.basic.type = basic_dir_ent::kind::symlink;
                }
                else {
                    entry.basic.type = basic_dir_ent::kind::file;
                }

                if (path_equals_exactly(entry.basic.path, ".")) {
                    continue;
                }

                if (path_equals_exactly(entry.basic.path, "..")) {
                    if (opts.show_dotdot_dir) {
                        expl.cwd_entries.emplace_back(entry);
                        std::swap(expl.cwd_entries.back(), expl.cwd_entries.front());
                    }
                } else {
                    for (auto const &prev_selected_entry : selected_entries) {
                        bool was_selected_before_refresh = path_strictly_same(entry.basic.path, prev_selected_entry);
                        if (was_selected_before_refresh) {
                            entry.is_selected = true;
                        }
                    }

                    expl.cwd_entries.emplace_back(entry);
                }

                ++expl.num_file_finds;
                ++id;
            }
            while (FindNextFileA(find_handle, &find_data));
        }
    }

    if (actions & filter) {
        scoped_timer<timer_unit::MICROSECONDS> filter_timer(&expl.update_cwd_entries_filter_us);

        expl.filter_error.clear();

        for (auto &dir_ent : expl.cwd_entries) {
            dir_ent.is_filtered_out = false;
        }

        bool filter_is_blank = expl.filter.data()[0] == '\0';

        if (!filter_is_blank) {
            switch (expl.filter_mode) {
                default:
                case explorer_window::filter_mode::contains: {
                    auto matcher = expl.filter_case_sensitive ? StrStrA : StrStrIA;

                    for (auto &dir_ent : expl.cwd_entries) {
                        dir_ent.is_filtered_out = !matcher(dir_ent.basic.path.data(), expl.filter.data());
                    }
                    break;
                }

                case explorer_window::filter_mode::regex: {
                    static std::regex filter_regex;
                    try {
                        scoped_timer<timer_unit::MICROSECONDS> regex_ctor_timer(&expl.update_cwd_entries_regex_ctor_us);
                        filter_regex = expl.filter.data();
                    }
                    catch (std::exception const &except) {
                        debug_log("[%s] error constructing std::regex, %s", expl.name, except.what());
                        expl.filter_error = except.what();
                        break;
                    }

                    auto match_flags = std::regex_constants::match_default | (
                        std::regex_constants::icase * (expl.filter_case_sensitive == 0)
                    );

                    for (auto &dir_ent : expl.cwd_entries) {
                        dir_ent.is_filtered_out = !std::regex_match(
                            dir_ent.basic.path.data(),
                            filter_regex,
                            (std::regex_constants::match_flag_type)match_flags
                        );
                    }

                    break;
                }

                // case filter_mode::glob: {
                //     throw std::runtime_error("not implemented");
                //     break;
                // }
            }
        }
    }

    expl.last_refresh_time = current_time();

    return parent_dir_exists;
}

bool explorer_window::save_to_disk() const noexcept(true)
{
    scoped_timer<timer_unit::MICROSECONDS> save_to_disk_timer(&(this->save_to_disk_us));

    char file_name[32];
    snprintf(file_name, lengthof(file_name), "data/Explorer_%c.txt", this->name[9]);

    bool result = true;

    try {
        std::ofstream out(file_name);
        if (!out) {
            result = false;
        } else {
            out << "cwd " << path_length(cwd) << ' ' << cwd.data() << '\n';

            out << "filter " << strlen(filter.data()) << ' ' << filter.data() << '\n';

            out << "filter_mode " << (i32)filter_mode << '\n';

            out << "filter_case_sensitive " << (i32)filter_case_sensitive << '\n';

            out << "wd_history_pos " << wd_history_pos << '\n';

            out << "wd_history.size() " << wd_history.size() << '\n';

            for (auto const &item : wd_history) {
                out << path_length(item) << ' ' << item.data() << '\n';
            }
        }
    }
    catch (...) {
        result = false;
    }

    debug_log("[%s] save attempted, result: %d", file_name, result);
    return result;
}

bool explorer_window::load_from_disk(char dir_separator) noexcept(true)
{
    assert(this->name != nullptr);

    char file_name[32];
    snprintf(file_name, lengthof(file_name), "data/Explorer_%c.txt", this->name[9]);

    try {
        std::ifstream in(file_name);
        if (!in) {
            debug_log("failed to open file [%s]", file_name);
            return false;
        }

        char whitespace = 0;
        std::string what = {};
        what.reserve(256);

        {
            in >> what;
            assert(what == "cwd");

            u64 cwd_len = 0;
            in >> cwd_len;
            debug_log("[%s] cwd_len = %zu", file_name, cwd_len);

            in.read(&whitespace, 1);

            in.read(cwd.data(), cwd_len);
            path_force_separator(cwd, dir_separator);
            debug_log("[%s] cwd = [%s]", file_name, cwd.data());

            cwd_last_frame = cwd;
        }
        {
            in >> what;
            assert(what == "filter");

            u64 filter_len = 0;
            in >> filter_len;
            debug_log("[%s] filter_len = %zu", file_name, filter_len);

            in.read(&whitespace, 1);

            in.read(filter.data(), filter_len);
            debug_log("[%s] filter = [%s]", file_name, filter.data());
        }
        {
            in >> what;
            assert(what == "filter_mode");

            in >> (i32 &)filter_mode;
            debug_log("[%s] filter_mode = %d", file_name, filter_mode);
        }
        {
            in >> what;
            assert(what == "filter_case_sensitive");

            i32 val = 0;
            in >> val;

            filter_case_sensitive = (bool)val;
            debug_log("[%s] filter_case_sensitive = %d", file_name, filter_case_sensitive);
        }
        {
            in >> what;
            assert(what == "wd_history_pos");

            in >> wd_history_pos;
            debug_log("[%s] wd_history_pos = %zu", file_name, wd_history_pos);
        }

        u64 wd_history_size = 0;
        {
            in >> what;
            assert(what == "wd_history.size()");

            in >> wd_history_size;
            debug_log("[%s] wd_history_size = %zu", file_name, wd_history_size);
        }

        wd_history.resize(wd_history_size);
        for (u64 i = 0; i < wd_history_size; ++i) {
            u64 item_len = 0;
            in >> item_len;

            in.read(&whitespace, 1);

            in.read(wd_history[i].data(), item_len);
            path_force_separator(wd_history[i], dir_separator);
            debug_log("[%s] history[%zu] = [%s]", file_name, i, wd_history[i].data());
        }
    }
    catch (...) {
        return false;
    }

    return true;
}

void new_history_from(explorer_window &expl, path_t const &new_latest_entry)
{
    path_t new_latest_entry_clean;
    new_latest_entry_clean = new_latest_entry;
    path_pop_back_if(new_latest_entry_clean, '\\');
    path_pop_back_if(new_latest_entry_clean, '/');

    if (expl.wd_history.empty()) {
        expl.wd_history_pos = 0;
    }
    else {
        u64 num_trailing_history_items_to_del = expl.wd_history.size() - expl.wd_history_pos - 1;

        for (u64 i = 0; i < num_trailing_history_items_to_del; ++i) {
            expl.wd_history.pop_back();
        }

        if (MAX_EXPLORER_WD_HISTORY == expl.wd_history.size()) {
            expl.wd_history.pop_front();
        } else {
            ++expl.wd_history_pos;
        }
    }

    expl.wd_history.push_back(new_latest_entry_clean);
}

void try_ascend_directory(explorer_window &expl, explorer_options const &opts)
{
    auto &cwd = expl.cwd;

    char dir_separator = opts.dir_separator();

    // if there is a trailing separator, remove it
    path_pop_back_if(cwd, dir_separator);

    // remove anything between end and final separator
    while (path_pop_back_if_not(cwd, dir_separator));

    update_cwd_entries(full_refresh, &expl, expl.cwd.data(), opts);

    new_history_from(expl, expl.cwd);
    expl.cwd_prev_selected_dirent_idx = explorer_window::NO_SELECTION;
    expl.filter_error.clear();
}

void try_descend_to_directory(explorer_window &expl, char const *child_dir, explorer_options const &opts)
{
    path_t new_cwd = expl.cwd;
    char dir_separator = opts.dir_separator();

    if (path_append(expl.cwd, child_dir, dir_separator, true)) {
        if (PathCanonicalizeA(new_cwd.data(), expl.cwd.data())) {
            debug_log("[%s] PathCanonicalizeA success: new_cwd = [%s]", expl.name, new_cwd.data());

            update_cwd_entries(full_refresh, &expl, new_cwd.data(), opts);

            new_history_from(expl, new_cwd);
            expl.cwd = new_cwd;
            expl.cwd_prev_selected_dirent_idx = explorer_window::NO_SELECTION;
            expl.filter_error.clear();
        }
        else {
            debug_log("[%s] PathCanonicalizeA failed", expl.name);
        }
    }
    else {
        debug_log("[%s] path_append failed, new_cwd = [%s], append data = [%c%s]", expl.name, new_cwd.data(), dir_separator, child_dir);
        expl.cwd = new_cwd;
    }
}

struct cwd_text_input_callback_user_data
{
    explorer_window *expl_ptr;
    explorer_options *opts_ptr;
    wchar_t dir_separator_w;
};

i32 cwd_text_input_callback(ImGuiInputTextCallbackData *data)
{
    auto user_data = (cwd_text_input_callback_user_data *)(data->UserData);
    auto &expl = *user_data->expl_ptr;
    auto &opts = *user_data->opts_ptr;
    auto &cwd = user_data->expl_ptr->cwd;

    auto is_separator = [](wchar_t ch) { return ch == L'/' || ch == L'\\'; };

    if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        if (is_separator(data->EventChar)) {
            data->EventChar = user_data->dir_separator_w;
        }
        else {
            static wchar_t const *forbidden_chars = L"<>\"|?*";
            bool is_forbidden = StrChrW(forbidden_chars, data->EventChar);
            if (is_forbidden) {
                data->EventChar = L'\0';
            }
        }
    }
    else if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit) {
        debug_log("[%s] ImGuiInputTextFlags_CallbackEdit, data->Buf = [%s], cwd = [%s]", expl.name, data->Buf, cwd);

        auto const &new_cwd = data->Buf;

        bool cwd_exists_after_edit = update_cwd_entries(full_refresh, &expl, new_cwd, opts);

        if (cwd_exists_after_edit && !path_is_empty(expl.cwd)) {
            expl.cwd = path_create(data->Buf);

            if (path_is_empty(expl.prev_valid_cwd) || !path_loosely_same(expl.cwd, expl.prev_valid_cwd)) {
                new_history_from(expl, expl.cwd);
            }

            if (!path_loosely_same(expl.cwd, expl.prev_valid_cwd)) {
                expl.prev_valid_cwd = expl.cwd;
            }

            expl.latest_save_to_disk_result = (i8)expl.save_to_disk();
        }
    }

    return 0;
}

void render_explorer_window(explorer_window &expl, explorer_options &opts)
{
    if (!ImGui::Begin(expl.name)) {
        ImGui::End();
        return;
    }

    static ImVec4 const orange(1, 0.5f, 0, 1);
    static ImVec4 const red(1, 0.2f, 0, 1);

    auto &io = ImGui::GetIO();
    bool window_focused = ImGui::IsWindowFocused();

    bool cwd_exists_before_edit = directory_exists(expl.cwd.data());
    char dir_separator = opts.unix_directory_separator ? '/' : '\\';
    wchar_t dir_separator_w = opts.unix_directory_separator ? L'/' : L'\\';

    path_force_separator(expl.cwd, dir_separator);

    // handle enter key pressed on cwd entry
    if (window_focused && ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        if (explorer_window::NO_SELECTION == expl.cwd_prev_selected_dirent_idx) {
            debug_log("[%s] pressed enter but cwd_prev_selected_dirent_idx was NO_SELECTION", expl.name);
        } else {
            auto dirent_which_enter_pressed_on = expl.cwd_entries[expl.cwd_prev_selected_dirent_idx];
            debug_log("[%s] pressed enter on [%s]", expl.name, dirent_which_enter_pressed_on.basic.path.data());
            if (dirent_which_enter_pressed_on.basic.is_directory()) {
                try_descend_to_directory(expl, dirent_which_enter_pressed_on.basic.path.data(), opts);
            }
        }
    }

    // debug info start
    if (opts.show_debug_info) {
        ImGui::SeparatorText("DEBUG");
        ImGui::Spacing();

        auto calc_perc_total_time = [&expl](f64 time) {
            return time == 0.f
                ? 0.f
                : ( (time / expl.update_cwd_entries_total_us) * 100.f );
        };

        ImGui::Text("prev_valid_cwd = [%s]", expl.prev_valid_cwd.data());
        ImGui::Text("num_file_finds = %zu", expl.num_file_finds);
        ImGui::Text("cwd_prev_selected_dirent_idx = %lld", expl.cwd_prev_selected_dirent_idx);
        ImGui::Text("num_selected_cwd_entries = %zu", expl.num_selected_cwd_entries);
        ImGui::Text("latest_save_to_disk_result = %d", expl.latest_save_to_disk_result);
        ImGui::Text("sort_us = %.1lf", expl.sort_us);
        ImGui::Text("unpin_us = %.1lf", expl.unpin_us);
        ImGui::Text("save_to_disk_us = %.1lf", expl.save_to_disk_us);

        ImGui::Spacing();
        ImGui::SeparatorText("update_cwd_entries timings");
        ImGui::Spacing();

        ImGui::Text("total_us            : %9.1lf (%.1lf ms)",
             expl.update_cwd_entries_total_us,
             expl.update_cwd_entries_total_us / 1000.f);

        ImGui::Text("searchpath_setup_us : %9.1lf (%4.1lf %%)",
            expl.update_cwd_entries_searchpath_setup_us,
            calc_perc_total_time(expl.update_cwd_entries_searchpath_setup_us));

        ImGui::Text("filesystem_us       : %9.1lf (%4.1lf %%)",
            expl.update_cwd_entries_filesystem_us,
            calc_perc_total_time(expl.update_cwd_entries_filesystem_us));

        ImGui::Text("filter_us           : %9.1lf (%4.1lf %%)",
            expl.update_cwd_entries_filter_us,
            calc_perc_total_time(expl.update_cwd_entries_filter_us));

        ImGui::Text("regex_ctor_us       : %9.1lf",
            expl.update_cwd_entries_regex_ctor_us);

        ImGui::Spacing();
        ImGui::SeparatorText("DEBUG end");
    }
    // debug info end

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 15.f));
    if (ImGui::BeginTable("first_3_control_rows", 1, ImGuiTableFlags_SizingFixedFit)) {

        ImGui::TableNextColumn();

        // refresh button, ctrl-r refresh logic, automatic refreshing
        {
            bool refreshed = false; // to avoid refreshing twice in one frame

            auto refresh = [&]() {
                if (!refreshed) {
                    update_cwd_entries(full_refresh, &expl, expl.cwd.data(), opts);
                    refreshed = true;
                }
            };

            if (
                opts.ref_mode == explorer_options::refresh_mode::manual ||
                (opts.ref_mode == explorer_options::refresh_mode::adaptive && expl.cwd_entries.size() > opts.adaptive_refresh_threshold)
            ) {
                if (ImGui::Button("Refresh") && !refreshed) {
                    debug_log("[%s] refresh button pressed", expl.name);
                    refresh();
                }

                ImGui::SameLine();
                ImGui::Spacing();
                ImGui::SameLine();
                ImGui::Spacing();
                ImGui::SameLine();
            }
            else {
                if (!refreshed) {
                    // see if it's time for an automatic refresh

                    time_point_t now = current_time();

                    i64 diff_ms = compute_diff_ms(expl.last_refresh_time, now);

                    if (diff_ms >= max(explorer_options::min_tolerable_refresh_interval_ms, opts.auto_refresh_interval_ms)) {
                        refresh();
                    }
                }
            }

            if (window_focused && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R)) {
                debug_log("[%s] ctrl-r, refresh triggered", expl.name);
                refresh();
            }
        }
        // end of refresh button, ctrl-r refresh logic, automatic refreshing



        // pin cwd button start
        {
            u64 pin_idx;
            {
                scoped_timer<timer_unit::MICROSECONDS> check_if_pinned_timer(&expl.check_if_pinned_us);
                pin_idx = find_pin_idx(expl.cwd);
            }
            bool already_pinned = pin_idx != std::string::npos;

            char buffer[4];
            snprintf(buffer, lengthof(buffer), "[%c]", (already_pinned ? '*' : ' '));

            ImGui::BeginDisabled(!cwd_exists_before_edit && !already_pinned);

            if (ImGui::Button(buffer)) {
                if (already_pinned) {
                    debug_log("[%s] pin_idx = %zu", expl.name, pin_idx);
                    scoped_timer<timer_unit::MICROSECONDS> unpin_timer(&expl.unpin_us);
                    unpin(pin_idx);
                }
                else {
                    (void) pin(expl.cwd, dir_separator);
                }
                bool result = save_pins_to_disk();
                debug_log("save_pins_to_disk result = %d", result);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(" \n Click here to %s the current working directory. \n ",
                    already_pinned ? "unpin" : "pin");
            }

            ImGui::EndDisabled();
        }
        // pin cwd button end

        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();

        // up a directory arrow start
        {
            ImGui::BeginDisabled(!cwd_exists_before_edit);

            if (ImGui::ArrowButton("Up", ImGuiDir_Up)) {
                debug_log("[%s] up arrow button triggered", expl.name);
                try_ascend_directory(expl, opts);
            }

            ImGui::EndDisabled();
        }
        // up a directory arrow end

        ImGui::SameLine();

        // history back (left) arrow start
        {
            ImGui::BeginDisabled(expl.wd_history_pos == 0);

            if (ImGui::ArrowButton("Back", ImGuiDir_Left)) {
                debug_log("[%s] back arrow button triggered", expl.name);

                if (io.KeyShift || io.KeyCtrl) {
                    expl.wd_history_pos = 0;
                } else {
                    expl.wd_history_pos -= 1;
                }

                expl.cwd = expl.wd_history[expl.wd_history_pos];
                update_cwd_entries(full_refresh, &expl, expl.cwd.data(), opts);
            }

            ImGui::EndDisabled();
        }
        // history back (left) arrow end

        ImGui::SameLine();

        // history forward (right) arrow
        {
            // assert(!expl.wd_history.empty());

            u64 wd_history_last_idx = expl.wd_history.empty() ? 0 : expl.wd_history.size() - 1;

            ImGui::BeginDisabled(expl.wd_history_pos == wd_history_last_idx);

            if (ImGui::ArrowButton("Forward", ImGuiDir_Right)) {
                debug_log("[%s] forward arrow button triggered", expl.name);

                if (io.KeyShift || io.KeyCtrl) {
                    expl.wd_history_pos = wd_history_last_idx;
                } else {
                    expl.wd_history_pos += 1;
                }

                expl.cwd = expl.wd_history[expl.wd_history_pos];
                update_cwd_entries(full_refresh, &expl, expl.cwd.data(), opts);
            }

            ImGui::EndDisabled();
        }
        // history forward (right) arrow end

        ImGui::SameLine();

        // history browser start
        {
            if (ImGui::Button("History")) {
                ImGui::OpenPopup("history_popup");
            }

            if (ImGui::BeginPopup("history_popup")) {
                ImGui::TextUnformatted("History");

                ImGui::SameLine();

                ImGui::BeginDisabled(expl.wd_history.empty());
                if (ImGui::SmallButton("Clear")) {
                    expl.wd_history.clear();
                    expl.wd_history_pos = 0;

                    if (cwd_exists_before_edit) {
                        new_history_from(expl, expl.cwd);
                    }

                    expl.save_to_disk();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (expl.wd_history.empty()) {
                    ImGui::TextUnformatted("(empty)");
                }

                u64 i = expl.wd_history.size() - 1;
                u64 i_inverse = 0;

                for (auto iter = expl.wd_history.rbegin(); iter != expl.wd_history.rend(); ++iter, --i, ++i_inverse) {
                    path_t const &hist_path = *iter;

                    i32 const history_pos_max_digits = 3;
                    char buffer[512];

                    snprintf(buffer, lengthof(buffer), "%s %-*zu %s ",
                        (i == expl.wd_history_pos ? "->" : "  "),
                        history_pos_max_digits,
                        i_inverse + 1,
                        hist_path.data());

                    if (ImGui::Selectable(buffer, false)) {
                        expl.wd_history_pos = i;
                        expl.cwd = expl.wd_history[i];
                        update_cwd_entries(full_refresh, &expl, expl.cwd.data(), opts);
                    }
                }

                ImGui::EndPopup();
            }
        }
        // history browser end

        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();

        // filter type start
        {
            ImGui::Text("filter:");
            ImGui::SameLine();

            // important that they are all the same length,
            // this assumption is leveraged for calculation of combo box width
            static char const *filter_modes[explorer_window::filter_mode::count] = {
                "Contains",
                "RegExp  ",
            };
            ImVec2 max_dropdown_elem_size = ImGui::CalcTextSize(filter_modes[0]);

            ImGui::PushItemWidth(max_dropdown_elem_size.x + 30.f); // some extra for the dropdown button
            ImGui::Combo("##filter_mode", (i32 *)(&expl.filter_mode), filter_modes, lengthof(filter_modes));
            ImGui::PopItemWidth();
        }
        // filter type end

        ImGui::SameLine();

        // filter case sensitivity button start
        {
            if (ImGui::Button(expl.filter_case_sensitive ? "s" : "i")) {
                flip_bool(expl.filter_case_sensitive);
                update_cwd_entries(filter, &expl, expl.cwd.data(), opts);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    " \n Toggle filter case sensitivity \n\n"
                    " i: %sinsensitive%s \n"
                    " s: %ssensitive  %s \n ",
                    !expl.filter_case_sensitive ? "[" : " ", !expl.filter_case_sensitive ? "]" : " ",
                    expl.filter_case_sensitive ? "[" : " ", expl.filter_case_sensitive ? "]" : " ");
            }
        }
        // filter case sensitivity button start

        ImGui::SameLine();

        // filter text input start
        {
            ImGui::PushItemWidth(max(
                ImGui::CalcTextSize(expl.filter.data()).x + (ImGui::GetStyle().FramePadding.x * 2) + 10.f,
                ImGui::CalcTextSize("123456789012345").x
            ));
            if (ImGui::InputText("##filter", expl.filter.data(), expl.filter.size())) {
                update_cwd_entries(filter, &expl, expl.cwd.data(), opts);
            }
            ImGui::PopItemWidth();
        }
        // filter text input end

        ImGui::TableNextColumn();

        ImGui::BeginDisabled(s_paste_payload.items.empty());
        if (ImGui::Button("Clear")) {
            s_paste_payload.items.clear();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();

        ImGui::BeginDisabled(expl.num_selected_cwd_entries == 0);
        if (ImGui::Button("Cut")) {
            s_paste_payload.window_name = expl.name;
            s_paste_payload.items.clear();
            s_paste_payload.keep_src = false;

            for (auto const &dir_ent : expl.cwd_entries) {
                if (dir_ent.is_selected) {
                    path_t src = expl.cwd;
                    if (path_append(src, dir_ent.basic.path.data(), dir_separator, true)) {
                        s_paste_payload.items.push_back({ dir_ent.basic.size, dir_ent.basic.type, src });
                    } else {
                        // error
                    }
                }
            }
        }
        ImGui::EndDisabled();

        ImGui::SameLine();

        ImGui::BeginDisabled(expl.num_selected_cwd_entries == 0);
        if (ImGui::Button("Copy")) {
            s_paste_payload.window_name = expl.name;
            s_paste_payload.items.clear();
            s_paste_payload.keep_src = true;

            for (auto const &dir_ent : expl.cwd_entries) {
                if (dir_ent.is_selected) {
                    path_t src = expl.cwd;
                    if (path_append(src, dir_ent.basic.path.data(), dir_separator, true)) {
                        s_paste_payload.items.push_back({ dir_ent.basic.size, dir_ent.basic.type, src });
                    } else {
                        // error
                    }
                }
            }
        }
        ImGui::EndDisabled();

        ImGui::SameLine();

        ImGui::BeginDisabled(expl.num_selected_cwd_entries == 0);
        if (ImGui::Button("Delete")) {
            s_paste_payload.window_name = expl.name;
            s_paste_payload.items.clear();
            s_paste_payload.keep_src = false;

            for (auto const &dir_ent : expl.cwd_entries) {
                if (dir_ent.is_selected) {
                    path_t src = expl.cwd;
                    if (path_append(src, dir_ent.basic.path.data(), dir_separator, true)) {
                        enqueue_file_op(file_operation::type::remove, dir_ent.basic.size, src, {}, dir_separator);
                    } else {
                        // error
                    }
                }
            }
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();

        {
            ImGui::BeginDisabled(s_paste_payload.items.empty());

            if (ImGui::Button("Paste into cwd")) {
                auto op_type = s_paste_payload.keep_src ? file_operation::type::copy : file_operation::type::move;

                for (auto const &paste_item : s_paste_payload.items) {
                    if (paste_item.type == basic_dir_ent::kind::directory) {

                    }
                    else {
                        enqueue_file_op(op_type, paste_item.size, paste_item.path, expl.cwd, dir_separator);
                    }
                }
            }

            ImGui::EndDisabled();
        }

        // paste payload description start
        if (!s_paste_payload.items.empty()) {
            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();

            u64 num_dirs = 0, num_symlinks = 0, num_files = 0;
            for (auto const &item : s_paste_payload.items) {
                num_dirs     += u64(item.type == basic_dir_ent::kind::directory);
                num_symlinks += u64(item.type == basic_dir_ent::kind::symlink);
                num_files    += u64(item.type == basic_dir_ent::kind::file);
            }

            if (num_dirs > 0) {
                ImGui::SameLine();
                ImGui::TextColored(basic_dir_ent::get_color(basic_dir_ent::kind::directory), "%zuD", num_dirs);
            }
            if (num_symlinks > 0) {
                ImGui::SameLine();
                ImGui::TextColored(basic_dir_ent::get_color(basic_dir_ent::kind::symlink), "%zuS", num_symlinks);
            }
            if (num_files > 0) {
                ImGui::SameLine();
                ImGui::TextColored(basic_dir_ent::get_color(basic_dir_ent::kind::file), "%zuF", num_files);
            }

            ImGui::SameLine();
            ImGui::Text("ready to be %s from %s", (s_paste_payload.keep_src ? "copied" : "cut"), s_paste_payload.window_name);
        }
        // paste payload description end

        ImGui::TableNextColumn();

        // cwd text input start
        {
            cwd_text_input_callback_user_data user_data;
            user_data.expl_ptr = &expl;
            user_data.opts_ptr = &opts;
            user_data.dir_separator_w = dir_separator_w;

            ImGui::PushItemWidth(
                max(ImGui::CalcTextSize(expl.cwd.data()).x + (ImGui::GetStyle().FramePadding.x * 2),
                    ImGui::CalcTextSize("123456789_123456789_").x)
                + 60.f
            );

            ImGui::InputText("##cwd", expl.cwd.data(), expl.cwd.size(),
                ImGuiInputTextFlags_CallbackCharFilter|ImGuiInputTextFlags_CallbackEdit,
                cwd_text_input_callback, (void *)&user_data);

            expl.cwd = path_squish_adjacent_separators(expl.cwd);

            ImGui::PopItemWidth();

            ImGui::SameLine();

            // label
            if (opts.show_cwd_len) {
                ImGui::Text("cwd(%3d)", path_length(expl.cwd));
            }
        }
        // cwd text input end

        // clicknav start
        #if 0
        if (cwd_exists_after_edit) {
            ImGui::TableNextColumn();

            static std::vector<char const *> slices = {};
            slices.reserve(50);
            slices.clear();

            path_t sliced_path = expl.cwd;
            char const *slice = strtok(sliced_path.data(), "\\/");
            while (slice != nullptr) {
                slices.push_back(slice);
                slice = strtok(nullptr, "\\/");
            }

            auto cd_to_slice = [&expl, &sliced_path](char const *slice) {
                char const *slice_end = slice;
                while (*slice_end != '\0') {
                    ++slice_end;
                }

                u64 len = slice_end - sliced_path.data();

                if (len == path_length(expl.cwd)) {
                    debug_log("[%s] cd_to_slice: slice == cwd, not updating cwd|history", expl.name);
                }
                else {
                    expl.cwd[len] = '\0';
                    new_history_from(expl, expl.cwd);
                }
            };

            f32 original_spacing = ImGui::GetStyle().ItemSpacing.x;

            for (auto slice_it = slices.begin(); slice_it != slices.end() - 1; ++slice_it) {
                if (ImGui::Button(*slice_it)) {
                    debug_log("[%s] clicked slice [%s]", expl.name, *slice_it);
                    cd_to_slice(*slice_it);
                    update_cwd_entries(full_refresh, &expl, expl.cwd.data(), opts);
                }
                ImGui::GetStyle().ItemSpacing.x = 2;
                ImGui::SameLine();
                ImGui::Text("%c", dir_separator);
                ImGui::SameLine();
            }

            if (ImGui::Button(slices.back())) {
                debug_log("[%s] clicked slice [%s]", expl.name, slices.back());
                cd_to_slice(slices.back());
                update_cwd_entries(full_refresh, &expl, expl.cwd.data(), opts);
            }

            if (slices.size() > 1) {
                ImGui::GetStyle().ItemSpacing.x = original_spacing;
            }
        }
        #endif
        // clicknav end
    }
    ImGui::EndTable();
    ImGui::PopStyleVar();

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();

    // cwd entries stats & table start

    if (!cwd_exists_before_edit) {
        ImGui::TextColored(orange, "Invalid directory.");
    }
    else if (expl.cwd_entries.empty()) {
        // cwd exists but is empty
        ImGui::TextColored(orange, "Empty directory.");
    }
    else {
        u64 num_selected_directories = 0;
        u64 num_selected_symlinks = 0;
        u64 num_selected_files = 0;

        u64 num_filtered_directories = 0;
        u64 num_filtered_symlinks = 0;
        u64 num_filtered_files = 0;

        u64 num_child_directories = 0;
        u64 num_child_symlinks = 0;

        for (auto const &dir_ent : expl.cwd_entries) {
            static_assert(false == 0);
            static_assert(true == 1);

            num_selected_directories += u64(dir_ent.is_selected && dir_ent.basic.is_directory());
            num_selected_symlinks    += u64(dir_ent.is_selected && dir_ent.basic.is_symlink());
            num_selected_files       += u64(dir_ent.is_selected && dir_ent.basic.is_non_symlink_file());

            num_filtered_directories += u64(dir_ent.is_filtered_out && dir_ent.basic.is_directory());
            num_filtered_symlinks    += u64(dir_ent.is_filtered_out && dir_ent.basic.is_symlink());
            num_filtered_files       += u64(dir_ent.is_filtered_out && dir_ent.basic.is_non_symlink_file());

            num_child_directories += u64(dir_ent.basic.is_directory());
            num_child_symlinks    += u64(dir_ent.basic.is_symlink());
        }

        u64 num_filtered_dirents = num_filtered_directories + num_filtered_symlinks + num_filtered_files;
        u64 num_selected_dirents = num_selected_directories + num_selected_symlinks + num_selected_files;
        u64 num_child_dirents = expl.cwd_entries.size();
        u64 num_child_files = num_child_dirents - num_child_directories - num_child_symlinks;

        if (expl.filter_error != "") {
            ImGui::PushTextWrapPos(ImGui::GetColumnWidth());
            ImGui::TextColored(red, "%s", expl.filter_error.c_str());
            ImGui::PopTextWrapPos();

            ImGui::Spacing();
            ImGui::Spacing();
            ImGui::Spacing();
        }

    #if 1
        (void) num_selected_dirents;

        {
            ImGui::Text("%zu items", num_child_dirents);
            if (num_child_dirents > 0) {
                ImGui::SameLine();
                ImGui::Text(" ");
            }
            if (num_child_directories > 0) {
                ImGui::SameLine();
                ImGui::TextColored(
                    basic_dir_ent::get_color(basic_dir_ent::kind::directory),
                    "%zu director%s", num_child_directories, num_child_directories == 1 ? "y" : "ies");
            }
            if (num_child_symlinks > 0) {
                ImGui::SameLine();
                ImGui::TextColored(
                    basic_dir_ent::get_color(basic_dir_ent::kind::symlink),
                    " %zu symlink%s", num_child_symlinks, num_child_symlinks == 1 ? "" : "s");
            }
            if (num_child_files > 0) {
                ImGui::SameLine();
                ImGui::TextColored(
                    basic_dir_ent::get_color(basic_dir_ent::kind::file),
                    " %zu file%s", num_child_files, num_child_files == 1 ? "" : "s");
            }
        }

    #if 0
        ImGui::SameLine();
        ImGui::Text(" ");
        ImGui::SameLine();

        {
            time_point_t now = current_time();
            ImGui::Text("refreshed %s", compute_when_str(expl.last_refresh_time, now).data());
        }
    #endif

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();
    #else
        if (expl.filter_error == "") {
            ImGui::Text("%zu filtered", num_filtered_dirents);
            if (num_filtered_dirents > 0) {
                ImGui::SameLine();
                ImGui::Text(":");
            }
            if (num_filtered_files > 0) {
                ImGui::SameLine();
                ImGui::Text("%zu file%s", num_filtered_files, num_filtered_files == 1 ? "" : "s");
            }
            if (num_filtered_directories > 0) {
                ImGui::SameLine();
                ImGui::Text("%zu director%s", num_filtered_directories, num_filtered_directories == 1 ? "y" : "ies");
            }
            if (num_filtered_symlinks > 0) {
                ImGui::SameLine();
                ImGui::Text("%zu shortcut%s", num_filtered_symlinks, num_filtered_symlinks == 1 ? "" : "s");
            }
        }

        ImGui::Text("%zu selected", num_selected_dirents);
        if (num_selected_dirents > 0) {
            ImGui::SameLine();
            ImGui::Text(":");
        }
        if (num_selected_files > 0) {
            ImGui::SameLine();
            ImGui::Text("%zu file%s", num_selected_files, num_selected_files == 1 ? "" : "s");
        }
        if (num_selected_directories > 0) {
            ImGui::SameLine();
            ImGui::Text("%zu director%s", num_selected_directories, num_selected_directories == 1 ? "y" : "ies");
        }
        if (num_selected_symlinks > 0) {
            ImGui::SameLine();
            ImGui::Text("%zu shortcut%s", num_selected_symlinks, num_selected_symlinks == 1 ? "" : "s");
        }

        ImGui::Spacing();
        ImGui::Spacing();
    #endif
        // ImGui::SeparatorText("Directory contents");

        expl.num_selected_cwd_entries = 0; // will get computed as we render cwd_entries table

        if (ImGui::BeginChild("cwd_entries_child", ImVec2(0, ImGui::GetContentRegionAvail().y))) {
            if (num_filtered_dirents == expl.cwd_entries.size()) {
                if (ImGui::Button("Clear filter")) {
                    debug_log("[%s] clear filter button pressed", expl.name);
                    expl.filter[0] = '\0';
                    update_cwd_entries(filter, &expl, expl.cwd.data(), opts);
                }

                ImGui::SameLine();
                ImGui::Spacing();
                ImGui::SameLine();

                ImGui::TextColored(orange, "All items filtered.");
            }
            else if (ImGui::BeginTable("cwd_entries", cwd_entries_table_col_count,
                ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_Hideable|ImGuiTableFlags_Resizable|ImGuiTableFlags_Reorderable|ImGuiTableFlags_Sortable
            )) {
                ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_NoSort, 0.0f, cwd_entries_table_col_number);
                ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_DefaultSort, 0.0f, cwd_entries_table_col_id);
                ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_DefaultSort, 0.0f, cwd_entries_table_col_path);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_DefaultSort, 0.0f, cwd_entries_table_col_type);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_DefaultSort, 0.0f, cwd_entries_table_col_size_pretty);
                ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_DefaultSort, 0.0f, cwd_entries_table_col_size_bytes);
                // ImGui::TableSetupColumn("Created", ImGuiTableColumnFlags_DefaultSort, 0.0f, cwd_entries_table_col_creation_time);
                ImGui::TableSetupColumn("Last Edited", ImGuiTableColumnFlags_DefaultSort, 0.0f, cwd_entries_table_col_last_write_time);
                ImGui::TableHeadersRow();

                ImGuiTableSortSpecs *sort_specs = ImGui::TableGetSortSpecs();
                if (sort_specs != nullptr && (expl.needs_initial_sort || sort_specs->SpecsDirty)) {
                    sort_cwd_entries(expl, sort_specs);
                    sort_specs->SpecsDirty = false;
                    expl.needs_initial_sort = false;
                }

                for (u64 i = 0; i < expl.cwd_entries.size(); ++i) {
                    auto &dir_ent = expl.cwd_entries[i];

                    if (dir_ent.is_filtered_out) {
                        ++num_filtered_dirents;
                        continue;
                    }

                    ImGui::TableNextRow();

                    if (ImGui::TableSetColumnIndex(cwd_entries_table_col_number)) {
                        ImGui::Text("%zu", i + 1);
                    }

                    if (ImGui::TableSetColumnIndex(cwd_entries_table_col_id)) {
                        ImGui::Text("%zu", dir_ent.basic.id);
                    }

                    if (ImGui::TableSetColumnIndex(cwd_entries_table_col_path)) {
                        ImGui::PushStyleColor(ImGuiCol_Text, dir_ent.basic.get_color());

                        if (ImGui::Selectable(dir_ent.basic.path.data(), dir_ent.is_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                            if (!io.KeyCtrl && !io.KeyShift) {
                                // entry was selected but Ctrl was not held, so deselect everything
                                for (auto &dir_ent2 : expl.cwd_entries)
                                    dir_ent2.is_selected = false;

                                // expl.num_selected_cwd_entries = 0;
                            }

                            flip_bool(dir_ent.is_selected);

                            if (io.KeyShift) {
                                // shift click, select everything between the current item and the previously clicked item

                                u64 first_idx, last_idx;

                                if (explorer_window::NO_SELECTION == expl.cwd_prev_selected_dirent_idx) {
                                    // nothing in cwd has been selected, so start selection from very first entry
                                    expl.cwd_prev_selected_dirent_idx = 0;
                                }

                                if (i <= expl.cwd_prev_selected_dirent_idx) {
                                    // prev selected item below current one
                                    first_idx = i;
                                    last_idx = expl.cwd_prev_selected_dirent_idx;
                                }
                                else {
                                    first_idx = expl.cwd_prev_selected_dirent_idx;
                                    last_idx = i;
                                }

                                debug_log("[%s] shift click, [%zu, %zu]", expl.name, first_idx, last_idx);

                                for (u64 j = first_idx; j <= last_idx; ++j) {
                                    expl.cwd_entries[j].is_selected = true;
                                }
                            }

                            static f64 last_click_time = 0;
                            f64 current_time = ImGui::GetTime();

                            if (current_time - last_click_time <= 0.2) {
                                if (dir_ent.basic.is_directory()) {
                                    debug_log("[%s] double clicked directory [%s]", expl.name, dir_ent.basic.path.data());

                                    auto const &descend_target = dir_ent.basic.path;

                                    if (strcmp(descend_target.data(), "..") == 0) {
                                        try_ascend_directory(expl, opts);
                                    } else {
                                        try_descend_to_directory(expl, dir_ent.basic.path.data(), opts);
                                    }
                                }
                                else if (dir_ent.basic.is_symlink()) {
                                    char const *lnk_file_path = dir_ent.basic.path.data();
                                    debug_log("[%s] double clicked link [%s]", expl.name, lnk_file_path);

                                    path_t shortcut_path = {};

                                    // Load the .lnk file
                                    MultiByteToWideChar(CP_ACP, 0, lnk_file_path, -1, reinterpret_cast<LPWSTR>(shortcut_path.data()), (i32)shortcut_path.size());
                                    HRESULT com_handle = s_persist_file_interface->Load(reinterpret_cast<LPCOLESTR>(shortcut_path.data()), 0);

                                    if (FAILED(com_handle)) {
                                        debug_log("[%s] failed to load file [%s]", expl.name, lnk_file_path);
                                    } else {
                                        LPITEMIDLIST pidl;
                                        com_handle = s_shell_link->GetIDList(&pidl);
                                        if (FAILED(com_handle)) {
                                            debug_log("[%s] failed to GetPath from [%s]", expl.name, lnk_file_path);
                                            goto symlink_end;
                                        }

                                        path_t link_wd = {};
                                        if (!SHGetPathFromIDListA(pidl, link_wd.data())) {
                                            debug_log("[%s] failed to SHGetPathFromIDList from [%s]", expl.name, lnk_file_path);
                                            goto symlink_end;
                                        }

                                        debug_log("[%s] link dest = [%s]", expl.name, link_wd.data());

                                        expl.cwd = link_wd;
                                        update_cwd_entries(full_refresh, &expl, expl.cwd.data(), opts);
                                    }

                                    symlink_end:;
                                }
                                else {
                                    debug_log("[%s] double clicked file [%s]", expl.name, dir_ent.basic.path.data());

                                    path_t target_full_path = expl.cwd;

                                    if (path_append(target_full_path, dir_ent.basic.path.data(), opts.dir_separator(), true)) {
                                        debug_log("[%s] target_full_path = [%s]", expl.name, target_full_path.data());
                                        [[maybe_unused]] HINSTANCE result = ShellExecuteA(nullptr, "open", target_full_path.data(), nullptr, nullptr, SW_SHOWNORMAL);
                                    }
                                    else {
                                        debug_log("[%s] path_append failed, cwd = [%s], append data = [\\%s]", expl.name, expl.cwd.data(), dir_ent.basic.path.data());
                                    }
                                }
                            }
                            else {
                                debug_log("[%s] selected [%s]", expl.name, dir_ent.basic.path.data());
                            }

                            last_click_time = current_time;
                            expl.cwd_prev_selected_dirent_idx = i;
                        }

                        ImGui::PopStyleColor();
                    }

                    if (ImGui::TableSetColumnIndex(cwd_entries_table_col_type)) {
                        if (dir_ent.basic.is_directory()) {
                            ImGui::TextUnformatted("dir");
                        }
                        else if (dir_ent.basic.is_symlink()) {
                            ImGui::TextUnformatted("symlink");
                        }
                        else {
                            ImGui::TextUnformatted("file");
                        }
                    }

                    if (ImGui::TableSetColumnIndex(cwd_entries_table_col_size_pretty)) {
                        if (dir_ent.basic.is_directory()) {
                            ImGui::Text("");
                        }
                        else {
                            std::array<char, 32> pretty_size = {};
                            format_file_size(dir_ent.basic.size, pretty_size.data(), pretty_size.size(), opts.binary_size_system ? 1024 : 1000);
                            ImGui::TextUnformatted(pretty_size.data());
                        }
                    }

                    if (ImGui::TableSetColumnIndex(cwd_entries_table_col_size_bytes)) {
                        if (dir_ent.basic.is_directory()) {
                            ImGui::TextUnformatted("");
                        }
                        else {
                            ImGui::Text("%zu", dir_ent.basic.size);
                        }
                    }

                    // if (ImGui::TableSetColumnIndex(cwd_entries_table_col_creation_time)) {
                    //     auto [result, buffer] = filetime_to_string(&dir_ent.last_write_time_raw);
                    //     ImGui::TextUnformatted(buffer.data());
                    // }

                    if (ImGui::TableSetColumnIndex(cwd_entries_table_col_last_write_time)) {
                        auto [result, buffer] = filetime_to_string(&dir_ent.basic.last_write_time_raw);
                        ImGui::TextUnformatted(buffer.data());
                    }

                    expl.num_selected_cwd_entries += u64(dir_ent.is_selected);
                }

                ImGui::EndTable();
            }
            if (ImGui::IsItemHovered() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
                // select all cwd entries when hovering over the table and pressing Ctrl-a
                for (auto &dir_ent2 : expl.cwd_entries)
                    dir_ent2.is_selected = true;

                expl.num_selected_cwd_entries = expl.cwd_entries.size();
            }
            if (window_focused && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                for (auto &dir_ent2 : expl.cwd_entries)
                    dir_ent2.is_selected = false;

                expl.num_selected_cwd_entries = 0;
            }
        }

        ImGui::EndChild();
    }
    // cwd entries stats & table end

    ImGui::End();

    if (cwd_exists_before_edit && !path_loosely_same(expl.cwd, expl.prev_valid_cwd)) {
        expl.prev_valid_cwd = expl.cwd;
    }
}

#endif // SWAN_EXPLORER_WINDOW_CPP
