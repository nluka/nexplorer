@call cl.exe ^
src/swan.cpp ^
src/common.cpp ^
src/path.cpp ^
obj_debug/imgui_demo.obj ^
obj_debug/imgui_draw.obj ^
obj_debug/imgui_impl_glfw.obj ^
obj_debug/imgui_impl_opengl3.obj ^
obj_debug/imgui_tables.obj ^
obj_debug/imgui_widgets.obj ^
obj_debug/imgui.obj ^
.res ^
/Fo:obj_debug\ ^
/Fe:swan_debug.exe ^
/I"C:/code/glfw" ^
/I"C:/code/boost_1_80_0" ^
/std:c++20 ^
/nologo ^
/W4 /WX ^
/EHsc ^
/MT ^
/D_CRT_SECURE_NO_WARNINGS ^
/link ^
/NODEFAULTLIB:MSVCRTD ^
/NODEFAULTLIB:LIBCMT ^
/LIBPATH:"C:/code/glfw" ^
/LIBPATH:"C:/code/boost_1_80_0/stage/lib" ^
glfw3.lib ^
opengl32.lib ^
gdi32.lib ^
shell32.lib ^
kernel32.lib ^
msvcrt.lib ^
ole32.lib ^
shlwapi.lib
