cmake_minimum_required(VERSION 3.16)
project(DS4SpotifyController)

set(CMAKE_CXX_STANDARD 17)

# --------------
# Dependencies -
# --------------
# SDL2: set SDL2_DIR or SDL2_ROOT, e.g.:
#   cmake -DSDL2_ROOT=C:/libs/SDL2 ..

# Dear ImGui: we expect the imgui source to be in ./imgui/
# (clone https://github.com/ocornut/imgui into the project root)

# Link SDL2 statically
set(SDL2_STATIC ON)
set(BUILD_SHARED_LIBS OFF)
find_package(SDL2 REQUIRED)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)

add_executable(DS4SpotifyController WIN32
    src/main.cpp
    src/controllers.cpp
    ${CMAKE_SOURCE_DIR}/app.rc
    ${IMGUI_SOURCES}
)

# Tell the resource compiler where to find logo.ico
set(RC_INCLUDE_FLAG "/I ${CMAKE_SOURCE_DIR}")
set_source_files_properties(${CMAKE_SOURCE_DIR}/app.rc PROPERTIES
    COMPILE_FLAGS "${RC_INCLUDE_FLAG}"
)

target_include_directories(DS4SpotifyController PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${SDL2_INCLUDE_DIRS}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# SDL2::SDL2-static is the static target; fall back to SDL2_LIBRARIES if not available
if(TARGET SDL2::SDL2-static)
    target_link_libraries(DS4SpotifyController PRIVATE SDL2::SDL2-static)
else()
    target_link_libraries(DS4SpotifyController PRIVATE ${SDL2_LIBRARIES})
endif()

if(MSVC)
    # Use static CRT to match SDL2-static
    set_property(TARGET DS4SpotifyController PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_target_properties(DS4SpotifyController PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /NODEFAULTLIB:MSVCRT"
    )
endif()
