extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/avutil.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/opt.h>
    #include <libswscale/swscale.h>

    #define WIN32_LEAN_AND_MEAN

    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    
}

#include <iostream>
#include <utility>
#include <format>
#include <cstdint>
#include "VHInclude.h"
#include "VEInclude.h"
#include <vector>
#include <atomic>
#include <string>
#include <chrono>
#include <thread>
#include <map> 
#include <unordered_map>
#include <algorithm> 
#include <SDL.h>
#include <glm/gtc/type_ptr.hpp>

#include "stb_image_write.h"

#pragma comment(lib, "ws2_32.lib")

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

constexpr uint16_t LISTEN_PORT = 8888;
std::atomic<bool> runInputThread{true};

class UDPsend {
    public:
        int sock = 0;
        struct sockaddr_in6 addr;
        unsigned int packetnum = 0;

        static constexpr int MTU = 1400;
        static constexpr int HEADER_SIZE = sizeof(RTHeader_t) + sizeof(FragmentHeader_t);
        static constexpr int MAX_PAYLOAD = MTU - HEADER_SIZE;

        UDPsend() {};

        ~UDPsend() {};

        void init(const char *address, int port) {
            sock = socket( AF_INET6, SOCK_DGRAM, 0);
            struct addrinfo hints;

            memset(&addr, 0, sizeof(addr));
            memset(&hints, 0, sizeof(hints));

            hints.ai_family = AF_INET6;
            hints.ai_socktype = SOCK_DGRAM;
            hints.ai_flags = 0;

            struct addrinfo *result = NULL;
            auto dwRetval = getaddrinfo(address, nullptr, &hints, &result);
            if ( dwRetval != 0 ) {
                printf("getaddrinfo failed with error: %d\n", dwRetval);
                return;
            }
            for (addrinfo* ptr = result; ptr != NULL; ptr = ptr->ai_next) {
                if (ptr->ai_family == AF_INET6) {
                    memcpy(&addr, ptr->ai_addr, ptr->ai_addrlen);
                    addr.sin6_port = htons(port);
                    addr.sin6_family = AF_INET6;
                }
            }
            freeaddrinfo(result);
        };
        

        int send_fragmented(char* buffer, int len) {
            packetnum++; // new frame ID
            
            int total_fragments = (len + MAX_PAYLOAD - 1) / MAX_PAYLOAD;
        
            for (int i = 0; i < total_fragments; ++i) {
                int payload_size = std::min(MAX_PAYLOAD, len - i * MAX_PAYLOAD);
        
                char sendbuffer[MTU];

                RTHeader_t rt_header;
                auto now = std::chrono::steady_clock::now();
                rt_header.time = std::chrono::duration<double>(now.time_since_epoch()).count();
                rt_header.packetnum = packetnum;
                // std::cout << "rt_header.packetnum: " << rt_header.packetnum << std::endl;


                FragmentHeader_t frag_header;
                frag_header.frame_id = packetnum;
                frag_header.total_fragments = total_fragments;
                frag_header.fragment_index = i;
                // std::cout << "frag_header.frame_id: " << frag_header.frame_id << std::endl;

        
                memcpy(sendbuffer, &rt_header, sizeof(rt_header));
                memcpy(sendbuffer + sizeof(rt_header), &frag_header, sizeof(frag_header));
                memcpy(sendbuffer + HEADER_SIZE, buffer + i * MAX_PAYLOAD, payload_size);
        
                int ret = sendto(sock, sendbuffer, HEADER_SIZE + payload_size, 0,
                                 (const sockaddr*)&addr, sizeof(addr));
                    // std::cout << "HEADER_SIZE: " << HEADER_SIZE << std::endl;
                if (ret < 0) {
                    std::cerr << "Failed to send fragment " << i << "\n";
                    return ret;
                }
            }
        
            return total_fragments;
        }
        
        void closeSock() {
            closesocket(sock);
            sock=0;
        };
};

class FrameLimiter {
    public:
    FrameLimiter(float targetFPS)
    : m_frameDuration
        (
        std::chrono::duration_cast<std::chrono::steady_clock::duration>
            (
            std::chrono::duration<float>(1.0f / targetFPS)
            )
        ),
    m_nextFrameTime
        (
        std::chrono::steady_clock::now()
        ) {}

    
        void Wait() {
            auto now = std::chrono::steady_clock::now();
    
            if (now < m_nextFrameTime) {
                std::this_thread::sleep_until(m_nextFrameTime);
            }
    
            m_nextFrameTime += m_frameDuration;
        }
    
    private:
        std::chrono::steady_clock::duration m_frameDuration;
        std::chrono::steady_clock::time_point m_nextFrameTime;
};
    
class FFmpegWriter {
    public:
        FFmpegWriter(int width, int height, const std::string& outputFile)
            : m_width(width), m_height(height)
        {
            std::string cmd = std::format(
                R"(ffmpeg -y -f rawvideo -pixel_format rgba -video_size {}x{} -framerate 60 -i - -c:v libx264 -preset ultrafast -pix_fmt yuv420p -f h264 "{}")",
                width, height, outputFile
            );
    
            m_pipe = _popen(cmd.c_str(), "wb");
            if (!m_pipe) {
                std::cerr << "Failed to start FFmpeg." << std::endl;
            }
        }
    
        void WriteFrame(const uint8_t* frameData) {
            if (m_pipe) {
                fwrite(frameData, 1, m_width * m_height * 4, m_pipe);
            }
        }
    
        ~FFmpegWriter() {
            if (m_pipe) {
                _pclose(m_pipe);
            }
        }
    
    private:
        int m_width;
        int m_height;
        FILE* m_pipe = nullptr;
};

class FFmpegEncoder {
    public:
        FFmpegEncoder(int width, int height) 
            : m_width(width), m_height(height)
        {
    
            const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
            if (!codec) {
                std::cerr << "Codec not found\n";
                return;
            }
            m_codecCtx = avcodec_alloc_context3(codec);
            if (!m_codecCtx) {
                std::cerr << "Could not allocate codec context\n";
                return;
            }

            m_codecCtx->bit_rate = 400000;
            m_codecCtx->width = m_width;
            m_codecCtx->height = m_height;
            m_codecCtx->time_base = {1, 60};
            m_codecCtx->framerate = {60, 1};
            m_codecCtx->gop_size = 10;
            m_codecCtx->max_b_frames = 1;
            m_codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    
            av_opt_set(m_codecCtx->priv_data, "annexb", "1", 0);

            if (avcodec_open2(m_codecCtx, codec, NULL) < 0) {
                std::cerr << "Could not open codec\n";
                return;
            }
    
            m_frame = av_frame_alloc();
            if (!m_frame) {
                std::cerr << "Could not allocate frame\n";
                return;
            }

            m_frame->format = m_codecCtx->pix_fmt;
            m_frame->width = m_codecCtx->width;
            m_frame->height = m_codecCtx->height;
            av_frame_get_buffer(m_frame, 32);
    
            m_packet = av_packet_alloc();
            if (!m_packet) {
                std::cerr << "Could not allocate packet\n";
                return;
            }

    
            // SwsContext to convert from RGBA to YUV420P
            m_swsCtx = sws_getContext(
                width, height, AV_PIX_FMT_RGBA,
                width, height, AV_PIX_FMT_YUV420P,
                SWS_BILINEAR, nullptr, nullptr, nullptr
            );
            if (!m_swsCtx) {
                std::cerr << "Could not allocate SwsContext\n";
                return;
            }
        }
    
        ~FFmpegEncoder() {
            sws_freeContext(m_swsCtx);
            av_packet_free(&m_packet);
            av_frame_free(&m_frame);
            avcodec_free_context(&m_codecCtx);
        }
    
        // Returns encoded H.264 buffer (in packet), and size
        std::pair<uint8_t*, int> EncodeFrame(const uint8_t* rgbaData) {
            const uint8_t* srcSlice[] = { rgbaData };
            int srcStride[] = { 4 * m_width };
    
            // Convert RGBA → YUV420P
            sws_scale(m_swsCtx, srcSlice, srcStride, 0, m_height, m_frame->data, m_frame->linesize);
    
            m_frame->pts = m_pts++;
    
            // Send frame to encoder
            int ret = avcodec_send_frame(m_codecCtx, m_frame);
            if (ret < 0) return {nullptr, 0};
    
            ret = avcodec_receive_packet(m_codecCtx, m_packet);
            if (ret < 0) return {nullptr, 0};
    
            // Return pointer and size (packet data is owned by FFmpeg)
            return { m_packet->data, m_packet->size };
        }
    
        void FreePacket() {
            av_packet_unref(m_packet);
        }
    
    private:
        int m_width, m_height;
        int m_pts = 0;
        AVCodecContext* m_codecCtx = nullptr;
        AVFrame* m_frame = nullptr;
        AVPacket* m_packet = nullptr;
        SwsContext* m_swsCtx = nullptr;
    };
    

    std::unique_ptr<FFmpegWriter> m_ffmpegWriter;
    std::unique_ptr<FFmpegEncoder> m_ffmpegEncoder;


int startWinsock(void) {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 0), &wsa);
}

enum Tags : size_t {
    Tag_Retrievable = 1,
    // Tag_Cypher = 2,
    // Tag_Player = 3
};

class MyGame : public vve::System {
    public:
        MyGame(vve::Engine& engine) 
            : vve::System("MyGame", engine) 
        {
            m_engine.RegisterCallbacks({
                {this, 0, "LOAD_LEVEL", [this](Message& message){ return OnLoadLevel(message); }},
                {this, 10000, "UPDATE", [this](Message& message){ return OnUpdate(message); }},
                {this, -10000, "RECORD_NEXT_FRAME", [this](Message& message){ return OnRecordNextFrame(message); }},
                {this, 0, "FRAME_END", [this](Message& message){ return OnFrameEnd(message); } },
                {this, 0, "SDL_KEY_DOWN", [this](Message& message){ return OnKeyDown(message);} },
                {this, 0, "SDL_KEY_REPEAT", [this](Message& message){ return OnKeyDown(message);} }
            });
        }
    
        ~MyGame() = default;
    
    private:
        // --- Constants ---
        enum class PlayerState { STATIONARY, MOVING };
    
        // --- Inline asset paths ---
        inline static const std::string plane_obj  { "assets/test/plane/plane_t_n_s.obj" };
        inline static const std::string plane_mesh { "assets/test/plane/plane_t_n_s.obj/plane" };
        inline static const std::string plane_txt  { "assets/test/plane/grass.jpg" };
        inline static const std::string cube_obj   { "assets/test/crate0/cube.obj" };
        inline static const std::string cypher_obj   { "../escape/assets/furniture/Models/computerScreen.obj" };
        inline static std::string player_obj{ "../escape/assets/mini_characters/Models/OBJ format/character-female-a.obj" };
    
        // --- Game State ---
        PlayerState m_playerState = PlayerState::STATIONARY;
        int m_cubeCollected = 0;
        float m_volume = 100.0f;
        bool m_lastCollisionState = false;
        glm::vec3 m_cameraOffsetLocal = glm::vec3(0.0f, -2.0f, 2.0f);  
    
        // --- Handles ---
        vecs::Handle m_cameraHandle{};
        vecs::Handle m_cameraNodeHandle{};
        vecs::Handle m_playerHandle{};
        vecs::Handle m_cypherHandle{};
        vecs::Handle m_buffHandle{};
        // vecs::Handle m_handlePlane{};
        // std::map<vecs::Handle, std::string> m_objects;
        std::vector<std::string> m_mapGrid;
        std::vector<vecs::Handle> m_cubeHandles;
        std::vector<vecs::Handle> m_cypherHandles;

        // // --- Streaming Variables ---
        UDPsend m_UDPsender;
        FrameLimiter m_frameLimiter{25.0f};

        // --- Helper ---
        vec3_t RandomPosition() {
            return vec3_t{ float(rand() % 40 - 20), float(rand() % 40 - 20), 0.5f };
        }
    
        void GetCamera() {
            if (!m_cameraHandle.IsValid()) {
                auto [handle, camera, parent] = *m_registry.GetView<vecs::Handle, vve::Camera&, vve::ParentHandle>().begin();
                m_cameraHandle = handle; 
                m_cameraNodeHandle = parent; 
            }
        }

        vec3_t RandomWalkablePosition() {
            std::vector<std::pair<int, int>> walkableTiles;
        
            for (int row = 0; row < m_mapGrid.size(); ++row) {
                const auto& line = m_mapGrid[row];
                for (int col = 0; col < line.length(); ++col) {
                    if (line[col] != '#') {
                        walkableTiles.emplace_back(row, col);
                    }
                }
            }
        
            if (walkableTiles.empty()) return {0.0f, 0.0f, 0.5f}; // fallback
        
            auto [row, col] = walkableTiles[rand() % walkableTiles.size()];
        
            float spacing = 1.0f;
            float x = (float)(col - 5) * spacing;
            float y = (float)(row - 1) * spacing;
        
            return { x, y, 0.5f };
        }
        

        void SpawnCyphers(int count) {
            for (int i = 0; i < count; ++i) {
                vec3_t pos = RandomWalkablePosition();
                std::string cypher_name = "Cypher " + std::to_string(i + 1);
                vecs::Handle handle = m_registry.Insert(
                    vve::Position{ pos },
                    vve::Rotation{ glm::mat3( glm::rotate( glm::mat4(1.0f), glm::radians(180.0f), glm::vec3(0,0,1)) * glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1,0,0) ) ) },
                    vve::Scale{ vec3_t{0.1f} },
                    vve::Name{ cypher_name }
                );
                m_registry.AddTags(handle, static_cast<size_t>(Tags::Tag_Retrievable));

                m_cypherHandles.push_back(handle);
        
                // m_engine.SendMsg(MsgSceneCreate{
                //     vve::ObjectHandle(handle), vve::ParentHandle{}, vve::Filename{cypher_obj}, aiProcess_FlipWindingOrder
                // });
            }
        }

        void LoadMapAndSpawnWalls(const std::string& mapFilePath) {
            std::ifstream mapFile(mapFilePath);
            if (!mapFile.is_open()) {
                std::cerr << "Failed to open map file: " << mapFilePath << std::endl;
                return;
            }
        
            std::string line;
            int row = 0;
            int cube_num = 0;
            m_mapGrid.clear();
        
            while (std::getline(mapFile, line)) {
                m_mapGrid.push_back(line);
                for (int col = 0; col < line.length(); ++col) {
                    if (line[col] == '#') { // '#' means place a wall cube
                        // Each cube is at grid (col, row), but we might want to scale it (spacing between cubes)
                        float spacing = 1.0f; // How far apart cubes are
                        float x = (float)(col-5) * spacing;
                        float y = (float)(row-1) * spacing; // -row so Y axis goes downward
                        // std::cout << x << std::endl;
                        // std::cout << y << std::endl;
                        // std::cout << "spawned cube" << std::endl;

        
                        // Insert cube
                        cube_num ++;
                        std::string cube_name = "Cube " + std::to_string(cube_num);
                        vecs::Handle handle = m_registry.Insert(
                            vve::Position{ {x, y, 0.5f} },
                            vve::Rotation{ mat3_t{1.0f} },
                            vve::Scale{ vec3_t{1.0f} },
                            vve::Name{ cube_name }
                        );
                        m_registry.AddTags(handle, static_cast<size_t>(Tags::Tag_Retrievable));
        
                        m_cubeHandles.push_back(handle);
        
                        m_engine.SendMsg(MsgSceneCreate{
                            vve::ObjectHandle(handle),
                            vve::ParentHandle{},
                            vve::Filename{cube_obj},
                            aiProcess_FlipWindingOrder
                        });
                    }
                }
                row++;
            }
        
            mapFile.close();
        }

        bool CheckCollision(const glm::vec3& proposedPos) {
            float playerRadius = 0.3f; // adjust if you want tighter/looser collision
            glm::vec3 cubeHalfExtents(0.5f, 0.5f, 0.5f); // assuming 1x1x1 cubes
            glm::vec3 cypherHalfExtents(0.05f, 0.05f, 0.05f); // assuming 1x1x1 cubes
        
            for (auto& cubeHandle : m_cubeHandles) {
                if (!cubeHandle.IsValid()) continue;
        
                auto& cubePos = m_registry.Get<vve::Position&>(cubeHandle)();
        
                float minX = cubePos.x - cubeHalfExtents.x;
                float maxX = cubePos.x + cubeHalfExtents.x;
                float minY = cubePos.y - cubeHalfExtents.y;
                float maxY = cubePos.y + cubeHalfExtents.y;
        
                float closestX = glm::clamp(proposedPos.x, minX, maxX);
                float closestY = glm::clamp(proposedPos.y, minY, maxY);
        
                float dx = proposedPos.x - closestX;
                float dy = proposedPos.y - closestY;
        
                if (dx * dx + dy * dy < playerRadius * playerRadius) {
                    return true; // collision detected
                }
            }
            
            for (auto& cypherHandle : m_cypherHandles) {
                if (!cypherHandle.IsValid()) continue;
        
                auto& cypherPos = m_registry.Get<vve::Position&>(cypherHandle)();
        
                float minX = cypherPos.x - cypherHalfExtents.x;
                float maxX = cypherPos.x + cypherHalfExtents.x;
                float minY = cypherPos.y - cypherHalfExtents.y;
                float maxY = cypherPos.y + cypherHalfExtents.y;
        
                float closestX = glm::clamp(proposedPos.x, minX, maxX);
                float closestY = glm::clamp(proposedPos.y, minY, maxY);
        
                float dx = proposedPos.x - closestX;
                float dy = proposedPos.y - closestY;
        
                if (dx * dx + dy * dy < playerRadius * playerRadius) {
                    return true; // collision detected
                }
            }
        
            return false; // no collision
        }
    
        void StartInputListener() {
            SOCKET sock = socket(AF_INET6, SOCK_DGRAM, 0); // IPv6
            if (sock == INVALID_SOCKET) {
                std::cerr << "Failed to create socket\n";
                return;
            }

            sockaddr_in6 addr{};
            addr.sin6_family = AF_INET6;
            addr.sin6_port = htons(LISTEN_PORT);
            addr.sin6_addr = in6addr_any;

            if (bind(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
                std::cerr << "Bind failed\n";
                return;
            }



            char buffer[64];
            std::cout << "yessss" << std::endl;
            while (runInputThread.load()) {
                sockaddr_in6 sender;
                int len = sizeof(sender);
                int recvLen = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&sender, &len);
                if (recvLen > 0) {
                    std::cout << "yayyyy this ran!" << std::endl;
                    buffer[recvLen] = '\0';
                    uint32_t code;
                    float dt;

                    if (sscanf(buffer, "%u:%f", &code, &dt) == 2) {  
                        std::cout << "[Receiver Input] Key code: " << code << " | dt: " << dt << "s\n";

                        SDL_Event e{};
                        e.type = SDL_KEYDOWN;
                        e.key.type = SDL_KEYDOWN;
                        e.key.timestamp = SDL_GetTicks();
                        e.key.windowID = 0; // 0 = default window
                        e.key.state = SDL_PRESSED;
                        e.key.repeat = 0;
                        e.key.keysym.scancode = static_cast<SDL_Scancode>(code); // set this too
                        e.key.keysym.sym = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(code));
                        e.key.keysym.mod = KMOD_NONE;
                        SDL_Scancode key = static_cast<SDL_Scancode>(code);
                        this->HandleRemoteKey(key, dt);

                        // SDL_PushEvent(&e);
                        std::cout << "Pushed SDL_KEYDOWN for keycode: " << code << "\n";
                    }
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }

            closesocket(sock);
        }


        void HandleRemoteKey(SDL_Scancode key, float dt) {
            auto& playerPos = m_registry.Get<vve::Position&>(m_playerHandle)();
            auto& playerRot = m_registry.Get<vve::Rotation&>(m_playerHandle)();

            float moveSpeed = dt * 5.0f;
            float rotSpeed = glm::radians(90.0f) * dt;

            glm::vec3 moveDir{0.0f};
            glm::vec3 forward = playerRot * glm::vec3(0.0f, 0.0f, 1.0f);
            glm::vec3 right   = playerRot * glm::vec3(1.0f, 0.0f, 0.0f);

            switch (key) {
                case SDL_SCANCODE_W: moveDir += forward; break;
                case SDL_SCANCODE_S: moveDir -= forward; break;
                case SDL_SCANCODE_A: moveDir += right; break;
                case SDL_SCANCODE_D: moveDir -= right; break;
                case SDL_SCANCODE_LEFT:
                    playerRot = glm::mat3(glm::rotate(glm::mat4(playerRot), rotSpeed, glm::vec3(0.0f, 1.0f, 0.0f)));
                    break;
                case SDL_SCANCODE_RIGHT:
                    playerRot = glm::mat3(glm::rotate(glm::mat4(playerRot), -rotSpeed, glm::vec3(0.0f, 1.0f, 0.0f)));
                    break;
                case SDL_SCANCODE_ESCAPE:
                    m_engine.Stop();
                    break;
            }

            if (glm::length(moveDir) > 0.0f) {
                glm::vec3 proposedPos = playerPos + glm::normalize(moveDir) * moveSpeed;
                if (!CheckCollision(proposedPos)) {
                    playerPos = proposedPos;
                    m_engine.SendMsg(vve::System::MsgPlaySound{ vve::Filename{"../escape/assets/sounds/bump.wav"}, 1, 100 });
                }
            }
        }

        // --- Callbacks ---
        bool OnLoadLevel(Message message) {
            auto msg = message.template GetData<vve::System::MsgLoadLevel>();
            // std::cout << "Loading level: " << msg.m_level << std::endl;
    
            // initialise plane
            m_engine.SendMsg(vve::System::MsgSceneLoad{ vve::Filename{plane_obj}, aiProcess_FlipWindingOrder });
    
            auto m_handlePlane = m_registry.Insert(
                vve::Position{ {0.0f,0.0f,0.0f } },
                vve::Rotation{ glm::mat3(glm::rotate(glm::mat4(1.0f), 3.14159f / 2.0f, glm::vec3(1.0f,0.0f,0.0f))) },
                vve::Scale{ vec3_t{1000.0f,1000.0f,1000.0f} },
                vve::MeshName{ plane_mesh },
                vve::TextureName{ plane_txt },
                vve::UVScale{ { 1000.0f, 1000.0f } }
            );
    
            m_engine.SendMsg(MsgObjectCreate{ vve::ObjectHandle(m_handlePlane), vve::ParentHandle{} });
    
            // initialise player
            glm::mat3 p_rotation = glm::mat3( glm::rotate( glm::mat4(1.0f), glm::radians(180.0f), glm::vec3(0,0,1)) * glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1,0,0) ) );
            
            m_playerHandle = m_registry.Insert(
                vve::Position{ {0, 0, 0} },
                vve::Rotation{ p_rotation },
                vve::Scale{ vec3_t{1.0f} },
                vve::Name{ "Player" }
            );
            m_registry.AddTags(m_playerHandle, static_cast<size_t>(Tags::Tag_Retrievable));
            
            m_engine.SendMsg(MsgSceneCreate{
                vve::ObjectHandle(m_playerHandle), vve::ParentHandle{}, vve::Filename{player_obj}
            });

            // initialise camera
            GetCamera();
            m_engine.SendMsg(MsgObjectSetParent{
                vve::ObjectHandle{m_cameraNodeHandle},
                vve::ParentHandle{m_playerHandle}
            });
            m_registry.AddTags(m_cameraNodeHandle, static_cast<size_t>(Tags::Tag_Retrievable));
            
            m_registry.Get<vve::Position&>(m_cameraNodeHandle)() = glm::vec3(0.0f, 1.0f, -1.0f);
            
            auto& camPos = m_registry.Get<vve::Position&>(m_cameraNodeHandle)();
            glm::vec3 target = m_registry.Get<vve::Position&>(m_playerHandle)();
            glm::vec3 offsetTarget = target + glm::vec3(0.0f, 0.0f, 2.0f); 
            glm::mat4 viewMatrix = glm::lookAt(camPos, offsetTarget, glm::vec3(0.0f, 10.0f, 1.0f));
            m_registry.Get<vve::Rotation&>(m_cameraHandle)() = glm::mat3(glm::inverse(viewMatrix));

            // initialise map
            LoadMapAndSpawnWalls("../escape/assets/maps/map.txt");

            // initialise cypher machines
            SpawnCyphers(3); 

            // initialise bgm
            m_engine.SendMsg(vve::System::MsgPlaySound{ vve::Filename{"../escape/assets/sounds/stardew.wav"}, 2, 80 });
            m_engine.SendMsg(vve::System::MsgSetVolume{ static_cast<int>(m_volume) });
    
            std::thread listener(&MyGame::StartInputListener, this);
            listener.detach();
            
            // initialise UDP sender
            m_UDPsender.init("::1", 9999);
            // m_registry.Print();

            return false;
        }
    
        bool OnUpdate(Message& message) {
            auto msg = message.GetData<vve::System::MsgUpdate>();
            auto& playerPos = m_registry.Get<vve::Position&>(m_playerHandle)();
            auto& playerRot = m_registry.Get<vve::Rotation&>(m_playerHandle)();
        
            return false;
        }
    
        bool OnRecordNextFrame(Message message) {

            static bool showSceneObjects = false;
            static bool showSettings = true;

            if (showSceneObjects && showSettings) {
                showSettings = false;
            }
            if (showSettings && showSceneObjects) {
                showSceneObjects = false;
            }


            ImGui::SetNextWindowPos(ImVec2(10, 10));
            ImGui::SetNextWindowSize(ImVec2(300, 540));
            ImGui::SetNextWindowCollapsed(!showSceneObjects, ImGuiCond_Once);
            ImGui::Begin("Scene Objects");
            if (ImGui::IsWindowCollapsed()) showSceneObjects = false;
            else showSceneObjects = true;
        
            auto retrievables = m_registry.GetView<vve::Name, vve::Position>(std::vector<size_t>{Tags::Tag_Retrievable});
            for (auto [name, pos] : retrievables) {
                std::string objName = name;
                auto& objPos = pos;
                ImGui::Text("%s", objName.c_str());
                ImGui::Text("  Position: (%.2f, %.2f, %.2f)", objPos().x, objPos().y, objPos().z);
                ImGui::Separator();
            }
            
            ImGui::End();

            ImGui::SetNextWindowPos(ImVec2(10, 30));
            ImGui::SetNextWindowSize(ImVec2(300, 520));
            ImGui::SetNextWindowCollapsed(!showSettings, ImGuiCond_Once);
            ImGui::Begin("Scene Settings");
            if (ImGui::IsWindowCollapsed()) showSettings = false;
            else showSettings = true;

            ImGui::SeparatorText("Player Status");
            const char* stateText = (m_playerState == PlayerState::MOVING) ? "MOVING" : "STATIONARY";
            ImGui::Text("State: %s", stateText);
            auto& playerPos = m_registry.Get<vve::Position&>(m_playerHandle)();
            ImGui::Text("player z coordinate: %.2f", playerPos.y);
            ImGui::Text("player x coordinate: %.2f", playerPos.x);

            ImGui::SeparatorText("Light Settings");
            if (ImGui::BeginTabBar("Light Settings")) {
                if (ImGui::BeginTabItem("Spot Light")) {
                    for (auto [handle, name, position, rotation, spotLight] : m_registry.GetView<
                    vecs::Handle, vve::Name, vve::Position, vve::Rotation, vve::SpotLight&>()) {

                        auto& s = spotLight();

                        auto& rawPos = position();
                        auto& pointPos = m_registry.Get<vve::Position&>(handle)();
                        float pos[3] = { rawPos.x, rawPos.y, rawPos.z };

                        if (ImGui::SliderFloat3(("Position##" + std::string(name)).c_str(), pos, -50.0f, 50.0f)) {
                            pointPos.x = pos[0];
                            pointPos.y = pos[1];
                            pointPos.z = pos[2];
                        }
                        
                        ImGui::Text("");

                        auto& rawRot = rotation();
                        auto& pointRot = m_registry.Get<vve::Rotation&>(handle)();

                        glm::vec3 eulerAngles = glm::eulerAngles(glm::quat_cast(rawRot)); // radians

                        float degrees[3] = {
                            glm::degrees(eulerAngles.x),
                            glm::degrees(eulerAngles.y),
                            glm::degrees(eulerAngles.z)
                        };

                        std::string rotLabel = "Rotation##" + static_cast<std::string>(name);
                        if (ImGui::SliderFloat3(rotLabel.c_str(), degrees, -180.0f, 180.0f)) {
                            glm::vec3 newAngles = glm::radians(glm::vec3(degrees[0], degrees[1], degrees[2]));
                            glm::quat q = glm::quat(newAngles);
                            pointRot = glm::mat3_cast(q); 
                        }
                        
                        ImGui::Text("");

                        float color[4] = { s.color.r, s.color.g, s.color.b, 1.0f };
                        std::string label = "Color##" + static_cast<std::string>(name);
                        if (ImGui::ColorPicker4(label.c_str(), color)) {
                            s.color.r = color[0]; s.color.g = color[1]; s.color.b = color[2];
                        }
                    }
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Point Light")) {
                    for (auto [handle, name, position, pointLight] : m_registry.GetView<
                    vecs::Handle, vve::Name, vve::Position, vve::PointLight&>()) {
                        auto& p = pointLight();

                        auto& rawPos = position();
                        auto& pointPos = m_registry.Get<vve::Position&>(handle)();
                        float pos[3] = { rawPos.x, rawPos.y, rawPos.z };

                        if (ImGui::SliderFloat3(("Position##" + std::string(name)).c_str(), pos, -50.0f, 50.0f)) {
                            pointPos.x = pos[0];
                            pointPos.y = pos[1];
                            pointPos.z = pos[2];
                        }
                        
                        ImGui::Text("");

                        float color[4] = { p.color.r, p.color.g, p.color.b, 1.0f };
                        std::string label = "Color##" + static_cast<std::string>(name);
                        if (ImGui::ColorPicker4(label.c_str(), color)) {
                            p.color.r = color[0]; p.color.g = color[1]; p.color.b = color[2];
                        }
                    }
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Directional Light")) {
                    for (auto [handle, name, position, dirLight] : m_registry.GetView<
                    vecs::Handle, vve::Name, vve::Position, vve::DirectionalLight&>()) {
                        auto& d = dirLight();
                        
                        float color[4] = { d.color.r, d.color.g, d.color.b, 1.0f };
                        std::string label = "Color##" + static_cast<std::string>(name);
                        if (ImGui::ColorPicker4(label.c_str(), color)) {
                            d.color.r = color[0]; d.color.g = color[1]; d.color.b = color[2];
                        }
                    }
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            ImGui::End();
            // if (showControls) {
                
            // } else {
            //     showControls = false;
            //     showSceneObjects = true;
            // }
            
            // ImGui::CollapsingHeader("Control Panel");

            // if (ImGui::CollapsingHeader("Lighting Adjustments", &showLighting, ImGuiTreeNodeFlags_DefaultOpen)) {
            //     // If opened, collapse the other
            //     if (!showSceneObjects) showSceneObjects = false;

            //     const char* stateText = (m_playerState == PlayerState::MOVING) ? "MOVING" : "STATIONARY";
            //     ImGui::Text("State: %s", stateText);
            //     auto& playerPos = m_registry.Get<vve::Position&>(m_playerHandle)();
            //     ImGui::Text("player z coordinate: %.2f", playerPos.y);
            //     ImGui::Text("player x coordinate: %.2f", playerPos.x);
            //     ImGui::Separator();

            //     for (auto [handle, name, position, pointLight] : m_registry.GetView<
            //         vecs::Handle, vve::Name, vve::Position, vve::PointLight&>()) {
                    
            //         auto& p = pointLight();
            //         float color[4] = { p.color.r, p.color.g, p.color.b, 1.0f };
            //         std::string label = "Light Color##" + static_cast<std::string>(name);
            //         if (ImGui::ColorPicker4(label.c_str(), color)) {
            //             p.color.r = color[0]; p.color.g = color[1]; p.color.b = color[2];
            //         }
            //         ImGui::Separator();
            //     }

            //     for (auto [handle, name, position, dirLight] : m_registry.GetView<
            //         vecs::Handle, vve::Name, vve::Position, vve::DirectionalLight&>()) {

            //         auto& d = dirLight();
            //         float color[4] = { d.color.r, d.color.g, d.color.b, 1.0f };
            //         std::string label = "Light Color##" + static_cast<std::string>(name);
            //         if (ImGui::ColorPicker4(label.c_str(), color)) {
            //             d.color.r = color[0]; d.color.g = color[1]; d.color.b = color[2];
            //         }
            //         ImGui::Separator();
            //     }

            //     for (auto [handle, name, position, spotLight] : m_registry.GetView<
            //         vecs::Handle, vve::Name, vve::Position, vve::SpotLight&>()) {

            //         auto& s = spotLight();
            //         float color[4] = { s.color.r, s.color.g, s.color.b, 1.0f };
            //         std::string label = "Light Color##" + static_cast<std::string>(name);
            //         if (ImGui::ColorPicker4(label.c_str(), color)) {
            //             s.color.r = color[0]; s.color.g = color[1]; s.color.b = color[2];
            //         }
            //         ImGui::Separator();
            //     }
            // } else {
            //     showLighting = false;
            // }

            // if (ImGui::CollapsingHeader("Scene Objects", &showSceneObjects)) {
            //     // If opened, collapse the other
            //     if (!showLighting) showLighting = false;

            //     auto retrievables = m_registry.GetView<vve::Name, vve::Position>(
            //         std::vector<size_t>{Tags::Tag_Retrievable});

            //     for (auto [name, pos] : retrievables) {
            //         std::string objName = name;
            //         auto& objPos = pos;
            //         ImGui::Text("%s", objName.c_str());
            //         ImGui::Text("  Position: (%.2f, %.2f, %.2f)", objPos().x, objPos().y, objPos().z);
            //         ImGui::Separator();
            //     }
            // } else {
            //     showSceneObjects = false;
            // }

            // ImGui::End();
            return false;
        }

        bool OnFrameEnd(Message& message) {
            auto [rhandle, renderer] = vve::Renderer::GetState(m_registry);
            auto [whandle, window] = vve::Window::GetState(m_registry, "");

            if (!renderer.IsValid() || !window.IsValid()) return false;

            VkExtent2D extent = {
                static_cast<uint32_t>(window().m_width),
                static_cast<uint32_t>(window().m_height)
            };
            
            uint32_t imageSize = extent.width * extent.height * 4;
            VkImage image = renderer().m_swapChain.m_swapChainImages[renderer().m_imageIndex];

            uint8_t* dataImage = new uint8_t[imageSize];

            vh::ImgCopyImageToHost(
                renderer().m_device,
                renderer().m_vmaAllocator,
                renderer().m_graphicsQueue,
                renderer().m_commandPool,
                image,
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                dataImage,
                extent.width,
                extent.height,
                imageSize,
                2, 1, 0, 3
            );

            if (!m_ffmpegWriter) {
                m_ffmpegWriter = std::make_unique<FFmpegWriter>(extent.width, extent.height, "C:/Users/aanny/source/repos/fork/escape/videos/recording.h264");
            }

            m_ffmpegWriter->WriteFrame(dataImage);

            // After copying image to dataImage
            if (!m_ffmpegEncoder) {
                m_ffmpegEncoder = std::make_unique<FFmpegEncoder>(extent.width, extent.height);
            }
            
            auto [encodedData, encodedSize] = m_ffmpegEncoder->EncodeFrame(dataImage);
            if (encodedData && encodedSize > 0) {
                // std::cout << "Sending H264 frame: " << encodedSize << " bytes\n";
                m_UDPsender.send_fragmented((char*)encodedData, encodedSize);
                m_ffmpegEncoder->FreePacket();
            }
            
            // std::vector<uint8_t> encoded = m_udpSender.compress(dataImage, extent.width, extent.height);

            // m_udpSender.send((char*)encoded.data(), encoded.size());
            delete[] dataImage;

            m_frameLimiter.Wait();

            return true;

        }
        
        bool OnKeyDown(Message& message) {
            GetCamera();
        
            // Get component references
            auto [pn, rn, sn, LtoPn] = m_registry.template Get<vve::Position&, vve::Rotation&, vve::Scale&, vve::LocalToParentMatrix>(m_cameraNodeHandle);
            auto [pc, rc, sc, LtoPc] = m_registry.template Get<vve::Position&, vve::Rotation&, vve::Scale&, vve::LocalToParentMatrix>(m_cameraHandle);
    
            auto& playerPos = m_registry.Get<vve::Position&>(m_playerHandle)();
            auto& playerRot = m_registry.Get<vve::Rotation&>(m_playerHandle)();
        
            int key; 
            float dt;
            if (message.HasType<MsgKeyDown>()) {
                auto msg = message.template GetData<MsgKeyDown>();
                key = msg.m_key;
                dt = msg.m_dt;
                
            } else {
                auto msg = message.template GetData<MsgKeyRepeat>();
                key = msg.m_key;
                dt = msg.m_dt;
            }
        
            float moveSpeed = dt * 5.0f;
            float rotSpeed = glm::radians(90.0f) * dt; // 90 degrees/sec
        
            glm::vec3 moveDir{0.0f};
        
            // Define local movement directions
            glm::vec3 localForward = glm::vec3(0.0f, 0.0f, 1.0f); // +Z
            glm::vec3 localRight   = glm::vec3(1.0f, 0.0f, 0.0f); // +X
        
            // Convert local directions into world directions
            glm::vec3 forward = playerRot * localForward;
            glm::vec3 right   = playerRot * localRight;
            // glm::vec3 c_forward = camNodeRot * localForward;
            // glm::vec3 c_right   = camNodeRot * localRight;
    
            // for Camera
            glm::vec3 translate(0.0f);
            glm::vec3 axis1(1.0f), axis2(1.0f);
            float angle1 = 0.0f, angle2 = 0.0f;
            int dx = 0, dy = 0;
            
    
            switch (key) {
                case SDL_SCANCODE_W:
                    moveDir += forward;
                    break;
                case SDL_SCANCODE_S:
                    moveDir -= forward;
                    break;
                case SDL_SCANCODE_A:
                    moveDir += right;
                    break;
                case SDL_SCANCODE_D:
                    moveDir -= right;
                    break;
            
                case SDL_SCANCODE_LEFT: {
                    glm::vec3 rotAxis = glm::vec3(0.0f, 1.0f, 0.0f); // rotate around Z
                    playerRot = glm::mat3(glm::rotate(glm::mat4(playerRot), rotSpeed, rotAxis));
                    dx = -1;
                    break;
                }
                case SDL_SCANCODE_RIGHT: {
                    glm::vec3 rotAxis = glm::vec3(0.0f, 1.0f, 0.0f); // rotate around Z
                    playerRot = glm::mat3(glm::rotate(glm::mat4(playerRot), -rotSpeed, rotAxis));
                    dx = 1;
                    break;
                }
                case SDL_SCANCODE_SPACE: {
                    if (message.HasType<MsgKeyRepeat>()) break;
    
                    // if (m_cubeCollected > 0) {
                    //     glm::vec3 forward = playerRot * glm::vec3(0.0f, 0.0f, 1.0f);
                    //     glm::vec3 placePos = playerPos + forward * 1.5f + glm::vec3(0.0f, 0.0f, 0.5f); // 1.5 units ahead of player
    
                    //     vecs::Handle placedCube = m_registry.Insert(
                    //         vve::Position{ placePos },
                    //         vve::Rotation{ mat3_t{1.0f} },
                    //         vve::Scale{ vec3_t{1.0f} }
                    //     );
    
                    //     m_cubeHandles.push_back(placedCube);
    
                    //     m_engine.SendMsg(MsgSceneCreate{
                    //         vve::ObjectHandle(placedCube), vve::ParentHandle{}, vve::Filename{cube_obj}, aiProcess_FlipWindingOrder
                    //     });
                    //     m_engine.SendMsg(MsgPlaySound{ vve::Filename{"assets/sounds/putdown.wav"}, 1, 50 });
    
                    //     m_cubeCollected--; 
                    // }
                    break;
                }
    
                case SDL_SCANCODE_ESCAPE: {
                    const char* shutdownMsg = "__SHUTDOWN__";
                    m_UDPsender.send_fragmented((char*)shutdownMsg, strlen(shutdownMsg));
                    m_engine.Stop();
                    break;
                }                
            }
    
            if (glm::length(moveDir) > 0.0f) {
                moveDir = glm::normalize(moveDir) * moveSpeed;
                glm::vec3 proposedPos = playerPos + moveDir;
                m_playerState = PlayerState::MOVING;
                bool collision = CheckCollision(proposedPos);
                if (!collision) {
                    playerPos = proposedPos;
                    m_lastCollisionState = false; // no collision now
                } else {
                    m_playerState = PlayerState::STATIONARY;
                    if (!m_lastCollisionState) { // only if new collision!
                        m_engine.SendMsg(MsgPlaySound{ vve::Filename{"assets/sounds/bump.wav"}, 1, 100 });
                        std::cout << "boink" << std::endl;
                    }
                    m_lastCollisionState = true; // remember that we are colliding now
                }
            }
            
            return true;
        }
    };

int main() {
    startWinsock();
    vve::Engine engine("My Engine", VK_MAKE_VERSION(1, 3, 0)) ;
    MyGame mygui{engine};  
    engine.Run();
    WSACleanup();
    return 0;
}
    
    