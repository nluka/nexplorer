#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <string>
#include <cstring>
#include <memory>
#include <iostream>
#include <vector>
#include <array>

#if 0
    // [Win32] Our example includes a copy of glfw3.lib pre-compiled with VS2010 to maximize ease of testing and compatibility with old VS compilers.
    // To link with VS2010-era libraries, VS2015+ requires linking with legacy_stdio_definitions.lib, which we do using this pragma.
    // Your own project should not be affected, as you are likely to link with a newer binary of GLFW that is adequate for your version of Visual Studio.
    #if defined(_MSC_VER) && (_MSC_VER >= 1900) && !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
    #pragma comment(lib, "legacy_stdio_definitions")
    #endif
#endif

#include <Windows.h>

#define GL_SILENCE_DEPRECATION
#include <glfw3.h> // Will drag system OpenGL headers

#include "imgui/imgui.h"
#include "imgui/imgui_impl_glfw.h"
#include "imgui/imgui_impl_opengl3.h"

#include "on_scope_exit.hpp"
#include "primitives.hpp"
#include "util.hpp"

using string_t = std::string;

static
void glfw_error_callback(int error, const char* description)
{
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

static
GLFWwindow *init_glfw_and_imgui()
{
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        return nullptr;
    }

    // GL 3.0 + GLSL 130
    {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        //glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
        //glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // 3.0+ only
    }

    GLFWwindow *window = nullptr;
    // Create window with graphics context
    {
        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        window = glfwCreateWindow(screenWidth, screenHeight, "nexplorer", nullptr, nullptr);
        if (window == nullptr) {
            return nullptr;
        }
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.ConfigDockingWithShift = true;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowSizeConstraints(io.DisplaySize, io.DisplaySize);
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    {
        char const *glsl_version = "#version 130";
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init(glsl_version);
    }

    {
        [[maybe_unused]] auto font = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/consola.ttf", 15.0f);
        // IM_ASSERT(font != nullptr);

        io.Fonts->AddFontDefault();
    }

    return window;
}

static
i32 directory_exists(char const *path)
{
    DWORD attributes = GetFileAttributesA(path);
    return (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY));
}

typedef std::array<char, MAX_PATH> path_t;

struct dir_entry {
    bool is_directory;
    path_t path;
};

static std::vector<dir_entry> s_dir_entries{};
static path_t s_working_dir{};
static u64 s_num_file_searches = 0;
static bool s_working_dir_changed = false;

static
void update_dir_entries(std::string_view parent_dir)
{
    s_dir_entries.clear();

    WIN32_FIND_DATAA find_data;

    while (parent_dir.ends_with(' ')) {
        parent_dir = std::string_view(parent_dir.data(), parent_dir.size() - 1);
    }

    if (!directory_exists(parent_dir.data())) {
        std::cerr << "directory [" << parent_dir.data() << "] doesn't exist\n";
        return;
    }

    static std::string search_path{};
    search_path.reserve(parent_dir.size() + strlen("/*"));
    search_path = parent_dir;
    search_path += "/*";

    std::cerr << "search_path = [" << search_path << "]\n";

    HANDLE find_handle = FindFirstFileA(search_path.data(), &find_data);
    auto find_handle_cleanup_routine = make_on_scope_exit([&find_handle] { FindClose(find_handle); });

    if (find_handle == INVALID_HANDLE_VALUE) {
        std::cerr << "find_handle == INVALID_HANDLE_VALUE\n";
    }

    do
    {
        dir_entry entry;
        entry.is_directory = find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
        std::memcpy(entry.path.data(), find_data.cFileName, entry.path.size());

        s_dir_entries.emplace_back(entry);
        ++s_num_file_searches;
    }
    while (FindNextFileA(find_handle, &find_data));
}

static
i32 cwd_text_input_callback(ImGuiInputTextCallbackData *data)
{
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        static std::wstring const forbidden_chars = L"<>\"|?*";
        bool is_forbidden = forbidden_chars.find(data->EventChar) != std::string::npos;
        if (is_forbidden) {
            data->EventChar = L'\0';
        }
    }
    else if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit) {
        std::cerr << "ImGuiInputTextFlags_CallbackEdit, data->Buf = [" << data->Buf << "]\n";
        update_dir_entries(data->Buf);
    }

    return 0;
}

i32 main(i32, char**)
{
    GLFWwindow *window = init_glfw_and_imgui();
    if (window == nullptr) {
        return 1;
    }

    auto window_cleanup_routine = make_on_scope_exit([window]() {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
    });

    auto &io = ImGui::GetIO();

    s_dir_entries.reserve(1024);

    {
        i32 written = GetCurrentDirectoryA((i32)s_working_dir.size(), s_working_dir.data());
        if (written == 0) {
            std::exit(1);
        }
    }

    update_dir_entries(s_working_dir.data());

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::DockSpaceOverViewport(0, ImGuiDockNodeFlags_PassthruCentralNode);

        // {
        //     ImGui::Begin("Pinned");
        //     ImGui::Text("Pinned directories go here.");
        //     ImGui::End();
        // }

        {
            ImGui::Begin("Browse");

            ImGui::InputText(
                "##cwd", s_working_dir.data(), s_working_dir.size(),
                ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_CallbackEdit,
                cwd_text_input_callback
            );

            ImGui::Spacing();

            if (s_dir_entries.empty()) {
                ImGui::Text("Not a directory.");
            }
            else {
                static ImVec4 const white(255, 255, 255, 255);
                static ImVec4 const yellow(255, 255, 0, 255);

                for (auto const &dir_ent : s_dir_entries) {
                    ImGui::TextColored(dir_ent.is_directory ? yellow : white, dir_ent.path.data());
                }
            }

            ImGui::End();
        }

        {
            ImGui::Begin("Analytics");
            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
            ImGui::Text("s_working_dir = [%s]", s_working_dir);
            ImGui::Text("cwd_exists = [%d]", directory_exists(s_working_dir.data()));
            ImGui::Text("s_num_file_searches = [%zu]", s_num_file_searches);
            ImGui::Text("s_dir_entries.size() = [%zu]", s_dir_entries.size());
            ImGui::End();
        }

        ImGui::ShowDemoWindow();

        // Rendering
        {
            ImGui::Render();

            int display_w, display_h;
            ImVec4 clear_color(0.45f, 0.55f, 0.60f, 1.00f);

            glfwGetFramebufferSize(window, &display_w, &display_h);
            glViewport(0, 0, display_w, display_h);
            glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            glfwSwapBuffers(window);
        }
    }

    return 0;
}
