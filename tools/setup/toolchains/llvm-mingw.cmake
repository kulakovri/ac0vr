# The player's compiler, llvm-mingw (clang + lld, UCRT):
#   cmake -DCMAKE_TOOLCHAIN_FILE=tools/setup/toolchains/llvm-mingw.cmake
# with ACZ_TC_ROOT in the environment = the unpacked llvm-mingw folder (holds bin/clang.exe). See tools/setup/toolchain_build.sh.
if(NOT DEFINED ENV{ACZ_TC_ROOT})
  message(FATAL_ERROR "ACZ_TC_ROOT: the unpacked llvm-mingw folder")
endif()
file(TO_CMAKE_PATH "$ENV{ACZ_TC_ROOT}" TC_ROOT)
# the runtime uses Windows 10 APIs (CPU sets); this compiler's headers default to an older version
set(CMAKE_C_FLAGS_INIT "-D_WIN32_WINNT=0x0A00")
set(CMAKE_CXX_FLAGS_INIT "-D_WIN32_WINNT=0x0A00")
# the threads' stacks come from the exe's header (the runtime gives pthread_create no size): the 2 MB of GNU ld, as the tested builds
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--stack,2097152")
set(CMAKE_C_COMPILER "${TC_ROOT}/bin/x86_64-w64-mingw32-clang.exe" CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER "${TC_ROOT}/bin/x86_64-w64-mingw32-clang++.exe" CACHE FILEPATH "")
set(CMAKE_AR "${TC_ROOT}/bin/llvm-ar.exe" CACHE FILEPATH "")
set(CMAKE_RANLIB "${TC_ROOT}/bin/llvm-ranlib.exe" CACHE FILEPATH "")
