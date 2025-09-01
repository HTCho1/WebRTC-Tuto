# Problem 1-Pager: WebRTC 수신기 UI 메인 스레드 이동

## Context
- `webrtc_receiver`는 GStreamer의 `g_main_loop_run`을 메인 스레드에서 실행하고,
  OpenCV GUI (`cv::imshow`)를 별도 스레드에서 처리하고 있음.
- Qt 기반 OpenCV HighGUI는 GUI 호출이 메인 스레드에서만 안전함.

## Problem
- 별도 UI 스레드에서 `cv::imshow`를 호출하면 창은 뜨지만 프레임이 갱신되지 않고
  GLib 컨텍스트 경고가 발생함.

## Goal
- `cv::namedWindow`, `cv::imshow`, `cv::waitKey`를 메인 스레드에서 실행한다.
- `g_main_loop_run`은 별도 스레드에서 실행하고 해당 스레드에서
  `g_main_context_push_thread_default` / `g_main_context_pop_thread_default`를 호출한다.

## Non-Goals
- 미디어 파이프라인 로직이나 SDP 처리 방식 변경
- 대규모 리팩터링 및 성능 최적화

## Constraints
- 기존 뮤텍스 기반 프레임 공유 구조 유지
- Ubuntu 24.04 환경에서 Qt/GLib 호환성을 유지해야 한다.

## Options
1. UI를 메인 스레드로 이동하고 GStreamer 루프를 별도 스레드에서 실행
   - 장점: HighGUI와 GLib 모두 정상 동작
   - 단점: 스레드 관리 복잡성 증가
   - 위험: 컨텍스트 push/pop 누락 시 경고 발생 가능
2. 현 구조 유지(메인 스레드에서 GStreamer, UI는 별도 스레드)
   - 장점: 변경 없음
   - 단점: 영상 출력 불가, 경고 지속
   - 위험: 사용자 경험 저하

## Decision
옵션 1을 채택하여 UI를 메인 스레드로 이동하고 GStreamer 메인 루프는
별도 스레드에서 실행한다.
