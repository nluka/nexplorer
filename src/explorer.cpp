
#include "stdafx.hpp"
#include "common_fns.hpp"
#include "imgui_specific.hpp"
#include "path.hpp"
#include "scoped_timer.hpp"
#include "util.hpp"

static IShellLinkW *s_shell_link = nullptr;
static IPersistFile *s_persist_file_interface = nullptr;
static std::array<explorer_window, global_constants::num_explorers> s_explorers = {};
static file_operation_command_buf s_file_op_payload = {};

std::array<explorer_window, global_constants::num_explorers> &global_state::explorers() noexcept { return s_explorers; }

void init_COM_for_explorers(GLFWwindow *window, char const *ini_file_path) noexcept
{
    bool retry = true; // start true to do initial load
    char const *what_failed = nullptr;

    while (true) {
        if (retry) {
            retry = false;
            what_failed = nullptr;

            HRESULT result = CoInitialize(nullptr);

            if (FAILED(result)) {
                CoUninitialize();
                what_failed = "CoInitialize";
            }
            else {
                result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (LPVOID *)&s_shell_link);

                if (FAILED(result)) {
                    what_failed = "CoCreateInstance(CLSID_ShellLink, ..., CLSCTX_INPROC_SERVER, IID_IShellLinkW, ...)";
                }
                else {
                    result = s_shell_link->QueryInterface(IID_IPersistFile, (LPVOID *)&s_persist_file_interface);

                    if (FAILED(result)) {
                        s_persist_file_interface->Release();
                        CoUninitialize();
                        what_failed = "IUnknown::QueryInterface(IID_IPersistFile, ...)";
                    }
                }
            }
        }

        if (!what_failed) {
            break;
        }

        new_frame(ini_file_path);

        if (imgui::Begin("Startup Error", nullptr, ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_AlwaysAutoResize)) {
            imgui::TextColored(red(), "Application is unable to continue, critical initialization failed:");
            imgui::TextUnformatted(what_failed);
            retry = imgui::Button("Retry");
        }
        imgui::End();

        render_frame(window);
    }
}

void clean_COM_for_explorers() noexcept
try {
    s_persist_file_interface->Release();
    s_shell_link->Release();
    CoUninitialize();
}
catch (...) {}

struct cwd_count_info
{
    u64 selected_directories;
    u64 selected_symlinks;
    u64 selected_files;
    u64 filtered_directories;
    u64 filtered_symlinks;
    u64 filtered_files;
    u64 child_dirents;
    u64 child_directories;
    u64 child_symlinks;
    u64 child_files;
    u64 filtered_dirents;
    u64 selected_dirents;
};

static
void render_table_rows_for_cwd_entries(
    explorer_window &expl,
    cwd_count_info const &cnt,
    u64 size_unit_multiplier,
    bool window_focused,
    bool any_popups_open,
    char dir_sep_utf8,
    wchar_t dir_sep_utf16) noexcept;

struct render_dirent_right_click_context_menu_result
{
    bool open_bulk_rename_popup;
    bool open_single_rename_popup;
    explorer_window::dirent *single_dirent_to_be_renamed;
};
static
render_dirent_right_click_context_menu_result
render_dirent_right_click_context_menu(explorer_window &expl, cwd_count_info const &cnt, swan_settings const &settings) noexcept;

static
void accept_move_dirents_drag_drop(explorer_window &expl) noexcept;

static
void render_footer(explorer_window &expl, cwd_count_info const &cnt, ImGuiStyle &style) noexcept;

void explorer_window::deselect_all_cwd_entries() noexcept
{
    for (auto &dirent : this->cwd_entries) {
        dirent.is_selected = false;
    }
}

void explorer_window::select_all_visible_cwd_entries(bool select_dotdot_dir) noexcept
{
    for (auto &dirent : this->cwd_entries) {
        if ( (!select_dotdot_dir && dirent.basic.is_path_dotdot()) || dirent.is_filtered_out ) {
            continue;
        } else {
            dirent.is_selected = true;
        }
    }
}

void explorer_window::invert_selected_visible_cwd_entries() noexcept
{
    for (auto &dirent : this->cwd_entries) {
        if (dirent.is_filtered_out) {
            dirent.is_selected = false;
        } else if (!dirent.basic.is_dotdot_dir()) {
            dirent.is_selected = !dirent.is_selected;
        }
    }
}

void explorer_window::set_latest_valid_cwd(swan_path_t const &new_latest_valid_cwd) noexcept
{
    if (global_state::settings().clear_filter_on_cwd_change) {
        init_empty_cstr(this->filter_text.data());
    }
    this->latest_valid_cwd = new_latest_valid_cwd;
    while (path_pop_back_if(this->latest_valid_cwd, "\\/ "));
}

void explorer_window::uncut() noexcept
{
    for (auto &dirent : this->cwd_entries) {
        dirent.is_cut = false;
    }
}

void explorer_window::reset_filter() noexcept
{
    init_empty_cstr(this->filter_text.data());

    this->filter_show_directories = true;
    this->filter_show_files = true;
    this->filter_show_invalid_symlinks = true;
    this->filter_show_symlink_directories = true;
    this->filter_show_symlink_files = true;

    this->filter_polarity = true;
    this->filter_case_sensitive = false;
    this->filter_mode = explorer_window::filter_mode::contains;
}

static
std::pair<s32, std::array<char, 64>> filetime_to_string(FILETIME *time) noexcept
{
    std::array<char, 64> buffer_raw;
    std::array<char, 64> buffer_final;

    DWORD flags = FDTF_SHORTDATE | FDTF_SHORTTIME;
    s32 length = SHFormatDateTimeA(time, &flags, buffer_raw.data(), (u32)buffer_raw.size());

    // for some reason, SHFormatDateTimeA will pad parts of the string with ASCII 63 (?)
    // when using LONGDATE or LONGTIME, we are discarding them
    std::copy_if(buffer_raw.begin(), buffer_raw.end(), buffer_final.begin(), [](char ch) { return ch != '?'; });

    // std::replace(buffer_final.begin(), buffer_final.end(), '-', ' ');

    return { length, buffer_final };
}

static
generic_result add_selected_entries_to_file_op_payload(explorer_window &expl, char const *operation, char operation_code) noexcept
{
    std::stringstream err = {};

    for (auto &dirent : expl.cwd_entries) {
        if (!dirent.is_selected || dirent.basic.is_path_dotdot()) {
            continue;
        }

        if (operation_code == 'X') {
            if (dirent.is_cut) {
                continue; // prevent same dirent from being cut multiple times
                          // (although multiple copy commands of the same dirent are permitted, intentionally)
            } else {
                dirent.is_cut = true;
            }
        }

        if (operation_code == 'C' && dirent.is_cut) {
            // this situation wouldn't make sense, because you can't CopyItem after MoveItem since there's nothing left to copy
            // TODO: maybe indicate something to the user rather than ignoring their request?
            continue;
        }

        swan_path_t src = expl.cwd;

        if (path_append(src, dirent.basic.path.data(), global_state::settings().dir_separator_utf8, true)) {
            s_file_op_payload.items.push_back({ operation, operation_code, dirent.basic.type, src });
        } else {
            err << "Current working directory path + [" << src.data() << "] exceeds max allowed path length.\n";
        }
    }

    std::string errors = err.str();

    return { errors.empty(), errors };
}

static
generic_result delete_selected_entries(explorer_window &expl) noexcept
{
    auto file_operation_task = [](
        std::wstring working_directory_utf16,
        std::wstring paths_to_delete_utf16,
        std::mutex *init_done_mutex,
        std::condition_variable *init_done_cond,
        bool *init_done,
        std::string *init_error
    ) {
        assert(!working_directory_utf16.empty());
        if (!StrChrW(L"\\/", working_directory_utf16.back())) {
            working_directory_utf16 += L'\\';
        }

        auto set_init_error_and_notify = [&](char const *err) {
            std::unique_lock lock(*init_done_mutex);
            *init_done = true;
            *init_error = err;
            init_done_cond->notify_one();
        };

        HRESULT result = {};

        result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(result)) {
            return set_init_error_and_notify("CoInitializeEx(COINIT_APARTMENTTHREADED)");
        }
        SCOPE_EXIT { CoUninitialize(); };

        IFileOperation *file_op = nullptr;

        result = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&file_op));
        if (FAILED(result)) {
            return set_init_error_and_notify("CoCreateInstance(CLSID_FileOperation)");
        }
        SCOPE_EXIT { file_op->Release(); };

        result = file_op->SetOperationFlags(/*FOF_NOCONFIRMATION | */FOF_ALLOWUNDO);
        if (FAILED(result)) {
            return set_init_error_and_notify("IFileOperation::SetOperationFlags");
        }

        // add items (IShellItem) for exec to IFileOperation
        {
            auto items_to_delete = std::wstring_view(paths_to_delete_utf16.data()) | std::ranges::views::split('\n');
            std::stringstream err = {};
            std::wstring full_path_to_delete_utf16 = {};

            full_path_to_delete_utf16.reserve((global_state::page_size() / 2) - 1);

            for (auto item_utf16 : items_to_delete) {
                full_path_to_delete_utf16.clear();
                full_path_to_delete_utf16.append(working_directory_utf16);
                std::wstring_view view(item_utf16.begin(), item_utf16.end());
                full_path_to_delete_utf16 += view;

                // shlwapi doesn't like '/', force them all to '\'
                std::replace(full_path_to_delete_utf16.begin(), full_path_to_delete_utf16.end(), L'/', L'\\');

                swan_path_t item_path_utf8 = path_create("");
                s32 written = 0;

                IShellItem *to_delete = nullptr;
                result = SHCreateItemFromParsingName(full_path_to_delete_utf16.c_str(), nullptr, IID_PPV_ARGS(&to_delete));
                if (FAILED(result)) {
                    HANDLE accessible = CreateFileW(
                        full_path_to_delete_utf16.c_str(),
                        FILE_LIST_DIRECTORY,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL,
                        OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS,
                        NULL);

                    WCOUT_IF_DEBUG("FAILED: SHCreateItemFromParsingName [" << full_path_to_delete_utf16.c_str() << "]\n");
                    written = utf16_to_utf8(full_path_to_delete_utf16.data(), item_path_utf8.data(), item_path_utf8.size());
                    if (written == 0) {
                        err << "SHCreateItemFromParsingName and conversion of delete path from UTF-16 to UTF-8.\n";
                    } else {
                        if (accessible == INVALID_HANDLE_VALUE) {
                            err << "File or directory is not accessible, maybe it is locked or has been moved/deleted? ";
                        }
                        err << "FAILED SHCreateItemFromParsingName for [" << item_path_utf8.data() << "]\n";
                    }
                    continue;
                }

                SCOPE_EXIT { to_delete->Release(); };

                result = file_op->DeleteItem(to_delete, nullptr);
                if (FAILED(result)) {
                    WCOUT_IF_DEBUG("FAILED: IFileOperation::DeleteItem [" << full_path_to_delete_utf16.c_str() << "]\n");
                    written = utf16_to_utf8(full_path_to_delete_utf16.data(), item_path_utf8.data(), item_path_utf8.size());
                    if (written == 0) {
                        err << "IFileOperation::DeleteItem and conversion of delete path from UTF-16 to UTF-8.\n";
                    } else {
                        err << "IFileOperation::DeleteItem [" << item_path_utf8.data() << "]";
                    }
                } else {
                    WCOUT_IF_DEBUG("file_op->DeleteItem [" << full_path_to_delete_utf16.c_str() << "]\n");
                }
            }

            std::string errors = err.str();
            if (!errors.empty()) {
                errors.pop_back(); // remove trailing '\n'
                return set_init_error_and_notify(errors.c_str());
            }
        }

        progress_sink prog_sink;
        prog_sink.contains_delete_operations = true;
        DWORD cookie = {};

        result = file_op->Advise(&prog_sink, &cookie);
        if (FAILED(result)) {
            return set_init_error_and_notify("IFileOperation::Advise(IFileOperationProgressSink *pfops, DWORD *pdwCookie)");
        }
        print_debug_msg("IFileOperation::Advise(%d)", cookie);

        set_init_error_and_notify(""); // init succeeded, no error

        result = file_op->PerformOperations();
        if (FAILED(result)) {
            print_debug_msg("FAILED IFileOperation::PerformOperations()");
        }

        file_op->Unadvise(cookie);
        if (FAILED(result)) {
            print_debug_msg("FAILED IFileOperation::Unadvise(%d)", cookie);
        }
    };

    wchar_t cwd_utf16[2048]; init_empty_cstr(cwd_utf16);
    s32 written = utf8_to_utf16(expl.cwd.data(), cwd_utf16, lengthof(cwd_utf16));
    if (written == 0) {
        print_debug_msg("FAILED utf8_to_utf16(expl.cwd)");
        return { false, "Conversion of current working directory path from UTF-8 to UTF-16." };
    }

    std::wstring packed_paths_to_delete_utf16 = {};
    u64 item_count = 0;
    {
        wchar_t item_utf16[MAX_PATH];
        std::stringstream err = {};

        for (auto const &item : expl.cwd_entries) {
            if (!item.is_filtered_out && item.is_selected) {
                init_empty_cstr(item_utf16);
                written = utf8_to_utf16(item.basic.path.data(), item_utf16, lengthof(item_utf16));

                if (written == 0) {
                    print_debug_msg("FAILED utf8_to_utf16(item.basic.path)");
                    err << "Conversion of [" << item.basic.path.data() << "] from UTF-8 to UTF-16.\n";
                }

                packed_paths_to_delete_utf16.append(item_utf16).append(L"\n");
                ++item_count;
            }
        }

        // WCOUT_IF_DEBUG("packed_paths_to_delete_utf16:\n" << packed_paths_to_delete_utf16 << '\n');

        if (!packed_paths_to_delete_utf16.empty()) {
            packed_paths_to_delete_utf16.pop_back(); // remove trailing \n
        }

        std::string errors = err.str();
        if (!errors.empty()) {
            return { false, errors };
        }
    }

    bool initialization_done = false;
    std::string initialization_error = {};

    global_state::thread_pool().push_task(file_operation_task,
        cwd_utf16,
        std::move(packed_paths_to_delete_utf16),
        &expl.shlwapi_task_initialization_mutex,
        &expl.shlwapi_task_initialization_cond,
        &initialization_done,
        &initialization_error);

    {
        std::unique_lock lock(expl.shlwapi_task_initialization_mutex);
        expl.shlwapi_task_initialization_cond.wait(lock, [&]() { return initialization_done; });
    }

    return { initialization_error.empty(), initialization_error };
}

generic_result move_files_into(swan_path_t const &destination_utf8, explorer_window &expl, move_dirents_drag_drop_payload &payload) noexcept
{
    SCOPE_EXIT { delete[] payload.absolute_paths_delimited_by_newlines; };

    wchar_t destination_utf16[2048]; init_empty_cstr(destination_utf16);
    s32 written = utf8_to_utf16(destination_utf8.data(), destination_utf16, lengthof(destination_utf16));
    if (written == 0) {
        print_debug_msg("FAILED utf8_to_utf16(destination)");
        return { false, "Conversion of destination directory path from UTF-8 to UTF-16." };
    }

    bool initialization_done = false;
    std::string initialization_error = {};

    global_state::thread_pool().push_task(perform_file_operations,
        expl.id,
        destination_utf16,
        std::wstring(payload.absolute_paths_delimited_by_newlines),
        std::vector<char>(payload.num_items, 'X'),
        &expl.shlwapi_task_initialization_mutex,
        &expl.shlwapi_task_initialization_cond,
        &initialization_done,
        &initialization_error);

    {
        std::unique_lock lock(expl.shlwapi_task_initialization_mutex);
        expl.shlwapi_task_initialization_cond.wait(lock, [&]() { return initialization_done; });
    }

    return { initialization_error.empty(), initialization_error };
}

static
generic_result handle_drag_drop_onto_dirent(
    explorer_window &expl,
    explorer_window::dirent const &target_dirent,
    ImGuiPayload const *payload_wrapper,
    char dir_sep_utf8) noexcept
{
    auto payload_data = (move_dirents_drag_drop_payload *)payload_wrapper->Data;
    assert(payload_data != nullptr);
    swan_path_t destination_utf8 = expl.cwd;

    if (target_dirent.basic.is_dotdot_dir()) {
        // we cannot simply append ".." to `destination_utf8` and give that to `move_files_into`,
        // because shlwapi does not accept a path like "C:/some/path/../"
        while (path_pop_back_if_not(destination_utf8, dir_sep_utf8));

        return move_files_into(destination_utf8, expl, *payload_data);
    }
    else {
        if (!path_append(destination_utf8, target_dirent.basic.path.data(), dir_sep_utf8, true)) {
            return { false, make_str("Append current working directory to drop target [%s]", target_dirent.basic.path.data()) };
        } else {
            return move_files_into(destination_utf8, expl, *payload_data);
        }
    }
}

generic_result reveal_in_file_explorer(explorer_window::dirent const &entry, explorer_window &expl, wchar_t dir_sep_utf16) noexcept
{
    wchar_t select_path_cwd_buffer_utf16[MAX_PATH];     init_empty_cstr(select_path_cwd_buffer_utf16);
    wchar_t select_path_dirent_buffer_utf16[MAX_PATH];  init_empty_cstr(select_path_dirent_buffer_utf16);
    std::wstring select_command = {};
    s32 utf_written = 0;

    select_command.reserve(1024);

    utf_written = utf8_to_utf16(expl.cwd.data(), select_path_cwd_buffer_utf16, lengthof(select_path_cwd_buffer_utf16));

    if (utf_written == 0) {
        print_debug_msg("[ %d ] FAILED utf8_to_utf16(expl.cwd)", expl.id);
        return { false, "Conversion of cwd path from UTF-8 to UTF-16." };
    }

    select_command += L"/select,";
    select_command += L'"';
    select_command += select_path_cwd_buffer_utf16;
    if (!select_command.ends_with(dir_sep_utf16)) {
        select_command += dir_sep_utf16;
    }

    utf_written = utf8_to_utf16(entry.basic.path.data(), select_path_dirent_buffer_utf16, lengthof(select_path_dirent_buffer_utf16));

    if (utf_written == 0) {
        print_debug_msg("[ %d ] FAILED utf8_to_utf16(entry.basic.path)", expl.id);
        return { false, "Conversion of selected entry's path from UTF-8 to UTF-16." };
    }

    select_command += select_path_dirent_buffer_utf16;
    select_command += L'"';

    WCOUT_IF_DEBUG("select_command: [" << select_command.c_str() << "]\n");

    HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", select_command.c_str(), nullptr, SW_SHOWNORMAL);

    if ((intptr_t)result > HINSTANCE_ERROR) {
        return { true, "" };
    } else {
        return { false, get_last_error_string() };
    }
}

generic_result open_file(char const *file_name, char const *file_directory, bool as_admin) noexcept
{
    swan_path_t target_full_path_utf8 = path_create(file_directory);

    if (!path_append(target_full_path_utf8, file_name, global_state::settings().dir_separator_utf8, true)) {
        print_debug_msg("FAILED path_append, file_directory = [%s], file_name = [\\%s]", file_directory, file_name);
        return { false, "Full file path exceeds max path length." };
    }

    wchar_t target_full_path_utf16[MAX_PATH]; init_empty_cstr(target_full_path_utf16);

    s32 utf_written = utf8_to_utf16(target_full_path_utf8.data(), target_full_path_utf16, lengthof(target_full_path_utf16));

    if (utf_written == 0) {
        return { false, "Conversion of target's full path from UTF-8 to UTF-16." };
    }

    wchar_t const *operation = as_admin ? L"runas" : L"open";

    HINSTANCE result = ShellExecuteW(nullptr, operation, target_full_path_utf16, nullptr, nullptr, SW_SHOWNORMAL);

    auto ec = (intptr_t)result;

    if (ec > HINSTANCE_ERROR) {
        print_debug_msg("ShellExecuteW success");
        return { true, target_full_path_utf8.data() };
    }
    else if (ec == SE_ERR_NOASSOC) {
        print_debug_msg("FAILED ShellExecuteW: SE_ERR_NOASSOC");
        return { false, "No association between file type and program (ShellExecuteW: SE_ERR_NOASSOC)." };
    }
    else if (ec == SE_ERR_FNF) {
        print_debug_msg("FAILED ShellExecuteW: SE_ERR_FNF");
        return { false, "File not found (ShellExecuteW: SE_ERR_FNF)." };
    }
    else {
        auto err = get_last_error_string();
        print_debug_msg("FAILED ShellExecuteW: %s", err.c_str());
        return { false, err };
    }
}

generic_result symlink_data::extract(char const *lnk_file_path_utf8, char const *cwd) noexcept
{
    assert(lnk_file_path_utf8 != nullptr);

    swan_path_t lnk_file_full_path_utf8;
    wchar_t lnk_file_path_utf16[MAX_PATH]; init_empty_cstr(lnk_file_path_utf16);
    s32 utf_written = {};
    HRESULT com_handle = {};
    LPITEMIDLIST item_id_list = nullptr;

    if (cwd) {
        lnk_file_full_path_utf8 = path_create(cwd);

        if (!path_append(lnk_file_full_path_utf8, lnk_file_path_utf8, global_state::settings().dir_separator_utf8, true)) {
            return { false, "Max path length exceeded when appending symlink name to current working directory path." };
        }
    } else {
        lnk_file_full_path_utf8 = path_create(lnk_file_path_utf8);
    }

    utf_written = utf8_to_utf16(lnk_file_full_path_utf8.data(), lnk_file_path_utf16, lengthof(lnk_file_path_utf16));

    if (utf_written == 0) {
        return { false, "Conversion of symlink path from UTF-8 to UTF-16." };
    }

    com_handle = s_persist_file_interface->Load(lnk_file_path_utf16, STGM_READ);

    if (com_handle != S_OK) {
        return { false, "IPersistFile::Load(..., STGM_READ)." };
    }

    com_handle = s_shell_link->GetIDList(&item_id_list);

    if (com_handle != S_OK) {
        auto err = get_last_error_string();
        return { false, err + " (IShellLinkW::GetIDList)." };
    }

    if (!SHGetPathFromIDListW(item_id_list, this->target_path_utf16)) {
        auto err = get_last_error_string();
        return { false, err + " (SHGetPathFromIDListW)." };
    }

    utf_written = utf16_to_utf8(this->target_path_utf16, this->target_path_utf8.data(), this->target_path_utf8.size());

    if (utf_written == 0) {
        return { false, "Conversion of symlink target path from UTF-16 to UTF-8." };
    }

    com_handle = s_shell_link->GetWorkingDirectory(this->working_directory_path_utf16, MAX_PATH);

    if (com_handle != S_OK) {
        return { false, get_last_error_string() + " (IShellLinkW::GetWorkingDirectory)." };
    }

    com_handle = s_shell_link->GetArguments(this->arguments_utf16, 1024);

    if (com_handle != S_OK) {
        return { false, get_last_error_string() + " (IShellLinkW::GetArguments)." };
    }

    com_handle = s_shell_link->GetShowCmd(&this->show_cmd);

    if (com_handle != S_OK) {
        return { false, get_last_error_string() + " (IShellLinkW::GetShowCmd)" };
    }

    return { true, "" }; // success
}

/// @brief Attempts to extract information from a .lnk file and do something with it depending on the target type.
/// @param dirent The .lnk file.
/// @param expl Contextual explorer window.
/// @param symlink_type_out The type of the symlink.
/// @return If symlink points to a file, attempt ShellExecuteW("open") and return the result.
/// If symlink points to a directory, return the path of the pointed to directory in `error_or_utf8_path`.
/// If data extraction fails, the reason is stated in `error_or_utf8_path`.
static
generic_result open_symlink(explorer_window::dirent const &dirent, explorer_window &expl) noexcept
{
    symlink_data lnk_data = {};
    auto extract_result = lnk_data.extract(dirent.basic.path.data(), expl.cwd.data());
    if (!extract_result.success) {
        return extract_result; // propogate failure to caller
    }

    if (directory_exists(lnk_data.target_path_utf8.data())) {
        // symlink to a directory, tell caller to navigate there
        return { true, lnk_data.target_path_utf8.data() };
    }
    else {
        // symlink to a file, let's open it
        HINSTANCE result = ShellExecuteW(nullptr, L"open",
                                         lnk_data.target_path_utf16,
                                         lnk_data.arguments_utf16,
                                         lnk_data.working_directory_path_utf16,
                                         lnk_data.show_cmd);

        intptr_t err_code = (intptr_t)result;

        if (err_code > HINSTANCE_ERROR) {
            print_debug_msg("[ %d ] ShellExecuteW success", expl.id);
            return { true, lnk_data.target_path_utf8.data() };
        }
        else if (err_code == SE_ERR_NOASSOC) {
            print_debug_msg("[ %d ] ShellExecuteW error: SE_ERR_NOASSOC", expl.id);
            return { false, "No association between file type and program (ShellExecuteW: SE_ERR_NOASSOC)." };
        }
        else if (err_code == SE_ERR_FNF) {
            print_debug_msg("[ %d ] ShellExecuteW error: SE_ERR_FNF", expl.id);
            return { false, "File not found (ShellExecuteW: SE_ERR_FNF)." };
        }
        else {
            print_debug_msg("[ %d ] ShellExecuteW error: unexpected error", expl.id);
            return { false, get_last_error_string() };
        }
    }
}

/// @brief Sorts `expl.cwd_entries` in place according to `expl.sort_specs`.
/// Entries where `is_filtered_out` is true are forced to a contiguous block at the back of the vector.
/// The result is 2 distinct halves. the first half [expl.cwd_entries.begin(), retval) are entries which are unfiltered (to be shown),
/// sorted according to `expl.sort_specs`. The second half [retval, expl.cwd_entries.end()) are entries which are filtered (not to be shown).
/// @return Iterator to the first filtered entry if there is one, `cwd_entries.end()` otherwise.
static
std::vector<explorer_window::dirent>::iterator
sort_cwd_entries(explorer_window &expl, std::source_location sloc = std::source_location::current()) noexcept
{
    f64 sort_us = 0;
    SCOPE_EXIT { expl.sort_timing_samples.push_back(sort_us); };
    scoped_timer<timer_unit::MICROSECONDS> sort_timer(&sort_us);

    auto &cwd_entries = expl.cwd_entries;

    print_debug_msg("[ %d ] sort_cwd_entries() called from [%s:%d]", expl.id, cget_file_name(sloc.file_name()), sloc.line());

    using dir_ent_t = explorer_window::dirent;

    // move all filtered entries to the back of the vector
    std::sort(cwd_entries.begin(), cwd_entries.end(), [](dir_ent_t const &left, dir_ent_t const &right) {
        return left.is_filtered_out < right.is_filtered_out;
    });

    auto first_filtered_dirent = std::find_if(cwd_entries.begin(), cwd_entries.end(), [](dir_ent_t const &ent) {
        return ent.is_filtered_out;
    });

    // preliminary sort to ensure deterministic behaviour regardless of initial state.
    // necessary or else auto refresh can cause unexpected reordering of directory entries.
    std::sort(cwd_entries.begin(), first_filtered_dirent, [](dir_ent_t const &left, dir_ent_t const &right) {
        return left.basic.id < right.basic.id;
    });

    // comparator needs to return true when left < right
    std::sort(cwd_entries.begin(), first_filtered_dirent, [&](dir_ent_t const &left, dir_ent_t const &right) {
        bool left_lt_right = false;

        for (auto const &col_sort_spec : expl.column_sort_specs) {
            // comparing with this variable using == will handle the sort direction
            bool direction_flipper = col_sort_spec.SortDirection == ImGuiSortDirection_Ascending ? false : true;

            switch (col_sort_spec.ColumnUserID) {
                default:
                case explorer_window::cwd_entries_table_col_id: {
                    left_lt_right = (left.basic.id < right.basic.id) == direction_flipper;
                    break;
                }
                case explorer_window::cwd_entries_table_col_path: {
                    left_lt_right = (lstrcmpiA(left.basic.path.data(), right.basic.path.data()) < 0) == direction_flipper;
                    break;
                }
                case explorer_window::cwd_entries_table_col_type: {
                    auto compute_precedence = [](explorer_window::dirent const &ent) -> u32 {
                        // larger values have greater precedence
                        enum class precedence : u32
                        {
                            everything_else,
                            symlink_invalid,
                            symlink_file,
                            file,
                            symlink_directory,
                            directory,
                        };

                        if      (ent.basic.is_directory())            return (u32)precedence::directory;
                        else if (ent.basic.is_file())                 return (u32)precedence::file;
                        else if (ent.basic.is_symlink_to_directory()) return (u32)precedence::symlink_directory;
                        else if (ent.basic.is_symlink_to_file())      return (u32)precedence::symlink_file;
                        else if (ent.basic.is_symlink())              return (u32)precedence::symlink_invalid;
                        else                                          return (u32)precedence::everything_else;
                    };

                    u32 left_precedence = compute_precedence(left);
                    u32 right_precedence = compute_precedence(right);

                    left_lt_right = (left_precedence > right_precedence) == direction_flipper;
                    break;
                }
                case explorer_window::cwd_entries_table_col_size_pretty:
                case explorer_window::cwd_entries_table_col_size_bytes: {
                    if (left.basic.is_directory() && right.basic.is_file() && right.basic.size == 0) {
                        left_lt_right = true == direction_flipper;
                    } else {
                        left_lt_right = (left.basic.size < right.basic.size) == direction_flipper;
                    }
                    break;
                }
                case explorer_window::cwd_entries_table_col_creation_time: {
                    s32 cmp = CompareFileTime(&left.basic.creation_time_raw, &right.basic.creation_time_raw);
                    left_lt_right = (cmp <= 0) == direction_flipper;
                    break;
                }
                case explorer_window::cwd_entries_table_col_last_write_time: {
                    s32 cmp = CompareFileTime(&left.basic.last_write_time_raw, &right.basic.last_write_time_raw);
                    left_lt_right = (cmp <= 0) == direction_flipper;
                    break;
                }
            }
        }

        return left_lt_right;
    });

    return first_filtered_dirent;
}

bool explorer_window::update_cwd_entries(
    update_cwd_entries_actions actions,
    std::string_view parent_dir,
    std::source_location sloc) noexcept
{
    print_debug_msg("[ %d ] expl.update_cwd_entries(%d) called from [%s:%d]", this->id, actions, cget_file_name(sloc.file_name()), sloc.line());

    this->scroll_to_nth_selected_entry_next_frame = u64(-1);

    update_cwd_entries_timers timers = {};
    SCOPE_EXIT { this->update_cwd_entries_timing_samples.push_back(timers); };

    bool parent_dir_exists = false; // retval
    char dir_sep_utf8 = global_state::settings().dir_separator_utf8;

    {
        scoped_timer<timer_unit::MICROSECONDS> function_timer(&timers.total_us);

        if (actions & query_filesystem) {
            static std::vector<swan_path_t> selected_entries = {};
            selected_entries.clear();

            for (auto const &dirent : this->cwd_entries) {
                if (dirent.is_selected) {
                    // this could throw on alloc failure, which will call std::terminate
                    selected_entries.push_back(dirent.basic.path);
                }
            }

            this->cwd_entries.clear();

            if (parent_dir != "") {
                wchar_t search_path_utf16[512]; init_empty_cstr(search_path_utf16);
                {
                    scoped_timer<timer_unit::MICROSECONDS> searchpath_setup_timer(&timers.searchpath_setup_us);

                    u64 num_trailing_spaces = 0;
                    while (*(&parent_dir.back() - num_trailing_spaces) == ' ') {
                        ++num_trailing_spaces;
                    }
                    swan_path_t parent_dir_trimmed = {};
                    strncpy(parent_dir_trimmed.data(), parent_dir.data(), parent_dir.size() - num_trailing_spaces);

                    utf8_to_utf16(parent_dir_trimmed.data(), search_path_utf16, lengthof(search_path_utf16));

                    wchar_t dir_sep_w[] = { (wchar_t)dir_sep_utf8, L'\0' };

                    if (!parent_dir.ends_with(dir_sep_utf8)) {
                        (void) StrCatW(search_path_utf16, dir_sep_w);
                    }
                    (void) StrCatW(search_path_utf16, L"*");
                }

                // just for debug log
                {
                    char utf8_buffer[2048]; init_empty_cstr(utf8_buffer);

                    u64 utf_written = utf16_to_utf8(search_path_utf16, utf8_buffer, lengthof(utf8_buffer));

                    if (utf_written == 0) {
                        print_debug_msg("[ %d ] FAILED utf16_to_utf8(search_path)", this->id);
                        return parent_dir_exists;
                    }

                    print_debug_msg("[ %d ] querying filesystem, search_path = [%s]", this->id, utf8_buffer);
                }

                #if 1
                {
                    std::scoped_lock lock(select_cwd_entries_on_next_update_mutex);
                    scoped_timer<timer_unit::MICROSECONDS> sort_timer(&timers.entries_to_select_sort);
                    std::sort(select_cwd_entries_on_next_update.begin(), select_cwd_entries_on_next_update.end(), std::greater<swan_path_t>());
                }
                #endif

                scoped_timer<timer_unit::MICROSECONDS> filesystem_timer(&timers.filesystem_us);

                WIN32_FIND_DATAW find_data;
                HANDLE find_handle = FindFirstFileW(search_path_utf16, &find_data);
                SCOPE_EXIT { FindClose(find_handle); };

                if (find_handle == INVALID_HANDLE_VALUE) {
                    print_debug_msg("[ %d ] find_handle == INVALID_HANDLE_VALUE", this->id);
                    parent_dir_exists = false;
                    return parent_dir_exists;
                } else {
                    parent_dir_exists = true;
                }

                u32 entry_id = 0;

                do {
                    explorer_window::dirent entry = {};
                    entry.basic.id = entry_id;
                    entry.basic.size = two_u32_to_one_u64(find_data.nFileSizeLow, find_data.nFileSizeHigh);
                    entry.basic.creation_time_raw = find_data.ftCreationTime;
                    entry.basic.last_write_time_raw = find_data.ftLastWriteTime;

                    // entry.basic.path = find_data.cFileName;
                    {
                        s32 utf_written = utf16_to_utf8(find_data.cFileName, entry.basic.path.data(), entry.basic.path.size());

                        if (utf_written == 0) {
                            print_debug_msg("[ %d ] FAILED utf16_to_utf8(find_data.cFileName)", this->id);
                            continue;
                        }
                    }

                    if (path_equals_exactly(entry.basic.path, ".")) {
                        continue;
                    }

                    if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        entry.basic.type = basic_dirent::kind::directory;
                    }
                    else if (path_ends_with(entry.basic.path, ".lnk")) {
                        // TODO: this branch is quite slow, there should probably be an option to opt out of checking the type and validity of symlinks

                        entry.basic.type = basic_dirent::kind::invalid_symlink; // default value, if something fails below

                        static std::wstring full_path_utf16 = {};
                        full_path_utf16.clear();
                        full_path_utf16.append(search_path_utf16);
                        full_path_utf16.pop_back(); // remove '*'
                        full_path_utf16.append(find_data.cFileName);

                        // Load the shortcut
                        HRESULT com_handle = s_persist_file_interface->Load(full_path_utf16.c_str(), STGM_READ);
                        if (FAILED(com_handle)) {
                            WCOUT_IF_DEBUG("FAILED IPersistFile::Load [" << full_path_utf16.c_str() << "]\n");
                        }
                        else {
                            // Get the target path
                            wchar_t target_path_utf16[MAX_PATH];
                            com_handle = s_shell_link->GetPath(target_path_utf16, lengthof(target_path_utf16), NULL, SLGP_RAWPATH);
                            if (FAILED(com_handle)) {
                                WCOUT_IF_DEBUG("FAILED IShellLinkW::GetPath [" << full_path_utf16.c_str() << "]\n");
                            }
                            else {
                                if      (PathIsDirectoryW(target_path_utf16)) entry.basic.type = basic_dirent::kind::symlink_to_directory;
                                else if (PathFileExistsW(target_path_utf16))  entry.basic.type = basic_dirent::kind::symlink_to_file;
                                else                                          entry.basic.type = basic_dirent::kind::invalid_symlink;
                            }
                        }
                    }
                    else {
                        entry.basic.type = basic_dirent::kind::file;
                    }

                    if (entry.basic.is_path_dotdot()) {
                        if (global_state::settings().show_dotdot_dir) {
                            this->cwd_entries.emplace_back(entry);
                            std::swap(this->cwd_entries.back(), this->cwd_entries.front());
                        }
                    } else {
                        for (auto prev_selected_entry = selected_entries.begin(); prev_selected_entry != selected_entries.end(); ++prev_selected_entry) {
                            bool was_selected_before_refresh = path_equals_exactly(entry.basic.path, *prev_selected_entry);
                            if (was_selected_before_refresh) {
                                entry.is_selected = true;
                                std::swap(*prev_selected_entry, selected_entries.back());
                                selected_entries.pop_back();
                                break;
                            }
                        }

                        {
                            std::scoped_lock lock(select_cwd_entries_on_next_update_mutex);

                            f64 search_us = 0;
                            scoped_timer<timer_unit::MICROSECONDS> search_timer(&search_us);

                        #if 1
                            bool found = std::binary_search(select_cwd_entries_on_next_update.begin(), select_cwd_entries_on_next_update.end(), entry.basic.path);
                            if (found) {
                                auto lower_it = std::lower_bound(select_cwd_entries_on_next_update.begin(), select_cwd_entries_on_next_update.end(), entry.basic.path);
                                auto actual_found_thing = select_cwd_entries_on_next_update.begin() + std::distance(select_cwd_entries_on_next_update.begin(), lower_it);
                                std::swap(*actual_found_thing, select_cwd_entries_on_next_update.back());
                                select_cwd_entries_on_next_update.pop_back();
                                entry.is_selected = true;
                            }
                        #else
                            for (auto entry_to_select = this->select_cwd_entries_on_next_update.begin(); entry_to_select != this->select_cwd_entries_on_next_update.end(); ++entry_to_select) {
                                if (path_equals_exactly(*entry_to_select, entry.basic.path.data())) {
                                    entry.is_selected = true;
                                    std::swap(*entry_to_select, this->select_cwd_entries_on_next_update.back());
                                    this->select_cwd_entries_on_next_update.pop_back();
                                    break;
                                }
                            }
                        #endif

                            timers.entries_to_select_search += search_us;
                        }

                        // this could throw on alloc failure, which will call std::terminate
                        this->cwd_entries.emplace_back(entry);
                    }

                    ++this->num_file_finds;
                    ++entry_id;
                }
                while (FindNextFileW(find_handle, &find_data));

                this->refresh_message.clear();
                this->last_filesystem_query_time = current_time_precise();
                {
                    std::scoped_lock lock(select_cwd_entries_on_next_update_mutex);
                    this->select_cwd_entries_on_next_update.clear();
                }
            }
        }

        if (actions & filter) {
            scoped_timer<timer_unit::MICROSECONDS> filter_timer(&timers.filter_us);

            this->filter_error.clear();

            bool dirent_type_to_visibility_table[(u64)basic_dirent::kind::count] = {
                this->filter_show_directories,
                this->filter_show_files,
                this->filter_show_symlink_directories,
                this->filter_show_symlink_files,
                this->filter_show_invalid_symlinks,
            };

            u64 filter_text_len = strlen(this->filter_text.data());

            for (auto &dirent : this->cwd_entries) {
                bool this_type_of_dirent_is_visible = dirent_type_to_visibility_table[(u64)dirent.basic.type];

                dirent.is_filtered_out = !this_type_of_dirent_is_visible;
                dirent.highlight_start_idx = 0;
                dirent.highlight_len = 0;

                if (this_type_of_dirent_is_visible && filter_text_len > 0) { // apply textual filter against dirent name
                    char const *dirent_name = dirent.basic.path.data();

                    switch (this->filter_mode) {
                        default:
                        case explorer_window::filter_mode::contains: {
                            auto matcher = this->filter_case_sensitive ? StrStrA : StrStrIA;

                            char const *match_start = matcher(dirent_name, this->filter_text.data());;
                            bool filtered_out = this->filter_polarity != (bool)match_start;
                            dirent.is_filtered_out = filtered_out;

                            if (!filtered_out && filter_polarity == true) {
                                // highlight just the substring
                                dirent.highlight_start_idx = std::distance(dirent_name, match_start);
                                dirent.highlight_len = filter_text_len;
                            }

                            break;
                        }

                        case explorer_window::filter_mode::regex_match: {
                            static std::regex filter_regex;
                            try {
                                scoped_timer<timer_unit::MICROSECONDS> regex_ctor_timer(&timers.regex_ctor_us);
                                filter_regex = this->filter_text.data();
                            }
                            catch (std::exception const &except) {
                                this->filter_error = except.what();
                                break;
                            }

                            auto match_flags = std::regex_constants::match_default | (std::regex_constants::icase * (this->filter_case_sensitive == 0));

                            bool filtered_out = this->filter_polarity != std::regex_match(dirent_name, filter_regex, (std::regex_constants::match_flag_type)match_flags);
                            dirent.is_filtered_out = filtered_out;

                            if (!filtered_out && filter_polarity == true) {
                                // highlight the whole path since we are using std::regex_match
                                dirent.highlight_start_idx = 0;
                                dirent.highlight_len = path_length(dirent.basic.path);
                            }

                            break;
                        }
                    }
                }
            }
        }
    }

    (void) sort_cwd_entries(*this);

    this->frame_count_when_cwd_entries_updated = imgui::GetFrameCount();

    return parent_dir_exists;
}

bool explorer_window::save_to_disk() const noexcept
{
    f64 save_to_disk_us = {};
    SCOPE_EXIT { this->save_to_disk_timing_samples.push_back(save_to_disk_us); };
    scoped_timer<timer_unit::MICROSECONDS> save_to_disk_timer(&save_to_disk_us);

    char file_name[32]; init_empty_cstr(file_name);
    [[maybe_unused]] s32 written = snprintf(file_name, lengthof(file_name), "data\\explorer_%d.txt", this->id);
    assert(written < lengthof(file_name));
    std::filesystem::path full_path = global_state::execution_path() / file_name;

    bool result = true;

    try {
        std::ofstream out(full_path);
        if (!out) {
            result = false;
        } else {
            out << "cwd " << path_length(cwd) << ' ' << cwd.data() << '\n';

            out << "filter " << strlen(filter_text.data()) << ' ' << filter_text.data() << '\n';

            out << "filter_mode "                       << (s32)filter_mode << '\n';
            out << "filter_case_sensitive "             << (s32)filter_case_sensitive << '\n';
            out << "filter_polarity "                   << (s32)filter_polarity << '\n';
            out << "filter_show_directories "           << (s32)filter_show_directories << '\n';
            out << "filter_show_symlink_directories "   << (s32)filter_show_symlink_directories << '\n';
            out << "filter_show_files "                 << (s32)filter_show_files << '\n';
            out << "filter_show_symlink_files "         << (s32)filter_show_symlink_files << '\n';
            out << "filter_show_invalid_symlinks "      << (s32)filter_show_invalid_symlinks << '\n';

            out << "wd_history_pos "                    << wd_history_pos << '\n';
            out << "wd_history.size() "                 << wd_history.size() << '\n';

            for (auto const &item : wd_history) {
                out << path_length(item) << ' ' << item.data() << '\n';
            }
        }
    }
    catch (...) {
        result = false;
    }

    print_debug_msg("[%s] save attempted, result: %d", file_name, result);
    this->latest_save_to_disk_result = (s8)result;

    return result;
}

bool explorer_window::load_from_disk(char dir_separator) noexcept
{
    assert(this->name != nullptr);

    char file_name[32]; init_empty_cstr(file_name);
    [[maybe_unused]] s32 written = snprintf(file_name, lengthof(file_name), "data\\explorer_%d.txt", this->id);
    assert(written < lengthof(file_name));
    std::filesystem::path full_path = global_state::execution_path() / file_name;

    try {
        std::ifstream in(full_path);
        if (!in) {
            print_debug_msg("FAILED to open file [%s]", file_name);
            return false;
        }

        char whitespace = 0;
        std::string what = {};
        what.reserve(256);

        auto read_bool = [&](char const *label, bool &value) {
            in >> what;
            assert(what == label);

            s32 read_val = 0;
            in >> read_val;

            value = (bool)read_val;
            print_debug_msg("[%s] %s = %d", file_name, label, value);
        };

        {
            in >> what;
            assert(what == "cwd");

            u64 cwd_len = 0;
            in >> cwd_len;
            print_debug_msg("[%s] cwd_len = %zu", file_name, cwd_len);

            in.read(&whitespace, 1);

            in.read(cwd.data(), cwd_len);
            path_force_separator(cwd, dir_separator);
            print_debug_msg("[%s] cwd = [%s]", file_name, cwd.data());
        }

        {
            in >> what;
            assert(what == "filter");

            u64 filter_len = 0;
            in >> filter_len;
            print_debug_msg("[%s] filter_len = %zu", file_name, filter_len);

            in.read(&whitespace, 1);

            in.read(filter_text.data(), filter_len);
            print_debug_msg("[%s] filter = [%s]", file_name, filter_text.data());
        }

        {
            in >> what;
            assert(what == "filter_mode");

            in >> (s32 &)filter_mode;
            print_debug_msg("[%s] filter_mode = %d", file_name, filter_mode);
        }

        read_bool("filter_case_sensitive", filter_case_sensitive);
        read_bool("filter_polarity", filter_polarity);
        read_bool("filter_show_directories", filter_show_directories);
        read_bool("filter_show_symlink_directories", filter_show_symlink_directories);
        read_bool("filter_show_files", filter_show_files);
        read_bool("filter_show_symlink_files", filter_show_symlink_files);
        read_bool("filter_show_invalid_symlinks", filter_show_invalid_symlinks);

        {
            in >> what;
            assert(what == "wd_history_pos");

            in >> wd_history_pos;
            print_debug_msg("[%s] wd_history_pos = %zu", file_name, wd_history_pos);
        }

        u64 wd_history_size = 0;
        {
            in >> what;
            assert(what == "wd_history.size()");

            in >> wd_history_size;
            print_debug_msg("[%s] wd_history_size = %zu", file_name, wd_history_size);
        }

        wd_history.resize(wd_history_size);
        for (u64 i = 0; i < wd_history_size; ++i) {
            u64 item_len = 0;
            in >> item_len;

            in.read(&whitespace, 1);

            in.read(wd_history[i].data(), item_len);
            path_force_separator(wd_history[i], dir_separator);
            print_debug_msg("[%s] history[%zu] = [%s]", file_name, i, wd_history[i].data());
        }
    }
    catch (...) {
        return false;
    }

    return true;
}

void explorer_window::push_history_item(swan_path_t const &new_latest_entry) noexcept
{
    char dir_sep_utf8 = global_state::settings().dir_separator_utf8;

    swan_path_t new_latest_entry_clean = new_latest_entry;
    path_force_separator(new_latest_entry_clean, dir_sep_utf8);

    path_pop_back_if(new_latest_entry_clean, dir_sep_utf8);

    // TODO: split `new_latest_entry_clean` by `dir_sep_utf8` and progressively reconstruct path with canonical capitalization
    // new_latest_entry_clean = path_reconstruct_canonically(new_latest_entry_clean.data(), dir_sep_utf8);

    if (this->wd_history.empty()) {
        this->wd_history_pos = 0;
    }
    else {
        u64 num_trailing_history_items_to_del = this->wd_history.size() - this->wd_history_pos - 1;

        for (u64 i = 0; i < num_trailing_history_items_to_del; ++i) {
            this->wd_history.pop_back();
        }

        if (this->wd_history.size() == explorer_window::MAX_WD_HISTORY_SIZE) {
            this->wd_history.pop_front();
        } else {
            ++this->wd_history_pos;
        }
    }

    this->wd_history.push_back(new_latest_entry_clean);
}

struct ascend_result
{
    bool success;
    swan_path_t parent_dir;
};

static
ascend_result try_ascend_directory(explorer_window &expl) noexcept
{
    char dir_sep_utf8 = global_state::settings().dir_separator_utf8;
    ascend_result res = {};
    res.parent_dir = expl.cwd;

    // if there is a trailing separator, remove it
    path_pop_back_if(res.parent_dir, dir_sep_utf8);
    // remove anything between end and final separator
    while (path_pop_back_if_not(res.parent_dir, dir_sep_utf8));

    bool parent_dir_exists = expl.update_cwd_entries(query_filesystem, res.parent_dir.data());
    res.success = parent_dir_exists;
    print_debug_msg("[ %d ] try_ascend_directory parent_dir=[%s] res.success=%d", expl.id, res.parent_dir.data(), res.success);

    if (parent_dir_exists) {
        if (!path_is_empty(expl.cwd)) {
            expl.push_history_item(res.parent_dir);
        }
        expl.filter_error.clear();
        expl.cwd_latest_selected_dirent_idx = explorer_window::NO_SELECTION;
        expl.cwd_latest_selected_dirent_idx_changed = false;
        expl.cwd = res.parent_dir;
        expl.set_latest_valid_cwd(expl.cwd); // this may mutate filter
        (void) expl.update_cwd_entries(filter, res.parent_dir.data());
        (void) expl.save_to_disk();
    }

    return res;
}

struct descend_result
{
    bool success;
    std::string err_msg;
};

static
descend_result try_descend_to_directory(explorer_window &expl, char const *target_utf8) noexcept
{
    char dir_sep_utf8 = global_state::settings().dir_separator_utf8;

    swan_path_t new_cwd_utf8 = expl.cwd;

    bool prepend_separator = path_length(expl.cwd) > 0;
    if (!path_append(new_cwd_utf8, target_utf8, dir_sep_utf8, prepend_separator)) {
        print_debug_msg("[ %d ] FAILED path_append, new_cwd_utf8 = [%s], append data = [%c%s]", expl.id, new_cwd_utf8.data(), dir_sep_utf8, target_utf8);
        descend_result res;
        res.success = false;
        res.err_msg = "Max path length exceeded when trying to append target to current working directory path.";
        return res;
    }

    wchar_t new_cwd_utf16[MAX_PATH]; init_empty_cstr(new_cwd_utf16);

    s32 utf_written = utf8_to_utf16(new_cwd_utf8.data(), new_cwd_utf16, lengthof(new_cwd_utf16));

    if (utf_written == 0) {
        print_debug_msg("[ %d ] utf8_to_utf16(new_cwd) FAILED", expl.id);
        descend_result res;
        res.success = false;
        res.err_msg = "Conversion of new cwd path from UTF-8 to UTF-16.";
        return res;
    }

    wchar_t new_cwd_canonical_utf16[MAX_PATH]; init_empty_cstr(new_cwd_canonical_utf16);

    {
        HRESULT handle = PathCchCanonicalize(new_cwd_canonical_utf16, lengthof(new_cwd_canonical_utf16), new_cwd_utf16);
        if (handle != S_OK) {
            descend_result res;
            res.success = false;
            switch (handle) {
                case E_INVALIDARG: res.err_msg = "PathCchCanonicalize E_INVALIDARG - the cchPathOut value is > PATHCCH_MAX_CCH."; break;
                case E_OUTOFMEMORY: res.err_msg = "PathCchCanonicalize E_OUTOFMEMORY - the function could not allocate a buffer of the necessary size."; break;
                default: res.err_msg = "Unknown PathCchCanonicalize error."; break;
            }
            return res;
        }
    }

    swan_path_t new_cwd_canoncial_utf8;

    utf_written = utf16_to_utf8(new_cwd_canonical_utf16, new_cwd_canoncial_utf8.data(), new_cwd_canoncial_utf8.size());

    if (utf_written == 0) {
        print_debug_msg("[ %d ] utf16_to_utf8(new_cwd_canonical) FAILED", expl.id);
        descend_result res;
        res.success = false;
        res.err_msg = "Conversion of new canonical cwd path from UTF-8 to UTF-16.";
        return res;
    }

    bool cwd_exists = expl.update_cwd_entries(query_filesystem, new_cwd_canoncial_utf8.data());

    if (!cwd_exists) {
        print_debug_msg("[ %d ] target directory not found", expl.id);
        descend_result res;
        res.success = false;
        res.err_msg = make_str("Target directory [%s] not found.", new_cwd_canoncial_utf8.data());
        return res;
    }

    expl.push_history_item(new_cwd_canoncial_utf8);
    expl.cwd = new_cwd_canoncial_utf8;
    expl.set_latest_valid_cwd(expl.cwd); // this may mutate filter
    (void) expl.update_cwd_entries(filter, new_cwd_canoncial_utf8.data());
    expl.cwd_latest_selected_dirent_idx = explorer_window::NO_SELECTION;
    expl.cwd_latest_selected_dirent_idx_changed = false;
    expl.filter_error.clear();
    (void) expl.save_to_disk();

    descend_result res;
    res.success = true;
    res.err_msg = "";

    return res;
}

struct cwd_text_input_callback_user_data
{
    s64 expl_id;
    wchar_t dir_sep_utf16;
    bool edit_occurred;
    char *text_content;
};

static
s32 cwd_text_input_callback(ImGuiInputTextCallbackData *data) noexcept
{
    auto user_data = (cwd_text_input_callback_user_data *)(data->UserData);
    user_data->edit_occurred = false;

    auto is_separator = [](wchar_t ch) { return ch == L'/' || ch == L'\\'; };

    if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        if (is_separator(data->EventChar)) {
            data->EventChar = user_data->dir_sep_utf16;
        }
    }
    else if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit) {
        print_debug_msg("[ %d ] ImGuiInputTextFlags_CallbackEdit Buf:[%s]", user_data->expl_id, data->Buf);
        user_data->edit_occurred = true;
    }

    return 0;
}

static
void render_debug_info(explorer_window &expl, u64 size_unit_multiplier) noexcept
{
#if 1
    {
        u64 bytes_occupied = expl.cwd_entries.size() * sizeof(swan_path_t);
        u64 bytes_actually_used = 0;

        for (auto const &dirent : expl.cwd_entries) {
            bytes_actually_used += path_length(dirent.basic.path);
        }

        char buffer1[32]; init_empty_cstr(buffer1);
        format_file_size(bytes_occupied, buffer1, lengthof(buffer1), size_unit_multiplier);

        f64 usage_ratio = ( f64(bytes_actually_used) + (bytes_occupied == 0) ) / ( f64(bytes_occupied) + (bytes_occupied == 0) );
        f64 waste_percent = 100.0 - (usage_ratio * 100.0);
        u64 bytes_wasted = u64( bytes_occupied * (1.0 - usage_ratio) );

        char buffer2[32]; init_empty_cstr(buffer2);
        format_file_size(bytes_wasted, buffer2, lengthof(buffer1), size_unit_multiplier);

        imgui::Text("swan_path_t memory footprint: %s, %3.1lf %% waste -> %s", buffer1, waste_percent, buffer2);
    }
#endif

    imgui::Text("latest_valid_cwd: [%s]", expl.latest_valid_cwd.data());
    imgui::Text("select_cwd_entries_on_next_update.size(): %zu", expl.select_cwd_entries_on_next_update.size());
    imgui::Text("num_file_finds: %zu", expl.num_file_finds);
    imgui::Text("cwd_latest_selected_dirent_idx: %zu", expl.cwd_latest_selected_dirent_idx);
    imgui::Text("latest_save_to_disk_result: %d", expl.latest_save_to_disk_result);

    imgui::Text("entries_to_select_sort: %.1lf us", expl.update_cwd_entries_timing_samples.empty() ? NAN : expl.update_cwd_entries_timing_samples.back().entries_to_select_sort);
    imgui::Text("entries_to_select_search: %.1lf us", expl.update_cwd_entries_timing_samples.empty() ? NAN : expl.update_cwd_entries_timing_samples.back().entries_to_select_search);

#if 0
    {
        char const *labels[] = {
            "searchpath_setup_us",
            "filesystem_us",
            "filter_us",
            "regex_ctor_us",
            "remainder_us",
        };

        constexpr u64 rows = explorer_window::num_timing_samples;
        constexpr u64 cols = lengthof(labels);

        f64 matrix[rows][cols] = {};

        for (u64 r = 0; r < rows; ++r) {
            memcpy(&matrix[r][0], &expl.update_cwd_entries_timing_samples[r], 4 * sizeof(f64));
            // matrix[0][r] = expl.update_cwd_entries_timing_samples[r].searchpath_setup_us;
            // matrix[1][r] = expl.update_cwd_entries_timing_samples[r].filesystem_us;
            // matrix[2][r] = expl.update_cwd_entries_timing_samples[r].filter_us;
            // matrix[3][r] = expl.update_cwd_entries_timing_samples[r].regex_ctor_us;

            // remainder
            matrix[4][r] = expl.update_cwd_entries_timing_samples[r].total_us - (
                expl.update_cwd_entries_timing_samples[r].searchpath_setup_us +
                expl.update_cwd_entries_timing_samples[r].filesystem_us +
                expl.update_cwd_entries_timing_samples[r].filter_us +
                expl.update_cwd_entries_timing_samples[r].regex_ctor_us
            );
        }

        if (implot::BeginPlot("update_cwd_entries timings")) {
            implot::PlotBarGroups(labels, matrix, rows, cols, 0.67, 0, ImPlotBarGroupsFlags_Stacked);
            implot::EndPlot();
        }
    }
#endif
}

static
void render_num_cwd_items(cwd_count_info const &cnt) noexcept
{
    if (cnt.filtered_dirents == 0) {
        imgui::Text("%zu items", cnt.child_dirents);
    } else {
        u64 cnt_visible = cnt.child_dirents - cnt.filtered_dirents;
        imgui::Text("%zu (of %zu) items", cnt_visible, cnt.child_dirents);
    }

    if (imgui::IsItemHovered() && imgui::BeginTooltip()) {
        if (cnt.child_dirents == 0) {
            imgui::TextUnformatted("No items in this directory.");
        }
        else {
            imgui::Text("%zu total entries in this directory.", cnt.child_dirents);

            f64 percent_dirs     = (f64(cnt.child_directories) / f64(cnt.child_dirents)) * 100.0;
            f64 percent_symlinks = (f64(cnt.child_symlinks)    / f64(cnt.child_dirents)) * 100.0;
            f64 percent_files    = (f64(cnt.child_files)       / f64(cnt.child_dirents)) * 100.0;

            if (cnt.child_directories > 0) {
                imgui::TextColored(dir_color(), "%zu (%.2lf %%) director%s.", cnt.child_directories, percent_dirs, pluralized(cnt.child_directories, "y", "ies"));
            }
            if (cnt.child_symlinks > 0) {
                imgui::TextColored(symlink_color(), "%zu (%.2lf %%) symlink%s.", cnt.child_symlinks, percent_symlinks, pluralized(cnt.child_symlinks, "", "s"));
            }
            if (cnt.child_files > 0) {
                imgui::TextColored(file_color(), "%zu (%.2lf %%) file%s.", cnt.child_files, percent_files, pluralized(cnt.child_files, "", "s"));
            }
        }

        imgui::EndTooltip();
    }
}

static
void render_num_cwd_items_filtered(cwd_count_info const &cnt) noexcept
{
    imgui::Text("(%zu filtered)", cnt.filtered_dirents);

    if (imgui::IsItemHovered() && imgui::BeginTooltip()) {
        if (cnt.filtered_directories > 0) {
            imgui::TextColored(dir_color(), "%zu director%s filtered.", cnt.filtered_directories, pluralized(cnt.filtered_directories, "y", "ies"));
        }
        if (cnt.filtered_symlinks > 0) {
            imgui::TextColored(symlink_color(), "%zu symlink%s filtered.", cnt.filtered_symlinks, pluralized(cnt.filtered_symlinks, "", "s"));
        }
        if (cnt.filtered_files > 0) {
            imgui::TextColored(file_color(), "%zu file%s filtered.", cnt.filtered_files, pluralized(cnt.filtered_files, "", "s"));
        }

        imgui::EndTooltip();
    }
}

static
void render_num_cwd_items_selected(explorer_window &expl, cwd_count_info const &cnt) noexcept
{
    assert(cnt.child_dirents > 0);

    imgui::Text("(%zu selected)", cnt.selected_dirents);

    if (imgui::IsItemHovered() && imgui::BeginTooltip()) {
        imgui::TextUnformatted("Click to see next selected entry.");

        if (cnt.selected_directories > 0) {
            imgui::TextColored(dir_color(), "%zu director%s selected.", cnt.selected_directories, pluralized(cnt.selected_directories, "y", "ies"));
        }
        if (cnt.selected_symlinks > 0) {
            imgui::TextColored(symlink_color(), "%zu symlink%s selected.", cnt.selected_symlinks, pluralized(cnt.selected_symlinks, "", "s"));
        }
        if (cnt.selected_files > 0) {
            imgui::TextColored(file_color(), "%zu file%s selected.", cnt.selected_files, pluralized(cnt.selected_files, "", "s"));
        }

        imgui::EndTooltip();
    }

    if (imgui::IsItemClicked()) {
        bool start_spotlight_from_the_top = expl.nth_last_cwd_dirent_scrolled == u64(-1)                  || // haven't started spotlighting yet
                                            expl.nth_last_cwd_dirent_scrolled == cnt.selected_dirents - 1 || // spotlighting the bottom-most dirent, need to wrap around
                                            expl.cwd_latest_selected_dirent_idx_changed;

        if (start_spotlight_from_the_top) {
            expl.nth_last_cwd_dirent_scrolled = 0;
            expl.cwd_latest_selected_dirent_idx_changed = false;
        } else {
            expl.nth_last_cwd_dirent_scrolled += 1;
        }

        expl.scroll_to_nth_selected_entry_next_frame = expl.nth_last_cwd_dirent_scrolled;
    }
}

static
void render_file_op_payload_hint() noexcept
{
    {
        char buffer[64]; init_empty_cstr(buffer);
        (void) snprintf(buffer, lengthof(buffer), "%s %zu", ICON_CI_COPY, s_file_op_payload.items.size());
        if (imgui::Selectable(buffer, false, 0, imgui::CalcTextSize(buffer))) {
            imgui::OpenPopup("File Operation Payload");
        }
    }
    if (imgui::IsItemHovered() && imgui::IsMouseClicked(ImGuiMouseButton_Right)) {
        s_file_op_payload.clear();
    }
    if (imgui::IsItemHovered()) {
        imgui::SetTooltip("File operation(s) ready\n""Left click to view\n""Right click to clear\n");
    }
    if (imgui::BeginPopup("File Operation Payload")) {
        imgui::Text("%zu operation%s will be executed on next paste:",
            s_file_op_payload.items.size(),
            s_file_op_payload.items.size() > 1 ? "s" : "");

        imgui::Spacing();
        imgui::Separator();

        if (imgui::BeginTable("file_operation_command_buf", 2)) {
            ImGuiListClipper clipper;
            {
                u64 num_dirents_to_render = s_file_op_payload.items.size();
                assert(num_dirents_to_render <= (u64)INT32_MAX);
                clipper.Begin(s32(num_dirents_to_render));
            }

            while (clipper.Step())
            for (u64 i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                auto const &item = s_file_op_payload.items[i];

                imgui::TableNextColumn();
                imgui::TextUnformatted(item.operation);

                imgui::TableNextColumn();
                imgui::TextColored(get_color(item.type), get_icon(item.type));
                imgui::SameLine();
                #if 1
                imgui::TextUnformatted(item.path.data());
                #else
                {
                    char buffer[2048]; init_empty_cstr(buffer);
                    (void) snprintf(buffer, lengthof(buffer), "%s##file_operation_command_buf.items[%zu]", item.path.data(), i);
                    if (imgui::Selectable(buffer)) {
                        // TODO: jump to dirent in explorer
                    }
                }
                #endif
            }

            imgui::EndTable();
        }

        imgui::EndPopup();
    }
}

static
void render_back_to_prev_valid_cwd_button(explorer_window &expl) noexcept
{
    auto io = imgui::GetIO();

    imgui::ScopedDisable disabled(expl.wd_history_pos == 0);

    if (imgui::Button(ICON_CI_CHEVRON_LEFT "##back")) {
        print_debug_msg("[ %d ] back arrow button triggered", expl.id);

        if (io.KeyShift || io.KeyCtrl) {
            expl.wd_history_pos = 0;
        } else {
            expl.wd_history_pos -= 1;
        }

        expl.cwd = expl.wd_history[expl.wd_history_pos];
        bool back_dir_exists = expl.update_cwd_entries(query_filesystem, expl.cwd.data());
        if (back_dir_exists) {
            expl.set_latest_valid_cwd(expl.cwd); // this may mutate filter
            (void) expl.update_cwd_entries(filter, expl.cwd.data());
            (void) expl.save_to_disk();
        }
    }
}

static
void render_forward_to_next_valid_cwd_button(explorer_window &expl) noexcept
{
    u64 wd_history_last_idx = expl.wd_history.empty() ? 0 : expl.wd_history.size() - 1;
    auto io = imgui::GetIO();

    imgui::ScopedDisable disabled(expl.wd_history_pos == wd_history_last_idx);

    if (imgui::Button(ICON_CI_CHEVRON_RIGHT "##forward")) {
        print_debug_msg("[ %d ] forward arrow button triggered", expl.id);

        if (io.KeyShift || io.KeyCtrl) {
            expl.wd_history_pos = wd_history_last_idx;
        } else {
            expl.wd_history_pos += 1;
        }

        expl.cwd = expl.wd_history[expl.wd_history_pos];
        bool forward_dir_exists = expl.update_cwd_entries(query_filesystem, expl.cwd.data());
        if (forward_dir_exists) {
            expl.set_latest_valid_cwd(expl.cwd); // this may mutate filter
            (void) expl.update_cwd_entries(filter, expl.cwd.data());
        }
    }
}

static
bool render_history_browser_popup(explorer_window &expl, bool cwd_exists_before_edit) noexcept
{
    auto cleanup_and_close_popup = []() {
        imgui::CloseCurrentPopup();
    };

    SCOPE_EXIT { imgui::EndPopup(); };

    imgui::TextUnformatted("History");

    imgui::SameLine();

    imgui::BeginDisabled(expl.wd_history.empty());
    if (imgui::SmallButton("Clear")) {
        expl.wd_history.clear();
        expl.wd_history_pos = 0;

        if (cwd_exists_before_edit) {
            expl.push_history_item(expl.cwd);
        }

        expl.save_to_disk();
        cleanup_and_close_popup();
    }
    imgui::EndDisabled();

    imgui::Separator();

    if (expl.wd_history.empty()) {
        imgui::TextUnformatted("(empty)");
    }
    else {
        if (imgui::BeginTable("history_table", 3)) {
            u64 i = expl.wd_history.size() - 1;
            u64 i_inverse = 0;

            for (auto iter = expl.wd_history.rbegin(); iter != expl.wd_history.rend(); ++iter, --i, ++i_inverse) {
                imgui::TableNextRow();
                swan_path_t const &hist_path = *iter;

                imgui::TableNextColumn();
                if (i == expl.wd_history_pos) {
                    imgui::TextColored(orange(), ICON_FA_LONG_ARROW_ALT_RIGHT);
                }

                imgui::TableNextColumn();
                imgui::Text("%3zu ", i_inverse + 1);

                char buffer[2048]; init_empty_cstr(buffer);
                {
                    [[maybe_unused]] s32 written = snprintf(buffer, lengthof(buffer), "%s ##%zu", hist_path.data(), i);
                    assert(written < lengthof(buffer));
                }

                imgui::TableNextColumn();

                bool pressed;
                {
                    imgui::ScopedTextColor tc(dir_color());
                    pressed = imgui::Selectable(buffer, false, ImGuiSelectableFlags_SpanAllColumns);
                }

                if (pressed) {
                    expl.wd_history_pos = i;
                    expl.cwd = expl.wd_history[i];

                    cleanup_and_close_popup();
                    imgui::EndTable();

                    return true;
                }
            }

            imgui::EndTable();
        }
    }

    return false;
}

static
void render_pins_popup(explorer_window &expl) noexcept
{
    auto cleanup_and_close_popup = []() {
        imgui::CloseCurrentPopup();
    };

    imgui::AlignTextToFramePadding();
    imgui::TextUnformatted("Pins");

    imgui::SameLine();

    if (imgui::Button("Manage")) {
        global_state::settings().show.pin_manager = true;
        (void) global_state::settings().save_to_disk();
        ImGui::SetWindowFocus(" Pinned ");
    }

    imgui::Separator();

    auto const &pins = global_state::pins();

    if (pins.empty()) {
        imgui::TextUnformatted("(empty)");
    }
    else {
        for (auto const &pin : pins) {
            {
                imgui::ScopedTextColor tc(pin.color);
                bool selected = false;

                if (imgui::Selectable(pin.label.c_str(), &selected)) {
                    if (!directory_exists(pin.path.data())) {
                        swan_popup_modals::open_error(
                            make_str("Open pin [%s].", pin.label.c_str()).c_str(),
                            make_str("Pin path [%s] does not exit.", pin.path.data()).c_str());
                    }
                    else {
                        bool pin_is_valid_dir = expl.update_cwd_entries(query_filesystem, pin.path.data());
                        if (pin_is_valid_dir) {
                            expl.cwd = pin.path;
                            expl.push_history_item(pin.path);
                            expl.set_latest_valid_cwd(pin.path); // this may mutate filter
                            (void) expl.update_cwd_entries(filter, pin.path.data());
                            (void) expl.save_to_disk();
                        } else {
                            (void) expl.update_cwd_entries(full_refresh, expl.cwd.data());
                        }
                    }
                }
            }
            if (imgui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                imgui::SetTooltip("%s", pin.path.data());
            }
        }
    }
}

static
void render_create_directory_popup(explorer_window &expl, wchar_t dir_sep_utf16) noexcept
{
    static char dir_name_utf8[MAX_PATH] = {};
    static std::string err_msg = {};

    auto cleanup_and_close_popup = [&]() {
        dir_name_utf8[0] = L'\0';
        err_msg.clear();
        imgui::CloseCurrentPopup();
    };

    auto attempt_create = [&]() {
        if (strempty(dir_name_utf8)) {
            err_msg = "Directory name cannot be blank.";
            return;
        }

        wchar_t cwd_utf16[MAX_PATH];        init_empty_cstr(cwd_utf16);
        wchar_t dir_name_utf16[MAX_PATH];   init_empty_cstr(dir_name_utf16);
        std::wstring create_path = {};
        s32 utf_written = 0;
        BOOL result = {};

        utf_written = utf8_to_utf16(expl.cwd.data(), cwd_utf16, lengthof(cwd_utf16));
        if (utf_written == 0) {
            print_debug_msg("[ %d ] FAILED utf8_to_utf16(expl.cwd)", expl.id);
            cleanup_and_close_popup();
            return;
        }

        utf_written = utf8_to_utf16(dir_name_utf8, dir_name_utf16, lengthof(dir_name_utf16));
        if (utf_written == 0) {
            print_debug_msg("[ %d ] FAILED utf8_to_utf16(dir_name)", expl.id);
            cleanup_and_close_popup();
            return;
        }

        create_path.reserve(1024);

        create_path = cwd_utf16;
        if (!create_path.ends_with(dir_sep_utf16)) {
            create_path += dir_sep_utf16;
        }
        create_path += dir_name_utf16;

        WCOUT_IF_DEBUG("CreateDirectoryW [" << create_path << "]\n");
        result = CreateDirectoryW(create_path.c_str(), nullptr);

        if (result == 0) {
            auto error = GetLastError();
            switch (error) {
                case ERROR_ALREADY_EXISTS: err_msg = "File or directory with same name already exists."; break;
                case ERROR_PATH_NOT_FOUND: err_msg = "One or more intermediate directories do not exist. This is probably a bug. Sorry!"; break;
                default: err_msg = get_last_error_string(); break;
            }
            print_debug_msg("[ %d ] FAILED CreateDirectoryW: %d, %s", expl.id, result, err_msg.c_str());
        } else {
            {
                expl.deselect_all_cwd_entries();
                std::scoped_lock lock(expl.select_cwd_entries_on_next_update_mutex);
                expl.select_cwd_entries_on_next_update.push_back(path_create(dir_name_utf8));
            }
            cleanup_and_close_popup();
        }
    };

    imgui::SetKeyboardFocusHere(0);
    {
        imgui::ScopedAvailWidth w(imgui::CalcTextSize("(?)").x + imgui::GetStyle().ItemSpacing.x);

        if (imgui::InputTextWithHint("##dir_name_input", "Directory name", dir_name_utf8, lengthof(dir_name_utf8),
                                     ImGuiInputTextFlags_CallbackCharFilter, filter_chars_callback, (void *)windows_illegal_filename_chars()))
        {
            err_msg.clear();
        }
    }
    if (imgui::IsItemFocused() && imgui::IsKeyPressed(ImGuiKey_Enter)) {
        attempt_create();
    }

    imgui::SameLine();
    imgui::TextUnformatted("(?)");
    if (imgui::IsItemHovered()) {
        imgui::SetTooltip("Press [Enter] to create directory\n""Press [Escape] to exit");
    }

    if (!err_msg.empty()) {
        imgui::TextColored(red(), "Error: %s", err_msg.c_str());
    }

    if (imgui::IsWindowFocused() && imgui::IsKeyPressed(ImGuiKey_Escape)) {
        cleanup_and_close_popup();
    }
}

static
void render_filter_reset_button(explorer_window &expl) noexcept
{
    if (imgui::Button(ICON_CI_DEBUG_RESTART "##clear_filter")) {
        expl.reset_filter();
        (void) expl.update_cwd_entries(filter, expl.cwd.data());
        (void) expl.save_to_disk();
    }
    if (imgui::IsItemHovered()) {
        imgui::SetTooltip("Reset filter settings");
    }
}

static
void render_drives_table(explorer_window &expl, char dir_sep_utf8, u64 size_unit_multiplier) noexcept
{
    static precise_time_point_t last_refresh_time = {};
    static drive_list_t drives = {};

    // refresh drives occasionally
    {
        precise_time_point_t now = current_time_precise();
        s64 diff_ms = compute_diff_ms(last_refresh_time, now);
        if (diff_ms >= 1000) {
            drives = query_drive_list();
            last_refresh_time = current_time_precise();
        }
    }

    enum drive_table_col_id : s32
    {
        drive_table_col_id_letter,
        drive_table_col_id_name,
        drive_table_col_id_filesystem,
        drive_table_col_id_total_space,
        drive_table_col_id_used_percent,
        drive_table_col_id_free_space,
        drive_table_col_id_count,
    };

    if (imgui::BeginTable("drives", drive_table_col_id_count, ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_BordersInnerV |
                                                              ImGuiTableFlags_Reorderable|ImGuiTableFlags_Resizable|ImGuiTableFlags_BordersV |
                                                              (global_state::settings().cwd_entries_table_borders_in_body ? 0 : ImGuiTableFlags_NoBordersInBody) |
                                                              (global_state::settings().cwd_entries_table_alt_row_bg ? ImGuiTableFlags_RowBg : 0)))
    {
        imgui::TableSetupColumn("Drive", ImGuiTableColumnFlags_NoSort, 0.0f, drive_table_col_id_letter);
        imgui::TableSetupColumn("Name", ImGuiTableColumnFlags_NoSort, 0.0f, drive_table_col_id_name);
        imgui::TableSetupColumn("Filesystem", ImGuiTableColumnFlags_NoSort, 0.0f, drive_table_col_id_filesystem);
        imgui::TableSetupColumn("Total Space", ImGuiTableColumnFlags_NoSort, 0.0f, drive_table_col_id_total_space);
        imgui::TableSetupColumn("Usage", ImGuiTableColumnFlags_NoSort, 0.0f, drive_table_col_id_used_percent);
        imgui::TableSetupColumn("Free Space", ImGuiTableColumnFlags_NoSort, 0.0f, drive_table_col_id_free_space);
        imgui::TableHeadersRow();

        for (auto &drive : drives) {
            imgui::TableNextRow();

            if (imgui::TableSetColumnIndex(drive_table_col_id_letter)) {
                imgui::TextColored(get_color(basic_dirent::kind::directory), "%C:", drive.letter);
            }

            if (imgui::TableSetColumnIndex(drive_table_col_id_name)) {
                bool selected = false;

                if (imgui::Selectable(drive.name_utf8[0] == '\0' ? "Local Disk" : drive.name_utf8,
                                        &selected, ImGuiSelectableFlags_SpanAllColumns))
                {
                    char root[] = { drive.letter, ':', dir_sep_utf8, '\0' };
                    expl.cwd = expl.latest_valid_cwd = path_create(root);
                    expl.set_latest_valid_cwd(expl.cwd);
                    expl.push_history_item(expl.cwd);
                    bool drive_exists = expl.update_cwd_entries(query_filesystem, expl.cwd.data());
                    if (drive_exists) {
                        (void) expl.update_cwd_entries(filter, expl.cwd.data());
                        (void) expl.save_to_disk();
                    } else {
                        // TODO: handle error
                    }
                }
            }

            if (imgui::TableSetColumnIndex(drive_table_col_id_filesystem)) {
                imgui::TextUnformatted(drive.filesystem_name_utf8);
            }

            if (imgui::TableSetColumnIndex(drive_table_col_id_total_space)) {
                char buffer[32]; init_empty_cstr(buffer);
                format_file_size(drive.total_bytes, buffer, lengthof(buffer), size_unit_multiplier);
                imgui::TextUnformatted(buffer);
            }

            if (imgui::TableSetColumnIndex(drive_table_col_id_free_space)) {
                char buffer[32]; init_empty_cstr(buffer);
                format_file_size(drive.available_bytes, buffer, lengthof(buffer), size_unit_multiplier);
                imgui::TextUnformatted(buffer);
            }

            if (imgui::TableSetColumnIndex(drive_table_col_id_used_percent)) {
                u64 used_bytes = drive.total_bytes - drive.available_bytes;
                f32 fraction_used = ( f32(used_bytes) / f32(drive.total_bytes) );
                f32 percent_used = fraction_used * 100.0f;
                imgui::Text("%3.0lf%%", percent_used);
                imgui::SameLine();

                f32 red = fraction_used < 0.5f ? 0 : ( 4 * ((fraction_used - 0.5f) * (fraction_used - 0.5f)) );
                f32 green = 1.5f - fraction_used;
                f32 blue = 0;

                imgui::ScopedColor c(ImGuiCol_PlotHistogram, ImVec4(red, green, blue, 1));
                imgui::ProgressBar(f32(used_bytes) / f32(drive.total_bytes), ImVec2(-1, imgui::CalcTextSize("1").y), "");
            }
        }

        imgui::EndTable();
    }
}

static
void render_filter_text_input(explorer_window &expl) noexcept
{
    auto width = max(
        imgui::CalcTextSize(expl.filter_text.data()).x + (imgui::GetStyle().FramePadding.x * 2) + 10.f,
        imgui::CalcTextSize("1").x * 20.75f
    );

    imgui::ScopedItemWidth iw(width);

    if (imgui::InputTextWithHint("##filter", ICON_CI_FILTER, expl.filter_text.data(), expl.filter_text.size())) {
        (void) expl.update_cwd_entries(filter, expl.cwd.data());
        (void) expl.save_to_disk();
    }
}

static
void render_filter_type_toggler_buttons(explorer_window &expl) noexcept
{
    std::tuple<basic_dirent::kind, bool &, char const *> button_defs[(u64)basic_dirent::kind::count] = {
        { basic_dirent::kind::directory,             expl.filter_show_directories,          "directories"         },
        { basic_dirent::kind::symlink_to_directory,  expl.filter_show_symlink_directories,  "directory shortcuts" },
        { basic_dirent::kind::file,                  expl.filter_show_files,                "files"               },
        { basic_dirent::kind::symlink_to_file,       expl.filter_show_symlink_files,        "file shortcuts"      },
        { basic_dirent::kind::invalid_symlink,       expl.filter_show_invalid_symlinks,     "invalid shortcuts"   },
    };

    for (auto &button_def : button_defs) {
        basic_dirent::kind type = std::get<0>(button_def);
        bool &show              = std::get<1>(button_def);
        char const *type_str    = std::get<2>(button_def);

        imgui::SameLine();

        {
            imgui::ScopedColor ct(ImGuiCol_Text, show ? get_color(type) : ImVec4(0.3f, 0.3f, 0.3f, 1));
            imgui::Button(get_icon(type));
        }

        if (imgui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            imgui::SetTooltip("Toggle mute %s\n\n"
                              "[Shift + click] to unmute all\n"
                              "[Ctrl  + click] to mute others",
                              type_str);
        }

        if (imgui::IsItemClicked()) {
            auto &io = imgui::GetIO();
            if (io.KeyCtrl) { // disable all but self
                for (auto &button : button_defs) {
                    std::get<1>(button) = false;
                }
                show = true;
            }
            else if (io.KeyShift) { // enable all
                for (auto &button : button_defs) {
                    std::get<1>(button) = true;
                }
            }
            else {
                flip_bool(show);
            }
            (void) expl.update_cwd_entries(filter, expl.cwd.data());
            (void) expl.save_to_disk();
        }
    }
}

static
void render_filter_mode_toggle(explorer_window &expl) noexcept
{
    static char const *filter_modes[] = {
        ICON_CI_WHOLE_WORD,
        ICON_CI_REGEX,
        // "(" ICON_CI_REGEX ")",
    };

    static_assert(lengthof(filter_modes) == (u64)explorer_window::filter_mode::count);

    static char const *current_mode = nullptr;
    current_mode = filter_modes[expl.filter_mode];

    char buffer[64]; init_empty_cstr(buffer);
    snprintf(buffer, lengthof(buffer), "%s##%zu", current_mode, expl.filter_mode);

    if (imgui::Button(buffer)) {
        inc_or_wrap<u64>((u64 &)expl.filter_mode, 0, u64(explorer_window::filter_mode::count) - 1);
        (void) expl.update_cwd_entries(filter, expl.cwd.data());
        (void) expl.save_to_disk();
    }

    if (imgui::IsItemHovered()) {
        imgui::SetTooltip(
            "Filter mode:\n"
            "%s Contains substring \n"
            "%s RegExp match       \n",
            // "%s RegExp find          ",
            expl.filter_mode == explorer_window::filter_mode::contains    ? ">>" : "  ",
            expl.filter_mode == explorer_window::filter_mode::regex_match ? ">>" : "  "//,
            // expl.filter_mode == explorer_window::filter_mode::regex_find  ? ">>" : "  "
        );
    }
}

static
void render_filter_case_sensitivity_button(explorer_window &expl) noexcept
{
    {
        imgui::ScopedStyle<f32> s(imgui::GetStyle().Alpha, expl.filter_case_sensitive ? 1 : 0.4f);

        if (imgui::Button(ICON_CI_CASE_SENSITIVE)) {
            flip_bool(expl.filter_case_sensitive);
            (void) expl.update_cwd_entries(filter, expl.cwd.data());
            (void) expl.save_to_disk();
        }
    }
    if (imgui::IsItemHovered()) {
        imgui::SetTooltip(
            "Filter case sensitivity:\n"
            "%s insensitive          \n"
            "%s sensitive              ",
            !expl.filter_case_sensitive ? ">>" : "  ",
             expl.filter_case_sensitive ? ">>" : "  "
        );
    }
}

static
void render_filter_polarity_button(explorer_window &expl) noexcept
{
    if (imgui::Button(expl.filter_polarity ? (ICON_CI_EYE "##filter_polarity") : (ICON_CI_EYE_CLOSED "##filter_polarity"))) {
        flip_bool(expl.filter_polarity);
        (void) expl.update_cwd_entries(filter, expl.cwd.data());
        (void) expl.save_to_disk();
    }
    if (imgui::IsItemHovered()) {
        imgui::SetTooltip(
            "Filter polarity:\n"
            "%s positive     \n"
            "%s negative       ",
             expl.filter_polarity ? ">>" : "  ",
            !expl.filter_polarity ? ">>" : "  "
        );
    }
}

// static
// void render_blank_button() noexcept
// {
//     imgui::ScopedDisable d(true);
//     imgui::ScopedColor c(ImGuiCol_Button, imgui::GetStyle().Colors[ImGuiCol_WindowBg]);
//     imgui::Button(ICON_CI_BLANK);
// }

static
void render_create_file_popup(explorer_window &expl, wchar_t dir_sep_utf16) noexcept
{
    static char file_name_utf8[MAX_PATH] = {};
    static std::string err_msg = {};

    auto cleanup_and_close_popup = [&]() {
        file_name_utf8[0] = L'\0';
        err_msg.clear();
        imgui::CloseCurrentPopup();
    };

    auto attempt_create = [&]() {
        if (strempty(file_name_utf8)) {
            err_msg = "File name cannot be blank.";
            return;
        }

        wchar_t cwd_utf16[MAX_PATH];        init_empty_cstr(cwd_utf16);
        wchar_t file_name_utf16[MAX_PATH];  init_empty_cstr(file_name_utf16);
        std::wstring create_path_utf16 = {};
        s32 utf_written = 0;
        HANDLE result = {};

        utf_written = utf8_to_utf16(expl.cwd.data(), cwd_utf16, lengthof(cwd_utf16));
        if (utf_written == 0) {
            print_debug_msg("[ %d ] FAILED utf8_to_utf16(expl.cwd)", expl.id);
            cleanup_and_close_popup();
            return;
        }

        utf_written = utf8_to_utf16(file_name_utf8, file_name_utf16, lengthof(file_name_utf16));
        if (utf_written == 0) {
            print_debug_msg("[ %d ] FAILED utf8_to_utf16(file_name)", expl.id);
            cleanup_and_close_popup();
            return;
        }

        create_path_utf16.reserve(1024);

        create_path_utf16 = cwd_utf16;
        if (!create_path_utf16.ends_with(dir_sep_utf16)) {
            create_path_utf16 += dir_sep_utf16;
        }
        create_path_utf16 += file_name_utf16;

        WCOUT_IF_DEBUG("CreateFileW [" << create_path_utf16 << "]\n");
        result = CreateFileW(
            create_path_utf16.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (result == INVALID_HANDLE_VALUE) {
            auto error = GetLastError();
            switch (error) {
                case ERROR_ALREADY_EXISTS: err_msg = "File or directory with same name already exists."; break;
                case ERROR_PATH_NOT_FOUND: err_msg = "One or more intermediate directories do not exist; probably a bug. Sorry!"; break;
                default: err_msg = get_last_error_string(); break;
            }
            print_debug_msg("[ %d ] FAILED CreateFileW: %d, %s", expl.id, result, err_msg.c_str());
        } else {
            swan_path_t create_path_utf8;
            s32 written = utf16_to_utf8(create_path_utf16.c_str(), create_path_utf8.data(), create_path_utf8.max_size());

            if (written != 0) {
                global_state::add_recent_file("Created", create_path_utf8.data());
                (void) global_state::save_recent_files_to_disk();
            } else {
                print_debug_msg("[ %d ] FAILED utf16_to_utf8(create_path)", expl.id);
            }

            {
                expl.deselect_all_cwd_entries();
                std::scoped_lock lock(expl.select_cwd_entries_on_next_update_mutex);
                expl.select_cwd_entries_on_next_update.push_back(path_create(file_name_utf8));
            }

            cleanup_and_close_popup();
        }
    };

    imgui::SetKeyboardFocusHere(0);
    {
        imgui::ScopedAvailWidth w(imgui::CalcTextSize("(?)").x + imgui::GetStyle().ItemSpacing.x);

        if (imgui::InputTextWithHint("##file_name_input", "File name", file_name_utf8, lengthof(file_name_utf8),
                                    ImGuiInputTextFlags_CallbackCharFilter, filter_chars_callback, (void *)windows_illegal_filename_chars()))
        {
            err_msg.clear();
        }
    }
    if (imgui::IsItemFocused() && imgui::IsKeyPressed(ImGuiKey_Enter)) {
        attempt_create();
    }

    imgui::SameLine();
    imgui::TextUnformatted("(?)");
    if (imgui::IsItemHovered()) {
        imgui::SetTooltip("Press [Enter] to create file\n""Press [Escape] to exit");
    }

    if (!err_msg.empty()) {
        imgui::TextColored(red(), "Error: %s", err_msg.c_str());
    }

    if (imgui::IsWindowFocused() && imgui::IsKeyPressed(ImGuiKey_Escape)) {
        cleanup_and_close_popup();
    }
}

static
void render_button_pin_cwd(explorer_window &expl, bool cwd_exists_before_edit) noexcept
{
    u64 pin_idx;
    {
        scoped_timer<timer_unit::MICROSECONDS> check_if_pinned_timer(&expl.check_if_pinned_us);
        pin_idx = global_state::find_pin_idx(expl.cwd);
    }
    bool already_pinned = pin_idx != std::string::npos;

    char buffer[4] = {};
    {
#if 0
        s32 written = snprintf(buffer, lengthof(buffer), "[%c]", (already_pinned ? '*' : ' '));
#else
        [[maybe_unused]] s32 written = snprintf(buffer, lengthof(buffer), "%s", already_pinned ? ICON_CI_STAR_FULL : ICON_CI_STAR_EMPTY);
#endif
        assert(written < lengthof(buffer));
    }

    imgui::ScopedDisable disabled(!cwd_exists_before_edit && !already_pinned);

    if (imgui::Button(buffer)) {
        if (already_pinned) {
            print_debug_msg("[ %d ] pin_idx = %zu", expl.id, pin_idx);
            scoped_timer<timer_unit::MICROSECONDS> unpin_timer(&expl.unpin_us);
            global_state::remove_pin(pin_idx);
        }
        else {
            swan_popup_modals::open_new_pin(expl.cwd, false);
        }
        bool result = global_state::save_pins_to_disk();
        print_debug_msg("save_pins_to_disk: %d", result);
    }
    if (imgui::IsItemHovered()) {
        imgui::SetTooltip("%s current working directory", already_pinned ? "Unpin" : "Pin");
    }
}

static
void render_up_to_cwd_parent_button(explorer_window &expl, bool cwd_exists_before_edit) noexcept
{
    imgui::ScopedDisable disabled(!cwd_exists_before_edit);

    if (imgui::Button(ICON_CI_ARROW_UP "##up")) {
    // if (imgui::ArrowButton("..##up", ImGuiDir_Up)) {
        print_debug_msg("[ %d ] (..) button triggered", expl.id);

        auto result = try_ascend_directory(expl);

        if (!result.success) {
            auto cwd_len = path_length(expl.cwd);

            if ( (cwd_len == 2 && IsCharAlphaA(expl.cwd[0]) && expl.cwd[1] == ':') ||
                 (cwd_len == 3 && IsCharAlphaA(expl.cwd[0]) && expl.cwd[1] == ':' && strchr("\\/", expl.cwd[2])) )
            {
                path_clear(expl.cwd);
            }
            else {
                char action[2048]; init_empty_cstr(action);
                [[maybe_unused]] s32 written = snprintf(action, lengthof(action), "ascend to directory [%s]", result.parent_dir.data());
                assert(written < lengthof(action));

                swan_popup_modals::open_error(action, "could not find directory");
            }
        }
    }
}

enum cwd_mode : s32
{
    cwd_mode_text_input = 0,
    cwd_mode_clicknav,
    cwd_mode_count
};

static
void render_cwd_clicknav(explorer_window &expl, bool cwd_exists, char dir_sep_utf8) noexcept
{
    if (!cwd_exists || path_is_empty(expl.cwd)) {
        return;
    }

    static std::vector<char const *> slices = {};
    slices.reserve(50);
    slices.clear();

    swan_path_t sliced_path = expl.cwd;
    char const *slice = strtok(sliced_path.data(), "\\/");
    while (slice != nullptr) {
        slices.push_back(slice);
        slice = strtok(nullptr, "\\/");
    }

    auto cd_to_slice = [&](char const *slice) {
        char const *slice_end = slice;
        while (*slice_end != '\0') {
            ++slice_end;
        }

        u64 len = slice_end - sliced_path.data();

        char backup_ch = '\0';

        if (len == path_length(expl.cwd)) {
            print_debug_msg("[ %d ] cd_to_slice: slice == cwd, not updating cwd|history", expl.id);
        }
        else {
            backup_ch = expl.cwd[len];
            expl.cwd[len] = '\0';
            expl.push_history_item(expl.cwd);
        }

        bool exists = expl.update_cwd_entries(query_filesystem, expl.cwd.data());
        if (exists) {
            expl.set_latest_valid_cwd(expl.cwd); // this may mutate filter
            (void) expl.update_cwd_entries(filter, expl.cwd.data());
            (void) expl.save_to_disk();
        }
        else {
            // restore state
            (void) expl.update_cwd_entries(full_refresh, expl.cwd.data());
            if (backup_ch) {
                expl.cwd[len] = backup_ch;
            }
        }

        imgui::CloseCurrentPopup();
    };

    imgui::ScopedStyle<f32> s(imgui::GetStyle().ItemSpacing.x, 2);

    {
        u64 i = 0;
        for (auto slice_it = slices.begin(); slice_it != slices.end() - 1; ++slice_it, ++i) {
            char buffer[1024]; init_empty_cstr(buffer);
            snprintf(buffer, lengthof(buffer), "%s##slice%zu", *slice_it, i);

            if (imgui::Button(buffer)) {
                print_debug_msg("[ %d ] clicked slice [%s]", expl.id, *slice_it);
                cd_to_slice(*slice_it);
            }

            imgui::SameLine();
            imgui::Text("%c", dir_sep_utf8);
            imgui::SameLine();
        }
    }

    imgui::Text("%s", slices.back());
}

static
void render_cwd_text_input(explorer_window &expl, bool &cwd_exists_after_edit, char dir_sep_utf8, wchar_t dir_sep_utf16) noexcept
{
    f32 button_space = imgui::CalcTextSize(ICON_CI_BLANK).x + imgui::GetStyle().ItemSpacing.x + (imgui::GetStyle().FramePadding.x * 2);
    imgui::ScopedAvailWidth w(button_space);

    static swan_path_t cwd_input = {};
    cwd_input = expl.cwd;

    cwd_text_input_callback_user_data user_data = { expl.id, dir_sep_utf16, false, cwd_input.data() };

    imgui::InputText("##cwd", cwd_input.data(), cwd_input.size(),
        ImGuiInputTextFlags_CallbackCharFilter|ImGuiInputTextFlags_CallbackEdit,
        cwd_text_input_callback, (void *)&user_data);

    if (user_data.edit_occurred) {
        if (!path_loosely_same(expl.cwd, cwd_input)) {
            expl.cwd = path_squish_adjacent_separators(cwd_input);
            path_force_separator(expl.cwd, dir_sep_utf8);

            cwd_exists_after_edit = expl.update_cwd_entries(query_filesystem, expl.cwd.data());

            if (cwd_exists_after_edit && !path_is_empty(expl.cwd)) {
                if (path_is_empty(expl.latest_valid_cwd) || !path_loosely_same(expl.cwd, expl.latest_valid_cwd)) {
                    expl.push_history_item(expl.cwd);
                }
                expl.set_latest_valid_cwd(expl.cwd); // this may mutate filter
                (void) expl.update_cwd_entries(filter, expl.cwd.data());
            }
        }
        (void) expl.save_to_disk();
    }
}

void swan_windows::render_explorer(explorer_window &expl, bool &open) noexcept
{
    {
        bool is_explorer_visible = imgui::Begin(expl.name, &open);
        if (!is_explorer_visible) {
            imgui::End();
            return;
        }
    }

    char child_name[64];
    (void) snprintf(child_name, lengthof(child_name), "%s##main_child", expl.name);

    if (imgui::BeginChild(child_name)) {

    if (imgui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
        assert(expl.id >= 0);
        global_state::save_focused_window(swan_windows::explorer_0 + expl.id);
    }

    auto &io = imgui::GetIO();
    auto &style = imgui::GetStyle();
    bool window_focused = imgui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    bool cwd_exists_before_edit = directory_exists(expl.cwd.data());
    bool cwd_exists_after_edit = cwd_exists_before_edit;

    char dir_sep_utf8 = global_state::settings().dir_separator_utf8;
    wchar_t dir_sep_utf16 = global_state::settings().dir_separator_utf16;
    u64 size_unit_multiplier = global_state::settings().size_unit_multiplier;

    bool open_single_rename_popup = false;
    bool open_bulk_rename_popup = false;

    bool any_popups_open = swan_popup_modals::is_open_bulk_rename() || swan_popup_modals::is_open_error() ||
                           swan_popup_modals::is_open_single_rename() || swan_popup_modals::is_open_edit_pin() ||
                           swan_popup_modals::is_open_new_pin();

    static std::string error_popup_action = {};
    static std::string error_popup_failure = {};

    static explorer_window::dirent const *dirent_to_be_renamed = nullptr;

    // handle [F2], [Ctrl-X], [Ctrl-C], [Ctrl-V]
    if (window_focused && !any_popups_open) {
        if (imgui::IsKeyPressed(ImGuiKey_F2)) {
            u64 num_entries_selected = std::count_if(expl.cwd_entries.begin(), expl.cwd_entries.end(),
                                                     [](explorer_window::dirent const &e) { return e.is_selected; });

            if (num_entries_selected == 0) {
                swan_popup_modals::open_error("Keybind for rename [F2] was pressed.", "Nothing is selected.");
            }
            else if (num_entries_selected == 1) {
                auto selected_dirent = std::find_if(expl.cwd_entries.begin(), expl.cwd_entries.end(),
                                                    [](explorer_window::dirent const &e) { return e.is_selected; });
                open_single_rename_popup = true;
                dirent_to_be_renamed = &(*selected_dirent);
            }
            else {
                open_bulk_rename_popup = true;
            }
        }

        if (io.KeyCtrl) {
            auto handle_failure = [](char const *operation, generic_result const &result) {
                if (!result.success) {
                    u64 num_failed = std::count(result.error_or_utf8_path.begin(), result.error_or_utf8_path.end(), '\n');
                    swan_popup_modals::open_error(
                        make_str("%s %zu items.", operation, num_failed).c_str(),
                        result.error_or_utf8_path.c_str());
                }
            };


            if (imgui::IsKeyPressed(ImGuiKey_X)) {
                s_file_op_payload.clear();
                auto result = add_selected_entries_to_file_op_payload(expl, "Cut", 'X');
                handle_failure("cut", result);
            }
            if (imgui::IsKeyPressed(ImGuiKey_C)) {
                s_file_op_payload.clear();
                auto result = add_selected_entries_to_file_op_payload(expl, "Copy", 'C');
                handle_failure("copy", result);
            }
            if (imgui::IsKeyPressed(ImGuiKey_V) && !s_file_op_payload.items.empty()) {
                s_file_op_payload.execute(expl);
            }
        }
    }

    // refresh logic start
    {
        auto refresh = [&](update_cwd_entries_actions actions, std::source_location sloc = std::source_location::current()) {
            cwd_exists_before_edit = expl.update_cwd_entries(actions, expl.cwd.data(), sloc);
            cwd_exists_after_edit = cwd_exists_before_edit;
        };

        if (expl.update_request_from_outside != nil) {
            refresh(expl.update_request_from_outside);
            expl.update_request_from_outside = nil;
        }
        else if (window_focused && io.KeyCtrl && imgui::IsKeyPressed(ImGuiKey_R)) {
            print_debug_msg("[ %d ] Ctrl-R, refresh triggered", expl.id);
            refresh(full_refresh);
        }
        else if (global_state::settings().expl_refresh_mode != swan_settings::explorer_refresh_mode_manual && cwd_exists_before_edit) {
            auto issue_read_dir_changes = [&]() {
                wchar_t cwd_utf16[MAX_PATH];
                s32 written = utf8_to_utf16(expl.cwd.data(), cwd_utf16, lengthof(cwd_utf16));
                if (written == 0) {
                    print_debug_msg("[ %d ] FAILED utf8_to_utf16(expl.cwd)", expl.id);
                }

                expl.read_dir_changes_handle = CreateFileW(
                    cwd_utf16,
                    FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL,
                    OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                    NULL);

                if (expl.read_dir_changes_handle == INVALID_HANDLE_VALUE) {
                    print_debug_msg("[ %d ] CreateFileW FAILED: INVALID_HANDLE_VALUE", expl.id);
                } else {
                    auto success = ReadDirectoryChangesW(
                        expl.read_dir_changes_handle,
                        reinterpret_cast<void *>(expl.read_dir_changes_buffer.data()),
                        (s32)expl.read_dir_changes_buffer.size(),
                        FALSE, // watch subtree
                        FILE_NOTIFY_CHANGE_CREATION|FILE_NOTIFY_CHANGE_DIR_NAME|FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_LAST_WRITE|FILE_NOTIFY_CHANGE_SIZE,
                        &expl.read_dir_changes_buffer_bytes_written,
                        &expl.read_dir_changes_overlapped,
                        nullptr);

                    expl.read_dir_changes_target = expl.cwd;

                    if (success) {
                        print_debug_msg("[ %d ] ReadDirectoryChangesW(%s) succeeded", expl.id, expl.cwd.data());
                    } else {
                        print_debug_msg("[ %d ] ReadDirectoryChangesW FAILED: %s", expl.id, get_last_error_string().c_str());
                    }
                }
            };

            if (expl.read_dir_changes_refresh_request_time != precise_time_point_t() &&
                compute_diff_ms(expl.last_filesystem_query_time, current_time_precise()) >= 250)
            {
                refresh(full_refresh);
                expl.read_dir_changes_refresh_request_time = precise_time_point_t();
            }
            else if (expl.read_dir_changes_handle != INVALID_HANDLE_VALUE && !path_loosely_same(expl.cwd, expl.read_dir_changes_target)) {
                // cwd changed while waiting for signal from ReadDirectoryChangesW,
                // therefore need to reissue ReadDirectoryChangesW (will be done on next frame)

                //? which one?
                CancelIo(expl.read_dir_changes_handle);
                CloseHandle(expl.read_dir_changes_handle);
                expl.read_dir_changes_handle = INVALID_HANDLE_VALUE;
            }
            else if (expl.read_dir_changes_handle != INVALID_HANDLE_VALUE) {
                BOOL overlap_check = GetOverlappedResult(expl.read_dir_changes_handle,
                                                         &expl.read_dir_changes_overlapped,
                                                         &expl.read_dir_changes_buffer_bytes_written,
                                                         FALSE);

                if (!overlap_check && GetLastError() == ERROR_IO_INCOMPLETE) {
                    // ReadDirectoryChangesW in flight but not yet signalled, thus no changes and no refresh needed
                    // print_debug_msg("[ %d ] GetOverlappedResult FAILED: %d %s", expl.id, GetLastError(), get_last_error_string().c_str());
                } else {
                    if (global_state::settings().expl_refresh_mode == swan_settings::explorer_refresh_mode_automatic) {
                        print_debug_msg("[ %d ] ReadDirectoryChangesW signalled a change && refresh mode == automatic, refreshing...");
                        issue_read_dir_changes();
                        if (expl.read_dir_changes_refresh_request_time == precise_time_point_t()) {
                            // no refresh pending, submit request to refresh
                            expl.read_dir_changes_refresh_request_time = current_time_precise();
                        }
                    } else { // explorer_options::refresh_mode::notify
                        print_debug_msg("[ %d ] ReadDirectoryChangesW signalled a change && refresh mode == notify, notifying...");
                        expl.refresh_message = ICON_FA_EXCLAMATION_TRIANGLE " Outdated" ;
                        issue_read_dir_changes();
                    }
                }
            }
            else { // expl.read_dir_changes_handle == INVALID_HANDLE_VALUE
                issue_read_dir_changes();
            }
        }
    }
    // refresh logic end

    if (global_state::settings().show_debug_info) {
        char buffer[64];
        (void) snprintf(buffer, lengthof(buffer), " Explorer %d Debug Info ", expl.id + 1);
        if (imgui::Begin(buffer, nullptr)) {
            render_debug_info(expl, size_unit_multiplier);
        }
        imgui::End();
    }

    if (imgui::BeginTable("explorer_header", 2, ImGuiTableFlags_SizingStretchProp, { imgui::GetWindowContentRegionWidth(), 0 }, 50)) {
        imgui::TableNextColumn();
        {
            render_back_to_prev_valid_cwd_button(expl);
            if (imgui::IsItemHovered()) {
                imgui::SetTooltip("Back");
            }

            imgui::SameLine();

            render_forward_to_next_valid_cwd_button(expl);
            if (imgui::IsItemHovered()) {
                imgui::SetTooltip("Forward");
            }

            imgui::SameLine();

            if (imgui::Button(ICON_CI_HISTORY "##expl.wd_history")) {
                imgui::OpenPopup("history_popup");
            }
            if (imgui::IsItemHovered()) {
                imgui::SetTooltip("Open history");
            }
            if (imgui::BeginPopup("history_popup")) {
                swan_path_t backup = expl.cwd;
                bool history_item_clicked = render_history_browser_popup(expl, cwd_exists_before_edit);

                if (history_item_clicked) {
                    bool history_item_exists = expl.update_cwd_entries(query_filesystem, expl.cwd.data());
                    if (history_item_exists) {
                        expl.set_latest_valid_cwd(expl.cwd); // this may mutate filter
                        (void) expl.update_cwd_entries(filter, expl.cwd.data());
                        (void) expl.save_to_disk();
                    } else {
                        swan_popup_modals::open_error(make_str("Navigate to history item [%s]", expl.cwd.data()).c_str(), "Path not found, maybe it was renamed or deleted?");
                        expl.cwd = backup;
                        (void) expl.update_cwd_entries(full_refresh, expl.cwd.data());
                    }
                }
            }

            imgui::SameLine();

            render_up_to_cwd_parent_button(expl, cwd_exists_before_edit);
            if (imgui::IsItemHovered()) {
                imgui::SetTooltip("Up to parent directory");
            }

            imgui::SameLineSpaced(1);
            imgui::Spacing(1);

            if (imgui::Button(ICON_CI_BOOKMARK "##pins")) {
                imgui::OpenPopup("pins_popup");
            }
            if (imgui::IsItemHovered()) {
                imgui::SetTooltip("Open pins");
            }
            if (imgui::BeginPopup("pins_popup")) {
                render_pins_popup(expl);
                imgui::EndPopup();
            }

            imgui::SameLine();

            if (imgui::Button(ICON_CI_SYMBOL_NULL "##invert_selection")) {
                expl.invert_selected_visible_cwd_entries();
            }
            if (imgui::IsItemHovered()) {
                imgui::SetTooltip("Invert selection");
            }

            imgui::SameLine();

            if (imgui::Button(ICON_CI_NEW_FOLDER "##+dir")) {
                imgui::OpenPopup("Create directory ##from_header");
            }
            if (imgui::IsItemHovered()) {
                imgui::SetTooltip("Create directory");
            }
            if (imgui::BeginPopupModal("Create directory ##from_header", nullptr, ImGuiWindowFlags_NoResize|ImGuiWindowFlags_AlwaysAutoResize)) {
                render_create_directory_popup(expl, dir_sep_utf16);
                imgui::EndPopup();
            }

            imgui::SameLine();

            if (imgui::Button(ICON_CI_NEW_FILE "##+file")) {
                imgui::OpenPopup("Create file ##from_header");
            }
            if (imgui::IsItemHovered()) {
                imgui::SetTooltip("Create file");
            }
            if (imgui::BeginPopupModal("Create file ##from_header", nullptr, ImGuiWindowFlags_NoResize|ImGuiWindowFlags_AlwaysAutoResize)) {
                render_create_file_popup(expl, dir_sep_utf16);
                imgui::EndPopup();
            }
        }

        imgui::TableNextColumn();
        {
            {
                imgui::ScopedDisable d(!cwd_exists_before_edit);
                if (imgui::Button(ICON_CI_DEBUG_LINE_BY_LINE "##clicknav")) {
                    imgui::OpenPopup("Click Navigation");
                }
            }
            if (cwd_exists_before_edit && imgui::IsItemHovered()) {
                imgui::SetTooltip("Navigate to segment");
            }
            if (imgui::BeginPopup("Click Navigation")) {
                render_cwd_clicknav(expl, cwd_exists_before_edit, dir_sep_utf8);
                imgui::EndPopup();
            }

            imgui::SameLine();

            render_cwd_text_input(expl, cwd_exists_after_edit, dir_sep_utf8, dir_sep_utf16);

            imgui::SameLine();

            render_button_pin_cwd(expl, cwd_exists_before_edit);

            // new line

            render_filter_reset_button(expl);

            imgui::SameLine();

            render_filter_text_input(expl);
            if (io.KeyCtrl && imgui::IsKeyPressed(ImGuiKey_F)) {
                imgui::ActivateItemByID(imgui::GetID("##filter"));
            }

            {
                imgui::ScopedStyle<f32> s(imgui::GetStyle().Alpha, 0.5);
                imgui::SameLineSpaced(0);
                imgui::TextUnformatted(ICON_CI_KEBAB_VERTICAL);
                imgui::SameLineSpaced(0);
            }

            render_filter_polarity_button(expl);
            imgui::SameLine();
            render_filter_case_sensitivity_button(expl);
            imgui::SameLine();
            render_filter_mode_toggle(expl);

            {
                imgui::ScopedStyle<f32> s(imgui::GetStyle().Alpha, 0.5);
                imgui::SameLineSpaced(0);
                imgui::TextUnformatted(ICON_CI_KEBAB_VERTICAL);
                imgui::SameLineSpaced(0);
            }

            render_filter_type_toggler_buttons(expl);
        }

        imgui::EndTable();
    }

    if (expl.filter_error != "") {
        imgui::PushTextWrapPos(imgui::GetColumnWidth());
        imgui::TextColored(red(), "%s", expl.filter_error.c_str());
        imgui::PopTextWrapPos();
        imgui::Spacing(1);
    }

    cwd_count_info cnt = {};
    ImVec2 cwd_entries_table_size = {};
    ImVec2 cwd_entries_table_min = {};
    ImVec2 cwd_entries_table_max = {};
    bool drives_table_rendered = false;

    if (path_is_empty(expl.cwd)) {
        render_drives_table(expl, dir_sep_utf8, size_unit_multiplier);
        drives_table_rendered = true;
    }
    else {
        // compute cwd_count_info
        {
            for (auto &dirent : expl.cwd_entries) {
                static_assert(u64(false) == 0);
                static_assert(u64(true)  == 1);

                [[maybe_unused]] char const *path = dirent.basic.path.data();

                bool is_path_dotdot = dirent.basic.is_path_dotdot();

                cnt.filtered_directories += u64(dirent.is_filtered_out && dirent.basic.is_directory());
                cnt.filtered_symlinks    += u64(dirent.is_filtered_out && dirent.basic.is_symlink());
                cnt.filtered_files       += u64(dirent.is_filtered_out && dirent.basic.is_file());

                cnt.child_dirents     += 1; // u64(!is_path_dotdot);
                cnt.child_directories += u64(dirent.basic.is_directory());
                cnt.child_symlinks    += u64(dirent.basic.is_symlink());
                cnt.child_files       += u64(dirent.basic.is_file());

                if (!dirent.is_filtered_out && dirent.is_selected) {
                    cnt.selected_directories += u64(dirent.is_selected && dirent.basic.is_directory() && !is_path_dotdot);
                    cnt.selected_symlinks    += u64(dirent.is_selected && dirent.basic.is_symlink());
                    cnt.selected_files       += u64(dirent.is_selected && dirent.basic.is_file());
                }
            }

            cnt.filtered_dirents = cnt.filtered_directories + cnt.filtered_symlinks + cnt.filtered_files;
            cnt.selected_dirents = cnt.selected_directories + cnt.selected_symlinks + cnt.selected_files;
        }

        // distance from top of cwd_entries table to top border of dirent we are trying to scroll to
        std::optional<f32> scrolled_to_dirent_offset_y = std::nullopt;

        if (expl.scroll_to_nth_selected_entry_next_frame != u64(-1)) {
            u64 target = expl.scroll_to_nth_selected_entry_next_frame;
            u64 counter = 0;

            auto scrolled_to_dirent = std::find_if(expl.cwd_entries.begin(), expl.cwd_entries.end(), [&counter, target](explorer_window::dirent &e) {
                // stop spotlighting the previous dirents, looks better when rapidly advancing the spotlighted dirent.
                // without it multiple dirents can be spotlighted at the same time which is visually distracting and possible confusing.
                e.spotlight_frames_remaining = 0;

                return e.is_selected && (counter++) == target;
            });

            expl.reset_filter();

            if (scrolled_to_dirent != expl.cwd_entries.end()) {
                scrolled_to_dirent->spotlight_frames_remaining = u32(imgui::GetIO().Framerate) / 3;
                scrolled_to_dirent_offset_y = ImGui::GetTextLineHeightWithSpacing() * f32(std::distance(expl.cwd_entries.begin(), scrolled_to_dirent));

                // stop spotlighting any dirents ahead which could linger if we just wrapped the spotlight back to the top,
                // looks better this way when rapidly advancing the spotlighted dirent.
                // without it multiple dirents can be spotlighted at the same time which is visually distracting and possible confusing.
                std::for_each(scrolled_to_dirent + 1, expl.cwd_entries.end(), [](explorer_window::dirent &e) { return e.spotlight_frames_remaining = 0; });
            }

            expl.scroll_to_nth_selected_entry_next_frame = u64(-1);
        }

        if (imgui::BeginChild("cwd_entries_child",
            ImVec2(0, imgui::GetContentRegionAvail().y - imgui::CalcTextSize("items").y - imgui::GetStyle().WindowPadding.y - 5)),
            ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoInputs)
        {
            if (scrolled_to_dirent_offset_y.has_value()) {
                f32 current_scroll_offset_y = imgui::GetScrollY();
                f32 window_height = imgui::GetWindowContentRegionMax().y - imgui::GetWindowContentRegionMin().y;
                f32 visible_region_top_y = current_scroll_offset_y;
                f32 visible_region_bottom_y = current_scroll_offset_y + window_height;
                f32 scroll_dirent_top_y = scrolled_to_dirent_offset_y.value() + imgui::TableGetHeaderRowHeight();
                f32 scroll_dirent_bottom_y = scroll_dirent_top_y + ImGui::GetTextLineHeightWithSpacing();

                if (scroll_dirent_top_y >= visible_region_top_y && scroll_dirent_bottom_y <= visible_region_bottom_y) {
                    // already completely in view, don't scroll to avoid ugliness caused by 1 frame delay in fulfillment of imgui::SetScrollY
                } else {
                    f32 half_window_height = window_height / 2.0f;
                    f32 half_dirent_row_height = ImGui::GetTextLineHeightWithSpacing() / 2.0f;
                    imgui::SetScrollY(scrolled_to_dirent_offset_y.value() + imgui::TableGetHeaderRowHeight() - half_window_height + half_dirent_row_height);
                }
            }

            if (expl.cwd_entries.empty()) {
                imgui::Spacing();
                if (!directory_exists(expl.cwd.data())) {
                    imgui::TextColored(orange(), "Directory not found.");
                } else {
                    imgui::TextColored(orange(), "Empty directory.");
                }
            }
            else if (cnt.filtered_dirents == expl.cwd_entries.size()) {
                imgui::Spacing();
                imgui::TextColored(orange(), "All items filtered.");
            }
            else if (cnt.filtered_dirents < expl.cwd_entries.size() && imgui::BeginTable("cwd_entries", explorer_window::cwd_entries_table_col_count,
                ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_Hideable|ImGuiTableFlags_Resizable|
                ImGuiTableFlags_Reorderable|ImGuiTableFlags_Sortable|ImGuiTableFlags_BordersV|
                (global_state::settings().cwd_entries_table_alt_row_bg ? ImGuiTableFlags_RowBg : 0)|
                (global_state::settings().cwd_entries_table_borders_in_body ? 0 : ImGuiTableFlags_NoBordersInBody)
            )) {
                imgui::TableSetupColumn("#", ImGuiTableColumnFlags_NoSort, 0.0f, explorer_window::cwd_entries_table_col_number);
                imgui::TableSetupColumn("ID", ImGuiTableColumnFlags_DefaultSort, 0.0f, explorer_window::cwd_entries_table_col_id);
                imgui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort, 0.0f, explorer_window::cwd_entries_table_col_path);
                imgui::TableSetupColumn("Type", ImGuiTableColumnFlags_DefaultSort, 0.0f, explorer_window::cwd_entries_table_col_type);
                imgui::TableSetupColumn("Size", ImGuiTableColumnFlags_DefaultSort, 0.0f, explorer_window::cwd_entries_table_col_size_pretty);
                imgui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_DefaultSort, 0.0f, explorer_window::cwd_entries_table_col_size_bytes);
                imgui::TableSetupColumn("Created", ImGuiTableColumnFlags_DefaultSort, 0.0f, explorer_window::cwd_entries_table_col_creation_time);
                imgui::TableSetupColumn("Modified", ImGuiTableColumnFlags_DefaultSort, 0.0f, explorer_window::cwd_entries_table_col_last_write_time);
                imgui::TableHeadersRow();

                //? ImGui does not allow you to hold a ImGuiTableSortSpecs pointer over multiple frames,
                //? so we make a copy of it for ourselves because we would like to use it in later frames.
                //? luckily it's a trivial and compact object so there is little implication making a copy each frame.
                ImGuiTableSortSpecs *table_sort_specs = imgui::TableGetSortSpecs();
                if (table_sort_specs != nullptr) {
                    expl.column_sort_specs = expl.copy_column_sort_specs(table_sort_specs);
                }

                std::vector<explorer_window::dirent>::iterator first_filtered_cwd_dirent;

                if (table_sort_specs != nullptr && table_sort_specs->SpecsDirty) {
                    table_sort_specs->SpecsDirty = false;
                    first_filtered_cwd_dirent = sort_cwd_entries(expl);
                } else {
                    f64 find_first_filtered_cwd_dirent_us = 0;
                    SCOPE_EXIT { expl.find_first_filtered_cwd_dirent_timing_samples.push_back(find_first_filtered_cwd_dirent_us); };
                    scoped_timer<timer_unit::MICROSECONDS> timer(&find_first_filtered_cwd_dirent_us);

                    // no point in binary search here, cost of linear traversal is tiny even for huge collection
                    first_filtered_cwd_dirent = std::find_if(expl.cwd_entries.begin(),
                                                             expl.cwd_entries.end(),
                                                             [](explorer_window::dirent const &ent) { return ent.is_filtered_out; });
                }

                // opens "Context" popup if a rendered dirent is right clicked
                render_table_rows_for_cwd_entries(expl, cnt, size_unit_multiplier, window_focused, any_popups_open, dir_sep_utf8, dir_sep_utf16);

                auto result = render_dirent_right_click_context_menu(expl, cnt, global_state::settings());
                open_bulk_rename_popup   = result.open_bulk_rename_popup;
                open_single_rename_popup = result.open_single_rename_popup;
                dirent_to_be_renamed     = result.single_dirent_to_be_renamed;

                imgui::EndTable();
            }
            if (ImGui::IsItemHovered() && io.KeyCtrl && imgui::IsKeyPressed(ImGuiKey_A)) {
                expl.select_all_visible_cwd_entries();
            }
            cwd_entries_table_size = imgui::GetItemRectSize();
            cwd_entries_table_min = imgui::GetItemRectMin();
            cwd_entries_table_max = imgui::GetItemRectMax();
        }

        imgui::ScopedStyle<f32> s2(style.ItemSpacing.y, 0); // to remove spacing between end of table and "leftovers" rectangle

        imgui::EndChild();

        accept_move_dirents_drag_drop(expl);
    }
    ImVec2 cwd_entries_child_size = imgui::GetItemRectSize();
    ImVec2 cwd_entries_child_min = imgui::GetItemRectMin();
    ImVec2 cwd_entries_child_max = imgui::GetItemRectMax();

    ImVec2 leftovers_rect_min(cwd_entries_child_min.x, cwd_entries_table_max.y);
    ImVec2 leftovers_rect_max = cwd_entries_child_max;

    if (window_focused && imgui::IsMouseHoveringRect(cwd_entries_child_min, cwd_entries_child_max) && imgui::IsKeyPressed(ImGuiKey_Escape)) {
        expl.deselect_all_cwd_entries();
    }
    if (window_focused && imgui::IsMouseHoveringRect(leftovers_rect_min, leftovers_rect_max) && imgui::IsMouseClicked(ImGuiMouseButton_Right)) {
        imgui::OpenPopup("##cwd_entries_child_leftovers");
    }
    if (!s_file_op_payload.items.empty() && !path_is_empty(expl.cwd) && imgui::BeginPopup("##cwd_entries_child_leftovers")) {
        if (imgui::Selectable("Paste")) {
            auto result = s_file_op_payload.execute(expl);
            // TODO: why is result unused?
        }

        // if (imgui::Selectable("New file")) {
        //     imgui::OpenPopup("Create file##from_footer");
        // }
        // if (imgui::Selectable("New directory")) {
        //     imgui::OpenPopup("Create directory##from_footer");
        // }

        imgui::EndPopup();
    }

    // TODO: figure out why these don't render
    // if (imgui::BeginPopupModal("Create file##from_footer", nullptr)) {
    //     render_create_file_popup(expl, dir_sep_utf16);
    // }
    // if (imgui::BeginPopupModal("Create directory##from_footer", nullptr)) {
    //     render_create_directory_popup(expl, dir_sep_utf16);
    // }

    if (!drives_table_rendered) {
        render_footer(expl, cnt, style);

        if (!path_is_empty(expl.cwd) && cwd_exists_after_edit) {
            imgui::OpenPopupOnItemClick("##explorer_footer_context", ImGuiPopupFlags_MouseButtonRight);
            if (!s_file_op_payload.items.empty() && imgui::BeginPopup("##explorer_footer_context")) {
                if (imgui::Selectable("Paste")) {
                    auto result = s_file_op_payload.execute(expl);
                }
                imgui::EndPopup();
            }
        }
    }

    if (open_single_rename_popup) {
        swan_popup_modals::open_single_rename(expl, *dirent_to_be_renamed, [&expl]() {
            /* on rename finished: */
            if (global_state::settings().expl_refresh_mode == swan_settings::explorer_refresh_mode_manual) {
                (void) expl.update_cwd_entries(full_refresh, expl.cwd.data());
            }
        });
    }
    if (open_bulk_rename_popup) {
        swan_popup_modals::open_bulk_rename(expl, [&]() {
            /* on rename finished: */
            if (global_state::settings().expl_refresh_mode == swan_settings::explorer_refresh_mode_manual) {
                (void) expl.update_cwd_entries(full_refresh, expl.cwd.data());
            }
        });
    }

    }
    imgui::EndChild();

    if (imgui::BeginDragDropTarget()) {
        auto payload_wrapper = imgui::AcceptDragDropPayload(typeid(pin_drag_drop_payload).name());

        if (payload_wrapper != nullptr) {
            assert(payload_wrapper->DataSize == sizeof(pin_drag_drop_payload));
            auto payload_data = (pin_drag_drop_payload *)payload_wrapper->Data;
            auto const &pin = global_state::pins()[payload_data->pin_idx];

            swan_path_t initial_cwd = expl.cwd;
            expl.cwd = path_create("");

            auto result = try_descend_to_directory(expl, pin.path.data());

            if (!result.success) {
                expl.cwd = initial_cwd;
                (void) expl.update_cwd_entries(full_refresh, expl.cwd.data()); // restore entries cleared by try_descend_to_directory
                swan_popup_modals::open_error(make_str("Open pin [%s] in Explorer %d.", pin.path.data(), expl.id+1).c_str(), result.err_msg.c_str());
            }
        }

        imgui::EndDragDropTarget();
    }

    imgui::End();
}

void file_operation_command_buf::clear() noexcept
{
    s_file_op_payload.items.clear();

    auto &all_explorers = global_state::explorers();
    for (auto &expl : all_explorers) {
        expl.uncut();
    }
}

generic_result file_operation_command_buf::execute(explorer_window &expl) noexcept
{
    wchar_t cwd_utf16[2048]; init_empty_cstr(cwd_utf16);
    s32 written = utf8_to_utf16(expl.cwd.data(), cwd_utf16, lengthof(cwd_utf16));
    if (written == 0) {
        print_debug_msg("FAILED utf8_to_utf16(expl.cwd)");
        return { false, "Conversion of current working directory path from UTF-8 to UTF-16." };
    }

    std::wstring packed_paths_to_exec_utf16 = {};
    std::vector<char> operations_to_exec = {};
    {
        wchar_t item_utf16[MAX_PATH];
        std::stringstream err = {};

        operations_to_exec.reserve(this->items.size());

        for (auto const &item : this->items) {
            init_empty_cstr(item_utf16);
            written = utf8_to_utf16(item.path.data(), item_utf16, lengthof(item_utf16));

            if (written == 0) {
                print_debug_msg("FAILED utf8_to_utf16(item.path)");
                err << "Conversion of [" << item.path.data() << "] from UTF-8 to UTF-16.\n";
            }

            packed_paths_to_exec_utf16.append(item_utf16).append(L"\n");
            operations_to_exec.push_back(item.operation_code);
        }

        // WCOUT_IF_DEBUG("packed_paths_to_exec_utf16:\n" << packed_paths_to_exec_utf16 << '\n');

        std::string errors = err.str();
        if (!errors.empty()) {
            return { false, errors };
        }
    }

    bool initialization_done = false;
    std::string initialization_error = {};

    global_state::thread_pool().push_task(perform_file_operations,
        expl.id,
        cwd_utf16,
        std::move(packed_paths_to_exec_utf16),
        std::move(operations_to_exec),
        &expl.shlwapi_task_initialization_mutex,
        &expl.shlwapi_task_initialization_cond,
        &initialization_done,
        &initialization_error);

    {
        std::unique_lock lock(expl.shlwapi_task_initialization_mutex);
        expl.shlwapi_task_initialization_cond.wait(lock, [&]() { return initialization_done; });
    }

    if (initialization_error.empty()) {
        this->items.clear();
    }

    return { initialization_error.empty(), initialization_error };
}

static
void render_table_rows_for_cwd_entries(
    explorer_window &expl,
    cwd_count_info const &cnt,
    u64 size_unit_multiplier,
    bool window_focused,
    bool any_popups_open,
    char dir_sep_utf8,
    wchar_t dir_sep_utf16) noexcept
{
    auto io = imgui::GetIO();

    ImGuiListClipper clipper;
    {
        u64 num_dirents_to_render = expl.cwd_entries.size() - cnt.filtered_dirents;
        assert(num_dirents_to_render <= (u64)INT32_MAX);
        clipper.Begin(s32(num_dirents_to_render));
    }

    while (clipper.Step()) {
        for (u64 i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            auto &dirent = expl.cwd_entries[i];
            [[maybe_unused]] char *path = dirent.basic.path.data();

            imgui::TableNextRow();

            SCOPE_EXIT { dirent.spotlight_frames_remaining -= 1 * (dirent.spotlight_frames_remaining > 0); };

            if (imgui::TableSetColumnIndex(explorer_window::cwd_entries_table_col_number)) {
                imgui::Text("%zu", i + 1);
            }

            if (imgui::TableSetColumnIndex(explorer_window::cwd_entries_table_col_id)) {
                imgui::Text("%zu", dirent.basic.id);
            }

            if (imgui::TableSetColumnIndex(explorer_window::cwd_entries_table_col_path)) {
                // render colored icon
                {
                    ImVec4 color = dirent.spotlight_frames_remaining > 0 ? red() : get_color(dirent.basic.type);
                    f32 &alpha = color.w;
                    alpha = 1.0f - (f32(dirent.is_cut) * 0.75f);

                    char const *icon = nullptr;
                    if (dirent.basic.is_file()) {
                        file_name_extension_splitter splitter(path);
                        icon = get_icon_for_extension(splitter.ext);
                    } else {
                        icon = dirent.basic.kind_icon();
                    }

                    imgui::TextColored(color, icon);
                }

                imgui::SameLine();

                ImVec2 path_text_rect_min = imgui::GetCursorScreenPos();

                char buffer[2048]; init_empty_cstr(buffer);
                snprintf(buffer, lengthof(buffer), "%s##dirent%zu", path, i);

                {
                    imgui::ScopedTextColor tc(dirent.spotlight_frames_remaining > 0 ? red() : white());

                    if (imgui::Selectable(buffer, dirent.is_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                        // print_debug_msg("[ %d ] selected [%s]", expl.id, dirent.basic.path.data());

                        if (!io.KeyCtrl && !io.KeyShift) {
                            // entry was selected but Ctrl was not held, so deselect everything
                            expl.deselect_all_cwd_entries();
                        }

                        flip_bool(dirent.is_selected);

                        if (io.KeyShift) {
                            // shift click, select everything between the current item and the previously clicked item

                            u64 first_idx, last_idx;

                            if (expl.cwd_latest_selected_dirent_idx == explorer_window::NO_SELECTION) {
                                // nothing in cwd has been selected, so start selection from very first entry
                                expl.cwd_latest_selected_dirent_idx = 0;
                                expl.cwd_latest_selected_dirent_idx_changed = true;
                            }

                            if (i <= expl.cwd_latest_selected_dirent_idx) {
                                // prev selected item below current one
                                first_idx = i;
                                last_idx = expl.cwd_latest_selected_dirent_idx;
                            }
                            else {
                                first_idx = expl.cwd_latest_selected_dirent_idx;
                                last_idx = i;
                            }

                            print_debug_msg("[ %d ] shift click, [%zu, %zu]", expl.id, first_idx, last_idx);

                            for (u64 j = first_idx; j <= last_idx; ++j) {
                                auto &dirent_ = expl.cwd_entries[j];
                                if (!dirent_.basic.is_path_dotdot()) {
                                    dirent_.is_selected = true;
                                }
                            }
                        }

                        static f64 last_click_time = 0;
                        static swan_path_t last_click_path = {};
                        swan_path_t const &current_click_path = dirent.basic.path;
                        f64 const double_click_window_sec = 0.3;
                        f64 current_time_precise = imgui::GetTime();
                        f64 seconds_between_clicks = current_time_precise - last_click_time;

                        if (seconds_between_clicks <= double_click_window_sec && path_equals_exactly(current_click_path, last_click_path)) {
                            if (dirent.basic.is_directory()) {
                                print_debug_msg("[ %d ] double clicked directory [%s]", expl.id, dirent.basic.path.data());

                                if (dirent.basic.is_path_dotdot()) {
                                    auto result = try_ascend_directory(expl);

                                    if (!result.success) {
                                        (void) expl.update_cwd_entries(full_refresh, expl.cwd.data()); // restore entries cleared by try_ascend_directory
                                        swan_popup_modals::open_error(make_str("Ascend to directory [%s].", result.parent_dir.data()).c_str(), "Could not find directory.");
                                    }

                                    return;
                                }
                                else {
                                    char const *target = dirent.basic.path.data();
                                    auto result = try_descend_to_directory(expl, target);

                                    if (!result.success) {
                                        (void) expl.update_cwd_entries(full_refresh, expl.cwd.data()); // restore entries cleared by try_descend_to_directory
                                        swan_popup_modals::open_error(make_str("Descend to directory [%s].", target).c_str(), result.err_msg.c_str());
                                    }

                                    return;
                                }
                            }
                            else if (dirent.basic.is_symlink()) {
                                print_debug_msg("[ %d ] double clicked symlink [%s]", expl.id, dirent.basic.path.data());

                                auto res = open_symlink(dirent, expl);

                                if (res.success) {
                                    if (dirent.basic.is_symlink_to_directory()) {
                                        char const *target_dir_path = res.error_or_utf8_path.c_str();
                                        expl.cwd = path_create(target_dir_path);
                                        expl.set_latest_valid_cwd(expl.cwd); // this may mutate filter
                                        expl.push_history_item(expl.cwd);
                                        (void) expl.update_cwd_entries(full_refresh, expl.cwd.data());
                                        (void) expl.save_to_disk();
                                    }
                                    else if (dirent.basic.is_symlink_to_file()) {
                                        char const *full_file_path = res.error_or_utf8_path.c_str();
                                        u64 recent_file_idx = global_state::find_recent_file_idx(full_file_path);

                                        if (recent_file_idx == u64(-1)) {
                                            global_state::add_recent_file("Opened", full_file_path);
                                        }
                                        else { // already in recent
                                            global_state::move_recent_file_idx_to_front(recent_file_idx, "Opened");
                                        }
                                        (void) global_state::save_recent_files_to_disk();
                                    }
                                } else {
                                    swan_popup_modals::open_error(make_str("Open symlink [%s].", dirent.basic.path.data()).c_str(), res.error_or_utf8_path.c_str());
                                }
                            }
                            else {
                                print_debug_msg("[ %d ] double clicked file [%s]", expl.id, dirent.basic.path.data());

                                auto res = open_file(dirent.basic.path.data(), expl.cwd.data());

                                if (res.success) {
                                    char const *full_file_path = res.error_or_utf8_path.c_str();
                                    u64 recent_file_idx = global_state::find_recent_file_idx(full_file_path);

                                    if (recent_file_idx == u64(-1)) {
                                        global_state::add_recent_file("Opened", full_file_path);
                                    }
                                    else { // already in recent
                                        global_state::move_recent_file_idx_to_front(recent_file_idx, "Opened");
                                    }
                                    (void) global_state::save_recent_files_to_disk();
                                } else {
                                    swan_popup_modals::open_error(make_str("Open file [%s].", dirent.basic.path.data()).c_str(), res.error_or_utf8_path.c_str());
                                }
                            }
                        }
                        else if (dirent.basic.is_path_dotdot()) {
                            print_debug_msg("[ %d ] selected [%s]", expl.id, dirent.basic.path.data());
                        }

                        last_click_time = current_time_precise;
                        last_click_path = current_click_path;
                        expl.cwd_latest_selected_dirent_idx = i;
                        expl.cwd_latest_selected_dirent_idx_changed = true;

                    } // imgui::Selectable
                }

                // draw rectangle to highlight filter match
                if (dirent.highlight_len > 0) {
                    ImVec2 min = path_text_rect_min;
                    ImVec2 max = path_text_rect_min + imgui::CalcTextSize(dirent.basic.path.data());

                    // at this point min & max draw a rectangle around the entire path

                    // move forward min.x to skip characters before highlight begins
                    min.x += imgui::CalcTextSize(dirent.basic.path.data(),
                                                    dirent.basic.path.data() + dirent.highlight_start_idx).x;

                    // move backward max.x to eliminate characters after the highlight ends
                    max.x -= imgui::CalcTextSize(dirent.basic.path.data() + dirent.highlight_start_idx + dirent.highlight_len).x;

                    ImGui::GetWindowDrawList()->AddRectFilled(min, max, IM_COL32(255, 255, 0, 50));
                }

                if (dirent.basic.is_path_dotdot()) {
                    dirent.is_selected = false; // do no allow [..] to be selected
                }

                if (window_focused && !any_popups_open && imgui::IsItemClicked(ImGuiMouseButton_Right) && !dirent.basic.is_path_dotdot()) {
                    print_debug_msg("[ %d ] right clicked [%s]", expl.id, dirent.basic.path.data());
                    imgui::OpenPopup("Context");
                    expl.right_clicked_ent = &dirent;
                }

            } // path column

            if (!dirent.basic.is_path_dotdot() && imgui::BeginDragDropSource()) {
                auto cwd_to_utf16 = [&]() {
                    std::wstring retval;

                    wchar_t cwd_utf16[MAX_PATH];
                    s32 written = utf8_to_utf16(expl.cwd.data(), cwd_utf16, lengthof(cwd_utf16));
                    if (written == 0) {
                        // TODO: handle error
                        retval = {};
                    } else {
                        retval = cwd_utf16;
                    }

                    return retval;
                };

                auto add_payload_item = [&](
                    move_dirents_drag_drop_payload &payload,
                    std::wstring &paths,
                    std::wstring const &cwd_utf16,
                    char const *item_utf8)
                {
                    ++payload.num_items;

                    paths += cwd_utf16;
                    if (paths.back() != dir_sep_utf16) {
                        paths += dir_sep_utf16;
                    }

                    wchar_t item_utf16[MAX_PATH];
                    s32 written = utf8_to_utf16(item_utf8, item_utf16, lengthof(item_utf16));

                    if (written == 0) {
                        // TODO: handle error
                    }
                    else {
                        paths += item_utf16;
                        paths += L'\n';
                    }
                };

                auto compute_and_submit_drag_drop_payload = [&]() {
                    std::wstring cwd_utf16 = cwd_to_utf16();
                    assert(!cwd_utf16.empty());
                    move_dirents_drag_drop_payload payload = {};
                    std::wstring paths = {};

                    payload.src_explorer_id = expl.id;

                    if (dirent.is_selected) {
                        for (auto const &dirent_ : expl.cwd_entries) {
                            if (!dirent_.is_filtered_out && dirent_.is_selected) {
                                add_payload_item(payload, paths, cwd_utf16, dirent_.basic.path.data());
                            }
                        }
                    }
                    else {
                        add_payload_item(payload, paths, cwd_utf16, dirent.basic.path.data());
                        global_state::move_dirents_payload_set() = true;
                    }

                    wchar_t *paths_data = new wchar_t[paths.size() + 1];
                    StrCpyNW(paths_data, paths.c_str(), s32(paths.size() + 1));
                    payload.absolute_paths_delimited_by_newlines = paths_data;

                    imgui::SetDragDropPayload("move_dirents_drag_drop_payload", (void *)&payload, sizeof(payload), ImGuiCond_Once);

                    global_state::move_dirents_payload_set() = true;
                    // WCOUT_IF_DEBUG("payload.src_explorer_id = " << payload.src_explorer_id << '\n');
                    // WCOUT_IF_DEBUG("payload.absolute_paths_delimited_by_newlines:\n" << payload.absolute_paths_delimited_by_newlines << '\n');
                };

                if (!global_state::move_dirents_payload_set()) {
                    compute_and_submit_drag_drop_payload();
                } else {
                    // don't recompute payload until someone accepts or it gets dropped
                }

                auto payload_wrapper = imgui::GetDragDropPayload();
                if (payload_wrapper != nullptr && streq(payload_wrapper->DataType, "move_dirents_drag_drop_payload")) {
                    auto payload_data = reinterpret_cast<move_dirents_drag_drop_payload *>(payload_wrapper->Data);
                    u64 num_items = payload_data->num_items;
                    imgui::Text("Move %zu item%s", num_items, num_items == 1 ? "" : "s");
                }

                imgui::EndDragDropSource();
            }

            if (!dirent.basic.is_file() && imgui::BeginDragDropTarget()) {
                auto payload_wrapper = imgui::GetDragDropPayload();

                if (payload_wrapper != nullptr && streq(payload_wrapper->DataType, "move_dirents_drag_drop_payload")) {
                    payload_wrapper = imgui::AcceptDragDropPayload("move_dirents_drag_drop_payload");
                    if (payload_wrapper != nullptr) {
                        handle_drag_drop_onto_dirent(expl, dirent, payload_wrapper, dir_sep_utf8);
                    }
                }

                imgui::EndDragDropTarget();
            }

            // free memory if the payload was not accepted and the user dropped it
            if (imgui::IsMouseReleased(ImGuiMouseButton_Left) && !imgui::IsDragDropPayloadBeingAccepted()) {
                global_state::move_dirents_payload_set() = false;

                auto payload_wrapper = imgui::GetDragDropPayload();

                if (payload_wrapper != nullptr && streq(payload_wrapper->DataType, "move_dirents_drag_drop_payload")) {
                    payload_wrapper = imgui::AcceptDragDropPayload("move_dirents_drag_drop_payload");
                    if (payload_wrapper != nullptr) {
                        auto payload_data = reinterpret_cast<move_dirents_drag_drop_payload *>(payload_wrapper->Data);
                        assert(payload_data != nullptr);
                        delete[] payload_data->absolute_paths_delimited_by_newlines;
                    }
                }
            }

            if (imgui::TableSetColumnIndex(explorer_window::cwd_entries_table_col_type)) {
                imgui::TextUnformatted(dirent.basic.kind_short_cstr());
            }

            if (imgui::TableSetColumnIndex(explorer_window::cwd_entries_table_col_size_pretty)) {
                if (!dirent.basic.is_directory()) {
                    std::array<char, 32> pretty_size = {};
                    format_file_size(dirent.basic.size, pretty_size.data(), pretty_size.size(), size_unit_multiplier);
                    imgui::TextUnformatted(pretty_size.data());
                }
            }

            if (imgui::TableSetColumnIndex(explorer_window::cwd_entries_table_col_size_bytes)) {
                if (!dirent.basic.is_directory()) {
                    imgui::Text("%zu", dirent.basic.size);
                }
            }

            if (imgui::TableSetColumnIndex(explorer_window::cwd_entries_table_col_creation_time)) {
                auto [result, buffer] = filetime_to_string(&dirent.basic.creation_time_raw);
                imgui::TextUnformatted(buffer.data());
            }

            if (imgui::TableSetColumnIndex(explorer_window::cwd_entries_table_col_last_write_time)) {
                auto [result, buffer] = filetime_to_string(&dirent.basic.last_write_time_raw);
                imgui::TextUnformatted(buffer.data());
            }
        }
    }
}

static
render_dirent_right_click_context_menu_result
render_dirent_right_click_context_menu(explorer_window &expl, cwd_count_info const &cnt, swan_settings const &settings) noexcept
{
    auto io = imgui::GetIO();
    render_dirent_right_click_context_menu_result retval = {};

    if (imgui::BeginPopup("Context")) {
        if (cnt.selected_dirents <= 1) {
            assert(expl.right_clicked_ent != nullptr);

            imgui::TextColored(get_color(expl.right_clicked_ent->basic.type), "%s", expl.right_clicked_ent->basic.path.data());
            imgui::Separator();

            // bool is_directory = expl.right_clicked_ent->basic.is_directory();

            if ((path_ends_with(expl.right_clicked_ent->basic.path, ".exe") || path_ends_with(expl.right_clicked_ent->basic.path, ".bat"))
                && imgui::Selectable("Run as administrator"))
            {
                auto res = open_file(expl.right_clicked_ent->basic.path.data(), expl.cwd.data(), true);

                if (res.success) {
                    char const *full_file_path = res.error_or_utf8_path.c_str();
                    u64 recent_file_idx = global_state::find_recent_file_idx(full_file_path);

                    if (recent_file_idx == u64(-1)) {
                        global_state::add_recent_file("Opened", full_file_path);
                    }
                    else { // already in recent
                        global_state::move_recent_file_idx_to_front(recent_file_idx, "Opened");
                    }
                    (void) global_state::save_recent_files_to_disk();
                } else {
                    swan_popup_modals::open_error(make_str("Open file as administrator [%s].", expl.right_clicked_ent->basic.path.data()).c_str(),
                                                    res.error_or_utf8_path.c_str());
                }
            }

            if (expl.right_clicked_ent->basic.is_symlink_to_file() && imgui::Selectable("Open file location")) {
                symlink_data lnk_data = {};
                auto extract_result = lnk_data.extract(expl.right_clicked_ent->basic.path.data(), expl.cwd.data());

                if (!extract_result.success) {
                    swan_popup_modals::open_error(make_str("Open file location of [%s].", expl.right_clicked_ent->basic.path.data()).c_str(),
                                                    extract_result.error_or_utf8_path.c_str());
                } else {
                    // no error checking because symlink_data::extract will already have validated things

                    std::string_view parent_dir = get_everything_minus_file_name(lnk_data.target_path_utf8.data());
                    expl.cwd = path_create(parent_dir.data(), parent_dir.size());

                    swan_path_t to_select = path_create(get_file_name(lnk_data.target_path_utf8.data()));
                    {
                        std::scoped_lock lock(expl.select_cwd_entries_on_next_update_mutex);
                        expl.select_cwd_entries_on_next_update.clear();
                        expl.select_cwd_entries_on_next_update.push_back(to_select);
                    }

                    expl.push_history_item(expl.cwd);
                    (void) expl.update_cwd_entries(full_refresh, expl.cwd.data());
                    (void) expl.save_to_disk();

                    expl.scroll_to_nth_selected_entry_next_frame = 0;
                }
            }
            if (imgui::Selectable("Copy name")) {
                imgui::SetClipboardText(expl.right_clicked_ent->basic.path.data());
            }
            if (imgui::Selectable("Copy full path")) {
                swan_path_t full_path = path_create(expl.cwd.data());
                if (!path_append(full_path, expl.right_clicked_ent->basic.path.data(), settings.dir_separator_utf8, true)) {
                    swan_popup_modals::open_error(make_str("Copy full path of [%s].", expl.right_clicked_ent->basic.path.data()).c_str(),
                                                    "Max path length exceeded when appending name to current working directory path.");
                } else {
                    imgui::SetClipboardText(full_path.data());
                }
            }
            if (imgui::Selectable("Copy size (bytes)")) {
                imgui::SetClipboardText(std::to_string(expl.right_clicked_ent->basic.size).c_str());
            }
            if (imgui::Selectable("Copy size (pretty)")) {
                char buffer[32]; init_empty_cstr(buffer);
                format_file_size(expl.right_clicked_ent->basic.size, buffer, lengthof(buffer), settings.size_unit_multiplier);
                imgui::SetClipboardText(buffer);
            }

            if (imgui::Selectable("Reveal in File Explorer")) {
                auto res = reveal_in_file_explorer(*expl.right_clicked_ent, expl, settings.dir_separator_utf8);
                if (!res.success) {
                    swan_popup_modals::open_error(
                        make_str("Reveal [%s] in File Explorer.", expl.right_clicked_ent->basic.path.data()).c_str(),
                        res.error_or_utf8_path.c_str());
                }
            }

            imgui::Separator();

            auto handle_failure = [&](char const *operation, generic_result const &result) {
                if (!result.success) {
                    swan_popup_modals::open_error(make_str("%s [%s].", operation, expl.right_clicked_ent->basic.path.data()).c_str(),
                                                    result.error_or_utf8_path.c_str());
                }
            };

            if (imgui::Selectable("Cut##single")) {
                if (!io.KeyShift) {
                    s_file_op_payload.clear();
                }
                expl.deselect_all_cwd_entries();
                expl.right_clicked_ent->is_selected = true;
                auto result = add_selected_entries_to_file_op_payload(expl, "Cut", 'X');
                handle_failure("cut", result);
            }
            if (imgui::Selectable("Copy##single")) {
                if (!io.KeyShift) {
                    s_file_op_payload.clear();
                }

                expl.deselect_all_cwd_entries();
                expl.right_clicked_ent->is_selected = true;

                auto result = add_selected_entries_to_file_op_payload(expl, "Copy", 'C');
                handle_failure("copy", result);
            }

            imgui::Separator();

            if (imgui::Selectable("Delete##single")) {
                expl.deselect_all_cwd_entries();
                expl.right_clicked_ent->is_selected = true;

                auto result = delete_selected_entries(expl);
                handle_failure("delete", result);
            }
            if (imgui::Selectable("Rename##single")) {
                retval.open_single_rename_popup = true;
                retval.single_dirent_to_be_renamed = expl.right_clicked_ent;
            }
        }
        else { // right click when > 1 dirents selected
            if (imgui::Selectable("Copy names")) {
                std::string clipboard = {};

                for (auto const &dirent : expl.cwd_entries) {
                    if (dirent.is_selected && !dirent.is_filtered_out) {
                        clipboard += dirent.basic.path.data();
                        clipboard += '\n';
                    }
                }

                imgui::SetClipboardText(clipboard.c_str());
            }
            if (imgui::Selectable("Copy full paths")) {
                std::string clipboard = {};

                for (auto const &dirent : expl.cwd_entries) {
                    if (dirent.is_selected && !dirent.is_filtered_out) {
                        swan_path_t full_path = path_create(expl.cwd.data());
                        if (!path_append(full_path, dirent.basic.path.data(), settings.dir_separator_utf8, true)) {
                            swan_popup_modals::open_error(make_str("Copy full path of [%s].", dirent.basic.path.data()).c_str(),
                                                            "Max path length exceeded when appending name to current working directory path.");
                            break;
                        }
                        clipboard += full_path.data();
                        clipboard += '\n';
                    }
                }

                imgui::SetClipboardText(clipboard.c_str());
            }

            imgui::Separator();

            auto handle_failure = [](char const *operation, generic_result const &result) {
                if (!result.success) {
                    u64 num_failed = std::count(result.error_or_utf8_path.begin(), result.error_or_utf8_path.end(), '\n');
                    swan_popup_modals::open_error(make_str("%s %zu items.", operation, num_failed).c_str(), result.error_or_utf8_path.c_str());
                }
            };

            if (imgui::Selectable("Cut##multi")) {
                if (!io.KeyShift) {
                    s_file_op_payload.clear();
                }
                auto result = add_selected_entries_to_file_op_payload(expl, "Cut", 'X');
                handle_failure("Cut", result);
            }
            if (imgui::Selectable("Copy##multi")) {
                if (!io.KeyShift) {
                    s_file_op_payload.clear();
                }
                auto result = add_selected_entries_to_file_op_payload(expl, "Copy", 'C');
                handle_failure("Copy", result);
            }

            imgui::Separator();

            if (imgui::Selectable("Delete##multi")) {
                auto result = delete_selected_entries(expl);
                handle_failure("Delete", result);
            }
            if (imgui::Selectable("Bulk Rename")) {
                retval.open_bulk_rename_popup = true;
            }
        }

        imgui::EndPopup();
    }

    return retval;
}

explorer_window::cwd_entries_column_sort_specs_t explorer_window::copy_column_sort_specs(ImGuiTableSortSpecs const *table_sort_specs) noexcept
{
    assert(table_sort_specs != nullptr);

    explorer_window::cwd_entries_column_sort_specs_t column_sort_specs_ret = {};

    for (s32 i = 0; i < table_sort_specs->SpecsCount; ++i) {
        auto column_sort_spec = table_sort_specs->Specs[i];
        column_sort_specs_ret.push_back(column_sort_spec);
    }

    return column_sort_specs_ret;
}

static
void accept_move_dirents_drag_drop(explorer_window &expl) noexcept
{
    if (imgui::BeginDragDropTarget()) {
        auto payload_wrapper = imgui::GetDragDropPayload();

        if (payload_wrapper != nullptr &&
            streq(payload_wrapper->DataType, "move_dirents_drag_drop_payload") &&
            reinterpret_cast<move_dirents_drag_drop_payload *>(payload_wrapper->Data)->src_explorer_id != expl.id)
        {
            payload_wrapper = imgui::AcceptDragDropPayload("move_dirents_drag_drop_payload");
            if (payload_wrapper != nullptr) {
                auto payload_data = (move_dirents_drag_drop_payload *)payload_wrapper->Data;
                assert(payload_data != nullptr);
                s32 src_explorer_id = payload_data->src_explorer_id;
                if (src_explorer_id != expl.id) {
                    auto result = move_files_into(expl.cwd, expl, *payload_data);
                    if (!result.success) {
                        // TODO: handle error
                    }
                }
            }
        }

        imgui::EndDragDropTarget();
    }
}

static
void render_footer(explorer_window &expl, cwd_count_info const &cnt, ImGuiStyle &style) noexcept
{
    if (imgui::BeginChild("explorer_footer", { 0, imgui::CalcTextSize("1").y + style.WindowPadding.y + 5 }), ImGuiWindowFlags_NoNav) {
        imgui::ScopedStyle<f32> s2(style.ItemSpacing.y, 5);
        imgui::Spacing();
        imgui::AlignTextToFramePadding();

        if (expl.refresh_message != "") {
            imgui::TextColored(orange(), "%s", expl.refresh_message.c_str());
            if (imgui::IsItemHovered()) {
                imgui::SetTooltip("Things have changed in this directory since this list of entries was last retrieved.\n"
                                  "These changes are not being shown, click to refresh and see them.\n"
                                  "Alternatively, you can set refresh mode to 'automatic' in [Explorer Options] > Refresh mode.");
            }
            if (imgui::IsItemClicked(ImGuiMouseButton_Left)) {
                (void) expl.update_cwd_entries(full_refresh, expl.cwd.data());
            }
            imgui::SameLineSpaced(1);
        }

        if (!path_is_empty(expl.cwd)) {
            render_num_cwd_items(cnt);

            if (expl.filter_error == "" && cnt.filtered_dirents > 0) {
                imgui::SameLineSpaced(1);
                render_num_cwd_items_filtered(cnt);
            }

            if (cnt.selected_dirents > 0) {
                imgui::SameLineSpaced(1);
                render_num_cwd_items_selected(expl, cnt);
            }

            if (!s_file_op_payload.items.empty()) {
                imgui::SameLineSpaced(1);
                render_file_op_payload_hint();
            }
        }
    }

    imgui::EndChild();
}
