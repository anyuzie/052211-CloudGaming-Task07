#include <iostream>
#include <vector>
#include <queue>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <map>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <atomic>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <sstream>
#include <iomanip>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#pragma comment(lib, "ws2_32.lib")

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libswscale/swscale.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/opt.h>
}

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

constexpr char GAME_HOST[] = "::1"; // IPv6 loopback address
constexpr uint16_t GAME_PORT = 8888;

SOCKET controlSocket;
sockaddr_in6 gameAddr;

bool initControlSocket() {
    controlSocket = socket(AF_INET6, SOCK_DGRAM, 0); // IPv6
    if (controlSocket == INVALID_SOCKET) return false;

    memset(&gameAddr, 0, sizeof(gameAddr));
    gameAddr.sin6_family = AF_INET6;
    gameAddr.sin6_port = htons(GAME_PORT);

    if (inet_pton(AF_INET6, GAME_HOST, &gameAddr.sin6_addr) != 1) {
        std::cerr << "Invalid IPv6 address\n";
        return false;
    }

    return true;
}

void sendKeyToGame(uint8_t scancode, float dt) {
    std::cout << "[KeyPress] scancode: " << static_cast<int>(scancode) << std::endl;
    char buffer[16];
    sprintf(buffer, "%u:%.3f", scancode, dt);
    int ret = sendto(controlSocket, buffer, strlen(buffer), 0, (sockaddr*)&gameAddr, sizeof(gameAddr));

    if (ret < 0) {
        std::cerr << "Failed to send information :< " << "\n";
    }
}

// === Structures and Types ===
typedef struct RTHeader {
    double time;
    unsigned long packetnum;
} RTHeader_t;

#pragma pack(push, 1)
typedef struct FragmentHeader {
    uint32_t frame_id;
    uint16_t total_fragments;
    uint16_t fragment_index;
} FragmentHeader_t;
#pragma pack(pop)

struct DecodedFrame {
    int width;
    int height;
    std::vector<uint8_t> rgba;
};

#pragma pack(push, 1)
struct ReceiverReport {
    double timestamp;
    uint32_t bytes_received;
    uint32_t expected_packets;
    uint32_t received_packets;
    float    frame_rate;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct NAKPacket {
    uint32_t frame_id;
    uint16_t missing_index;
};
#pragma pack(pop)

// === Globals ===
static constexpr int MAX_UDP_PACKET_SIZE = 65536;
static constexpr int BUFFER_THRESHOLD = 5;
bool isSDLInitialized = false;

std::mutex frameQueueMutex;
std::condition_variable frameQueueCondVar;
std::queue<DecodedFrame> frameQueue;
std::atomic<bool> running{true};
std::atomic<uint32_t> total_bytes{0};
std::atomic<uint32_t> expected_packet_count{0};
std::atomic<uint32_t> received_packet_count{0};
std::atomic<uint32_t> decoded_frame_count{0};

struct FrameBuffer {
    uint16_t total_fragments = 0;
    std::map<uint16_t, std::vector<uint8_t>> fragments;
    size_t total_size = 0;
};

// === Network Setup ===
int startWinsock() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa);
}

void send_nak(SOCKET sock, const sockaddr_in6& sender_addr, uint32_t frame_id, uint16_t missing_index) {
    NAKPacket nak{frame_id, missing_index};
    sendto(sock, (char*)&nak, sizeof(nak), 0, (sockaddr*)&sender_addr, sizeof(sender_addr));
    std::cout << "[NAK] Requested resend for frame " << frame_id << ", fragment " << missing_index << "\n";
}

int receive_fragment(SOCKET sock, uint32_t& out_frame_id, uint16_t& out_total, uint16_t& out_index, std::vector<uint8_t>& out_payload) {
    sockaddr_in6 si_other;
    socklen_t slen = sizeof(si_other);
    char recbuffer[MAX_UDP_PACKET_SIZE];

    int ret = recvfrom(sock, recbuffer, sizeof(recbuffer), 0, (sockaddr*)&si_other, &slen);
    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return 0;
        std::cerr << "recvfrom failed: " << err << "\n";
        return -1;
    }

    RTHeader_t* rt_header = (RTHeader_t*)recbuffer;
    FragmentHeader_t* frag_header = (FragmentHeader_t*)(recbuffer + sizeof(RTHeader_t));
    char* payload = recbuffer + sizeof(RTHeader_t) + sizeof(FragmentHeader_t);
    int payloadSize = ret - (sizeof(RTHeader_t) + sizeof(FragmentHeader_t));

    out_frame_id = frag_header->frame_id;
    out_total = frag_header->total_fragments;
    out_index = frag_header->fragment_index;
    out_payload.assign(payload, payload + payloadSize);

    total_bytes += payloadSize;
    expected_packet_count += out_total;
    received_packet_count++;

    return payloadSize;
}

void decode_thread_func(SOCKET sock, AVCodecContext* codecCtx) {
    std::unordered_map<uint32_t, FrameBuffer> frame_buffer_map;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    while (running.load()) {
        uint32_t frame_id;
        uint16_t total_fragments, fragment_index;
        std::vector<uint8_t> payload;

        int recv_ret = receive_fragment(sock, frame_id, total_fragments, fragment_index, payload);
        if (recv_ret <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        auto& buffer = frame_buffer_map[frame_id];
        buffer.total_fragments = total_fragments;
        buffer.fragments[fragment_index] = payload;
        buffer.total_size += payload.size();

        if (buffer.fragments.size() == total_fragments) {
            std::vector<uint8_t> full_frame;
            full_frame.reserve(buffer.total_size);
            for (int i = 0; i < total_fragments; ++i) {
                if (buffer.fragments.count(i) == 0) {
                    std::cerr << "Missing fragment index " << i << " for frame " << frame_id << "\n";
                    frame_buffer_map.erase(frame_id);
                    break;
                }
                auto& frag = buffer.fragments[i];
                full_frame.insert(full_frame.end(), frag.begin(), frag.end());
            }
            frame_buffer_map.erase(frame_id);
            decoded_frame_count++;

            av_packet_unref(packet);
            av_new_packet(packet, full_frame.size());
            memcpy(packet->data, full_frame.data(), full_frame.size());
            if (avcodec_send_packet(codecCtx, packet) == 0) {
                while (avcodec_receive_frame(codecCtx, frame) == 0) {
                    int w = frame->width, h = frame->height;
                    SwsContext* sws = sws_getContext(w, h, (AVPixelFormat)frame->format, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    std::vector<uint8_t> rgba(w * h * 4);
                    uint8_t* dst[1] = { rgba.data() };
                    int linesize[1] = { w * 4 };
                    sws_scale(sws, frame->data, frame->linesize, 0, h, dst, linesize);
                    sws_freeContext(sws);

                    std::unique_lock<std::mutex> lock(frameQueueMutex);
                    frameQueue.push({w, h, std::move(rgba)});
                    frameQueueCondVar.notify_one();
                }
            }
        }
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
}

void reportLoop(SOCKET reportSock, sockaddr_in6 senderAddr) {
    const int interval_seconds = 10;
    while (running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(interval_seconds));

        ReceiverReport report;
        report.timestamp = static_cast<double>(SDL_GetTicks()) / 1000.0;
        report.bytes_received = total_bytes.exchange(0);
        report.expected_packets = expected_packet_count.exchange(0);
        report.received_packets = received_packet_count.exchange(0);
        report.frame_rate = decoded_frame_count.exchange(0) / (float)interval_seconds;

        sendto(reportSock, reinterpret_cast<char*>(&report), sizeof(report), 0,
               reinterpret_cast<sockaddr*>(&senderAddr), sizeof(senderAddr));
    }
}

int main() {
    if (startWinsock() != 0) return -1;

    SOCKET sock = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) return -1;
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(9999);
    addr.sin6_addr = in6addr_any;
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) != 0) return -1;

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    avcodec_open2(codecCtx, codec, nullptr);

    std::thread(reportLoop, sock, addr).detach();
    std::thread decoderThread(decode_thread_func, sock, codecCtx);

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_Init(SDL_INIT_VIDEO);

    if (!initControlSocket()) {
        std::cerr << "Failed to initialize control socket\n";
        return -1;
    }

    SDL_Event e;
    while (running.load()) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT ||
                (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE)) {
                running.store(false);
                frameQueueCondVar.notify_all();
                break;
            }
            float dt = 0.033f; // Assume fixed timestep or calculate dynamically
            if (e.type == SDL_EVENT_KEY_DOWN) {
                sendKeyToGame(e.key.scancode, dt);
            }
        }

        std::unique_lock<std::mutex> lock(frameQueueMutex);
        if (!frameQueue.empty()) {
            auto frame = std::move(frameQueue.front());
            frameQueue.pop();
            lock.unlock();

            if (!isSDLInitialized) {
                window = SDL_CreateWindow("Receiver", frame.width, frame.height, 0);
                renderer = SDL_CreateRenderer(window, nullptr);
                texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height);
                isSDLInitialized = true;
            }

            SDL_UpdateTexture(texture, nullptr, frame.rgba.data(), frame.width * 4);
            SDL_RenderClear(renderer);
            SDL_RenderTexture(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        } else {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }


    decoderThread.join();
    avcodec_free_context(&codecCtx);
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    closesocket(sock);
    WSACleanup();
    return 0;
}
