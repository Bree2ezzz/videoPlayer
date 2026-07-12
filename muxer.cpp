#include "muxer.h"

#include <QByteArray>

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

Muxer::~Muxer()
{
    close();
}

int Muxer::open(const QString& rtmpUrl)
{
    if (rtmpUrl.isEmpty()) {
        return AVERROR(EINVAL);
    }
    close();

    const QByteArray url = rtmpUrl.toUtf8();
    AVFormatContext* context = nullptr;
    int ret = avformat_alloc_output_context2(&context, nullptr, "flv", url.constData());
    if (ret < 0 || !context) {
        return ret < 0 ? ret : AVERROR(ENOMEM);
    }
    context->interrupt_callback.callback = &Muxer::interruptCallback;
    context->interrupt_callback.opaque = this;
    if (!(context->oformat->flags & AVFMT_NOFILE)) {
        AVDictionary* options = nullptr;
        av_dict_set(&options, "rtmp_live", "live", 0);
        ret = avio_open2(&context->pb,
                          url.constData(),
                          AVIO_FLAG_WRITE,
                          &context->interrupt_callback,
                          &options);
        av_dict_free(&options);
        if (ret < 0) {
            avformat_free_context(context);
            return ret;
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    formatContext_ = context;
    headerWritten_ = false;
    trailerWritten_ = false;
    return 0;
}

int Muxer::addStream(const AVCodecContext* encoderContext)
{
    if (!encoderContext) {
        return AVERROR(EINVAL);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!formatContext_ || headerWritten_) {
        return AVERROR(EINVAL);
    }
    if ((encoderContext->codec_id == AV_CODEC_ID_H264 ||
         encoderContext->codec_id == AV_CODEC_ID_AAC) &&
        (!encoderContext->extradata || encoderContext->extradata_size <= 0)) {
        return AVERROR_INVALIDDATA;
    }
    AVStream* stream = avformat_new_stream(formatContext_, nullptr);
    if (!stream) {
        return AVERROR(ENOMEM);
    }
    const int ret = avcodec_parameters_from_context(stream->codecpar, encoderContext);
    if (ret < 0) {
        return ret;
    }
    stream->time_base = encoderContext->time_base;
    return stream->index;
}

int Muxer::writeHeader()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!formatContext_ || headerWritten_) {
        return AVERROR(EINVAL);
    }
    const int ret = avformat_write_header(formatContext_, nullptr);
    if (ret >= 0) {
        headerWritten_ = true;
    }
    return ret;
}

int Muxer::writePacket(int streamIndex, const AVPacket* packet, AVRational encoderTimeBase)
{
    if (!packet) {
        return AVERROR(EINVAL);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!formatContext_ || !headerWritten_ || trailerWritten_ || abort_) {
        return AVERROR_EXIT;
    }
    if (streamIndex < 0 || streamIndex >= static_cast<int>(formatContext_->nb_streams)) {
        return AVERROR(EINVAL);
    }

    AVPacket* output = av_packet_alloc();
    if (!output) {
        return AVERROR(ENOMEM);
    }
    int ret = av_packet_ref(output, packet);
    if (ret >= 0) {
        AVStream* stream = formatContext_->streams[streamIndex];
        av_packet_rescale_ts(output, encoderTimeBase, stream->time_base);
        output->stream_index = streamIndex;
        output->pos = -1;
        ret = av_interleaved_write_frame(formatContext_, output);
    }
    av_packet_free(&output);
    return ret;
}

int Muxer::writeTrailer()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return writeTrailerLocked();
}

void Muxer::abort()
{
    abort_ = true;
}

void Muxer::close()
{
    AVFormatContext* context = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!formatContext_) {
            return;
        }
        writeTrailerLocked();
        context = formatContext_;
        formatContext_ = nullptr;
        headerWritten_ = false;
        trailerWritten_ = false;
    }
    if (!(context->oformat->flags & AVFMT_NOFILE) && context->pb) {
        avio_closep(&context->pb);
    }
    avformat_free_context(context);
}

int Muxer::interruptCallback(void* opaque)
{
    const auto* muxer = static_cast<const Muxer*>(opaque);
    return muxer && muxer->abort_.load() ? 1 : 0;
}

int Muxer::writeTrailerLocked()
{
    if (!formatContext_ || !headerWritten_ || trailerWritten_) {
        return 0;
    }
    trailerWritten_ = true;
    return av_write_trailer(formatContext_);
}
