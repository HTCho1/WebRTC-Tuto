# Problem 1-Pager: OpenCV GUI 메인 스레드 이동

## Context
- `webrtc_receiver`는 GStreamer의 `g_main_loop_run`을 메인 스레드에서 실행하고,
  OpenCV GUI (`cv::imshow`)를 별도 스레드에서 처리하고 있음.
- OpenCV가 Qt 백엔드를 사용할 때 GUI 호출은 Qt 메인 스레드(`QThread`)에서만 안전함.

## Problem
- GUI를 일반 스레드에서 호출하여 `QObject::startTimer` 관련 경고가 발생하고
  영상 창이 뜨지 않는 문제가 있음.

## Goal
- `cv::namedWindow`, `cv::imshow`, `cv::waitKey`를 메인 스레드에서 실행.
- `g_main_loop_run`은 별도 스레드로 이동하여 동시 실행.

## Non-Goals
- 미디어 파이프라인/SDP 로직 수정 없음.
- 전체 파일 리팩터링은 범위에서 제외.

## Constraints
- 프레임 공유는 기존 mutex 구조 유지.
- UI 종료 시 GStreamer 루프 안전 종료 필요.
