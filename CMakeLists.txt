cmake_minimum_required(VERSION 4.0)
project(oswald_gis LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Downloaded dependencies still declare policy versions deprecated by CMake 4.
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM OR CMAKE_POLICY_VERSION_MINIMUM VERSION_LESS 3.10)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
endif()

include(FetchContent)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(
    glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG v2.0.8
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR cmake
)
FetchContent_MakeAvailable(glad)
glad_add_library(glad_gl_core_45 STATIC REPRODUCIBLE LOADER API gl:core=4.5)

FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glm)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC glfw)

FetchContent_Declare(
    qhull
    GIT_REPOSITORY https://github.com/qhull/qhull.git
    GIT_TAG v8.0.2
    GIT_SHALLOW TRUE
)
function(oswald_fetch_qhull)
    FetchContent_MakeAvailable(qhull)
    # Only build linked Qhull targets, not its tools or pre-C++20 C++ wrapper.
    set_property(DIRECTORY "${qhull_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
    target_include_directories(qhullstatic_r INTERFACE "$<BUILD_INTERFACE:${qhull_SOURCE_DIR}/src>")
endfunction()
oswald_fetch_qhull()

set(NANOFLANN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NANOFLANN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# Qhull owns the uninstall target; nanoflann attaches its own cleanup to it.
set(MASTER_PROJECT_HAS_TARGET_UNINSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    nanoflann
    GIT_REPOSITORY https://github.com/jlblancoc/nanoflann.git
    GIT_TAG v1.5.5
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nanoflann)

FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 4e9b368bfc071e97206ea5c6adba68dd0cbf8897
)
FetchContent_MakeAvailable(stb)

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
find_package(TIFF REQUIRED)

add_library(oswald_model STATIC src/Layer.cpp src/ColorMap.cpp src/SphericalGrid.cpp src/ProjectFile.cpp src/ElevationRaster.cpp src/BrushTool.cpp
    src/SelectionTool.cpp src/Filter.cpp)
target_include_directories(oswald_model PUBLIC include)
target_include_directories(oswald_model PRIVATE "${stb_SOURCE_DIR}")
target_link_libraries(oswald_model PUBLIC glm::glm nanoflann::nanoflann PRIVATE qhullstatic_r TIFF::TIFF)

add_library(oswald_controller STATIC src/CameraController.cpp)
target_include_directories(oswald_controller PUBLIC include)
target_link_libraries(oswald_controller PUBLIC glm::glm)

add_library(oswald_view STATIC src/GlobeRenderer.cpp)
target_include_directories(oswald_view PUBLIC include)
target_link_libraries(oswald_view PUBLIC oswald_model glad_gl_core_45)

set(OSWALD_RESOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/resources")
foreach(resource shaders/globe.vert shaders/globe.frag shaders/wire.vert shaders/wire.frag shaders/elevation.comp)
    configure_file("${resource}" "${OSWALD_RESOURCE_DIR}/${resource}" COPYONLY)
endforeach()
file(GLOB OSWALD_COLORMAP_FILES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/assets/colormaps/*.map")
foreach(colormap IN LISTS OSWALD_COLORMAP_FILES)
    get_filename_component(colormap_name "${colormap}" NAME)
    configure_file("${colormap}" "${OSWALD_RESOURCE_DIR}/assets/colormaps/${colormap_name}"
                   COPYONLY)
endforeach()

add_executable(oswald_gis src/main.cpp src/UIManager.cpp src/RasterFileBrowser.cpp)
target_compile_definitions(oswald_gis PRIVATE
    GLFW_INCLUDE_NONE
    OSWALD_RESOURCE_DIR="${OSWALD_RESOURCE_DIR}"
)
target_link_libraries(oswald_gis PRIVATE
    oswald_view
    oswald_model
    oswald_controller
    glfw
    glad_gl_core_45
    glm::glm
    imgui_lib
    nanoflann::nanoflann
    OpenGL::GL
    Threads::Threads
)

include(CTest)
if(BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_executable(spherical_grid_tests tests/SphericalGridTests.cpp)
    target_link_libraries(spherical_grid_tests PRIVATE oswald_model Threads::Threads)
    add_test(NAME spherical_grid COMMAND spherical_grid_tests)
    add_test(NAME spherical_grid_scale COMMAND spherical_grid_tests --scale)
    set_tests_properties(spherical_grid_scale PROPERTIES LABELS scale TIMEOUT 180)

    add_executable(camera_controller_tests tests/CameraControllerTests.cpp)
    target_link_libraries(camera_controller_tests PRIVATE oswald_controller)
    add_test(NAME camera_controller COMMAND camera_controller_tests)

    add_executable(elevation_raster_tests tests/ElevationRasterTests.cpp)
    target_include_directories(elevation_raster_tests PRIVATE "${stb_SOURCE_DIR}")
    target_link_libraries(elevation_raster_tests PRIVATE oswald_model TIFF::TIFF)
    add_test(NAME elevation_raster COMMAND elevation_raster_tests)

    add_executable(brush_tool_tests tests/BrushToolTests.cpp)
    target_link_libraries(brush_tool_tests PRIVATE oswald_model)
    add_test(NAME brush_tool COMMAND brush_tool_tests)

    add_executable(selection_tool_tests tests/SelectionToolTests.cpp)
    target_link_libraries(selection_tool_tests PRIVATE oswald_model)
    add_test(NAME selection_tool COMMAND selection_tool_tests)

    add_executable(filter_tests tests/FilterTests.cpp)
    target_link_libraries(filter_tests PRIVATE oswald_model)
    add_test(NAME filter COMMAND filter_tests)

    add_executable(layer_tests tests/LayerTests.cpp)
    target_link_libraries(layer_tests PRIVATE oswald_model)
    add_test(NAME layer COMMAND layer_tests)

    add_executable(project_file_tests tests/ProjectFileTests.cpp)
    target_link_libraries(project_file_tests PRIVATE oswald_model)
    add_test(NAME project_file COMMAND project_file_tests)

    add_executable(raster_file_browser_tests tests/RasterFileBrowserTests.cpp src/RasterFileBrowser.cpp)
    target_include_directories(raster_file_browser_tests PRIVATE include)
    add_test(NAME raster_file_browser COMMAND raster_file_browser_tests)

    add_executable(undo_stack_tests tests/UndoStackTests.cpp)
    target_include_directories(undo_stack_tests PRIVATE include)
    add_test(NAME undo_stack COMMAND undo_stack_tests)

    add_executable(ui_manager_tests tests/UIManagerTests.cpp src/UIManager.cpp src/RasterFileBrowser.cpp)
    target_link_libraries(ui_manager_tests PRIVATE oswald_view oswald_controller imgui_lib glfw Threads::Threads)
    target_compile_definitions(ui_manager_tests PRIVATE
        GLFW_INCLUDE_NONE
        OSWALD_RESOURCE_DIR="${OSWALD_RESOURCE_DIR}"
    )
    add_test(NAME raster_file_dialog COMMAND ui_manager_tests)
    set_tests_properties(raster_file_dialog PROPERTIES LABELS gpu TIMEOUT 60)

    add_executable(globe_renderer_tests tests/GlobeRendererTests.cpp)
    target_link_libraries(globe_renderer_tests PRIVATE oswald_view glfw)
    target_compile_definitions(globe_renderer_tests PRIVATE
        GLFW_INCLUDE_NONE
        OSWALD_RESOURCE_DIR="${OSWALD_RESOURCE_DIR}"
    )
    add_test(NAME color_map COMMAND globe_renderer_tests)
    add_test(NAME globe_renderer COMMAND globe_renderer_tests --gpu)
    set_tests_properties(globe_renderer PROPERTIES LABELS gpu TIMEOUT 60)
endif()
