#include "encoder.h"

#include "app_logger.h"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

#include <QByteArray>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <utility>

namespace {

std::string avErrorString(int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, buffer, sizeof(buffer));
    return buffer;
}

int validFrameRate(AVRational rate, int fallback)
{
    if (rate.num > 0 && rate.den > 0) {
        const int rounded = static_cast<int>(av_q2d(rate) + 0.5);
        if (rounded > 0 && rounded <= 240) {
            return rounded;
        }
    }
    return std::clamp(fallback, 1, 240);
}

int supportedSampleRate(const AVCodec* codec, int requested)
{
    if (!codec || !codec->supported_samplerates) {
        return requested;
    }
    int closest = codec->supported_samplerates[0];
    for (const int* rate = codec->supported_samplerates; *rate; ++rate) {
        if (*rate == requested) {
            return requested;
        }
        if (std::abs(*rate - requested) < std::abs(closest - requested)) {
            closest = *rate;
        }
    }
    return closest;
}

AVSampleFormat supportedSampleFormat(const AVCodec* codec)
{
    if (!codec || !codec->sample_fmts) {
        return AV_SAMPLE_FMT_FLTP;
    }
    for (const AVSampleFormat* format = codec->sample_fmts;
         *format != AV_SAMPLE_FMT_NONE;
         ++format) {
        if (*format == AV_SAMPLE_FMT_FLTP) {
            return *format;
        }
    }
    return codec->sample_fmts[0];
}

} // namespace

Encoder::~Encoder()
{
    stop();
    std::lock_guard<std::mutex> lock(codecMutex_);
    avcodec_free_context(&codecContext_);
}

void Encoder::setInputQueue(FrameQueue* queue)
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    inputQueue_ = queue;
}

void Encoder::setPacketCallback(PacketCallback cb)
{
    packetCb_ = std::move(cb);
}

void Encoder::setEofCallback(EofCallback cb)
{
    eofCb_ = std::move(cb);
}

void Encoder::setErrorCallback(ErrorCallback cb)
{
    errorCb_ = std::move(cb);
}

void Encoder::setPtsMapper(PtsMapper mapper)
{
    ptsMapper_ = std::move(mapper);
}

int Encoder::openCodec(const AVCodec* codec, AVCodecContext* context)
{
    if (!codec || !context) {
        avcodec_free_context(&context);
        return AVERROR(EINVAL);
    }

    close();
    const int ret = avcodec_open2(context, codec, nullptr);
    if (ret < 0) {
        avcodec_free_context(&context);
        return ret;
    }

    std::lock_guard<std::mutex> lock(codecMutex_);
    codecContext_ = context;
    abort_ = false;
    return 0;
}

int Encoder::start()
{
    if (running_) {
        return 0;
    }
    if (thread_.joinable() || !isOpen()) {
        return AVERROR(EALREADY);
    }
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (!inputQueue_) {
            return AVERROR(EINVAL);
        }
    }

    abort_ = false;
    running_ = true;
    thread_ = std::thread(&Encoder::encodeLoop, this);
    return 0;
}

void Encoder::stop()
{
    abort_ = true;
    FrameQueue* queue = nullptr;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queue = inputQueue_;
    }
    if (queue) {
        queue->closeConsumer();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    running_ = false;
}

int Encoder::drain()
{
    if (abort_) {
        return AVERROR_EXIT;
    }
    const int flushRet = flushInput();
    if (flushRet < 0) {
        reportError(flushRet, "encoder input drain failed: " + avErrorString(flushRet));
        return flushRet;
    }
    const int drainRet = drainCodec();
    if (drainRet < 0 && drainRet != AVERROR_EOF) {
        reportError(drainRet, "encoder codec drain failed: " + avErrorString(drainRet));
        return drainRet;
    }
    return 0;
}

void Encoder::close()
{
    stop();
    releaseConversionResources();
    std::lock_guard<std::mutex> lock(codecMutex_);
    avcodec_free_context(&codecContext_);
}

bool Encoder::isOpen() const
{
    std::lock_guard<std::mutex> lock(codecMutex_);
    return codecContext_ != nullptr;
}

bool Encoder::isRunning() const
{
    return running_;
}

AVCodecContext* Encoder::codecContext() const
{
    std::lock_guard<std::mutex> lock(codecMutex_);
    return codecContext_;
}

AVRational Encoder::timeBase() const
{
    AVCodecContext* context = codecContext();
    return context ? context->time_base : AVRational{0, 1};
}

int Encoder::sendFrame(AVFrame* frame)
{
    AVCodecContext* context = codecContext();
    if (!context) {
        return AVERROR(EINVAL);
    }
    const int ret = avcodec_send_frame(context, frame);
    if (ret < 0) {
        return ret;
    }
    return receivePackets();
}

int Encoder::mappedPts(const AVFrame* frame, AVRational inputTimeBase) const
{
    if (!frame) {
        return AV_NOPTS_VALUE;
    }
    if (ptsMapper_) {
        return ptsMapper_(frame, inputTimeBase);
    }
    return frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
}

void Encoder::reportError(int errCode, const std::string& message)
{
    VP_ERROR("Encoder error code={} msg={}", errCode, message);
    if (errorCb_) {
        errorCb_(errCode, message);
    }
}

void Encoder::encodeLoop()
{
    while (!abort_) {
        FrameQueue* queue = nullptr;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            queue = inputQueue_;
        }
        if (!queue) {
            reportError(AVERROR(EINVAL), "encoder input queue is not set");
            break;
        }

        AVFrame* frame = nullptr;
        int serial = -1;
        const int popRet = queue->pop(&frame, &serial, 100);
        if (popRet == FrameQueue::Timeout) {
            continue;
        }
        if (popRet == FrameQueue::EndOfStream) {
            const int drainRet = drain();
            if (drainRet < 0) {
                break;
            }
            if (!abort_ && eofCb_) {
                eofCb_();
            }
            break;
        }
        if (popRet != FrameQueue::Ok || !frame) {
            break;
        }

        const int ret = processFrame(frame);
        av_frame_free(&frame);
        if (ret < 0) {
            if (!abort_) {
                reportError(ret, "encoding frame failed: " + avErrorString(ret));
            }
            break;
        }
    }
    running_ = false;
}

int Encoder::drainCodec()
{
    AVCodecContext* context = codecContext();
    if (!context) {
        return AVERROR(EINVAL);
    }
    int ret = avcodec_send_frame(context, nullptr);
    if (ret == AVERROR_EOF) {
        return ret;
    }
    if (ret < 0) {
        return ret;
    }
    return receivePackets();
}

int Encoder::receivePackets()
{
    AVCodecContext* context = codecContext();
    if (!context) {
        return AVERROR(EINVAL);
    }
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return AVERROR(ENOMEM);
    }

    int result = 0;
    while (!abort_) {
        const int ret = avcodec_receive_packet(context, packet);
        if (ret == 0) {
            if (packetCb_) {
                result = packetCb_(packet, context->time_base);
                if (result < 0) {
                    av_packet_unref(packet);
                    break;
                }
            }
            av_packet_unref(packet);
            continue;
        }
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            result = ret == AVERROR_EOF ? AVERROR_EOF : 0;
            break;
        }
        result = ret;
        break;
    }
    av_packet_free(&packet);
    return result;
}

VideoEncoder::~VideoEncoder()
{
    close();
}

int VideoEncoder::open(const Input& input, const StreamOutputOptions& options)
{
    if (input.width <= 0 || input.height <= 0 || input.width % 2 || input.height % 2) {
        return AVERROR(EINVAL);
    }
    const QByteArray name = options.videoEncoderName.toUtf8();
    const AVCodec* codec = avcodec_find_encoder_by_name(name.constData());
    if (!codec || codec->id != AV_CODEC_ID_H264) {
        return AVERROR_ENCODER_NOT_FOUND;
    }
    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (!context) {
        return AVERROR(ENOMEM);
    }

    const int fps = validFrameRate(input.frameRate, options.videoFrameRate);
    context->codec_type = AVMEDIA_TYPE_VIDEO;
    context->width = input.width;
    context->height = input.height;
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->time_base = AVRational{1, fps};
    context->framerate = AVRational{fps, 1};
    context->bit_rate = std::max(options.videoBitRate, 1);
    context->rc_max_rate = context->bit_rate;
    context->rc_buffer_size = context->bit_rate;
    context->gop_size = std::max(1, fps * std::max(1, options.gopSeconds));
    context->max_b_frames = 0;
    context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_opt_set(context->priv_data, "preset", options.x264Preset.toUtf8().constData(), 0);
    av_opt_set(context->priv_data, "tune", options.x264Tune.toUtf8().constData(), 0);

    outputWidth_ = input.width;
    outputHeight_ = input.height;
    inputTimeBase_ = input.timeBase;
    lastPts_ = AV_NOPTS_VALUE;
    return openCodec(codec, context);
}

int VideoEncoder::processFrame(AVFrame* frame)
{
    if (!frame || frame->width != outputWidth_ || frame->height != outputHeight_) {
        return AVERROR(EINVAL);
    }
    int64_t pts = mappedPts(frame, inputTimeBase_);
    if (pts == AV_NOPTS_VALUE) {
        pts = lastPts_ == AV_NOPTS_VALUE ? 0 : lastPts_ + 1;
    }
    if (lastPts_ != AV_NOPTS_VALUE && pts <= lastPts_) {
        pts = lastPts_ + 1;
    }
    lastPts_ = pts;

    const AVPixelFormat sourceFormat = static_cast<AVPixelFormat>(frame->format);
    if (sourceFormat == AV_PIX_FMT_YUV420P) {
        frame->pts = pts;
        return sendFrame(frame);
    }
    if (sourceFormat == AV_PIX_FMT_NONE) {
        return AVERROR(EINVAL);
    }

    swsContext_ = sws_getCachedContext(swsContext_,
                                       frame->width,
                                       frame->height,
                                       sourceFormat,
                                       outputWidth_,
                                       outputHeight_,
                                       AV_PIX_FMT_YUV420P,
                                       SWS_BILINEAR,
                                       nullptr,
                                       nullptr,
                                       nullptr);
    if (!swsContext_) {
        return AVERROR(ENOMEM);
    }
    sourceWidth_ = frame->width;
    sourceHeight_ = frame->height;
    sourceFormat_ = sourceFormat;

    AVFrame* converted = av_frame_alloc();
    if (!converted) {
        return AVERROR(ENOMEM);
    }
    converted->format = AV_PIX_FMT_YUV420P;
    converted->width = outputWidth_;
    converted->height = outputHeight_;
    converted->pts = pts;
    int ret = av_frame_get_buffer(converted, 32);
    if (ret >= 0) {
        ret = sws_scale(swsContext_,
                        frame->data,
                        frame->linesize,
                        0,
                        frame->height,
                        converted->data,
                        converted->linesize);
        if (ret > 0) {
            ret = sendFrame(converted);
        } else {
            ret = AVERROR(EINVAL);
        }
    }
    av_frame_free(&converted);
    return ret;
}

int VideoEncoder::flushInput()
{
    return 0;
}

void VideoEncoder::releaseConversionResources()
{
    sws_freeContext(swsContext_);
    swsContext_ = nullptr;
    sourceWidth_ = 0;
    sourceHeight_ = 0;
    sourceFormat_ = AV_PIX_FMT_NONE;
    lastPts_ = AV_NOPTS_VALUE;
}

AudioEncoder::~AudioEncoder()
{
    close();
}

int AudioEncoder::open(const Input& input, const StreamOutputOptions& options)
{
    const QByteArray name = options.audioEncoderName.toUtf8();
    const AVCodec* codec = avcodec_find_encoder_by_name(name.constData());
    if (!codec || codec->id != AV_CODEC_ID_AAC) {
        return AVERROR_ENCODER_NOT_FOUND;
    }
    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (!context) {
        return AVERROR(ENOMEM);
    }
    const int sampleRate = supportedSampleRate(codec, std::max(1, options.audioSampleRate));
    context->codec_type = AVMEDIA_TYPE_AUDIO;
    context->sample_rate = sampleRate;
    context->sample_fmt = supportedSampleFormat(codec);
    context->time_base = AVRational{1, sampleRate};
    context->bit_rate = std::max(1, options.audioBitRate);
    context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_channel_layout_default(&context->ch_layout, std::max(1, options.audioChannels));

    inputTimeBase_ = input.timeBase;
    hasNextPts_ = false;
    nextPts_ = 0;
    const int ret = openCodec(codec, context);
    if (ret < 0) {
        return ret;
    }
    fifo_ = av_audio_fifo_alloc(context->sample_fmt, context->ch_layout.nb_channels, 1);
    if (!fifo_) {
        close();
        return AVERROR(ENOMEM);
    }
    return 0;
}

int AudioEncoder::ensureResampler(const AVFrame* frame)
{
    AVCodecContext* context = codecContext();
    if (!context || !frame || frame->sample_rate <= 0 || frame->ch_layout.nb_channels <= 0) {
        return AVERROR(EINVAL);
    }
    const AVSampleFormat format = static_cast<AVSampleFormat>(frame->format);
    const bool unchanged = swrContext_ && sourceFormat_ == format &&
                           sourceSampleRate_ == frame->sample_rate &&
                           av_channel_layout_compare(&sourceLayout_, &frame->ch_layout) == 0;
    if (unchanged) {
        return 0;
    }

    swr_free(&swrContext_);
    av_channel_layout_uninit(&sourceLayout_);
    int ret = av_channel_layout_copy(&sourceLayout_, &frame->ch_layout);
    if (ret < 0) {
        return ret;
    }
    sourceFormat_ = format;
    sourceSampleRate_ = frame->sample_rate;
    AVChannelLayout destination{};
    ret = av_channel_layout_copy(&destination, &context->ch_layout);
    if (ret < 0) {
        return ret;
    }
    ret = swr_alloc_set_opts2(&swrContext_,
                              &destination,
                              context->sample_fmt,
                              context->sample_rate,
                              &sourceLayout_,
                              sourceFormat_,
                              sourceSampleRate_,
                              0,
                              nullptr);
    av_channel_layout_uninit(&destination);
    if (ret < 0) {
        return ret;
    }
    return swr_init(swrContext_);
}

int AudioEncoder::appendResampled(const uint8_t** input, int inputSamples)
{
    AVCodecContext* context = codecContext();
    if (!context || !swrContext_ || !fifo_) {
        return AVERROR(EINVAL);
    }
    const int outputSamples = std::max(
        int64_t(1),
        av_rescale_rnd(swr_get_delay(swrContext_, sourceSampleRate_) + inputSamples,
                       context->sample_rate,
                       sourceSampleRate_,
                       AV_ROUND_UP));
    AVFrame* converted = av_frame_alloc();
    if (!converted) {
        return AVERROR(ENOMEM);
    }
    converted->format = context->sample_fmt;
    converted->sample_rate = context->sample_rate;
    converted->nb_samples = outputSamples;
    int ret = av_channel_layout_copy(&converted->ch_layout, &context->ch_layout);
    if (ret >= 0) {
        ret = av_frame_get_buffer(converted, 0);
    }
    if (ret >= 0) {
        ret = swr_convert(swrContext_,
                          converted->data,
                          outputSamples,
                          input,
                          inputSamples);
    }
    if (ret > 0) {
        if (av_audio_fifo_realloc(fifo_, av_audio_fifo_size(fifo_) + ret) < 0) {
            ret = AVERROR(ENOMEM);
        } else if (av_audio_fifo_write(fifo_, reinterpret_cast<void**>(converted->data), ret) < ret) {
            ret = AVERROR(EIO);
        } else {
            ret = 0;
        }
    } else if (ret == 0) {
        ret = 0;
    }
    av_frame_free(&converted);
    return ret;
}

int AudioEncoder::encodeFifoFrame(int samples, bool padWithSilence)
{
    AVCodecContext* context = codecContext();
    if (!context || !fifo_ || samples <= 0) {
        return AVERROR(EINVAL);
    }
    const int frameSamples = context->frame_size;
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        return AVERROR(ENOMEM);
    }
    frame->format = context->sample_fmt;
    frame->sample_rate = context->sample_rate;
    frame->nb_samples = frameSamples;
    frame->pts = nextPts_;
    int ret = av_channel_layout_copy(&frame->ch_layout, &context->ch_layout);
    if (ret >= 0) {
        ret = av_frame_get_buffer(frame, 0);
    }
    if (ret >= 0) {
        av_samples_set_silence(frame->data,
                               0,
                               frameSamples,
                               context->ch_layout.nb_channels,
                               context->sample_fmt);
        const int read = av_audio_fifo_read(fifo_, reinterpret_cast<void**>(frame->data), samples);
        if (read != samples || (!padWithSilence && samples != frameSamples)) {
            ret = AVERROR(EIO);
        } else {
            nextPts_ += frameSamples;
            ret = sendFrame(frame);
        }
    }
    av_frame_free(&frame);
    return ret;
}

int AudioEncoder::encodeAvailableFrames()
{
    AVCodecContext* context = codecContext();
    if (!context || context->frame_size <= 0) {
        return AVERROR(EINVAL);
    }
    while (av_audio_fifo_size(fifo_) >= context->frame_size) {
        const int ret = encodeFifoFrame(context->frame_size, false);
        if (ret < 0) {
            return ret;
        }
    }
    return 0;
}

int AudioEncoder::processFrame(AVFrame* frame)
{
    if (!frame || frame->nb_samples <= 0) {
        return AVERROR(EINVAL);
    }
    const int ret = ensureResampler(frame);
    if (ret < 0) {
        return ret;
    }
    if (!hasNextPts_) {
        const int64_t pts = mappedPts(frame, inputTimeBase_);
        nextPts_ = pts == AV_NOPTS_VALUE ? 0 : pts;
        hasNextPts_ = true;
    }
    const int convertRet = appendResampled(const_cast<const uint8_t**>(frame->extended_data),
                                           frame->nb_samples);
    return convertRet < 0 ? convertRet : encodeAvailableFrames();
}

int AudioEncoder::flushInput()
{
    if (swrContext_) {
        while (swr_get_delay(swrContext_, sourceSampleRate_) > 0) {
            const int before = av_audio_fifo_size(fifo_);
            const int ret = appendResampled(nullptr, 0);
            if (ret < 0) {
                return ret;
            }
            if (av_audio_fifo_size(fifo_) == before) {
                break;
            }
        }
    }
    int ret = encodeAvailableFrames();
    if (ret < 0 || !fifo_) {
        return ret;
    }
    const int remaining = av_audio_fifo_size(fifo_);
    if (remaining > 0) {
        ret = encodeFifoFrame(remaining, true);
    }
    return ret;
}

void AudioEncoder::releaseConversionResources()
{
    swr_free(&swrContext_);
    av_audio_fifo_free(fifo_);
    fifo_ = nullptr;
    av_channel_layout_uninit(&sourceLayout_);
    sourceFormat_ = AV_SAMPLE_FMT_NONE;
    sourceSampleRate_ = 0;
    hasNextPts_ = false;
    nextPts_ = 0;
}
