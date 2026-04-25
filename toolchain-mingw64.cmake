# CMake toolchain file for cross-compiling to Windows from Linux using MinGW-w64

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Specify the cross-compiler
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

# Where to find the target environment
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)

# Search for programs in the build host directories
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# Search for libraries and headers in the target directories
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# MinGW compatibility: disable C99 wide char functions that aren't available
# Use string() to append to avoid overriding existing flags
set(CMAKE_CXX_FLAGS_INIT "-D_GLIBCXX_USE_C99_WCHAR=0 -D__USE_MINGW_ANSI_STDIO=0")
set(CMAKE_C_FLAGS_INIT "-D_GLIBCXX_USE_C99_WCHAR=0 -D__USE_MINGW_ANSI_STDIO=0")

# Make sure we link statically to avoid DLL dependencies
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
