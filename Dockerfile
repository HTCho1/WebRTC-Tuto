# =================================================================
# Stage 1: Builder
# - 빌드에 필요한 모든 도구(-dev 패키지 포함)를 설치합니다.
# - 소스 코드를 컴파일하여 실행 파일을 만듭니다.
# =================================================================
FROM ubuntu:20.04 AS builder

# apt 같은 패키지 설치 시 대화형 프롬프트가 뜨는 것을 방지합니다.
ENV DEBIAN_FRONTEND=noninteractive

# 빌드에 필요한 모든 도구와 개발용 라이브러리를 설치합니다.
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    libglib2.0-dev \
    libopencv-dev \
    libnice-dev # libnice 개발용 라이브러리 추가

# 작업 디렉토리를 설정합니다.
WORKDIR /app

# 프로젝트의 모든 파일을 빌더 스테이지의 /app 디렉토리로 복사합니다.
# (.dockerignore 파일에 명시된 파일 및 폴더는 제외됩니다.)
COPY . .

# 빌드 디렉토리를 생성하고 CMake를 실행하여 코드를 빌드합니다.
RUN cmake -S . -B build && cmake --build build


# =================================================================
# Stage 2: Final Image
# - 깨끗한 우분투 이미지에서 시작합니다.
# - 실행에 필요한 최소한의 런타임 라이브러리만 설치합니다.
# - 빌더 스테이지에서 컴파일된 실행 파일만 복사해옵니다.
# =================================================================
FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

# 실행에 필요한 최소한의 런타임 라이브러리들을 설치합니다.
# (-dev 접미사가 없는 패키지들입니다.)
RUN apt-get update && apt-get install -y \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-tools \
    gstreamer1.0-nice \
    libopencv-core4.2 \
    libopencv-highgui4.2 \
    libopencv-imgproc4.2 \
    libopencv-videoio4.2 \
    # 이미지 용량을 줄이기 위해 apt 캐시를 정리합니다.
    && rm -rf /var/lib/apt/lists/*

# 작업 디렉토리를 설정합니다.
WORKDIR /app

# 빌더 스테이지의 /app/build/ 디렉토리에서 실행 파일만 복사해옵니다.
COPY --from=builder /app/build/webrtc_receiver .

# web/sender.html 파일도 최종 이미지에 복사 (필요 시)
COPY web/sender.html ./web/

# 컨테이너가 시작될 때 실행될 기본 명령을 설정합니다.
# CMD ["./webrtc_receiver"]
