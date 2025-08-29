# Problem 1-Pager: Qt 이벤트 루프 초기화

## Context
GStreamer와 OpenCV를 이용해 WebRTC 수신을 수행하며, OpenCV의 `imshow`로 영상을 표시합니다. 실행 시 Qt 타이머 관련 경고가 발생하며 창 표시가 불안정합니다.

## Problem
OpenCV의 기본 HighGUI가 Qt 이벤트 루프 없이 사용되어 타이머 경고가 출력되고 영상이 제대로 표시되지 않습니다.

## Goal
- `QApplication`을 사용해 메인 스레드를 Qt가 관리하도록 하고, `cv::startWindowThread()` 호출로 Qt 이벤트 루프를 초기화합니다.
- CMake에서 Qt 라이브러리를 링크하여 빌드 오류를 방지합니다.

## Non-Goals
- `main` 함수의 구조 개선이나 다른 모듈의 리팩터링은 이번 범위에 포함되지 않습니다.

## Constraints
- `main` 함수는 이미 50라인을 초과하지만, 변경 최소화를 위해 별도 리팩터링은 진행하지 않습니다.
- Qt5 사용을 가정합니다.
