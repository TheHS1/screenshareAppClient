#include <cstring>
#include <iostream>

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
const int MAX_SOCKETS = 1;
TCPsocket client = NULL;
SDLNet_SocketSet socket_set;
TCPsocket sockets[MAX_SOCKETS];

void clean() {
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(screen);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&c);
}

void display(AVCodecContext* ctx, AVPacket* pkt, AVFrame* frame, SDL_Rect* rect, SDL_Texture* texture, SDL_Renderer* renderer, double fpsrend) {
    int framenum = ctx->frame_num;
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

int main(int argc, char **argv) {
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

    cout << "Starting server..." << endl;

    //IMGUI state variables
    char port[20];
    char ipToTry[20];
    bool submit;
    socket_set = SDLNet_AllocSocketSet(MAX_SOCKETS);
    if(socket_set == NULL) {
        cout << "Could not allocate socket set";
        exit(1);
    }

    while (!done) {
        while (SDL_PollEvent(&evt)) {
            ImGui_ImplSDL2_ProcessEvent(&evt);
            switch(evt.type) {
                case SDL_QUIT:
                    done = true;

                case SDL_KEYDOWN: {
                    if(client) {
                        switch(evt.key.keysym.sym) {
                            case SDLK_LSHIFT:
                            case SDLK_RSHIFT: {
                                const char send[] = "6";

                                int len = SDLNet_TCP_Send(client, send, sizeof(send));
                                if(len == 0) {
                                    cout << SDLNet_GetError();
                                }
                                break;
                            }
                            default: {
                                char c = evt.key.keysym.sym;

                                // Combine '0' with the character and return as a char*
                                std::string combined_string = "0";
                                combined_string += c;

                                // Allocate memory for the resulting string (including null terminator)
                                char* send = new char[combined_string.length() + 1];

                                // Copy the combined string to the allocated memory
                                std::strcpy(send, combined_string.c_str());

                                int len = SDLNet_TCP_Send(client, send, sizeof(send));
                                if(len == 0) {
                                    cout << SDLNet_GetError();
                                }
                                break;
                            }
                        }
                    }
                    break;
                }

                case SDL_KEYUP: {
                    if(client) {
                        switch(evt.key.keysym.sym) {
                            case SDLK_LSHIFT:
                            case SDLK_RSHIFT: {
                                const char send[] = "7";

                                int len = SDLNet_TCP_Send(client, send, sizeof(send));
                                if(len == 0) {
                                    cout << SDLNet_GetError();
                                }
                                break;
                            }
                            default: {
                                char c = evt.key.keysym.sym;

                                // Combine '8' with the character and return as a char*
                                std::string combined_string = "8";
                                combined_string += c;

                                // Allocate memory for the resulting string (including null terminator)
                                char* send = new char[combined_string.length() + 1];

                                // Copy the combined string to the allocated memory
                                std::strcpy(send, combined_string.c_str());

                                int len = SDLNet_TCP_Send(client, send, sizeof(send));
                                if(len == 0) {
                                    cout << SDLNet_GetError();
                                }
                                break;
                            }
                        }
                    }
                    break;
                }


                case SDL_MOUSEMOTION: {

                    if(client) {
                        string ok = "1" + to_string((float) evt.motion.x / 1920) + "a" + to_string((float) evt.motion.y / 1080);
                        char* send = new char[ok.length() + 1];
                        strcpy(send, ok.c_str());
                        if(evt.motion.x >= 0 && evt.motion.y >= 0 && evt.motion.x <=1920 && evt.motion.y<=1080) {
                            int len = SDLNet_TCP_Send(client, send, ok.length()+1);
                            if(len == 0) {
                                cout << SDLNet_GetError();
                            }
                        }
                    }
                    break;
               }

                case SDL_MOUSEBUTTONDOWN: {

                    if(client) {
                        string opcode;
                        switch(evt.button.button) {
                            case SDL_BUTTON_LEFT: {
                                opcode = "2";
                                char* send = new char[opcode.length() + 1];
                                strcpy(send, opcode.c_str());
                                cout << "leftDown" << endl;
                                int len = SDLNet_TCP_Send(client, send, 1);
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
                                int len = SDLNet_TCP_Send(client, send, 1);
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
                    if(client) {
                        string opcode;
                        switch(evt.button.button) {
                            case SDL_BUTTON_LEFT: {
                                opcode = "4";
                                char* send = new char[opcode.length() + 1];
                                strcpy(send, opcode.c_str());
                                cout << "leftup" << endl;
                                int len = SDLNet_TCP_Send(client, send, 1);
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
                                int len = SDLNet_TCP_Send(client, send, 1);
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

        if(!client) {
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
            }

            ImGui::End();

            ImGui::Render();

            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
            SDL_RenderPresent(renderer);

            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
        } else {
            SDL_RenderSetLogicalSize(renderer, 1920, 1080);
        }

        if(submit) {
            submit = false;
            IPaddress ip;
            long temp = strtol(port, NULL, 10);
            if (SDLNet_ResolveHost(&ip, ipToTry, (uint16_t) temp) == -1) {
                cout << "SDLNet_ResolveHost: " << SDLNet_GetError();
                exit(1);
            }
            client = SDLNet_TCP_Open(&ip);
            if (!client) {
                cout << "SDLNet_TCP_Open: " << SDLNet_GetError();
                exit(1);
            }
            SDLNet_TCP_AddSocket(socket_set, client);
        }
        /* read the buffer from client */
        int ready = SDLNet_CheckSockets(socket_set, 0);
        if (ready > 0) {
            char message[INBUF_SIZE];
            int len = SDLNet_TCP_Recv(client, message, INBUF_SIZE);
            if(len <= 0 && client) {
                SDLNet_TCP_Close(client);
                SDLNet_TCP_DelSocket(socket_set, client);
                client = NULL;
            }

            /* print out the message */
            if (client) {
                uint8_t *data = (uint8_t*) message;
                size_t   data_size = len;
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
                //printf("Received (%i): \n", len);
            }
        }
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    clean();
    SDLNet_Quit();
    SDL_Quit();
}
