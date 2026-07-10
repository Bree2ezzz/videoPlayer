cmake_minimum_required(VERSION 3.16)

project(videoplayer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets)

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
    renderscheduler.h renderscheduler.cpp
    openglrenderer.h openglrenderer.cpp
    avsync.h avsync.cpp
    logging.h logging.cpp
    networkoptions.h
    PacketQueue.h
    FrameQueue.h
)

set(FFMPEG_DIR "D:/ffmpeg")
set(SDL2_DIR "D:/SDL2-devel-2.30.5-mingw/SDL2-2.30.5/x86_64-w64-mingw32")
target_include_directories(videoplayer PRIVATE "${FFMPEG_DIR}/include" "${SDL2_DIR}/include")
target_link_directories(videoplayer PRIVATE "${FFMPEG_DIR}/lib" "${SDL2_DIR}/lib")
target_link_libraries(videoplayer PRIVATE Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets avformat
    avcodec
    avfilter
    avutil
    swscale
    swresample
    SDL2main
    SDL2)
qt_finalize_executable(videoplayer)
