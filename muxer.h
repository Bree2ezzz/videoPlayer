#ifndef MUXER_H
#define MUXER_H

#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/rational.h>
}

#include <atomic>
#include <mutex>

class Muxer
{
public:
    Muxer() = default;
    ~Muxer();

    Muxer(const Muxer&) = delete;
    Muxer& operator=(const Muxer&) = delete;

    int open(const QString& rtmpUrl);
    int addStream(const AVCodecContext* encoderContext);
    int writeHeader();
    int writePacket(int streamIndex, const AVPacket* packet, AVRational encoderTimeBase);
    int writeTrailer();
    void abort();
    void close();

private:
    static int interruptCallback(void* opaque);
    int writeTrailerLocked();

private:
    AVFormatContext* formatContext_ = nullptr;
    std::mutex mutex_;
    std::atomic_bool abort_{false};
    bool headerWritten_ = false;
    bool trailerWritten_ = false;
};
#endif
