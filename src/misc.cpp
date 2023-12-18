#include "imgui/imgui.h"
#include "libs/thread_pool.hpp"

#include "common_fns.hpp"
#include "imgui_specific.hpp"
#include "path.hpp"

static s32 s_page_size = 0;
s32 &global_state::page_size() noexcept { return s_page_size; }

static swan_thread_pool_t s_thread_pool(1);
swan_thread_pool_t &global_state::thread_pool() noexcept { return s_thread_pool; }

ImVec4 orange()                noexcept { return ImVec4(1, 0.5f, 0, 1); }
ImVec4 red()                   noexcept { return ImVec4(1, 0.2f, 0, 1); }
ImVec4 dir_color()             noexcept { return get_color(basic_dirent::kind::directory); }
ImVec4 symlink_color()         noexcept { return get_color(basic_dirent::kind::symlink_to_directory); }
ImVec4 invalid_symlink_color() noexcept { return get_color(basic_dirent::kind::invalid_symlink); }
ImVec4 file_color()            noexcept { return get_color(basic_dirent::kind::file); }

bool basic_dirent::is_dotdot()               const noexcept { return path_equals_exactly(path, ".."); }
bool basic_dirent::is_dotdot_dir()           const noexcept { return type == basic_dirent::kind::directory && path_equals_exactly(path, ".."); }
bool basic_dirent::is_directory()            const noexcept { return type == basic_dirent::kind::directory; }
bool basic_dirent::is_symlink()              const noexcept { return one_of(type, { kind::symlink_to_directory, kind::symlink_to_file, kind::invalid_symlink }); }
bool basic_dirent::is_symlink_to_file()      const noexcept { return type == basic_dirent::kind::symlink_to_file; }
bool basic_dirent::is_symlink_to_directory() const noexcept { return type == basic_dirent::kind::symlink_to_directory; }
bool basic_dirent::is_file()                 const noexcept { return type == basic_dirent::kind::file; }

s32 filter_chars_callback(ImGuiInputTextCallbackData *data) noexcept
{
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        wchar_t *chars_to_filter = (filter_chars_callback_user_data_t)data->UserData;
        bool is_forbidden = StrChrW(chars_to_filter, data->EventChar);
        if (is_forbidden) {
            data->EventChar = L'\0';
        }
    }

    return 0;
}

char const *basic_dirent::kind_cstr() const noexcept
{
    assert(this->type >= basic_dirent::kind::nil && this->type <= basic_dirent::kind::count);

    switch (this->type) {
        case basic_dirent::kind::directory:            return "directory";
        case basic_dirent::kind::file:                 return "file";
        case basic_dirent::kind::symlink_to_directory: return "symlink_to_directory";
        case basic_dirent::kind::symlink_to_file:      return "symlink_to_file";
        case basic_dirent::kind::invalid_symlink:      return "invalid_symlink";
        default: return "";
    }
}

char const *basic_dirent::kind_short_cstr() const noexcept
{
    assert(this->type >= basic_dirent::kind::nil && this->type <= basic_dirent::kind::count);

    switch (this->type) {
        case basic_dirent::kind::directory:            return "dir";
        case basic_dirent::kind::file:                 return "file";
        case basic_dirent::kind::symlink_to_directory: return ICON_CI_ARROW_SMALL_RIGHT "dir";
        case basic_dirent::kind::symlink_to_file:      return ICON_CI_ARROW_SMALL_RIGHT "file";
        case basic_dirent::kind::invalid_symlink:      return ICON_CI_ARROW_SMALL_RIGHT "?";
        default:                                       return "";
    }
}

char const *basic_dirent::kind_icon() const noexcept
{
    return get_icon(this->type);
}

char const *get_icon(basic_dirent::kind t) noexcept
{
    switch (t) {
        case basic_dirent::kind::directory:            return ICON_FA_FOLDER;
        case basic_dirent::kind::file:                 return ICON_FA_FILE;
        case basic_dirent::kind::symlink_to_directory: return ICON_CI_FILE_SYMLINK_DIRECTORY;
        case basic_dirent::kind::symlink_to_file:      return ICON_CI_FILE_SYMLINK_FILE;
        case basic_dirent::kind::invalid_symlink:      return ICON_CI_ERROR;
        default:                                       assert(false && "has no icon"); break;
    }
    return ICON_MD_ERROR;
}

ImVec4 get_color(basic_dirent::kind t) noexcept
{
    switch (t) {
        case basic_dirent::kind::directory:            return ImVec4(1, 1, 0, 1);         // yellow
        case basic_dirent::kind::file:                 return ImVec4(0.85f, 1, 0.85f, 1); // pale_green
        case basic_dirent::kind::symlink_to_directory: return ImVec4(1, 1, 0, 1);         // yellow
        case basic_dirent::kind::symlink_to_file:      return ImVec4(0.85f, 1, 0.85f, 1); // pale_green
        case basic_dirent::kind::invalid_symlink:      return ImVec4(1, 0, 0, 1);         // red
        default:                                       return ImVec4(1, 1, 1, 1);         // white
    }
}

void imgui::Spacing(u64 n) noexcept
{
    for (u64 i = 0; i < n; ++i) {
        ImGui::Spacing();
    }
}

char explorer_options::dir_separator_utf8() const noexcept { return unix_directory_separator ? '/' : '\\'; }
wchar_t explorer_options::dir_separator_utf16() const noexcept { return unix_directory_separator ? L'/' : L'\\'; }
u16 explorer_options::size_unit_multiplier() const noexcept { return binary_size_system ? 1024 : 1000; }

std::string get_last_error_string() noexcept
{
    DWORD error_code = GetLastError();
    if (error_code == 0) {
        return "No error.";
    }

    LPSTR buffer = nullptr;
    DWORD buffer_size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer),
        0,
        nullptr
    );

    if (buffer_size == 0) {
        return "Error formatting message.";
    }

    std::string error_message(buffer, buffer + buffer_size);
    LocalFree(buffer);

    // Remove trailing newline characters
    while (!error_message.empty() && (error_message.back() == '\r' || error_message.back() == '\n')) {
        error_message.pop_back();
    }

    return error_message;
}

void imgui::SameLineSpaced(u64 num_spacing_calls) noexcept
{
    ImGui::SameLine();
    for (u64 i = 0; i < num_spacing_calls; ++i) {
        ImGui::Spacing();
        ImGui::SameLine();
    }
}
