#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <gst/app/gstappsink.h>
#include <gst/sdp/gstsdpmessage.h>
#include <gst/video/video.h>

#include <opencv2/opencv.hpp>

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <sstream>

static GMainLoop* g_loop = nullptr;
static GstElement* g_pipeline = nullptr;
static GstElement* g_webrtc = nullptr;
static GstElement* g_appsink = nullptr; // 최종 RAW 프레임 수신 지점

// UI 표시용 공유 프레임 버퍼
static std::mutex g_frame_mutex;
static cv::Mat g_latest_frame;
static std::atomic<bool> g_running{true};

static std::atomic<bool> g_answer_set{false};

// 브라우저 호환을 위해 SDP에서 문제 소지가 있는 라인들을 제거
static std::string sanitize_sdp_for_browser(const char* sdp_in) {
    std::string in = sdp_in ? sdp_in : "";
    // CR 제거
    in.erase(std::remove(in.begin(), in.end(), '\r'), in.end());
    std::stringstream iss(in);
    std::string line;
    std::ostringstream oss;
    while (std::getline(iss, line)) {
        // 좌/우 공백 정리
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t p = 0; while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
        std::string t = (p > 0) ? line.substr(p) : line;

        // 헤더/푸터 제거 안전망
        if (t.rfind("=====", 0) == 0) continue;
        // a=end-of-candidates 제거 (파서 민감 회피)
        if (t.rfind("a=end-of-candidates", 0) == 0) continue;
        // a=rtcp-mux-only 제거 (오래된 사양, 호환성 문제 유발)
        if (t.rfind("a=rtcp-mux-only", 0) == 0) continue;

        if (t.rfind("a=candidate:", 0) == 0) {
            std::string low = t;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c){ return std::tolower(c); });
            // TCP 후보 제거
            if (low.find(" tcp ") != std::string::npos || low.find("\ttcp ") != std::string::npos || low.find(" tcptype ") != std::string::npos) {
                continue;
            }
            // srflx 후보 제거 (브라우저 파서 에러 회피용)
            if (low.find(" typ srflx") != std::string::npos) {
                continue;
            }
            // host 후보 제거 (GStreamer webrtcbin이 일부 host 후보 처리시 문제 발생)
            if (low.find(" typ host") != std::string::npos) {
                continue;
            }
            // IPv6 주소 후보 제거 (일부 브라우저/환경에서 파서 민감)
            std::vector<std::string> tokens;
            {
                std::istringstream ts(t);
                std::string tok;
                while (ts >> tok) tokens.push_back(tok);
            }
            if (tokens.size() >= 6) {
                const std::string& addr = tokens[4];
                if (addr.find(':') != std::string::npos) {
                    // IPv6
                    continue;
                }
            }
        }

        // RFC를 더 잘 따르도록 CRLF로 줄바꿈
        oss << t << "\r\n";
    }
    return oss.str();
}

// ===== 유틸: STDIN에서 "===== END SDP =====" 까지 읽기 =====
static std::string read_sdp_from_stdin() {
    std::string line, sdp;
    while (std::getline(std::cin, line)) {
        if (line.find("===== END SDP =====") != std::string::npos) break;
        sdp += line + "\n";
    }
    return sdp;
}

// ===== 메인 스레드에서 SDP ANSWER 처리 =====
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
    return G_SOURCE_REMOVE; // 이 함수를 다시 호출하지 않음
}


// ===== appsink 콜백: RAW 프레임 수신 =====
static GstFlowReturn on_new_sample(GstElement* sink, gpointer /*user_data*/) {
    GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    if (!sample) return GST_FLOW_OK;

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
    if (gst_video_frame_map(&vframe, &vinfo, buffer, GST_MAP_READ)) {
        int width  = GST_VIDEO_FRAME_WIDTH(&vframe);
        int height = GST_VIDEO_FRAME_HEIGHT(&vframe);
        int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 0);
        guint8* data = reinterpret_cast<guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&vframe, 0));

        cv::Mat view(height, width, CV_8UC3, data, stride);
        cv::Mat copy;
        view.copyTo(copy);
        {
            std::lock_guard<std::mutex> lock(g_frame_mutex);
            g_latest_frame = std::move(copy);
        }

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

        gst_element_sync_state_with_parent(videoconvert);
        gst_element_sync_state_with_parent(videoscale);
        gst_element_sync_state_with_parent(capsfilter);
        gst_element_sync_state_with_parent(g_appsink);
    }

    if (caps) gst_caps_unref(caps);
}

static void on_incoming_stream(GstElement* /*webrtc*/, GstPad* pad, gpointer /*user_data*/) {
    g_print("\n[DEBUG] on_incoming_stream CALLED\n");
    GstCaps* caps = gst_pad_get_current_caps(pad);
    const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    g_print("[DEBUG] Incoming stream is of type: %s\n", name);

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

    g_signal_connect(decodebin, "pad-added", G_CALLBACK(on_decodebin_pad_added), nullptr);

    if (caps) gst_caps_unref(caps);
}

static void on_offer_created(GstPromise* promise, gpointer /*user_data*/) {
    const GstStructure* reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription* offer = nullptr;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
    g_signal_emit_by_name(g_webrtc, "set-local-description", offer, nullptr);
    gst_webrtc_session_description_free(offer);
    gst_promise_unref(promise);
}

static void on_negotiation_needed(GstElement* /*webrtc*/, gpointer /*user_data*/) {
    GstPromise* promise = gst_promise_new_with_change_func(on_offer_created, nullptr, nullptr);
    g_signal_emit_by_name(g_webrtc, "create-offer", nullptr, promise);
}

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

        std::thread([=]() {
            g_print("Paste the SDP ANSWER from browser, then end with a line: '===== END SDP ====='\n");
            std::string raw_answer = read_sdp_from_stdin();
            std::string answer = sanitize_sdp_for_browser(raw_answer.c_str());
            if (answer.empty()) {
                g_printerr("Empty ANSWER received, not processing.\n");
                return;
            }
            // Pass the sanitized SDP to the main loop for processing
            g_idle_add(on_answer_received, g_strdup(answer.c_str()));
        }).detach();
    }
}

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);

    const char* stun = "stun://stun.l.google.com:19302";
    if (argc >= 2) stun = argv[1];

    g_loop = g_main_loop_new(nullptr, FALSE);

    g_pipeline = gst_pipeline_new("webrtc-recv-pipeline");
    g_webrtc = gst_element_factory_make("webrtcbin", "webrtcbin");
    if (!g_pipeline || !g_webrtc) {
        g_printerr("Failed to create pipeline/webrtcbin. Ensure gstreamer-webrtc is installed.\n");
        return -1;
    }

    g_object_set(g_webrtc, "stun-server", stun, nullptr);

    gst_bin_add(GST_BIN(g_pipeline), g_webrtc);

    g_signal_connect(g_webrtc, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), nullptr);
    g_signal_connect(g_webrtc, "pad-added", G_CALLBACK(on_incoming_stream), nullptr);
    g_signal_connect(g_webrtc, "notify::ice-gathering-state", G_CALLBACK(on_notify_ice_gathering), nullptr);

    GstCaps* vcaps = gst_caps_from_string(
        "application/x-rtp,media=video,encoding-name=VP8,payload=96,clock-rate=90000");
    GstWebRTCRTPTransceiver* vtrans = nullptr;
    g_signal_emit_by_name(g_webrtc, "add-transceiver",
                          GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY,
                          vcaps, &vtrans);
    if (vcaps) gst_caps_unref(vcaps);
    if (vtrans) gst_object_unref(vtrans);

    std::thread ui_thread_func([](){
        cv::namedWindow("WebRTC-Recv", cv::WINDOW_AUTOSIZE);
        while (g_running.load()) {
            cv::Mat frame;
            {
                std::lock_guard<std::mutex> lock(g_frame_mutex);
                if (!g_latest_frame.empty()) frame = g_latest_frame.clone();
            }
            if (!frame.empty()) {
                cv::imshow("WebRTC-Recv", frame);
            }
            int key = cv::waitKey(10);
            if (key == 27 || key == 'q' || key == 'Q') {
                g_running.store(false);
                if (g_loop) g_main_loop_quit(g_loop);
                break;
            }
        }
        cv::destroyWindow("WebRTC-Recv");
    });
    ui_thread_func.detach();

    gst_element_set_state(g_pipeline, GST_STATE_PLAYING);

    g_print("\n[Receiver] Waiting... This app will create an SDP OFFER.\n");
    g_print("Once it prints the OFFER, paste it into the browser page.\n\n");

    g_main_loop_run(g_loop);

    gst_element_set_state(g_pipeline, GST_STATE_NULL);
    gst_object_unref(g_pipeline);
    g_main_loop_unref(g_loop);
    g_running.store(false);
    return 0;
}