# The player's compiler, zig cc (clang + lld, x86_64-windows-gnu):
#   cmake -DCMAKE_TOOLCHAIN_FILE=tools/setup/toolchains/zig.cmake
# with ACZ_TC_ROOT in the environment = the unpacked zig folder (holds zig.exe). See tools/setup/toolchain_build.sh.
# The target is explicit: without -target zig cc compiles for the build machine's CPU (AVX2, FMA contraction).
# CMake wants one program for each tool, so they go through .cmd files written beside the build (ninja runs them);
# ZIG_TC_WRAP = that folder (the toolchain file runs again inside CMake's try-compile folders: the variable keeps it one).
# The cache folders stay in the build folder (zig's default is the user's local application data).
if(NOT DEFINED ENV{ACZ_TC_ROOT})
  message(FATAL_ERROR "ACZ_TC_ROOT: the unpacked zig folder")
endif()
if(NOT DEFINED ENV{ACZ_TC_WRAP})
  message(FATAL_ERROR "ACZ_TC_WRAP: the folder for zig's wrapper files")
endif()
file(TO_CMAKE_PATH "$ENV{ACZ_TC_ROOT}" TC_ROOT)
file(TO_CMAKE_PATH "$ENV{ACZ_TC_WRAP}" ZIG_WRAP)
file(MAKE_DIRECTORY "${ZIG_WRAP}")
file(TO_NATIVE_PATH "${TC_ROOT}/zig.exe" ZIG_EXE)
file(TO_NATIVE_PATH "${ZIG_WRAP}" ZIG_WRAP_N)
foreach(pair "zig-cc|cc -target x86_64-windows-gnu" "zig-c++|c++ -target x86_64-windows-gnu" "zig-ar|ar" "zig-ranlib|ranlib")
  string(REPLACE "|" ";" pl "${pair}")
  list(GET pl 0 name)
  list(GET pl 1 args)
  file(WRITE "${ZIG_WRAP}/${name}.cmd"
       "@set ZIG_LOCAL_CACHE_DIR=${ZIG_WRAP_N}\\cache\r\n@set ZIG_GLOBAL_CACHE_DIR=${ZIG_WRAP_N}\\gcache\r\n@\"${ZIG_EXE}\" ${args} %*\r\n")
endforeach()
# zig cc makes __DATE__ an error (ps2_statecap.c uses it)
set(CMAKE_C_FLAGS_INIT "-Wno-date-time")
set(CMAKE_CXX_FLAGS_INIT "-Wno-date-time")
# the threads' stacks come from the exe's header (the runtime gives pthread_create no size): the 2 MB of GNU ld, as the tested builds
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--stack,2097152")
set(CMAKE_C_COMPILER "${ZIG_WRAP}/zig-cc.cmd" CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER "${ZIG_WRAP}/zig-c++.cmd" CACHE FILEPATH "")
set(CMAKE_AR "${ZIG_WRAP}/zig-ar.cmd" CACHE FILEPATH "")
set(CMAKE_RANLIB "${ZIG_WRAP}/zig-ranlib.cmd" CACHE FILEPATH "")
