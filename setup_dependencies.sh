#!/bin/bash

# ==============================================================================
# RTSP YOLO Project - 종속성 설치 스크립트 for Ubuntu 20.04/22.04
# ==============================================================================

set -e  # 오류 발생 시 즉시 스크립트 중단
pip install tk
# --- 1. 시스템 패키지 업데이트 ---
# echo "[+] 1/4: 시스템 패키지 목록 업데이트 중..."
sudo apt update

# --- 2. 필수 라이브러리 설치 ---
echo "[+] 2/4: GStreamer, ZeroMQ, OpenCV 등 필수 라이브러리 설치 중..."
sudo apt install -y \
    build-essential \
    cmake \
    pkg-config \
    git \
    git-lfs \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    gstreamer1.0-tools \
    libzmq3-dev \
    libopencv-dev

# --- 3. Git LFS 설정 ---
echo "[+] 3/4: Git LFS 설정 및 대용량 파일 다운로드 중..."
git lfs install
git lfs pull

# --- 4. 설치 완료 ---
echo "[+] 4/4: 모든 종속성 설치가 완료되었습니다!"
echo ""
echo "이제 다음 단계에 따라 프로젝트를 빌드하고 실행하세요:"
echo "1. mkdir build && cd build"
echo "2. cmake .."
echo "3. make"
