cmake_minimum_required(VERSION 3.22)
project(voicenotes)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

# ── Search paths ──────────────────────────────────────────────────────────────
if(NOT CMAKE_CROSSCOMPILING AND NOT WIN32)
    set(CMAKE_PREFIX_PATH "/usr/local;${CMAKE_PREFIX_PATH}")
    list(APPEND CMAKE_LIBRARY_PATH /usr/local/lib)
    set(ENV{PKG_CONFIG_PATH} "/usr/local/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()

if(WIN32 AND CMAKE_CROSSCOMPILING)
    set(ENV{PKG_CONFIG_LIBDIR}
        "/usr/x86_64-w64-mingw32/lib/pkgconfig:/usr/x86_64-w64-mingw32/sys-root/mingw/lib/pkgconfig")
endif()

# ── SDL3 ──────────────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED sdl3)

# ── OpenGL ────────────────────────────────────────────────────────────────────
find_package(OpenGL REQUIRED)

# ── ImGui (from rfx shared libs — same as junglizer) ─────────────────────────
set(RFX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../rfx")
set(IMGUI_DIR "${RFX_DIR}/shared/lib/imgui")
include_directories(
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${RFX_DIR}/shared/lib/tinyfiledialogs
    ${RFX_DIR}/ui          # regroove_ui.h
    ${RFX_DIR}/common      # sdl_audio_init.h
)

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl2.cpp
)

# ── whisper.cpp (cloned by setup.sh) ─────────────────────────────────────────
set(WHISPER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/whisper.cpp")
set(GGML_DIR   "${WHISPER_DIR}/ggml")

set(WHISPER_SOURCES
    ${WHISPER_DIR}/src/whisper.cpp
    ${GGML_DIR}/src/ggml.c
    ${GGML_DIR}/src/ggml-alloc.c
    ${GGML_DIR}/src/ggml-backend.cpp
    ${GGML_DIR}/src/ggml-backend-reg.cpp
    ${GGML_DIR}/src/ggml-opt.cpp
    ${GGML_DIR}/src/ggml-quants.c
    ${GGML_DIR}/src/ggml-threading.cpp
    ${GGML_DIR}/src/ggml-cpu/ggml-cpu.c
    ${GGML_DIR}/src/ggml-cpu/ggml-cpu.cpp
    ${GGML_DIR}/src/ggml-cpu/ggml-cpu-quants.c
    ${GGML_DIR}/src/ggml-cpu/ggml-cpu-traits.cpp
)

# x86_64: add x86 feature detection + SIMD flags
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|x86" OR
   (WIN32 AND CMAKE_CROSSCOMPILING))   # cross-compiled Windows is always x86_64
    if(EXISTS "${GGML_DIR}/src/ggml-cpu/cpu-feats-x86.cpp")
        list(APPEND WHISPER_SOURCES ${GGML_DIR}/src/ggml-cpu/cpu-feats-x86.cpp)
    endif()
    # Enable AVX2 + FMA — makes whisper ~10-20x faster on x86 vs scalar
    set(WHISPER_SIMD_FLAGS -mavx -mavx2 -mfma -mf16c)
else()
    # ARM (aarch64) — NEON
    if(EXISTS "${GGML_DIR}/src/ggml-cpu/ggml-cpu-aarch64.cpp")
        list(APPEND WHISPER_SOURCES ${GGML_DIR}/src/ggml-cpu/ggml-cpu-aarch64.cpp)
    endif()
    set(WHISPER_SIMD_FLAGS "")
endif()

if(EXISTS "${GGML_DIR}/src/ggml-cpu/ggml-cpu-hbm.cpp")
    list(APPEND WHISPER_SOURCES ${GGML_DIR}/src/ggml-cpu/ggml-cpu-hbm.cpp)
endif()

add_library(whisper STATIC ${WHISPER_SOURCES})
target_compile_definitions(whisper PUBLIC GGML_USE_CPU _GNU_SOURCE)
target_compile_options(whisper PRIVATE -O3 ${WHISPER_SIMD_FLAGS})
target_include_directories(whisper PUBLIC
    ${WHISPER_DIR}/include
    ${WHISPER_DIR}/src
    ${GGML_DIR}/include
    ${GGML_DIR}/src
    ${GGML_DIR}/src/ggml-cpu
)
if(UNIX)
    target_link_libraries(whisper pthread)
endif()

# ── Main executable ───────────────────────────────────────────────────────────
if(WIN32)
    set(WIN_RC voicenotes.rc)  # embeds icon.ico into the .exe
endif()

add_executable(voicenotes
    src/main.cpp
    ${IMGUI_SOURCES}
    ${RFX_DIR}/shared/lib/tinyfiledialogs/tinyfiledialogs.c
    ${WIN_RC}
)

target_include_directories(voicenotes PRIVATE
    ${SDL3_INCLUDE_DIRS}
    ${OPENGL_INCLUDE_DIR}
    ${WHISPER_DIR}/include
)

target_link_libraries(voicenotes
    whisper
    ${SDL3_LIBRARIES}
    ${OPENGL_LIBRARIES}
)

# libcurl for in-app model download
find_package(CURL REQUIRED)
target_include_directories(voicenotes PRIVATE ${CURL_INCLUDE_DIRS})
target_link_libraries(voicenotes ${CURL_LIBRARIES})

if(WIN32)
    # -mwindows hides the console window (GUI subsystem)
    target_link_options(voicenotes PRIVATE -mwindows)
    target_link_libraries(voicenotes comdlg32 ole32)
endif()

message(STATUS "SDL3: ${SDL3_LIBRARIES}")
message(STATUS "OpenGL: ${OPENGL_LIBRARIES}")
