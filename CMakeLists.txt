cmake_minimum_required(VERSION 3.15)
project(gst_webrtc_receive_appsink CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 gstreamer-video-1.0)

# OpenCV (for imshow)
find_package(OpenCV REQUIRED)
find_package(Qt5 COMPONENTS Widgets REQUIRED)

add_executable(webrtc_receiver src/webrtc_receiver.cpp)

target_include_directories(webrtc_receiver PRIVATE ${GST_INCLUDE_DIRS})
target_link_directories(webrtc_receiver PRIVATE ${GST_LIBRARY_DIRS})
# 일부 환경에서 필요할 수 있음
# add_definitions(${GST_CFLAGS_OTHER})

# PkgConfig가 제공하는 링크 플래그 적용
foreach(flag ${GST_LDFLAGS})
  set_property(TARGET webrtc_receiver APPEND_STRING PROPERTY LINK_FLAGS " ${flag}")
endforeach()

# 정적/동적 링크 라이브러리들
# modern CMake에선 target_link_libraries에 ${GST_LIBRARIES}를 그대로 넘겨도 동작
# (플랫폼별 경로가 잡힌 상태)
target_link_libraries(webrtc_receiver PRIVATE ${GST_LIBRARIES} ${OpenCV_LIBS} Qt5::Widgets)

# Windows에서 GStreamer MSI 설치 시 hint가 필요할 수 있음
# set(CMAKE_PREFIX_PATH "C:/gstreamer/1.0/msvc_x86_64")
