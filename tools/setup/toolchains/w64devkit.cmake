# The player's compiler, w64devkit (GCC, msvcrt by default):
#   cmake -DCMAKE_TOOLCHAIN_FILE=tools/setup/toolchains/w64devkit.cmake
# with ACZ_TC_ROOT in the environment = the unpacked w64devkit folder (holds bin/gcc.exe). See tools/setup/toolchain_build.sh.
if(NOT DEFINED ENV{ACZ_TC_ROOT})
  message(FATAL_ERROR "ACZ_TC_ROOT: the unpacked w64devkit folder")
endif()
file(TO_CMAKE_PATH "$ENV{ACZ_TC_ROOT}" TC_ROOT)
set(CMAKE_C_COMPILER "${TC_ROOT}/bin/gcc.exe" CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER "${TC_ROOT}/bin/g++.exe" CACHE FILEPATH "")
