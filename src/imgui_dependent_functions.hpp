/*
    Functions and structures coupled to the existence of ImGui.
*/

#pragma once

#include "stdafx.hpp"
#include "common_functions.hpp"
#include "imgui_extension.hpp"

void BeginFrame_GLFW_OpenGL3(char const *ini_file_path) noexcept;
void EndFrame_GLFW_OpenGL3(GLFWwindow *) noexcept;

void BeginFrame_Win32_DX11(char const *ini_file_path) noexcept;
bool EndFrame_Win32_DX11(ID3D11DeviceContext *, ID3D11RenderTargetView *, IDXGISwapChain *) noexcept;

void center_window_and_set_size_when_appearing(f32 width, f32 height) noexcept;

ImVec4 get_color(basic_dirent::kind t) noexcept;
ImVec4 get_color(bulk_rename_transform::status s) noexcept;

ImVec4 success_color() noexcept;
ImVec4 warning_color() noexcept;
ImVec4 warning_lite_color() noexcept;
ImVec4 error_color() noexcept;
ImVec4 directory_color() noexcept;
ImVec4 symlink_color() noexcept;
ImVec4 file_color() noexcept;

enum class serialize_ImGuiStyle_mode : s32
{
    plain_text,
    cpp_code,
};
std::string serialize_ImGuiStyle(ImGuiStyle const &style, u64 reserve_size, serialize_ImGuiStyle_mode mode) noexcept;
void serialize_ImGuiStyle_all_except_colors(ImGuiStyle const &style, std::string &out, serialize_ImGuiStyle_mode mode) noexcept;
void serialize_ImGuiStyle_only_colors(ImVec4 const *colors, std::string &out, serialize_ImGuiStyle_mode mode) noexcept;

ImGuiStyle swan_default_imgui_style() noexcept;

typedef wchar_t* filter_chars_callback_user_data_t;
s32 filter_chars_callback(ImGuiInputTextCallbackData *data) noexcept;

void render_path_with_stylish_separators(char const *path) noexcept;

struct debug_log_record
{
    std::string message;                static_assert(sizeof(message) == 32);
    std::source_location loc;           static_assert(sizeof(loc) == 24);
    time_point_system_t system_time;    static_assert(sizeof(system_time) == 8);
    u64 id;
    f64 imgui_time;
    s32 thread_id;
    u32 num_repeats;

    bool operator>(debug_log_record const &other) const noexcept { return this->id > other.id; }
    bool operator<(debug_log_record const &other) const noexcept { return !(*this > other); }

    bool matches_search_text(char const *search_substr) const noexcept
    {
        bool match =
            StrStrIA(this->loc.file_name(),                               search_substr) ||
            StrStrIA(this->loc.function_name(),                           search_substr) ||
            StrStrIA(this->message.c_str(),                               search_substr) ||
            StrStrIA(make_str_static<32>("%zu", this->loc.line()).data(), search_substr) ||
            StrStrIA(make_str_static<32>("%zu", this->thread_id).data(),  search_substr)
        ;
        return match;
    }
};

struct debug_log
{
    char const *fmt;
    std::source_location loc;

    static debug_log_record *g_last_record;
    static std::string g_search_text;
    static std::vector<debug_log_record> g_records_shown;
    static std::vector<debug_log_record> g_records_hidden;
    static std::mutex g_mutex;
    static u64 g_next_id;
    static bool g_logging_enabled;

    debug_log(char const *f, std::source_location l = std::source_location::current()) noexcept
        : fmt(f)
        , loc(l)
    {}

    static void clear() noexcept
    {
        g_records_shown.clear();
        g_records_hidden.clear();
    }
};

/// @brief Writes a message to the debug log window (not stdout!). Information such as time, thread id, source location are handled for you.
/// Use this function like you would sprintf, pass it a format string followed by your variadic arguments. Operation is threadsafe, you can
/// print messages from any thread at any time.
template <typename... Args>
void print_debug_msg([[maybe_unused]] debug_log pack, [[maybe_unused]] Args&&... args) noexcept
{
    // https://stackoverflow.com/questions/57547273/how-to-use-source-location-in-a-variadic-template-function

    if (!debug_log::g_logging_enabled) {
        return;
    }

    u64 max_size = global_state::debug_log_size_limit_megabytes() * 1024 * 1024;

    auto formatted_message = make_str_static<4096>(pack.fmt, args...);
    f64 imgui_time = imgui::GetTime();
    s32 thread_id = GetCurrentThreadId();
    time_point_system_t system_time = get_time_system();

    std::time_t time = std::chrono::system_clock::to_time_t(system_time);
    std::tm tm = *std::localtime(&time);

    auto markdown_line = make_str_static<4096*2>("| %d | %d:%02d.%02d | %.3lf | %s | %d | %s | %s |\n",
        thread_id,
        tm.tm_hour, tm.tm_min, tm.tm_sec,
        imgui_time,
        path_cfind_filename(pack.loc.file_name()),
        pack.loc.line(),
        pack.loc.function_name(),
        formatted_message.data());

    std::vector<debug_log_record> &dst_vec = debug_log::g_records_shown;

    if (!debug_log::g_search_text.empty()) {
        bool match = false;
        auto markdown_line_copy = markdown_line;
        char const *piece = strtok(markdown_line_copy.data(), "|");
        while (piece != nullptr) {
            if (StrStrIA(piece, debug_log::g_search_text.c_str())) {
                match = true;
                break;
            }
            piece = strtok(nullptr, "|");
        }
        dst_vec = match ? debug_log::g_records_shown : debug_log::g_records_hidden;
    }

    {
        std::scoped_lock lock(debug_log::g_mutex);

        if (debug_log::g_last_record && cstr_eq(debug_log::g_last_record->message.c_str(), formatted_message.data())) {
            debug_log::g_last_record->num_repeats += 1;
        }
        else {
            u64 used_size = sizeof(debug_log_record) * (debug_log::g_records_shown.size() + debug_log::g_records_hidden.size());
            if (used_size > max_size) {
                debug_log::clear();
            }
            dst_vec.reserve(max_size / sizeof(debug_log_record));
            debug_log::g_last_record = &dst_vec.emplace_back(formatted_message.data(), pack.loc, system_time, debug_log::g_next_id++, imgui_time, thread_id, 0);
        }
    }

    auto log_file_path = global_state::execution_path() / "debug_log.md";
    FILE *file = fopen(log_file_path.string().c_str(), "a");
    if (file) {
        fprintf(file, markdown_line.data());
        fflush(file);
        fclose(file);
    }
}
