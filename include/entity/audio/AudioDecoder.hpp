#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct SwrContext;
struct AVPacket;
struct AVFrame;

namespace entity {

// Decodes one media file's audio stream to canonical interleaved float32
// stereo at a target sample rate. Owns its own AVFormatContext — opened
// independently of the Clip's video context (av_read_frame is not safe
// to share across threads).
class AudioDecoder {
public:
    AudioDecoder() = default;
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    // Opens the file's audio stream. Returns false if the file has no
    // audio stream or cannot be opened.
    bool open(const std::string& filepath, int targetSampleRate);
    bool isOpen() const { return m_codecCtx != nullptr; }

    int sourceSampleRate() const { return m_sourceSampleRate; }
    int sourceChannels()   const { return m_sourceChannels; }

    // Total source-sample length of the audio stream at the SOURCE rate
    // (used for in/out clamping). 0 if unknown.
    int64_t sourceLengthSamples() const { return m_sourceLengthSamples; }

    // Seek so the next decodeChunk() starts at `targetSampleRate`-domain
    // sample `outSamplePos`.
    void seekToOutputSample(int64_t outSamplePos);

    // Decodes forward; appends interleaved float32 stereo frames to
    // `out`. Returns the number of frames appended; 0 at end of stream.
    size_t decodeChunk(std::vector<float>& out, size_t maxFrames);

private:
    AVFormatContext* m_fmtCtx{nullptr};
    AVCodecContext*  m_codecCtx{nullptr};
    SwrContext*      m_swr{nullptr};
    AVPacket*        m_packet{nullptr};
    AVFrame*         m_frame{nullptr};
    int m_streamIndex{-1};
    int m_targetSampleRate{48000};
    int m_sourceSampleRate{0};
    int m_sourceChannels{0};
    int64_t m_sourceLengthSamples{0};
};

} // namespace entity
