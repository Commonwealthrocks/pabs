## CMakeLists.txt
## last updated: 26/05/2026
## cmake -G "Ninja" ..                               > (dynamic, df)
## [or] cmake -G "Ninja" -DPABS_STATIC_BUILD=ON ..   > (static build)
## ninja
cmake_minimum_required(VERSION 3.20)
project(pabs VERSION 0.2 LANGUAGES CXX RC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})
option(PABS_STATIC_BUILD "link static build" OFF)
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/assets/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
set(PABS_SOURCES
    main.cpp
    .cpp/error_handler.cpp
    .cpp/logger.cpp
    .cpp/__drives.cpp
    .cpp/__image.cpp
    .cpp/__imapi_com.cpp
    .cpp/where_how_why_ffmpeg.cpp
    .cpp/__burn.cpp
    .cpp/__build.cpp
    .cpp/__erase.cpp
    .cpp/__verify.cpp
    .cpp/__read.cpp
    .cpp/__audio_h.cpp
    .cpp/9660_parser.cpp
    .cpp/joliet_parser.cpp
    .cpp/udf_parser.cpp
    .cpp/config.cpp
    .cpp/ui.cpp
    .cpp/log_window.cpp
    .cpp/outs.cpp
    assets/__cache/no/ic_h.rc
)
add_executable(pabs WIN32 ${PABS_SOURCES} ${IMGUI_SOURCES})
target_include_directories(pabs PRIVATE
    ${CMAKE_SOURCE_DIR}/.hpp
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_SOURCE_DIR}/assets/imgui
    ${CMAKE_SOURCE_DIR}/assets/imgui/backends
)
target_compile_options(pabs PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -O2
)
if(PABS_STATIC_BUILD)
    message(STATUS "static")
    target_link_options(pabs PRIVATE -static -static-libgcc -static-libstdc++)
else()
    message(STATUS "dynamic (df)")
endif()
target_link_libraries(pabs PRIVATE
    ntdll
    setupapi
    d3d11
    dxgi
    dwmapi
    d3dcompiler
    shlwapi
    ole32
    oleaut32
    uuid
    shell32
    winmm
)
message(STATUS "PABS v${PROJECT_VERSION} - build type: ${CMAKE_BUILD_TYPE}  static: ${PABS_STATIC_BUILD}")
message(STATUS "Toolchain: ${CMAKE_CXX_COMPILER}")

## end