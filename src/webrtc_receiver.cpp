#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <gst/app/gstappsink.h>
#include <gst/sdp/gstsdpmessage.h>
#include <gst/video/video.h>

#include <opencv2/opencv.hpp>
// #include <QApplication>

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <sstream>

// GStreamer 메인 루프와 주요 요소들을 전역으로 보관한다.
static GMainLoop* g_loop = nullptr;           // 이벤트 처리용 GLib 메인 루프
static GstElement* g_pipeline = nullptr;      // 전체 GStreamer 파이프라인
static GstElement* g_webrtc = nullptr;        // WebRTC 신호 교환을 담당하는 webrtcbin
static GstElement* g_appsink = nullptr;       // 디코딩된 RAW 프레임이 도달하는 앱 싱크

// UI 표시용 공유 프레임 버퍼
// UI 스레드와 GStreamer 스레드가 프레임을 주고받기 위한 공유 자원
static std::mutex g_frame_mutex;              // 프레임 보호용 뮤텍스
static cv::Mat g_latest_frame;                // 가장 최근에 수신한 프레임
static std::atomic<bool> g_running{true};     // 종료 플래그

// 브라우저에서 전달된 SDP ANSWER가 설정되었는지 여부
static std::atomic<bool> g_answer_set{false};

// 브라우저가 이해하지 못하는 SDP 라인을 제거하여 호환성을 높인다.
static std::string sanitize_sdp_for_browser(const char* sdp_in) {
    std::string in = sdp_in ? sdp_in : "";
    in.erase(std::remove(in.begin(), in.end(), '\r'), in.end()); // CR 제거
    std::stringstream iss(in);
    std::string line;
    std::ostringstream oss;
    while (std::getline(iss, line)) {
        // 각 줄의 앞뒤 공백을 정리한다.
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t p = 0; while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
        std::string t = (p > 0) ? line.substr(p) : line;

        // 가독성용 헤더/푸터나 브라우저가 거부하는 속성은 제거한다.
        if (t.rfind("=====", 0) == 0) continue;              // README 스타일 구분선
        if (t.rfind("a=end-of-candidates", 0) == 0) continue; // ICE 후보 종료 표시 제거
        if (t.rfind("a=rtcp-mux-only", 0) == 0) continue;     // 오래된 속성

        if (t.rfind("a=candidate:", 0) == 0) {
            std::string low = t;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c){ return std::tolower(c); });
            // TCP 기반 후보는 브라우저에서 처리하지 못하는 경우가 있어 제거한다.
            if (low.find(" tcp ") != std::string::npos || low.find("\ttcp ") != std::string::npos || low.find(" tcptype ") != std::string::npos) {
                continue;
            }
            // IPv6 주소 후보는 일부 환경에서 파싱 문제가 발생할 수 있어 제외한다.
            std::vector<std::string> tokens;
            {
                std::istringstream ts(t);
                std::string tok;
                while (ts >> tok) tokens.push_back(tok);
            }
            if (tokens.size() >= 6) {
                const std::string& addr = tokens[4];
                if (addr.find(':') != std::string::npos) {
                    continue; // IPv6 주소
                }
            }
        }

        // RFC 4566에 맞춰 CRLF로 줄바꿈을 맞춘다.
        oss << t << "\r\n";
    }
    return oss.str();
}

// 터미널에서 사용자가 붙여넣은 SDP를 끝 마커까지 읽어온다.
static std::string read_sdp_from_stdin() {
    std::string line, sdp;
    while (std::getline(std::cin, line)) {
        if (line.find("===== END SDP =====") != std::string::npos) break; // 끝 표시를 만나면 종료
        sdp += line + "\n"; // 줄 단위로 누적
    }
    return sdp;
}

// 메인 루프 컨텍스트에서 실행되어 브라우저로부터 받은 ANSWER SDP를 webrtcbin에 설정한다.
static gboolean on_answer_received(gpointer data) {
    char* answer_sdp_str = static_cast<char*>(data);
    if (!answer_sdp_str || *answer_sdp_str == '\0') {
        g_printerr("Empty ANSWER received in main thread.\n");
        g_free(answer_sdp_str);
        return G_SOURCE_REMOVE;
    }

    g_print("[main thread] Received answer SDP to parse.\n");

    GstSDPMessage* sdp = nullptr;
    if (gst_sdp_message_new(&sdp) != GST_SDP_OK) {
        g_printerr("Failed to create SDP message object.\n");
        g_free(answer_sdp_str);
        return G_SOURCE_REMOVE;
    }

    if (gst_sdp_message_parse_buffer(reinterpret_cast<const guint8*>(answer_sdp_str),
                                     static_cast<guint>(strlen(answer_sdp_str)), sdp) != GST_SDP_OK) {
        g_printerr("Failed to parse ANSWER SDP.\n");
        gst_sdp_message_free(sdp);
        g_free(answer_sdp_str);
        return G_SOURCE_REMOVE;
    }

    GstWebRTCSessionDescription* remote_desc =
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
    if (!remote_desc) {
        g_printerr("Failed to create remote description object.\n");
        gst_sdp_message_free(sdp); // sdp는 remote_desc가 소유권을 가져가지 않으므로 항상 해제
        g_free(answer_sdp_str);
        return G_SOURCE_REMOVE;
    }

    g_print("[main thread] Setting remote description...\n");
    g_signal_emit_by_name(g_webrtc, "set-remote-description", remote_desc, nullptr);
    gst_webrtc_session_description_free(remote_desc);

    g_answer_set = true;
    g_print("[main thread] Remote description set successfully.\n");

    g_free(answer_sdp_str); // 문자열 메모리 해제
    return G_SOURCE_REMOVE; // 이 함수는 한 번만 실행되므로 핸들러 제거
}


// ===== appsink 콜백: RAW 프레임 수신 =====
// appsink에서 새로운 프레임을 가져와 OpenCV 포맷으로 변환한다.
static GstFlowReturn on_new_sample(GstElement* sink, gpointer /*user_data*/) {
    GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    if (!sample) return GST_FLOW_OK; // EOF 또는 오류 시 nullptr

    GstCaps* caps = gst_sample_get_caps(sample);
    if (!caps) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstVideoInfo vinfo;
    if (!gst_video_info_from_caps(&vinfo, caps)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstVideoFrame vframe;
    // GstBuffer를 읽기 전용으로 매핑하여 영상 메타데이터를 얻는다.
    if (gst_video_frame_map(&vframe, &vinfo, buffer, GST_MAP_READ)) {
        int width  = GST_VIDEO_FRAME_WIDTH(&vframe);
        int height = GST_VIDEO_FRAME_HEIGHT(&vframe);
        int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 0);
        guint8* data = reinterpret_cast<guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&vframe, 0));

        // GStreamer가 관리하는 메모리를 OpenCV 행렬로 감싸고,
        // 별도의 복사본을 만들어 공유 메모리 문제를 방지한다.
        cv::Mat view(height, width, CV_8UC3, data, stride);
        cv::Mat copy;
        view.copyTo(copy);
        {
            std::lock_guard<std::mutex> lock(g_frame_mutex);
            g_latest_frame = std::move(copy); // 최신 프레임 갱신
        }

        // 60 프레임마다 디버그 정보를 출력한다.
        static int counter = 0;
        if ((counter++ % 60) == 0) {
            auto pts_ns = GST_BUFFER_PTS(buffer);
            double ts = (pts_ns == GST_CLOCK_TIME_NONE) ? -1.0 : (double)pts_ns / GST_SECOND;
            g_print("[appsink] frame %dx%d, pts=%.3f sec\n", width, height, ts);
        }

        gst_video_frame_unmap(&vframe);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// decodebin -> videoconvert -> capsfilter(BGR) -> appsink
// decodebin이 새 패드를 노출하면 비디오 파이프라인을 구성하여 appsink로 연결한다.
static void on_decodebin_pad_added(GstElement* decodebin, GstPad* pad, gpointer /*user_data*/) {
    g_print("\n[DEBUG] on_decodebin_pad_added CALLED\n");
    GstCaps* caps = gst_pad_get_current_caps(pad);
    const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    g_print("[DEBUG] Pad added is of type: %s\n", name);

    if (g_str_has_prefix(name, "video/")) {
        g_print("[DEBUG] It's a video pad, building decode branch...\n");
        GstElement* videoconvert = gst_element_factory_make("videoconvert", nullptr);
        GstElement* videoscale   = gst_element_factory_make("videoscale", nullptr);
        GstElement* capsfilter   = gst_element_factory_make("capsfilter", nullptr);
        g_appsink                = gst_element_factory_make("appsink", "mysink");

        if (!videoconvert || !videoscale || !capsfilter || !g_appsink) {
            g_printerr("Failed to create elements for decode branch\n");
            if (caps) gst_caps_unref(caps);
            return;
        }

        // BGR 포맷으로 맞춘 후 appsink로 보낸다.
        GstCaps* rawcaps = gst_caps_from_string("video/x-raw,format=BGR");
        g_object_set(capsfilter, "caps", rawcaps, nullptr);
        gst_caps_unref(rawcaps);

        g_object_set(g_appsink,
                     "emit-signals", TRUE,
                     "sync", FALSE,
                     "max-buffers", 1,
                     "drop", TRUE,
                     nullptr);
        g_signal_connect(g_appsink, "new-sample", G_CALLBACK(on_new_sample), nullptr);

        gst_bin_add_many(GST_BIN(g_pipeline), videoconvert, videoscale, capsfilter, g_appsink, nullptr);
        if (!gst_element_link_many(videoconvert, videoscale, capsfilter, g_appsink, nullptr)) {
            g_printerr("Failed to link video branch to appsink\n");
            if (caps) gst_caps_unref(caps);
            return;
        }

        GstPad* sinkpad = gst_element_get_static_pad(videoconvert, "sink");
        if (gst_pad_link(pad, sinkpad) != GST_PAD_LINK_OK) {
            g_printerr("Failed to link decodebin to videoconvert\n");
        }
        gst_object_unref(sinkpad);

        // 새로 추가한 요소들이 상위 파이프라인과 동일한 상태를 갖도록 맞춘다.
        gst_element_sync_state_with_parent(videoconvert);
        gst_element_sync_state_with_parent(videoscale);
        gst_element_sync_state_with_parent(capsfilter);
        gst_element_sync_state_with_parent(g_appsink);
    }

    if (caps) gst_caps_unref(caps);
}

// webrtcbin이 새로운 RTP 스트림 패드를 제공하면 디코딩을 위한 큐와 decodebin을 연결한다.
static void on_incoming_stream(GstElement* /*webrtc*/, GstPad* pad, gpointer /*user_data*/) {
    g_print("\n[DEBUG] on_incoming_stream CALLED\n");
    GstCaps* caps = gst_pad_get_current_caps(pad);
    const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    g_print("[DEBUG] Incoming stream is of type: %s\n", name);

    // 비디오 RTP 스트림만 처리한다.
    if (!g_str_has_prefix(name, "application/x-rtp")) {
        if (caps) gst_caps_unref(caps);
        return;
    }

    GstElement* queue = gst_element_factory_make("queue", nullptr);
    GstElement* decodebin = gst_element_factory_make("decodebin", nullptr);
    if (!queue || !decodebin) {
        g_printerr("Failed to create queue/decodebin\n");
        if (caps) gst_caps_unref(caps);
        return;
    }

    gst_bin_add_many(GST_BIN(g_pipeline), queue, decodebin, nullptr);
    gst_element_sync_state_with_parent(queue);
    gst_element_sync_state_with_parent(decodebin);

    GstPad* qsink = gst_element_get_static_pad(queue, "sink");
    if (gst_pad_link(pad, qsink) != GST_PAD_LINK_OK) {
        g_printerr("Failed to link webrtcbin src pad to queue\n");
    }
    gst_object_unref(qsink);

    if (!gst_element_link(queue, decodebin)) {
        g_printerr("Failed to link queue -> decodebin\n");
    }

    // decodebin이 실제로 비디오 스트림을 감지하면 on_decodebin_pad_added가 호출된다.
    g_signal_connect(decodebin, "pad-added", G_CALLBACK(on_decodebin_pad_added), nullptr);

    if (caps) gst_caps_unref(caps);
}

// webrtcbin에서 OFFER가 생성되면 로컬 SDP를 설정한다.
static void on_offer_created(GstPromise* promise, gpointer /*user_data*/) {
    const GstStructure* reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription* offer = nullptr;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
    g_signal_emit_by_name(g_webrtc, "set-local-description", offer, nullptr);
    gst_webrtc_session_description_free(offer);
    gst_promise_unref(promise);
}

// 원격 피어와의 협상이 필요해지면 새로운 OFFER 생성을 요청한다.
static void on_negotiation_needed(GstElement* /*webrtc*/, gpointer /*user_data*/) {
    GstPromise* promise = gst_promise_new_with_change_func(on_offer_created, nullptr, nullptr);
    g_signal_emit_by_name(g_webrtc, "create-offer", nullptr, promise);
}

// ICE 후보 수집이 완료되면 SDP OFFER를 출력하고 사용자에게 ANSWER를 입력받는다.
static void on_notify_ice_gathering(GObject* obj, GParamSpec* /*pspec*/, gpointer /*user_data*/) {
    GstWebRTCICEGatheringState state;
    g_object_get(obj, "ice-gathering-state", &state, nullptr);
    if (state == GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) {
        GstWebRTCSessionDescription* local_desc = nullptr;
        g_object_get(obj, "local-description", &local_desc, nullptr);
        if (!local_desc) return;
        gchar* sdp_str = gst_sdp_message_as_text(local_desc->sdp);
        std::string clean = sanitize_sdp_for_browser(sdp_str);
        g_print("\n===== SDP OFFER (paste into browser) =====\n%s\n===== END SDP =====\n\n", clean.c_str());
        g_free(sdp_str);
        gst_webrtc_session_description_free(local_desc);

        GMainContext* ctx = g_main_loop_get_context(g_loop);

        // 별도 스레드에서 사용자 입력을 기다린 후 메인 루프로 전달한다.
        std::thread([=]() {
            g_print("Paste the SDP ANSWER from browser, then end with a line: '===== END SDP ====='\n");
            std::string raw_answer = read_sdp_from_stdin();
            std::string answer = sanitize_sdp_for_browser(raw_answer.c_str());
            if (answer.empty()) {
                g_printerr("Empty ANSWER received, not processing.\n");
                return;
            }
            // 정제된 SDP를 메인 루프로 전달하여 on_answer_received가 실행되게 한다.
            g_main_context_invoke(ctx, on_answer_received, g_strdup(answer.c_str()));
        }).detach();
    }
}

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv); // GStreamer 초기화
    // QApplication app(argc, argv); // Qt 기반 GUI를 사용한다면 주석 해제

    const char* stun = "stun://stun.l.google.com:19302"; // 기본 STUN 서버
    if (argc >= 2) stun = argv[1]; // 사용자 지정 STUN 서버

    // 메인 루프와 파이프라인을 초기화한다.
    GMainContext* main_ctx = g_main_context_new();
    g_loop = g_main_loop_new(main_ctx, FALSE);
    g_main_context_unref(main_ctx);

    g_pipeline = gst_pipeline_new("webrtc-recv-pipeline");
    g_webrtc = gst_element_factory_make("webrtcbin", "webrtcbin");
    if (!g_pipeline || !g_webrtc) {
        g_printerr("Failed to create pipeline/webrtcbin. Ensure gstreamer-webrtc is installed.\n");
        return -1;
    }

    // webrtcbin 설정 및 파이프라인 구성
    g_object_set(g_webrtc, "stun-server", stun, nullptr);
    gst_bin_add(GST_BIN(g_pipeline), g_webrtc);

    g_signal_connect(g_webrtc, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), nullptr);
    g_signal_connect(g_webrtc, "pad-added", G_CALLBACK(on_incoming_stream), nullptr);
    g_signal_connect(g_webrtc, "notify::ice-gathering-state", G_CALLBACK(on_notify_ice_gathering), nullptr);

    // 원격에서 들어올 VP8 비디오 스트림을 받아들이도록 트랜시버 추가
    GstCaps* vcaps = gst_caps_from_string(
        "application/x-rtp,media=video,encoding-name=VP8,payload=96,clock-rate=90000");
    GstWebRTCRTPTransceiver* vtrans = nullptr;
    g_signal_emit_by_name(g_webrtc, "add-transceiver",
                          GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY,
                          vcaps, &vtrans);
    if (vcaps) gst_caps_unref(vcaps);
    if (vtrans) gst_object_unref(vtrans);

    gst_element_set_state(g_pipeline, GST_STATE_PLAYING);

    g_print("\n[Receiver] Waiting... This app will create an SDP OFFER.\n");
    g_print("Once it prints the OFFER, paste it into the browser page.\n\n");

    // GStreamer 메인 루프는 별도 스레드에서 돌려 UI와 분리한다.
    std::thread gst_thread([]() {
        GMainContext* ctx = g_main_loop_get_context(g_loop);
        g_main_context_push_thread_default(ctx);
        g_main_loop_run(g_loop); // 블로킹 호출
        g_main_context_pop_thread_default(ctx);
        g_running.store(false); // 루프가 종료되면 메인 루프도 종료
    });

    // 메인 스레드에서는 최신 프레임을 화면에 표시한다.
    cv::namedWindow("WebRTC-Recv", cv::WINDOW_AUTOSIZE);
    while (g_running.load()) {
        cv::Mat frame;
        {
            std::lock_guard<std::mutex> lock(g_frame_mutex);
            if (!g_latest_frame.empty()) frame = g_latest_frame.clone(); // 최신 프레임 복사
        }
        if (!frame.empty()) {
            std::cout << "frame width: " << frame.cols << ", frame height: " << frame.rows << std::endl;
            cv::imshow("WebRTC-Recv", frame);
        }
        int key = cv::waitKey(10); // 키 입력 처리
        if (key == 27 || key == 'q' || key == 'Q') {
            g_running.store(false);
            if (g_loop) g_main_loop_quit(g_loop);
            break;
        }
    }
    cv::destroyWindow("WebRTC-Recv");
    gst_thread.join();

    // 종료 시 자원 정리
    gst_element_set_state(g_pipeline, GST_STATE_NULL);
    gst_object_unref(g_pipeline);
    g_main_loop_unref(g_loop);
    return 0;
}
