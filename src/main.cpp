#include <cstring>
#include <iostream>
#include <thread>

#include <SDL2/SDL.h>
#include <SDL2/SDL_net.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

using namespace std;

#define INBUF_SIZE 50000
#define PORT 3478

SDL_Window *screen;
SDL_Renderer *renderer;
SDL_Texture *texture;
const AVCodec *codec;
AVCodecParserContext *parser;
AVCodecContext *c = NULL;
AVFrame *frame;
uint8_t *data;
size_t data_size;
AVPacket *pkt;
SDL_Rect rect;
UDPsocket sock = NULL;
UDPpacket *packet;
SDLNet_SocketSet socket_set;
bool haveClient = false, firstReceive = true;

void clean() {
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(screen);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&c);
}

void display(AVCodecContext* ctx, AVPacket* pkt, AVFrame* frame, SDL_Rect* rect, SDL_Texture* texture, SDL_Renderer* renderer, double fpsrend) {
#if AV_VERSION_MAJOR(AVCODEC_VERSION) < 60
    int framenum = ctx->frame_number;
#else
    int framenum = ctx->frame_num;
#endif
    SDL_UpdateYUVTexture(texture, rect,
            frame->data[0], frame->linesize[0],
            frame->data[1], frame->linesize[1],
            frame->data[2], frame->linesize[2]);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, rect);
    SDL_RenderPresent(renderer);
}

static void decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt)
{
    int ret;

    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        exit(1);
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            exit(1);
        }

        display(c, pkt, frame, &rect, texture, renderer, 30.0);
        fflush(stdout);
    }
}

void keepAlive() { 
    while(1) {
        SDL_Delay(200);
        if(haveClient) {
            string opcode = "7";
            char* send = new char[opcode.length() + 1];
            strcpy(send, opcode.c_str());
            packet->len = strlen(send);
            memcpy(packet->data, send, packet->len);
            int len = SDLNet_UDP_Send(sock, -1, packet);
            if(len == 0) {
                cout << SDLNet_GetError();
            }
        }
    }
}

int main(int argc, char **argv) {
    thread t(keepAlive);
    // ffmpeg setup

    pkt = av_packet_alloc();
    if (!pkt) {
        cout << "error allocating packet" << endl;
        exit(1);
    }

    codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if(!codec) {
	    cout << "error finding codec" << endl;
	    exit(1);
    }

    parser = av_parser_init(codec->id);
    if (!parser) {
        cout << "Error finding parser" << endl;
        exit(1);
    }

    c = avcodec_alloc_context3(codec);
    if (!c) {
        cout << "Could not allocate video context memory" << endl;
        exit(1);
    }

    if (avcodec_open2(c, codec, NULL) < 0) {
        cout << "Could not open codec" << endl;
        exit(1);
    }

    frame = av_frame_alloc();
    if (!frame) {
        cout << "could not allocate video frame" << endl;
        exit(1);
    }

    // sdl setup
    if (SDL_Init(0) == -1) {
        cout << "SDL_Init: " << SDL_GetError();
        exit(1);
    }
    if (SDLNet_Init() == -1) {
        cout << "SDLNet_Init: " << SDLNet_GetError();
        exit(1);
    }

    screen = SDL_CreateWindow("screenShareApp", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            1280, 720, SDL_WINDOW_OPENGL);

    renderer = SDL_CreateRenderer(screen, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        clean();
        SDL_Quit();
        return -1;
    }
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
            SDL_TEXTUREACCESS_STREAMING | SDL_TEXTUREACCESS_TARGET,
            1920, 1080);
    if (!texture) {
        clean();
        SDL_Quit();
        return -1;
    }

    rect.x = 0;
    rect.y = 0;
    rect.w = 1920;
    rect.h = 1080;

    // IMGUI setup
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui_ImplSDL2_InitForSDLRenderer(screen, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    bool done = false;
    SDL_Event evt;
    uint32_t windowID = SDL_GetWindowID(screen);

    //IMGUI state variables
    char port[20] = ""; 
    char ipToTry[30] = "";
    int len = 0;
    packet = SDLNet_AllocPacket(INBUF_SIZE);
    UDPpacket *recv = SDLNet_AllocPacket(INBUF_SIZE);
    socket_set = SDLNet_AllocSocketSet(1);
    if(socket_set == NULL) {
        cout << "Could not allocate socket set";
        exit(1);
    }
    bool submit = false;

    while (!done) {
        while (SDL_PollEvent(&evt)) {
            ImGui_ImplSDL2_ProcessEvent(&evt);
            switch(evt.type) {
                case SDL_QUIT:
                    done = true;
                    break;

                case SDL_KEYDOWN: {
                    if(haveClient) {
                        int c = evt.key.keysym.sym;

                        // Combine '0' with the character and return as a char*
                        std::string combined_string = "0" + to_string(c);

                        // Allocate memory for the resulting string (including null terminator)
                        char* send = new char[combined_string.length() + 1];

                        // Copy the combined string to the allocated memory
                        std::strcpy(send, combined_string.c_str());
                        cout << send << endl;

                        packet->len = strlen(send);
                        memcpy(packet->data, send, packet->len);
                        len = SDLNet_UDP_Send(sock, -1, packet);
                        if(len == 0) {
                                cout << SDLNet_GetError();
                        }
                    }
                    break;
                }

                case SDL_KEYUP: {
                    if(haveClient) {
                        int c = evt.key.keysym.sym;

                        // Combine '6' with the character and return as a char*
                        std::string combined_string = "6" + to_string(c);

                        // Allocate memory for the resulting string (including null terminator)
                        char* send = new char[combined_string.length() + 1];

                        // Copy the combined string to the allocated memory
                        std::strcpy(send, combined_string.c_str());

                        packet->len = strlen(send);
                        memcpy(packet->data, send, packet->len);
                        len = SDLNet_UDP_Send(sock, -1, packet);
                        if(len == 0) {
                            cout << SDLNet_GetError();
                        }
                    }
                    break;
                }
                case SDL_MOUSEMOTION: {

                    if(haveClient) {
                        string ok = "1" + to_string((float) evt.motion.x / 1920) + "a" + to_string((float) evt.motion.y / 1080);
                        char* send = new char[ok.length() + 1];
                        strcpy(send, ok.c_str());
                        if(evt.motion.x >= 0 && evt.motion.y >= 0 && evt.motion.x <=1920 && evt.motion.y<=1080) {
                            packet->len = strlen(send);
                            memcpy(packet->data, send, packet->len);
                            len = SDLNet_UDP_Send(sock, -1, packet);
                            if(len == 0) {
                                cout << SDLNet_GetError();
                            }
                        }
                    }
                    break;
               }

                case SDL_MOUSEBUTTONDOWN: {

                    if(haveClient) {
                        string opcode;
                        switch(evt.button.button) {
                            case SDL_BUTTON_LEFT: {
                                opcode = "2";
                                char* send = new char[opcode.length() + 1];
                                strcpy(send, opcode.c_str());
                                cout << "leftDown" << endl;
                                packet->len = strlen(send);
                                memcpy(packet->data, send, packet->len);
                                len = SDLNet_UDP_Send(sock, -1, packet);
                                if(len == 0) {
                                    cout << SDLNet_GetError();
                                }
                                break;
                            }
                            case SDL_BUTTON_RIGHT: {
                                opcode = "3";
                                char* send = new char[opcode.length() + 1];
                                strcpy(send, opcode.c_str());
                                cout << "rightDown" << endl;
                                packet->len = strlen(send);
                                memcpy(packet->data, send, packet->len);
                                len = SDLNet_UDP_Send(sock, -1, packet);
                                if(len == 0) {
                                    cout << SDLNet_GetError();
                                }
                                break;
                            }
                        }
                    }
                    break;
                }
                case SDL_MOUSEBUTTONUP: {
                    if(haveClient) {
                        string opcode;
                        switch(evt.button.button) {
                            case SDL_BUTTON_LEFT: {
                                opcode = "4";
                                char* send = new char[opcode.length() + 1];
                                strcpy(send, opcode.c_str());
                                cout << "leftup" << endl;
                                packet->len = strlen(send);
                                memcpy(packet->data, send, packet->len);
                                len = SDLNet_UDP_Send(sock, -1, packet);
                                if(len == 0) {
                                    cout << SDLNet_GetError();
                                }
                                break;
                            }
                            case SDL_BUTTON_RIGHT: {
                                opcode = "5";
                                char* send = new char[opcode.length() + 1];
                                strcpy(send, opcode.c_str());
                                cout << "rightup" << endl;
                                packet->len = strlen(send);
                                memcpy(packet->data, send, packet->len);
                                len = SDLNet_UDP_Send(sock, -1, packet);
                                if(len == 0) {
                                    cout << SDLNet_GetError();
                                }
                                break;
                            }
                        }
                    }
                    break;
                }
            }
        }

        if(!haveClient) {
            SDL_RenderSetLogicalSize(renderer, 0, 0);
            ImGui_ImplSDLRenderer2_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();
            ImGui::Begin("test", 0, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground);

            ImGui::SetWindowFontScale(1.2);
            ImGui::SetWindowPos(ImVec2((float)0, (float)0));
            ImGui::SetWindowSize(ImVec2((float)400 * 2, (float)300));

            ImGui::Text("Remote Desktop App");
            ImGui::NewLine();
            ImGui::InputText("IP Address", ipToTry, IM_ARRAYSIZE(ipToTry));
            ImGui::InputText("Port", port, IM_ARRAYSIZE(port));
            if (ImGui::Button("Submit")) {
                submit = true;
                /* ImGui::OpenPopup("Disconnect"); */
            }
            if(submit) {
                ImGui::Text("Connecting...");
            }

            /* if (ImGui::BeginPopupModal("Disconnect", NULL, ImGuiWindowFlags_AlwaysAutoResize)) */
            /* { */
            /*     ImGui::Button("Disconnect"); */
            /*     ImGui::EndPopup(); */
            /* } */

            ImGui::End();

            ImGui::Render();

            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
            SDL_RenderPresent(renderer);

            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
        } else {
            /* ImGui::Image((void*)texture, ImVec2(10, 10)); */
            SDL_RenderSetLogicalSize(renderer, 1920, 1080);
        }

        if(submit) {
            submit = false;
            IPaddress ip;
            long temp = strtol(port, NULL, 10);
            if (SDLNet_ResolveHost(&ip, "167.234.216.217", (uint16_t) PORT) == -1) {
                cout << "SDLNet_ResolveHost: " << SDLNet_GetError();
            } else {
                sock = SDLNet_UDP_Open(0);
                if (!sock) {
                    cout << "SDLNet_UDP_Open: " << SDLNet_GetError();
                    exit(1);
                } else {
                    static const char* data = "0";
                    packet->len = strlen(data) + 1;
                    packet->address = ip;
                    memcpy(packet->data, data, packet->len);
                    SDLNet_UDP_Send(sock, -1, packet);
                    int count = 0;
                    while(SDLNet_UDP_Recv(sock, recv) <= 0 && count < 5) {
                        SDL_Delay(500);
                        count++;
                        cout << count << endl;
                    }
                    if(count < 5) {
                        haveClient = true;
                        recv->data[recv->len] = '\0';
                        string ipPort = string((char*)recv->data);

                        cout << ipPort << endl;
                        if (SDLNet_ResolveHost(&ip, ipPort.substr(0, ipPort.find(":")).c_str(), (uint16_t) stoi(ipPort.substr(ipPort.find(":") + 1))) == -1) {
                            /* if (SDLNet_ResolveHost(&ip, "172.30.176.1", (uint16_t) stoi(ipPort.substr(ipPort.find(":") + 1))) == -1) { */
                            cout << "SDLNet_ResolveHost: " << SDLNet_GetError();
                        } else {
                            cout << "setting peer address and port" << endl;
                            packet->address = ip;
                        }
                        SDLNet_UDP_AddSocket(socket_set, sock);

                    }
                }
            }
        }
        /* read the buffer from sock */
        if(haveClient) {
            int ready;
            if(firstReceive) {
                ready = SDLNet_CheckSockets(socket_set, 30000);
                if(ready > 0) {
                    firstReceive = false;
                }
            } else {
                ready = SDLNet_CheckSockets(socket_set, 1000);
            }
            if(ready > 0) {
                SDLNet_UDP_Recv(sock, recv);
                /* cout << recv->len << endl; */
                /* cout << recv->data << endl; */
                /* print out the message */
                uint8_t *data = recv->data;
                size_t   data_size = recv->len;
                int ret;
                while(data_size > 0) {
                    ret = av_parser_parse2(parser, c, &pkt->data, &pkt->size,
                            data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                    if (ret < 0) {
                        fprintf(stderr, "Error while parsing\n");
                        exit(1);
                    }
                    data      += ret;
                    data_size -= ret;

                    if (pkt->size)
                        decode(c, frame, pkt);
                }
            } else {
                SDLNet_UDP_Close(sock);
                SDLNet_UDP_DelSocket(socket_set, sock);
                sock = NULL;
                haveClient = false;
                firstReceive = true;
            }
        }
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDLNet_UDP_Close(sock);

    clean();
    SDLNet_Quit();
    SDL_Quit();
}
