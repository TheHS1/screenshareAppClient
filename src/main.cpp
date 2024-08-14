#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

// GUI
#include <SDL2/SDL.h>
#include <SDL2/SDL_net.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

// FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

// Encryption
#include <openssl/evp.h>
#include <openssl/aes.h>

using namespace std;

#define INBUF_SIZE 50000
#define PORT 3478
constexpr auto maxPacketCount = 256 * 60;
#define maxByteVal 256

// threads
atomic<bool> run = true;
mutex retransMutex;

// SDL
SDL_Window *screen;
SDL_Renderer *renderer;
SDL_Texture *texture;

// FFMPEG
const AVCodec *codec;
AVCodecParserContext *parser;
AVCodecContext *c = NULL;
AVFrame *frame;
uint8_t *data;
size_t data_size;
AVPacket *pkt;
SDL_Rect rect;

// sockets
UDPsocket sock = NULL;
UDPpacket *packet;
SDLNet_SocketSet socket_set;
bool haveClient = false, firstReceive = true;

enum sendPacketTypes {
    NUMBERED = 1,
    RETRANSMIT = 2,
    UNNUMBERED = 3
};

/* ctx structures that libcrypto used to record encryption/decryption status */
EVP_CIPHER_CTX* en = EVP_CIPHER_CTX_new();
EVP_CIPHER_CTX* de = EVP_CIPHER_CTX_new();

// Create 128 bit key and IV using key_data and 8 byte salt and initializes ctx objects
int aes_init(string key_data, int key_data_len, unsigned char* salt, EVP_CIPHER_CTX* e_ctx,
		EVP_CIPHER_CTX* d_ctx)
{
	int i, nrounds = 5;
	unsigned char key[32], iv[32];

	/*
	 * Generate key & IV for AES 128 CBC mode. SHA1 digest is used to hash the supplied key material.
	 */
	i = EVP_BytesToKey(EVP_aes_128_cbc(), EVP_sha1(), salt, (unsigned char*) key_data.c_str(), key_data_len, nrounds, key, iv);

	EVP_CIPHER_CTX_init(e_ctx);
	EVP_EncryptInit_ex(e_ctx, EVP_aes_128_cbc(), NULL, key, iv);
	EVP_CIPHER_CTX_init(d_ctx);
	EVP_DecryptInit_ex(d_ctx, EVP_aes_128_cbc(), NULL, key, iv);

	return 0;
}

// Apply aes-128 encryption based on key and iv values
// All data going in & out is considered binary
unsigned char* aes_encrypt(EVP_CIPHER_CTX* e, unsigned char* plaintext, int* len)
{
	/* max ciphertext len for a n bytes of plaintext is n + AES_BLOCK_SIZE -1 bytes */
	int c_len = *len + AES_BLOCK_SIZE, f_len = 0;
	unsigned char* ciphertext = new unsigned char[c_len];

	/* allows reusing of 'e' for multiple encryption cycles */
	EVP_EncryptInit_ex(e, NULL, NULL, NULL, NULL);

	/* update ciphertext, c_len is filled with the length of ciphertext generated, *len is the size of plaintext in bytes */
	EVP_EncryptUpdate(e, ciphertext, &c_len, plaintext, *len);

	/* update ciphertext with the final remaining bytes */
	EVP_EncryptFinal_ex(e, ciphertext + c_len, &f_len);

	*len = c_len + f_len;
	return ciphertext;
}

// Decrypt aes-128 encryption based on key and iv values
unsigned char* aes_decrypt(EVP_CIPHER_CTX* e, unsigned char* ciphertext, int* len)
{
	/* plaintext will always be equal to or lesser than length of ciphertext*/
	int p_len = *len, f_len = 0;
	unsigned char* plaintext = new unsigned char[p_len];

	EVP_DecryptInit_ex(e, NULL, NULL, NULL, NULL);
	EVP_DecryptUpdate(e, plaintext, &p_len, ciphertext, *len);
	EVP_DecryptFinal_ex(e, plaintext + p_len, &f_len);

	*len = p_len + f_len;
	return plaintext;
}



//retransmission
const chrono::duration<int, milli> retransmitTimeout = 300ms;
struct retransmitRequest {
    chrono::time_point<chrono::steady_clock> start;
    chrono::duration<int, milli> elapsed = retransmitTimeout;
    bool isFrame = false;
    string data;
};
map<int, retransmitRequest> retransmits;

char backupBuf[maxPacketCount * 30];
int backupLens[maxPacketCount];
int hpCount = 0;
int lpCount = 0;
vector<int> unorderedPack;


// read buffer
enum receivePacketType {
    FRAME = 0,
    INPUTRETRANSMIT = 1,
    INPUTRETRANSMITIND = 2
};
struct recvPacket {
    bool transmitRequested = false;
    int dataLen = -1;
    uint8_t data[1500];
    receivePacketType type = FRAME;
    int visited = -1;
};
recvPacket buf[maxPacketCount];
/* int visited[maxPacketCount]; */
/* bool retransmitRequests [maxPacketCount]; */


void unreliableSendPacket(string toSend, bool retransmit);

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
    while(run) {
        SDL_Delay(200);
        if(haveClient) {
            string opcode = "";
            opcode += (char)0;
            packet->len = opcode.length() + 1;
            memcpy(packet->data, opcode.c_str(), packet->len);
            int len = SDLNet_UDP_Send(sock, -1, packet);
            if(len == 0) {
                cout << SDLNet_GetError();
            }
        }
    }
}

void handleRetransmit() { 
    while(run) {
        if(haveClient) {
            chrono::duration<int, milli> min = retransmitTimeout;
            if(!retransmits.empty()) {
                retransMutex.lock();
                for(auto it = retransmits.begin(); it != retransmits.end(); it++) {
                    if(it->second.elapsed < min) {
                        min = it->second.elapsed;
                    }
                }
                cout << "Sleeping for " << min.count() << endl;
                retransMutex.unlock();
                this_thread::sleep_for(min);
                auto end = chrono::steady_clock::now();
                retransMutex.lock();
                for(auto it = retransmits.begin(); it != retransmits.end(); it++) {
                    auto passed = end - it->second.start;
                    it->second.elapsed -= chrono::milliseconds(passed.count() / 1000000);
                    if(it->second.elapsed.count() <= 0) {
                        cout << "Timer expired for " << it->first;
                        if(!it->second.isFrame) {
                            stringstream send;
                            send << (char)(it->first / maxByteVal) << (char)(it->first % maxByteVal);
                            for(int j = 0; j < backupLens[it->first]; j++) {
                                send << (char)(backupBuf[it->first*30 + j]);
                            }
                            for(int i = 0; i < send.str().length(); i++) {
                                cout << (int)((uint8_t)send.str()[i]) << " ";
                            }
                            cout << endl;
                            unreliableSendPacket(send.str(), true);
                        } else {
                            stringstream send;
                            send << '7';
                            send << (char)(it->first / maxByteVal) << (char)(it->first % maxByteVal);
                            unreliableSendPacket(send.str(), false);
                        }
                        it->second.elapsed = retransmitTimeout;
                        it->second.start = chrono::steady_clock::now();
                    }
                }
                retransMutex.unlock();
            } else {
                this_thread::sleep_for(min);
            }
        } else {
            SDL_Delay(200);
        }
    }
}


void sendPacket(string toSend) {
    stringstream send;
    // 1 for first time numbered transmission
    send << (char)NUMBERED << (char)hpCount << (char)lpCount << toSend;
    packet->len = send.str().length() + 1;
    if(packet->len < 4) {
        cout << "Small packet len: " << send.str().length() << " " << send.str() << endl;
    }
    memcpy(packet->data, send.str().c_str(), packet->len);
    memcpy(&backupBuf[(hpCount * maxByteVal + lpCount) * 30], toSend.c_str(), packet->len - 3);
    backupLens[hpCount * maxByteVal + lpCount] = packet->len;

    lpCount++;
    if(lpCount > 255) {
        lpCount = 0;
        hpCount++;
        if(hpCount > 59) {
            hpCount = 0;
        }
    }

    int len = SDLNet_UDP_Send(sock, -1, packet);
    if(len == 0) {
        cout << SDLNet_GetError();
    }
}

void unreliableSendPacket(string toSend, bool retransmit) {
    stringstream send;
    if(!retransmit) {
        // 3 for first time unnumbered transmission
        send << (char)UNNUMBERED << toSend;
    } else {
        // 2 for data retransmission
        send << (char)RETRANSMIT << toSend;
    }
    packet->len = send.str().length() + 1;
    memcpy(packet->data, send.str().c_str(), packet->len);
    int len = SDLNet_UDP_Send(sock, -1, packet);
    if(len == 0) {
        cout << SDLNet_GetError();
    }
}

// compares two 16 bit sequence numbers
// returns -1 for num1 < num2, 0 for equal, and 1 for greater
int compareSeqNum(uint16_t num1, uint16_t num2) {
    if(num1 == num2) 
        return 0;
    
    if (num1 < num2 && num2 - num1 < (maxPacketCount) / 2 ||
        num1 > num2 && num1 - num2 > (maxPacketCount) / 2) {
        return -1;
    }

    return 1;
}

int main(int argc, char **argv) {

    thread alive(keepAlive);
    thread retransmit(handleRetransmit);

    // 8 bytes of salt data
    unsigned int salt[] = { 12345, 54321 };

    string key_data = "2B28AB097EAEF7CF15D2154F16A6883C";
    int key_data_len, i;

    key_data_len = key_data.length();

    /* gen key and iv. init the cipher ctx object */
    if (aes_init(key_data, key_data_len, (unsigned char*)&salt, en, de)) {
	    printf("Couldn't initialize AES cipher\n");
	    return -1;
    }

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

    // sockets
    int len = 0;
    packet = SDLNet_AllocPacket(INBUF_SIZE);
    UDPpacket *recv = SDLNet_AllocPacket(INBUF_SIZE);
    socket_set = SDLNet_AllocSocketSet(1);

    //IMGUI state variables
    char port[20] = ""; 
    char ipToTry[30] = "167.234.216.217";

    // for tracking position in packet queue
    int prevIndex = -1, index = -1;
    int packetPos = 0;

    retransMutex.lock();
    retransmits.clear();
    retransMutex.unlock();
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
                        // '0' used for keydown events
                        int c = evt.key.keysym.sym;
                        sendPacket("0" + to_string(c));
                    }
                    break;
                }

                case SDL_KEYUP: {
                    if(haveClient) {
                        // '6' used for keyup events
                        int c = evt.key.keysym.sym;
                        sendPacket("6" + to_string(c));
                    }
                    break;
                }
                case SDL_MOUSEMOTION: {

                    if(haveClient) {
                        if(evt.motion.x >= 0 && evt.motion.y >= 0 && evt.motion.x <=1920 && evt.motion.y<=1080) {
                            string motion = "1" + to_string((float) evt.motion.x / 1920) + "a" + to_string((float) evt.motion.y / 1080);
                            unreliableSendPacket(motion, false);
                        }
                    }
                    break;
               }

                case SDL_MOUSEBUTTONDOWN: {

                    if(haveClient) {
                        string opcode;
                        switch(evt.button.button) {
                            case SDL_BUTTON_LEFT: {
                                sendPacket("2");
                                cout << "leftDown" << endl;
                                break;
                            }
                            case SDL_BUTTON_RIGHT: {
                                sendPacket("3");
                                cout << "rightDown" << endl;
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
                                sendPacket("4");
                                cout << "leftup" << endl;
                                break;
                            }
                            case SDL_BUTTON_RIGHT: {
                                sendPacket("5");
                                cout << "rightup" << endl;
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
            if (SDLNet_ResolveHost(&ip, ipToTry, (uint16_t) PORT) == -1) {
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
                    }
                    if(count < 5) {
                        haveClient = true;
                        recv->data[recv->len] = '\0';
                        string ipPort = string((char*)recv->data);

                        cout << ipPort << endl;
                        if (SDLNet_ResolveHost(&ip, ipPort.substr(0, ipPort.find(":")).c_str(), (uint16_t) stoi(ipPort.substr(ipPort.find(":") + 1))) == -1) {
                            cout << "SDLNet_ResolveHost: " << SDLNet_GetError();
                        } else {
                            cout << "setting peer address and port" << endl;
                            //packet->address = ip;
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
                ready = SDLNet_CheckSockets(socket_set, 5000);
            }
            if(ready > 0) {
                SDLNet_UDP_Recv(sock, recv);
                /* cout << "length: " << recv->len << endl; */
                /* cout << recv->data << endl; */
                if((uint8_t) recv->data[0] == 2) {

                } else if ((uint8_t) recv->data[0] == 4) {
                    int index = (recv->data[1]) * maxByteVal + recv->data[2];
                    cout << "ack received" << index << endl;
                    retransMutex.lock();
                    retransmits.erase(index);
                    retransMutex.unlock();
                } else {
                    uint8_t *data = &recv->data[3];
                    size_t   data_size = recv->len - 3;
                    int ret;

                    index = (int) ((recv->data[1]) * maxByteVal + (recv->data[2]));
                    cout << index << endl;
                    if(recv->data[0] == 1) {
                        retransMutex.lock();
                        buf[index].transmitRequested = false;
                        retransmits.erase(index);
                        retransMutex.unlock();
                        stringstream send;
                        send << '9' << (char)(recv->data[1]) << (char)(recv->data[2]);
                        unreliableSendPacket(send.str(), false);
                        cout << "ack sent: " << index << endl;
                    }
                    if(compareSeqNum(index, packetPos) >= 0) {

			int len = data_size;
			unsigned char* plaintext = aes_decrypt(de, (unsigned char*)data, &len);

                        memcpy(&buf[index].data, plaintext, len);
                        buf[index].visited = len;

                        auto match = find(unorderedPack.begin(), unorderedPack.end(), index);
                        if (match != unorderedPack.end()) {
                            unorderedPack.erase(match);
                        } else {
                            if (unorderedPack.size() > 0) {
                                auto minVal = min_element(unorderedPack.begin(), unorderedPack.end());
                                int unorderedDiff = min(abs(index - *minVal), maxPacketCount - abs(index - *minVal));
                                if(unorderedDiff > 5 || unorderedPack.size() >= 5) {
                                    for (auto it = unorderedPack.begin(); it != unorderedPack.end(); it++) {
                                        if(!buf[*it].transmitRequested) {
                                            stringstream send;
                                            send << '7';
                                            send << (char)(*it / maxByteVal);
                                            send << (char)(*it % maxByteVal);
                                            buf[*it].transmitRequested = true;
                                            unreliableSendPacket(send.str(), false);
                                            retransmitRequest req;
                                            req.start = chrono::steady_clock::now();
                                            req.isFrame = true;
                                            req.data = send.str();
                                            retransMutex.lock();
                                            retransmits.emplace(*it, req);
                                            retransMutex.unlock();
                                        }
                                    }
                                    unorderedPack.clear();
                                }
                            }

                            if((prevIndex + 1) % maxPacketCount != index && recv->data[0] == 0) {
                                int diff = min(abs(index - prevIndex), maxPacketCount - abs(index - prevIndex));
                                int smaller, bigger;
                                if(compareSeqNum(prevIndex, index) < 0) {
                                    smaller = prevIndex;
                                    bigger = index;
                                } else {
                                    smaller = index;
                                    bigger = prevIndex;
                                }
                                cout << "Packets Dropped: " << (diff - 1) << endl << "Index: " << smaller << "\nPrev: " << bigger << endl << endl;
                                if (diff < 5) {
                                    for (int i = smaller + 1; i < bigger; i = (i + 1) % (maxPacketCount)) {
                                        unorderedPack.push_back(i);
                                    }
                                } else {
                                    for (int i = smaller + 1; i < bigger; i++) {
                                        if(!buf[i].transmitRequested) {
                                            stringstream send;
                                            send << '7';
                                            send << (char)(i / maxByteVal);
                                            send << (char)(i % maxByteVal);
                                            buf[i].transmitRequested = true;
                                            unreliableSendPacket(send.str(), false);
                                            retransmitRequest req;
                                            req.start = chrono::steady_clock::now();
                                            req.isFrame = true;
                                            req.data = send.str();
                                            retransMutex.lock();
                                            retransmits.emplace(i, req);
                                            retransMutex.unlock();
                                        }
                                    }
                                    /* send << '8' << (char)(((prevIndex + 1) % maxPacketCount) / maxByteVal) << (char)(((prevIndex + 1) % maxPacketCount) % maxByteVal) << (char)(recv->data[1]) << (char)(recv->data[2]); */
                                }
                            }

                            if(recv->data[0] == 0) {
                                buf[index].type = FRAME;
                                prevIndex = index;
                            } else if (recv->data[0] == 1){
                                buf[index].type = FRAME;
                            } else if((uint8_t) recv->data[0] == 3) {
                                buf[index].type = INPUTRETRANSMIT;
                                int sHigh, sLow, eHigh, eLow;
                                sHigh = (uint8_t)buf[index].data[0];
                                sLow = (uint8_t)buf[index].data[1];
                                eHigh = (uint8_t)buf[index].data[2];
                                eLow = (uint8_t)buf[index].data[3];
                                if (sHigh < 60 && eHigh < 60) {
                                    int beginning = sHigh * maxByteVal + sLow;
                                    int end = eHigh * maxByteVal + eLow;
                                    int smaller, bigger;
                                    if (compareSeqNum(beginning, end) < 0) {
                                        smaller = beginning;
                                        bigger = end;
                                    } else {
                                        smaller = end;
                                        bigger = beginning;
                                    }
                                    cout << "Retransmit " << beginning << " " << end << endl;
                                    for (int i = beginning; i < end; i = (i + 1) % maxPacketCount) {
                                        stringstream send;
                                        send << (char)(i / maxByteVal) << (char)(i % maxByteVal);
                                        for(int j = 0; j < backupLens[i]; j++) {
                                            send << (char)(backupBuf[i*30 + j]);
                                        }
                                        for(int j = 0; j < send.str().length(); j++) {
                                            cout << (int)send.str()[j] << " ";
                                        }
                                        cout << "with length " << backupLens[i] << endl;

                                        unreliableSendPacket(send.str(), true);

                                        retransmitRequest req;
                                        req.start = chrono::steady_clock::now();
                                        req.isFrame = false;
                                        retransMutex.lock();
                                        retransmits.emplace(i, req);
                                        retransMutex.unlock();
                                    }
                                }
                            } else if ((uint8_t) recv->data[0] == 5) {
                                for(int i = 1; i < recv->len; i+=2) {
                                    if (i+1 < recv->len) {
                                        index = (int) ((recv->data[i]) * maxByteVal + (recv->data[i+1]));
                                        cout << "Retransmit " << index << endl;
                                        if(index < maxPacketCount) {
                                            stringstream send;
                                            send << (char)(index / maxByteVal);
                                            send << (char)(index % maxByteVal);
                                            for(int j = 0; j < backupLens[index]; j++) {
                                                send << (char)(backupBuf[index*30 + j]);
                                            }

                                            for(int j = 0; j < send.str().length(); j++) {
                                                cout << (int)send.str()[j] << " ";
                                            }
                                            cout << endl;

                                            unreliableSendPacket(send.str(), true);
                                            retransmitRequest req;
                                            req.start = chrono::steady_clock::now();

                                            retransMutex.lock();
                                            req.isFrame = false;
                                            retransmits.emplace(index, req);
                                            retransMutex.unlock();
                                        }
                                    }
                                }
                                break;
                            }


                        while(buf[packetPos].visited != -1) {
                            switch(buf[packetPos].type) {
                                case FRAME: {
                                    data_size = buf[packetPos].visited;
                                    Uint8* bufPtr = buf[packetPos].data;
                                    while(data_size > 0) {
                                        ret = av_parser_parse2(parser, c, &pkt->data, &pkt->size,
                                                bufPtr, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                                        if (ret < 0) {
                                            fprintf(stderr, "Error while parsing\n");
                                            exit(1);
                                        }
                                        bufPtr      += ret;
                                        data_size -= ret;

                                        if (pkt->size) {
                                            decode(c, frame, pkt);
                                        }
                                    }
                                    //printf("Received (%i): \n", len);
                                    break;
                                }
                            }
                            buf[packetPos].visited = -1;
                            cout << "packetPos visit " << packetPos << endl;
                            packetPos++;
                            if(packetPos >= maxPacketCount) {
                                packetPos = 0;
                            }
                        }
                    }
                } 
            }
        } else {
                SDLNet_UDP_Close(sock);
                SDLNet_UDP_DelSocket(socket_set, sock);
                sock = NULL;
                haveClient = false;
                firstReceive = true;
                packetPos = 0;
                for(int i = 0; i < maxPacketCount; i++) {
                    buf[i].visited = -1;
                }
                prevIndex = -1;
                index = 0;
                hpCount = 0;
                lpCount = 0;
                unorderedPack.clear();
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

    run = false;
    alive.join();
    retransmit.join();
}
