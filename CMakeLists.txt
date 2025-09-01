cmake_minimum_required(VERSION 3.16)
project(gst_webrtc_receive_appsink CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# pkg-config를 사용하여 GStreamer 및 관련 라이브러리를 찾습니다.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-sdp-1.0 gstreamer-webrtc-1.0)

# find_package를 사용하여 OpenCV 라이브러리를 찾습니다.
find_package(OpenCV REQUIRED COMPONENTS core highgui imgproc videoio)

# 시스템 쓰레드 라이브러리를 찾습니다.
find_package(Threads REQUIRED)

# 실행 파일을 정의합니다.
add_executable(webrtc_receiver src/webrtc_receiver.cpp)

# 찾은 라이브러리들의 헤더 경로를 타겟에 추가합니다.
target_include_directories(webrtc_receiver PRIVATE
    ${GSTREAMER_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
)

# 찾은 라이브러리들을 타겟에 링크합니다.
target_link_libraries(webrtc_receiver PRIVATE
    ${GSTREAMER_LIBRARIES}
    ${OpenCV_LIBRARIES}
    Threads::Threads
)