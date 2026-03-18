cmake_minimum_required(VERSION 3.20)
project(ReplayKeeper)

set(CMAKE_CXX_STANDARD 20)

add_definitions(-DUNICODE -D_UNICODE)

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")

set(IMGUI_SOURCES
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_impl_dx11.cpp
    imgui/imgui_impl_win32.cpp
)


add_executable(ReplayKeeper WIN32
    main.cpp
    gui.cpp
    gui.hpp
    lcu.cpp
    lcu.hpp
    ${IMGUI_SOURCES}
)


target_include_directories(ReplayKeeper PRIVATE ${CMAKE_SOURCE_DIR}/imgui)


target_link_libraries(ReplayKeeper PRIVATE
    d3d11
    dxgi
    d3dcompiler
    dwmapi
    winhttp
    shell32
    ntdll
)