#include <iostream>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_id.h>
}

int main() {
    struct { AVCodecID id; const char* name; } checks[] = {
        {AV_CODEC_ID_PNG, "PNG"},
        {AV_CODEC_ID_HAP, "HAP"},
        {AV_CODEC_ID_H264, "H264"},
        {AV_CODEC_ID_PRORES, "ProRes"},
    };
    for (auto& c : checks) {
        const AVCodec* dec = avcodec_find_decoder(c.id);
        const AVCodec* enc = avcodec_find_encoder(c.id);
        std::cout << c.name
                  << ": decode=" << (dec ? "YES" : "no")
                  << " encode=" << (enc ? "YES" : "no") << std::endl;
    }
    return 0;
}
