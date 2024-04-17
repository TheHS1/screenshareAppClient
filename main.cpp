#include <time.h>
#include <SDL2/SDL.h>
extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
}

using namespace std;

AVFormatContext *pFormatCtx;
AVCodecContext *vidCtx;
AVCodecParameters *vidpar;
AVFrame *vframe, *aframe;
AVPacket *packet;

SDL_Window *screen;
SDL_Renderer *renderer;
SDL_Texture *texture;

void clean() {
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(screen);
    av_packet_free(&packet);
    av_frame_free(&vframe);
    av_frame_free(&aframe);
    avcodec_free_context(&vidCtx);
    avformat_close_input(&pFormatCtx);
    avformat_free_context(pFormatCtx);
}

void display(AVCodecContext* ctx, AVPacket* pkt, AVFrame* frame, SDL_Rect* rect, SDL_Texture* texture, SDL_Renderer* renderer, double fpsrend) {
    time_t start = time(NULL);
    if (avcodec_send_packet(ctx, pkt) < 0) {
        return;
    }
    if (avcodec_receive_frame(ctx, frame) < 0) {
        return;
    }
    int framenum = ctx->frame_number;
    if ((framenum % 1000) == 0) {
        printf("Frame %d (size=%d pts %d dts %d key_frame %d"
            " [ codec_picture_number %d, display_picture_number %d\n",
            framenum, frame->pkt_size, frame->pts, frame->pkt_dts, frame->key_frame,
            frame->coded_picture_number, frame->display_picture_number);
    }
    SDL_UpdateYUVTexture(texture, rect,
        frame->data[0], frame->linesize[0],
        frame->data[1], frame->linesize[1],
        frame->data[2], frame->linesize[2]);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, rect);
    SDL_RenderPresent(renderer);
    time_t end = time(NULL);
    double diffms = difftime(end, start) / 1000.0;
    if (diffms < fpsrend) {
        uint32_t diff = (uint32_t)((fpsrend - diffms) * 1000);
        printf("diffms: %f, delay time %d ms.\n", diffms, diff);
        SDL_Delay(diff);
    }
}

int main(int argc, char *argv[]) {
    int vidId = -1;
    double fpsrendering = 0.0;
    const AVCodec *vidCodec;

    int swidth, sheight;
    SDL_Rect rect;

    SDL_Init(SDL_INIT_EVERYTHING);
    pFormatCtx = avformat_alloc_context();
    char bufmsg[1024];
    if (avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) < 0) {
        clean();
        SDL_Quit();
        return -1;
    }
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
        clean();
        SDL_Quit();
        return -1;
    }
    bool foundVideo = false;
    for (int i = 0; i < pFormatCtx->nb_streams; i++) {
        AVCodecParameters *localparam = pFormatCtx->streams[i]->codecpar;
        const AVCodec *localcodec = avcodec_find_decoder(localparam->codec_id);
        if (localparam->codec_type == AVMEDIA_TYPE_VIDEO && !foundVideo) {
            vidCodec = localcodec;
            vidpar = localparam;
            vidId = i;
            AVRational rational = pFormatCtx->streams[i]->avg_frame_rate;
            fpsrendering = 1.0 / ((double)rational.num / (double)(rational.den));
            foundVideo = true;
        }
        if (foundVideo) { break; }
    }
    vidCtx = avcodec_alloc_context3(vidCodec);
    if (avcodec_parameters_to_context(vidCtx, vidpar) < 0) {
        clean();
        SDL_Quit();
        return -1;
    }
    if (avcodec_open2(vidCtx, vidCodec, NULL) < 0) {
        clean();
        SDL_Quit();
        return -1;
    }

    vframe = av_frame_alloc();
    aframe = av_frame_alloc();
    packet = av_packet_alloc();
    swidth = vidpar->width;
    sheight = vidpar->height;
    screen = SDL_CreateWindow("screenShareApp", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        swidth, sheight, SDL_WINDOW_OPENGL);
    if (!screen) {
        clean();
        SDL_Quit();
        return -1;
    }
    renderer = SDL_CreateRenderer(screen, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        clean();
        SDL_Quit();
        return -1;
    }
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
        SDL_TEXTUREACCESS_STREAMING | SDL_TEXTUREACCESS_TARGET,
        swidth, sheight);
    if (!texture) {
        clean();
        SDL_Quit();
        return -1;
    }
    rect.x = 0;
    rect.y = 0;
    rect.w = swidth;
    rect.h = sheight;

    SDL_Event evt;
    uint32_t windowID = SDL_GetWindowID(screen);
    bool running = true;
    while (running) {
        while (av_read_frame(pFormatCtx, packet) >= 0)
        {
            while (SDL_PollEvent(&evt))
            {
                switch (evt.type) {
                    case SDL_WINDOWEVENT: {
                        if (evt.window.windowID == windowID) {
                            switch (evt.window.event) {
                                case SDL_WINDOWEVENT_CLOSE: {
                                    evt.type = SDL_QUIT;
                                    running = false;
                                    SDL_PushEvent(&evt);
                                    break;
                                }
                            };
                        }
                        break;
                    }
                    case SDL_QUIT: {
                        running = false;
                        break;
                    }
                }
            }
            if (packet->stream_index == vidId) {
                display(vidCtx, packet, vframe, &rect,
                    texture, renderer, fpsrendering);

            }
            av_packet_unref(packet);
        }
    }

    clean();
    SDL_Quit();
    return 0;
}
