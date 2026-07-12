#ifndef ENCODER_H
#define ENCODER_H

#include "FrameQueue.h"
#include "streamoutputoptions.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/rational.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class Encoder
{
public:
    using PacketCallback = std::function<int(const AVPacket* packet, AVRational timeBase)>;
    using EofCallback = std::function<void()>;
    using ErrorCallback = std::function<void(int errCode, const std::string& message)>;
    using PtsMapper = std::function<int64_t(const AVFrame* frame, AVRational inputTimeBase)>;

    virtual ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    void setInputQueue(FrameQueue* queue);
    void setPacketCallback(PacketCallback cb);
    void setEofCallback(EofCallback cb);
    void setErrorCallback(ErrorCallback cb);
    void setPtsMapper(PtsMapper mapper);
    int start();
    void stop();
    int drain();
    virtual void close();
    bool isOpen() const;
    bool isRunning() const;
    AVCodecContext* codecContext() const;
    AVRational timeBase() const;

protected:
    Encoder() = default;
    int openCodec(const AVCodec* codec, AVCodecContext* context);
    int sendFrame(AVFrame* frame);
    int mappedPts(const AVFrame* frame, AVRational inputTimeBase) const;
    void reportError(int errCode, const std::string& message);
    virtual int processFrame(AVFrame* frame) = 0;
    virtual int flushInput() = 0;
    virtual void releaseConversionResources() = 0;

private:
    void encodeLoop();
    int drainCodec();
    int receivePackets();

private:
    mutable std::mutex codecMutex_;
    AVCodecContext* codecContext_ = nullptr;
    FrameQueue* inputQueue_ = nullptr;
    mutable std::mutex queueMutex_;
    std::thread thread_;
    std::atomic_bool abort_{false};
    std::atomic_bool running_{false};
    PacketCallback packetCb_;
    EofCallback eofCb_;
    ErrorCallback errorCb_;
    PtsMapper ptsMapper_;
};

class VideoEncoder final : public Encoder
{
public:
    struct Input {
        int width = 0;
        int height = 0;
        AVRational timeBase = {0, 1};
        AVRational frameRate = {0, 1};
    };
    VideoEncoder() = default;
    ~VideoEncoder() override;
    int open(const Input& input, const StreamOutputOptions& options);

protected:
    int processFrame(AVFrame* frame) override;
    int flushInput() override;
    void releaseConversionResources() override;

private:
    SwsContext* swsContext_ = nullptr;
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;
    AVPixelFormat sourceFormat_ = AV_PIX_FMT_NONE;
    int outputWidth_ = 0;
    int outputHeight_ = 0;
    AVRational inputTimeBase_ = {0, 1};
    int64_t lastPts_ = AV_NOPTS_VALUE;
};

class AudioEncoder final : public Encoder
{
public:
    struct Input { AVRational timeBase = {0, 1}; };
    AudioEncoder() = default;
    ~AudioEncoder() override;
    int open(const Input& input, const StreamOutputOptions& options);

protected:
    int processFrame(AVFrame* frame) override;
    int flushInput() override;
    void releaseConversionResources() override;

private:
    int ensureResampler(const AVFrame* frame);
    int appendResampled(const uint8_t** input, int inputSamples);
    int encodeAvailableFrames();
    int encodeFifoFrame(int samples, bool padWithSilence);
    SwrContext* swrContext_ = nullptr;
    AVAudioFifo* fifo_ = nullptr;
    AVSampleFormat sourceFormat_ = AV_SAMPLE_FMT_NONE;
    int sourceSampleRate_ = 0;
    AVChannelLayout sourceLayout_{};
    AVRational inputTimeBase_ = {0, 1};
    int64_t nextPts_ = 0;
    bool hasNextPts_ = false;
};

#endif // ENCODER_H
