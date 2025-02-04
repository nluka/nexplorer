// Single translation unit compilation nearly doubles compile speed.
// Precompiled header (stdafx.hpp) offers another 3x speed increase.

#include "stdafx.hpp"

#if defined(NDEBUG)
#   define DEBUG_MODE 0
#   define RELEASE_MODE 1
#else
#   define DEBUG_MODE 1
#   define RELEASE_MODE 0
#endif

#include "libs/ntest.cpp"

#include "analytics.cpp"
#include "debug_log.cpp"
#include "drop_target.cpp"
#include "explorer.cpp"
#include "explorer_drop_source.cpp"
#include "explorer_file_op_progress_sink.cpp"
#include "file_operations.cpp"
#include "finder.cpp"
#include "icon_glyphs.cpp"
#include "icon_library.cpp"
#include "imgui_dependent_functions.cpp"
#include "imgui_extension.cpp"
#include "imspinner_demo.cpp"
#include "main_menu_bar.cpp"
#include "miscellaneous_functions.cpp"
#include "miscellaneous_globals.cpp"
#include "path.cpp"
#include "pinned.cpp"
#include "popup_modal_bulk_rename.cpp"
#include "popup_modal_edit_pin.cpp"
#include "popup_modal_error.cpp"
#include "popup_modal_new_directory.cpp"
#include "popup_modal_new_file.cpp"
#include "popup_modal_new_pin.cpp"
#include "popup_modal_single_rename.cpp"
#include "recent_files.cpp"
#include "settings.cpp"
#include "stdafx.cpp"
#include "style.cpp"
#include "swan_glfw_opengl3.cpp"
// #include "swan_win32_dx11.cpp"
#include "tests.cpp"
#include "theme_editor.cpp"
#include "undelete_directory_progress_sink.cpp"
#include "util.cpp"
