#define SDL_MAIN_HANDLED
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <SDL2/SDL.h>
}

std::atomic<bool> g_running(true);
std::atomic<bool> g_hasNewFrame(false);
std::mutex g_frameMutex;

bool recvAll(SOCKET s, uint8_t* buf, int len) {
    int total = 0;
    while (total < len) {
        int r = recv(s, reinterpret_cast<char*>(buf + total), len - total, 0);
        if (r <= 0) return false;
        total += r;
    }
    return true;
}

uint32_t readBE32(SOCKET s) {
    uint32_t val = 0;
    if (!recvAll(s, reinterpret_cast<uint8_t*>(&val), 4)) return 0;
    return ntohl(val);
}

uint64_t readBE64(SOCKET s) {
    uint64_t high = readBE32(s);
    uint64_t low = readBE32(s);
    return (high << 32) | low;
}

SOCKET connectLocalPort(int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

void audioThreadFunc(SOCKET audioSock) {
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_S16LSB;
    want.channels = 2;
    want.samples = 512;
    want.callback = nullptr;

    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev == 0) return;
    SDL_PauseAudioDevice(dev, 0);

    uint8_t pcmBuf[1920];
    const uint32_t MAX_QUEUE_BYTES = 1920 * 5; // Tối đa 50ms buffer

    while (g_running) {
        int r = recv(audioSock, reinterpret_cast<char*>(pcmBuf), sizeof(pcmBuf), 0);
        if (r <= 0) break;

        if (SDL_GetQueuedAudioSize(dev) > MAX_QUEUE_BYTES) {
            SDL_ClearQueuedAudio(dev);
        }
        SDL_QueueAudio(dev, pcmBuf, r);
    }
    SDL_CloseAudioDevice(dev);
}

int main(int argc, char* argv[]) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    std::cout << "[Client] Dang ket noi toi Video Socket (127.0.0.1:27183)...\n";
    SOCKET videoSock = connectLocalPort(27183);
    if (videoSock == INVALID_SOCKET) {
        std::cerr << "[Loi] Khong the ket noi cong 27183!\n";
        return -1;
    }

    std::cout << "[Client] Dang ket noi toi Audio Socket (127.0.0.1:27184)...\n";
    SOCKET audioSock = connectLocalPort(27184);

    int width = static_cast<int>(readBE32(videoSock));
    int height = static_cast<int>(readBE32(videoSock));
    std::cout << "[Video] Nhan luong Video: " << width << "x" << height << " @ 60 FPS\n";

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    codecCtx->thread_count = 1;
    avcodec_open2(codecCtx, codec, nullptr);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* decodeFrame = av_frame_alloc();
    AVFrame* renderFrame = av_frame_alloc(); // Frame riêng cho luồng vẽ để không bao giờ xung đột

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    int winW = width, winH = height;
    if (winH > 880) {
        winW = winW * 880 / winH;
        winH = 880;
    }

    SDL_Window* window = SDL_CreateWindow(
        "Android Mirror (60 FPS - Low Latency)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        winW, winH,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(renderer, width, height);

    SDL_Texture* texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, width, height
    );

    std::thread audioThread;
    if (audioSock != INVALID_SOCKET) {
        audioThread = std::thread(audioThreadFunc, audioSock);
    }

    std::thread videoThread([&]() {
        std::vector<uint8_t> buffer(2 * 1024 * 1024);
        while (g_running) {
            uint32_t packetSize = readBE32(videoSock);
            if (packetSize == 0 || packetSize > 10 * 1024 * 1024) break;
            uint64_t pts = readBE64(videoSock);

            if (buffer.size() < packetSize + AV_INPUT_BUFFER_PADDING_SIZE) {
                buffer.resize(packetSize + AV_INPUT_BUFFER_PADDING_SIZE);
            }
            if (!recvAll(videoSock, buffer.data(), packetSize)) break;

            pkt->data = buffer.data();
            pkt->size = packetSize;
            pkt->pts = pts;

            if (avcodec_send_packet(codecCtx, pkt) >= 0) {
                while (avcodec_receive_frame(codecCtx, decodeFrame) >= 0) {
                    std::lock_guard<std::mutex> lock(g_frameMutex);
                    av_frame_unref(renderFrame);
                    av_frame_ref(renderFrame, decodeFrame);
                    g_hasNewFrame = true;
                }
            }
        }
        g_running = false;
    });

    // Vòng lặp vẽ tốc độ cao không bị kẹt bởi SDL_WaitEvent
    SDL_Event event;
    while (g_running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                g_running = false;
            }
        }

        if (g_hasNewFrame.exchange(false)) {
            std::lock_guard<std::mutex> lock(g_frameMutex);
            if (renderFrame->width > 0 && renderFrame->height > 0) {
                SDL_UpdateYUVTexture(
                    texture, nullptr,
                    renderFrame->data[0], renderFrame->linesize[0],
                    renderFrame->data[1], renderFrame->linesize[1],
                    renderFrame->data[2], renderFrame->linesize[2]
                );
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, nullptr, nullptr);
                SDL_RenderPresent(renderer);
            }
        } else {
            SDL_Delay(1); // Nghỉ 1ms để không tốn CPU khi chờ frame tiếp theo
        }
    }

    closesocket(videoSock);
    if (audioSock != INVALID_SOCKET) closesocket(audioSock);
    if (videoThread.joinable()) videoThread.join();
    if (audioThread.joinable()) audioThread.join();

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    av_frame_free(&decodeFrame);
    av_frame_free(&renderFrame);
    av_packet_free(&pkt);
    avcodec_free_context(&codecCtx);
    WSACleanup();
    return 0;
}