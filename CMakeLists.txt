cmake_minimum_required(VERSION 3.20)
project(ReplayKeeper C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 11)

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

set(PATCH_DOWNLOADER_CORE_SOURCES)
set(PATCH_DOWNLOADER_CORE_AVAILABLE OFF)

set(MANIFEST_DIR ${CMAKE_SOURCE_DIR}/manifest)
if(EXISTS "${MANIFEST_DIR}/rman.c" AND
   EXISTS "${MANIFEST_DIR}/zstd/lib/zstd.h" AND
   EXISTS "${MANIFEST_DIR}/BLAKE3/c/blake3.h")
    file(GLOB ZSTD_COMMON_SOURCES
        "${MANIFEST_DIR}/zstd/lib/common/*.c"
    )
    file(GLOB ZSTD_DECOMPRESS_SOURCES
        "${MANIFEST_DIR}/zstd/lib/decompress/*.c"
    )

    set(PATCH_DOWNLOADER_CORE_SOURCES
        ${MANIFEST_DIR}/rman.c
        ${MANIFEST_DIR}/sha/sha256.c
        ${MANIFEST_DIR}/sha/sha256-x86.c
        ${MANIFEST_DIR}/BLAKE3/c/blake3.c
        ${MANIFEST_DIR}/BLAKE3/c/blake3_dispatch.c
        ${MANIFEST_DIR}/BLAKE3/c/blake3_portable.c
        ${ZSTD_COMMON_SOURCES}
        ${ZSTD_DECOMPRESS_SOURCES}
    )
    set(PATCH_DOWNLOADER_CORE_AVAILABLE ON)
else()
    message(WARNING "Vendored patch downloader dependencies are missing; Patch Downloader UI will build, but full patch downloads are disabled.")
endif()

add_executable(ReplayKeeper WIN32
    main.cpp
    gui.cpp
    gui.hpp
    manifest_catalog.cpp
    manifest_catalog.hpp
    patch_downloader.cpp
    patch_downloader.hpp
    image_resources.cpp
    image_resources.hpp
    lcu.cpp
    lcu.hpp
    ${IMGUI_SOURCES}
    ${PATCH_DOWNLOADER_CORE_SOURCES}
)

target_include_directories(ReplayKeeper PRIVATE
    ${CMAKE_SOURCE_DIR}/imgui
    ${MANIFEST_DIR}
)

if(PATCH_DOWNLOADER_CORE_AVAILABLE)
    target_include_directories(ReplayKeeper PRIVATE
        ${MANIFEST_DIR}/zstd/lib
        ${MANIFEST_DIR}/BLAKE3/c
    )
    target_compile_definitions(ReplayKeeper PRIVATE
        REPLAYKEEPER_PATCH_DOWNLOAD_CORE=1
        BLAKE3_NO_SSE2
        BLAKE3_NO_SSE41
        BLAKE3_NO_AVX2
        BLAKE3_NO_AVX512
        ZSTD_DISABLE_ASM
        DYNAMIC_BMI2=0
    )
    set_source_files_properties(${MANIFEST_DIR}/sha/sha256-x86.c
        PROPERTIES COMPILE_OPTIONS "-msha;-msse4.1"
    )
else()
    target_compile_definitions(ReplayKeeper PRIVATE REPLAYKEEPER_PATCH_DOWNLOAD_CORE=0)
endif()

target_link_libraries(ReplayKeeper PRIVATE
    d3d11
    dxgi
    d3dcompiler
    dwmapi
    winhttp
    shell32
    ole32
    windowscodecs
    ntdll
)

option(REPLAYKEEPER_BUILD_TESTS "Build ReplayKeeper helper tests" OFF)
if(REPLAYKEEPER_BUILD_TESTS)
    add_executable(manifest_catalog_tests
        tests/manifest_catalog_tests.cpp
        manifest_catalog.cpp
    )
    target_link_libraries(manifest_catalog_tests PRIVATE winhttp)

    add_executable(patch_downloader_tests
        tests/patch_downloader_tests.cpp
        patch_downloader.cpp
        ${PATCH_DOWNLOADER_CORE_SOURCES}
    )
    target_include_directories(patch_downloader_tests PRIVATE
        ${MANIFEST_DIR}
    )
    if(PATCH_DOWNLOADER_CORE_AVAILABLE)
        target_include_directories(patch_downloader_tests PRIVATE
            ${MANIFEST_DIR}/zstd/lib
            ${MANIFEST_DIR}/BLAKE3/c
        )
        target_compile_definitions(patch_downloader_tests PRIVATE
            REPLAYKEEPER_PATCH_DOWNLOAD_CORE=1
            BLAKE3_NO_SSE2
            BLAKE3_NO_SSE41
            BLAKE3_NO_AVX2
            BLAKE3_NO_AVX512
            ZSTD_DISABLE_ASM
            DYNAMIC_BMI2=0
        )
        set_source_files_properties(${MANIFEST_DIR}/sha/sha256-x86.c
            PROPERTIES COMPILE_OPTIONS "-msha;-msse4.1"
        )
    else()
        target_compile_definitions(patch_downloader_tests PRIVATE REPLAYKEEPER_PATCH_DOWNLOAD_CORE=0)
    endif()
    target_link_libraries(patch_downloader_tests PRIVATE winhttp)

    add_executable(patch_catalog_ui_tests
        tests/patch_catalog_ui_tests.cpp
    )
endif()
