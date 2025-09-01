# WebRTC Receiver Thread Context Adjustment

## Context
GStreamer 메인 루프 `g_loop`는 메인 스레드에서 생성되지만 `g_main_loop_run`은 별도의 스레드에서 실행되고 있다. 이때 GLib은 해당 스레드에 메인 컨텍스트가 기본으로 설정되지 않으면 경고를 출력한다.

## Problem
현재 구조에서는 `g_main_loop_run`이 기본 컨텍스트 없이 실행되어, 실행 시 `g_main_context_push_thread_default()` 관련 경고가 발생한다.

## Goal
`g_loop`와 `g_main_loop_run`이 동일한 컨텍스트를 사용하도록 하여 경고 없이 안정적으로 동작하게 한다.

## Non-Goals
- 파이프라인 구성이나 UI 로직 변경
- 성능 최적화 또는 새로운 기능 추가

## Constraints
- 전체 구조를 크게 변경하지 않고 최소 수정만 수행한다.
- OpenCV 및 GStreamer 의존성은 기존대로 유지한다.

## Options
1. 메인 스레드에서 `g_main_loop_run`을 실행하고 UI를 별도 스레드로 이동한다.
   - 장점: GLib 컨텍스트 경고가 사라짐.
   - 단점: UI 스레드 관리가 추가되고 구조 변화가 큼.
   - 위험: 스레드 간 동기화 복잡도 증가.
2. 메인 루프를 실행하는 스레드에서 `g_main_context_push_thread_default`/`pop` 호출.
   - 장점: 최소 변경으로 경고 해결.
   - 단점: 컨텍스트를 직접 관리해야 함.
   - 위험: push/pop 짝이 맞지 않으면 다른 경고 발생.

## Decision
옵션 1을 선택하여 `g_main_loop_run`을 메인 스레드에서 실행하고 UI는 별도 스레드로 분리한다.
