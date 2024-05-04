#include <time.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_net.h>
#include <cstring>
#include <iostream>
#include "fstream"
#include<time.h>

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
uint8_t inbuf[INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
uint8_t *data;
size_t data_size;
AVPacket *pkt;
SDL_Rect rect;

void clean() {
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(screen);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&c);
}

void display(AVCodecContext* ctx, AVPacket* pkt, AVFrame* frame, SDL_Rect* rect, SDL_Texture* texture, SDL_Renderer* renderer, double fpsrend) {
    int framenum = ctx->frame_number;
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

	memset(inbuf + INBUF_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);
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
	SDL_RenderSetLogicalSize(renderer, 1920, 1080);

	bool done = false;
	SDL_Event evt;
	uint32_t windowID = SDL_GetWindowID(screen);

	cout << "Starting server..." << endl;

    const int MAX_SOCKETS = 1;
	TCPsocket server;
	IPaddress ip;
    SDLNet_SocketSet socket_set;
    TCPsocket sockets[MAX_SOCKETS];
	if (SDLNet_ResolveHost(&ip, NULL, 8080) == -1) {
		cout << "SDLNet_ResolveHost: " << SDLNet_GetError();
		exit(1);
	}
	server = SDLNet_TCP_Open(&ip);
	if (!server) {
		cout << "SDLNet_TCP_Open: " << SDLNet_GetError();
		exit(1);
	}

    socket_set = SDLNet_AllocSocketSet(MAX_SOCKETS+1);
    if(socket_set == NULL) {
        cout << "Could not allocate socket set";
        exit(1);
    }
    if(SDLNet_TCP_AddSocket(socket_set, server) == -1) {
        cout << "Could not add server socket to set";
        exit(1);
    }
	while (!done) {
        while (SDL_PollEvent(&evt)) {
            if(evt.type == SDL_QUIT)
                done = true;
            else if(evt.type == SDL_KEYDOWN) {
	        if(sockets[0]) {
			//string send = "a" + to_string('0' + htonl(evt.key.keysym.sym));
			char ok = evt.key.keysym.sym;

			// Combine 'a' with the character and return as a char*
			std::string combined_string = "0";
			combined_string += ok;

			// Allocate memory for the resulting string (including null terminator)
			char* send = new char[combined_string.length() + 1];

			// Copy the combined string to the allocated memory
			std::strcpy(send, combined_string.c_str());

			int len = SDLNet_TCP_Send(sockets[0], send, sizeof(send));
			if(len == 0) {
				cout << SDLNet_GetError();
			}
		}
            } else if (evt.type == SDL_MOUSEMOTION) {
		    if(sockets[0]) {
                string ok = "1" + to_string((float) evt.motion.x / 1920) + "a" + to_string((float) evt.motion.y / 1080);
                char* send = new char[ok.length() + 1];
                strcpy(send, ok.c_str());
                if(evt.motion.x >= 0 && evt.motion.y >= 0 && evt.motion.x <=1920 && evt.motion.y<=1080) {
                    cout << send << endl;
                    int len = SDLNet_TCP_Send(sockets[0], send, ok.length()+1);
                    if(len == 0) {
                        cout << SDLNet_GetError();
                    }
                }
		    }
            } else if(evt.type == SDL_MOUSEBUTTONDOWN) {
		    if(sockets[0]) {
			    string ok;
			    if(evt.button.button == SDL_BUTTON_LEFT) {
				    ok = "2";
				    char* send = new char[ok.length() + 1];
				    strcpy(send, ok.c_str());
				    int len = SDLNet_TCP_Send(sockets[0], send, 1);
				    if(len == 0) {
					    cout << SDLNet_GetError();
				    }
			    }
		    }
            }
        }

        /* read the buffer from client */
        int ready = SDLNet_CheckSockets(socket_set, 0);
        if(ready > 0) {
            if(SDLNet_SocketReady(server)) {
                /* try to accept a connection */
                sockets[0] = SDLNet_TCP_Accept(server);
                SDLNet_TCP_AddSocket(socket_set, sockets[0]);
                if (!sockets[0]) { /* no connection accepted */
                    SDL_Delay(100); /*sleep 1/10th of a second */
                    continue;
                }

                /* get the clients IP and port number */
                IPaddress *remoteip;
                remoteip = SDLNet_TCP_GetPeerAddress(sockets[0]);
                if (!remoteip) {
                    printf("SDLNet_TCP_GetPeerAddress: %s\n", SDLNet_GetError());
                    continue;
                }

                /* print out the clients IP and port number */
                Uint32 ipaddr;
                ipaddr = SDL_SwapBE32(remoteip->host);
                printf("Accepted a connection from %d.%d.%d.%d port %hu\n", ipaddr >> 24,
                        (ipaddr >> 16) & 0xff, (ipaddr >> 8) & 0xff, ipaddr & 0xff,
                        remoteip->port);
            } else {
                char message[1000000];
                int len = SDLNet_TCP_Recv(sockets[0], message, 1000000);
                if (!len) {
                    break;
                }
                /* print out the message */
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

    SDLNet_TCP_Close(sockets[0]);
	clean();
	SDLNet_Quit();
	SDL_Quit();
}
