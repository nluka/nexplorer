@call cl.exe ^
/c ^
/Ycstdafx.hpp ^
src/stdafx.cpp ^
/Yustdafx.hpp ^
/Fo:obj_debug\ ^
/Fe:dist\swan_debug.exe ^
/Fd:dist\vc140_debug.pdb ^
/I"C:/code/glfw" ^
/I"C:/code/boost_1_80_0" ^
/std:c++20 ^
/nologo ^
/W4 ^
/EHsc ^
/MP ^
/MT ^
/D_CRT_SECURE_NO_WARNINGS ^
/Zi ^
/link ^
/DEBUG:FULL ^
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
shlwapi.lib ^
Pathcch.lib
