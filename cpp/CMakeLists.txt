cmake_minimum_required(VERSION 3.24)
project(SeismicFDTD LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# ── GLFW ───────────────────────────────────────────────────────────────────
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)

# ── Dear ImGui (no CMake of its own) ──────────────────────────────────────
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _no_cmake)   # populate only, sources are listed below

# ── JSON + native dialogs (header only) ───────────────────────────────────
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_Declare(pfd
    GIT_REPOSITORY https://github.com/samhocevar/portable-file-dialogs.git
    GIT_TAG 0.1.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _no_cmake)

FetchContent_MakeAvailable(glfw json imgui pfd)

add_library(glad STATIC third_party/glad/src/glad.c)
target_include_directories(glad PUBLIC third_party/glad/include)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
# 32-bit glyphs so emoji codepoints (U+1Fxxx) from Segoe UI Emoji can be merged
target_compile_definitions(imgui PUBLIC IMGUI_USE_WCHAR32 IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(imgui PUBLIC glfw)

# ── Core simulation library (no GUI) ──────────────────────────────────────
add_library(fdtd_core STATIC
    src/core/Geology.cpp
    src/core/Cpml.cpp
    src/core/Eikonal.cpp
    src/core/CpuSolver.cpp
    src/core/GpuSolver.cpp
    src/core/Segy.cpp
    src/core/ModelImport.cpp
    src/core/ImageModel.cpp
    src/core/ModelCanvas.cpp
    src/core/ParamsIO.cpp
    src/core/Report.cpp
    src/core/Simulation.cpp)
target_include_directories(fdtd_core PUBLIC src)
target_include_directories(fdtd_core PRIVATE ${imgui_SOURCE_DIR})   # imstb_truetype.h (report fonts)
target_link_libraries(fdtd_core PUBLIC glad nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(fdtd_core PUBLIC windowscodecs ole32)   # image models (WIC)
endif()

if(MSVC)
    set(FAST_FLAGS /O2 /Oi /arch:AVX2 /fp:fast /utf-8 /W3 /permissive-)
    target_compile_options(fdtd_core PRIVATE ${FAST_FLAGS})
    target_compile_definitions(fdtd_core PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
else()
    target_compile_options(fdtd_core PRIVATE -O3 -march=native -ffast-math)
endif()

# ── Application ───────────────────────────────────────────────────────────
add_executable(SeismicFDTD WIN32
    src/main.cpp
    src/ui/Theme.cpp
    src/ui/Fonts.cpp
    src/ui/Widgets.cpp
    src/ui/App.cpp
    src/ui/SceneView.cpp
    src/ui/GatherView.cpp
    src/ui/ModelEditor.cpp
    src/app.rc)
target_include_directories(SeismicFDTD PRIVATE src ${pfd_SOURCE_DIR})
target_link_libraries(SeismicFDTD PRIVATE fdtd_core imgui glfw opengl32)
if(MSVC)
    target_compile_options(SeismicFDTD PRIVATE /O2 /utf-8 /W3 /permissive-)
    set_target_properties(SeismicFDTD PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
endif()

# ── Headless tests / benchmark ────────────────────────────────────────────
enable_testing()
add_executable(fdtd_tests tests/fdtd_tests.cpp)
target_link_libraries(fdtd_tests PRIVATE fdtd_core glfw opengl32)
if(MSVC)
    target_compile_options(fdtd_tests PRIVATE /O2 /utf-8)
endif()
add_test(NAME fdtd_tests COMMAND fdtd_tests)
