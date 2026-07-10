cmake_minimum_required(VERSION 3.16)

project(videoplayer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(Threads REQUIRED)

qt_standard_project_setup()

qt_add_executable(videoplayer
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    demuxer.h demuxer.cpp
    decoder.h decoder.cpp
    audiooutput.h audiooutput.cpp
    playbackcontroller.h playbackcontroller.cpp
    videorendererbase.h videorendererbase.cpp
    softwarerenderer.h softwarerenderer.cpp
    d3d11context.h d3d11context.cpp
    d3d11renderer.h d3d11renderer.cpp
    renderscheduler.h renderscheduler.cpp
    avsync.h avsync.cpp
    networkoptions.h
    app_logger.h app_logger.cpp
    PacketQueue.h
    FrameQueue.h
)

set(FFMPEG_DIR "D:/ffmpeg")
set(SDL2_DIR "D:/SDL2-devel-2.30.5-mingw/SDL2-2.30.5/x86_64-w64-mingw32")
set(SPDLOG_DIR "${CMAKE_SOURCE_DIR}/thirdParty/spdlog")

add_library(spdlog::spdlog STATIC IMPORTED)
set_target_properties(spdlog::spdlog PROPERTIES
    IMPORTED_LOCATION "${SPDLOG_DIR}/lib/libspdlog.a"
    INTERFACE_INCLUDE_DIRECTORIES "${SPDLOG_DIR}/include"
    INTERFACE_LINK_LIBRARIES Threads::Threads
)

target_compile_definitions(videoplayer PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)
target_include_directories(videoplayer PRIVATE "${FFMPEG_DIR}/include" "${SDL2_DIR}/include")
target_link_directories(videoplayer PRIVATE "${FFMPEG_DIR}/lib" "${SDL2_DIR}/lib")
target_link_libraries(videoplayer PRIVATE Qt6::Widgets spdlog::spdlog avformat
    avcodec
    avfilter
    avutil
    swscale
    swresample
    SDL2main
    SDL2
    d3d11
    dxgi
    d3dcompiler)
