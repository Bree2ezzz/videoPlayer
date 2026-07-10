#include "decoder.h"

#include "app_logger.h"


extern "C" {
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
}

#include <cerrno>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

namespace {


AVPixelFormat hardwarePixelFormatForDevice(AVHWDeviceType type)
{
    switch (type) {
    case AV_HWDEVICE_TYPE_D3D11VA:
        return AV_PIX_FMT_D3D11;
    case AV_HWDEVICE_TYPE_DXVA2:
        return AV_PIX_FMT_DXVA2_VLD;
    case AV_HWDEVICE_TYPE_VAAPI:
        return AV_PIX_FMT_VAAPI;
    case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
        return AV_PIX_FMT_VIDEOTOOLBOX;
    case AV_HWDEVICE_TYPE_CUDA:
        return AV_PIX_FMT_CUDA;
    case AV_HWDEVICE_TYPE_QSV:
        return AV_PIX_FMT_QSV;
    default:
        return AV_PIX_FMT_NONE;
    }
}

bool isHardwarePixelFormat(AVPixelFormat format)
{
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(format);
    return desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL);
}

AVPixelFormat firstSoftwarePixelFormat(const AVPixelFormat* formats)
{
    if (!formats) {
        return AV_PIX_FMT_NONE;
    }

    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (!isHardwarePixelFormat(*p)) {
            return *p;
        }
    }

    return formats[0];
}

std::string avErrorString(int errCode)
{
    char errBuf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errCode, errBuf, sizeof(errBuf));
    return errBuf;
}

} // namespace

Decoder::~Decoder()
{
    close();
}

int Decoder::open(AVStream* stream, const Options& options)
{
    if (!stream) {
        return AVERROR(EINVAL);
    }
    return open(stream->codecpar, stream->time_base, stream->index, options);
}

int Decoder::open(const AVCodecParameters* codecpar,
                  AVRational timeBase,
                  int streamIndex,
                  const Options& options)
{
    if (!codecpar) {
        return AVERROR(EINVAL);
    }

    if (codecpar->codec_type != expectedMediaType_) {
        return AVERROR(EINVAL);
    }

    close();

    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        const int ret = AVERROR_DECODER_NOT_FOUND;
        reportError(ret, "avcodec_find_decoder failed");
        return ret;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        const int ret = AVERROR(ENOMEM);
        reportError(ret, "avcodec_alloc_context3 failed: " + avErrorString(ret));
        return ret;
    }

    int ret = avcodec_parameters_to_context(ctx, codecpar);
    if (ret < 0) {
        reportError(ret, "avcodec_parameters_to_context failed: " + avErrorString(ret));
        avcodec_free_context(&ctx);
        return ret;
    }

    ctx->pkt_timebase = timeBase;

    ret = configureCodecContext(ctx, codec, options);
    if (ret < 0) {
        reportError(ret, "configureCodecContext failed: " + avErrorString(ret));
        avcodec_free_context(&ctx);
        return ret;
    }

    ret = avcodec_open2(ctx, codec, nullptr);
    if (ret < 0) {
        reportError(ret, "avcodec_open2 failed: " + avErrorString(ret));
        avcodec_free_context(&ctx);
        return ret;
    }

    {
        std::lock_guard<std::mutex> lock(codecMutex_);
        codecCtx_ = ctx;
        codec_ = codec;
        options_ = options;
        streamIndex_ = streamIndex;
        timeBase_ = timeBase;
    }

    abort_ = false;
    pktSerial_ = -1;
    return 0;
}

void Decoder::close()
{
    stop();

    std::lock_guard<std::mutex> lock(codecMutex_);
    if (codecCtx_) {
        avcodec_free_context(&codecCtx_);
    }

    codec_ = nullptr;
    streamIndex_ = -1;
    timeBase_ = {0, 1};
    pktSerial_ = -1;
}

void Decoder::setInputQueue(PacketQueue* queue)
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    inputQueue_ = queue;
}

void Decoder::setOutputQueue(FrameQueue* queue)
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    outputQueue_ = queue;
}

void Decoder::setQueues(PacketQueue* inputQueue, FrameQueue* outputQueue)
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    inputQueue_ = inputQueue;
    outputQueue_ = outputQueue;
}

int Decoder::start()
{
    if (started_) {
        return 0;
    }

    if (thread_.joinable()) {
        return AVERROR(EALREADY);
    }

    if (!isOpen()) {
        return AVERROR(EINVAL);
    }

    if (!inputQueueSnapshot() || !outputQueueSnapshot()) {
        return AVERROR(EINVAL);
    }

    abort_ = false;
    pktSerial_ = -1;
    started_ = true;
    thread_ = std::thread(&Decoder::decodeLoop, this);
    return 0;
}

void Decoder::stop()
{
    abort_ = true;
    pauseRequested_.store(false);
    pauseCv_.notify_all();
    abortBoundQueues();

    if (thread_.joinable()) {
        thread_.join();
    }

    started_ = false;
    paused_ = false;
}

void Decoder::pause(bool paused)
{
    if (paused) {
        if (!started_.load() || abort_.load()) {
            return;
        }
        pauseRequested_.store(true);
        // pop 阻塞期间收到 pauseRequested_ 看不到，需要外部戳一下队列
        // 唤醒它。flush 会 notify_all 但保持队列可用，避免误退出。
        PacketQueue* q = inputQueueSnapshot();
        if (q) {
            q->flush();
        }

        std::unique_lock<std::mutex> lock(pauseMutex_);
        pausedAckCv_.wait_for(lock, std::chrono::milliseconds(500), [this] {
            return paused_.load() || !started_.load() || abort_.load();
        });
    } else {
        pauseRequested_.store(false);
        {
            std::lock_guard<std::mutex> lock(pauseMutex_);
            paused_.store(false);
        }
        pauseCv_.notify_all();
    }
}

bool Decoder::isPaused() const
{
    return paused_.load();
}

bool Decoder::isOpen() const
{
    std::lock_guard<std::mutex> lock(codecMutex_);
    return codecCtx_ != nullptr;
}

bool Decoder::isRunning() const
{
    return started_;
}

int Decoder::streamIndex() const
{
    return streamIndex_;
}

AVRational Decoder::timeBase() const
{
    return timeBase_;
}

AVMediaType Decoder::expectedMediaType() const
{
    return expectedMediaType_;
}

AVCodecContext* Decoder::codecContext() const
{
    std::lock_guard<std::mutex> lock(codecMutex_);
    return codecCtx_;
}

void Decoder::setEofCallback(EofCallback cb)
{
    eofCb_ = std::move(cb);
}

void Decoder::setErrorCallback(ErrorCallback cb)
{
    errorCb_ = std::move(cb);
}

int Decoder::configureCodecContext(AVCodecContext* ctx,
                                   const AVCodec*,
                                   const Options& options)
{
    if (!ctx) {
        return AVERROR(EINVAL);
    }

    if (options.threadCount > 0) {
        ctx->thread_count = options.threadCount;
    }

    return 0;
}

int Decoder::handleDecodedFrame(AVFrame* frame, int serial)
{
    return pushFrameToOutput(frame, serial);
}

void Decoder::handleCodecDrained()
{
    if (eofCb_) {
        eofCb_(streamIndex_);
    }
}

int Decoder::pushFrameToOutput(AVFrame* frame, int serial)
{
    if (!frame) {
        return AVERROR(EINVAL);
    }

    FrameQueue* outputQueue = outputQueueSnapshot();
    if (!outputQueue) {
        return AVERROR(EINVAL);
    }

    return outputQueue->push(frame, serial);
}

void Decoder::reportError(int errCode, const std::string& msg)
{
    VP_ERROR("Decoder error stream={} code={} msg={}", streamIndex_, errCode, msg);
    if (errorCb_) {
        errorCb_(errCode, msg);
    }
}

void Decoder::decodeLoop()
{
    AVCodecContext* ctx = codecContext();
    if (!ctx) {
        started_ = false;
        return;
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        const int ret = AVERROR(ENOMEM);
        reportError(ret, "av_frame_alloc failed: " + avErrorString(ret));
        started_ = false;
        return;
    }

    // 接收当前已 send 的所有 frame，每个 frame 推入 output queue 时附带
    // 当前 pktSerial_。期间不再检查任何 flush 标志：seek 由 packet serial
    // 跳变驱动，发生在外层 while 循环的 send_packet 之前。
    auto receiveFrames = [&]() -> int {
        while (!abort_) {
            int ret = avcodec_receive_frame(ctx, frame);
            if (ret == 0) {
                const int handleRet = handleDecodedFrame(frame, pktSerial_);
                av_frame_unref(frame);
                if (handleRet < 0) {
                    if (!abort_) {
                        reportError(handleRet, "FrameQueue push failed");
                    }
                    return handleRet;
                }
                continue;
            }

            av_frame_unref(frame);

            if (ret == AVERROR(EAGAIN)) {
                return 0;
            }

            if (ret == AVERROR_EOF) {
                handleCodecDrained();
                return AVERROR_EOF;
            }

            reportError(ret, "avcodec_receive_frame failed: " + avErrorString(ret));
            return ret;
        }

        return AVERROR(EINTR);
    };

    while (!abort_) {
        if (pauseRequested_.load()) {
            std::unique_lock<std::mutex> lock(pauseMutex_);
            paused_.store(true);
            pausedAckCv_.notify_all();
            pauseCv_.wait(lock, [this] {
                return !pauseRequested_.load() || abort_.load();
            });
            paused_.store(false);
            if (abort_.load()) {
                break;
            }
        }

        // 先把 codec 内已有的 frame 全部 receive 干净，再去 pop 新 packet。
        int ret = receiveFrames();
        if (ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            break;
        }

        PacketQueue* inputQueue = inputQueueSnapshot();
        if (!inputQueue) {
            reportError(AVERROR(EINVAL), "Decoder input queue is not set");
            break;
        }

        AVPacket* pkt = nullptr;
        int pktSerial = -1;
        ret = inputQueue->pop(&pkt, &pktSerial, 100);
        if (ret == PacketQueue::Timeout) {
            continue;
        }
        if (ret == PacketQueue::EndOfStream) {
            ret = avcodec_send_packet(ctx, nullptr);
            if (ret == AVERROR_EOF) {
                handleCodecDrained();
                break;
            }
            if (ret < 0) {
                reportError(ret, "avcodec_send_packet drain failed: " + avErrorString(ret));
                break;
            }

            ret = receiveFrames();
            if (ret < 0 && ret != AVERROR_EOF) {
                break;
            }
            break;
        }
        if (ret == PacketQueue::Aborted) {
            break;
        }
        if (ret != PacketQueue::Ok) {
            break;
        }

        // serial 跳变 = seek 边界。pop 出来的这个 packet 是 seek 后第一个
        // 关键帧（demuxer flush 后的首个 push），它一定要进入新 codec 上下文，
        // 因此这里 flush_buffers 必须在 send_packet 之前调用。
        // 这种"内嵌在 packet 流里的 flush 信号"取代了原先的 flushPending_ 标志，
        // 时机精确卡在新旧 GOP 边界，不再有 mmco / Missing reference 错误。
        if (pktSerial != pktSerial_) {
            std::lock_guard<std::mutex> lock(codecMutex_);
            if (codecCtx_) {
                avcodec_flush_buffers(codecCtx_);
            }
            pktSerial_ = pktSerial;
        }

        ret = avcodec_send_packet(ctx, pkt);
        av_packet_free(&pkt);
        if (ret < 0) {
            if (!abort_) {
                reportError(ret, "avcodec_send_packet failed: " + avErrorString(ret));
            }
            break;
        }
    }

    av_frame_free(&frame);
    FrameQueue* outputQueue = outputQueueSnapshot();
    if (outputQueue) {
        outputQueue->closeProducer();
    }
    started_ = false;
}

void Decoder::abortBoundQueues()
{
    PacketQueue* inputQueue = nullptr;
    FrameQueue* outputQueue = nullptr;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        inputQueue = inputQueue_;
        outputQueue = outputQueue_;
    }

    if (inputQueue) {
        inputQueue->closeConsumer();
    }
    if (outputQueue) {
        outputQueue->closeProducer();
    }
}

PacketQueue* Decoder::inputQueueSnapshot() const
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    return inputQueue_;
}

FrameQueue* Decoder::outputQueueSnapshot() const
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    return outputQueue_;
}

VideoDecoder::VideoDecoder()
    : Decoder(AVMEDIA_TYPE_VIDEO)
{
}

VideoDecoder::~VideoDecoder()
{
    close();
}

void VideoDecoder::close()
{
    Decoder::close();
    resetHardwareContext();
}

int VideoDecoder::configureCodecContext(AVCodecContext* ctx,
                                        const AVCodec* codec,
                                        const Options& options)
{
    int ret = Decoder::configureCodecContext(ctx, codec, options);
    if (ret < 0) {
        return ret;
    }

    if (!ctx) {
        return AVERROR(EINVAL);
    }

    ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    resetHardwareContext();
    if (!options.enableHardware || options.hwDeviceType == AV_HWDEVICE_TYPE_NONE) {
        return 0;
    }

    const AVPixelFormat hwFormat = hardwarePixelFormatForDevice(options.hwDeviceType);
    if (hwFormat == AV_PIX_FMT_NONE) {
        VP_WARN("hardware decoding requested but device type {} has no mapped pixel format; falling back to software decode",
                static_cast<int>(options.hwDeviceType));
        return 0;
    }

    ret = av_hwdevice_ctx_create(&hwDeviceCtx_, options.hwDeviceType, nullptr, nullptr, 0);
    if (ret < 0) {
        VP_WARN("av_hwdevice_ctx_create failed device={} ret={} msg={}; falling back to software decode",
                static_cast<int>(options.hwDeviceType), ret, avErrorString(ret));
        resetHardwareContext();
        return 0;
    }
    ctx->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
    if (!ctx->hw_device_ctx) {
        resetHardwareContext();
        return 0;
    }

    ctx->opaque = this;
    ctx->get_format = &VideoDecoder::getHardwareFormat;
    hwPixelFormat_ = hwFormat;
    hwDeviceType_ = options.hwDeviceType;
    VP_INFO("hardware decoding configured stream={} device={} pixel_format={}",
            streamIndex(), static_cast<int>(hwDeviceType_), static_cast<int>(hwPixelFormat_));
    return 0;
}

int VideoDecoder::handleDecodedFrame(AVFrame* frame, int serial)
{
    if (!frame) {
        return AVERROR(EINVAL);
    }

    const AVPixelFormat format = static_cast<AVPixelFormat>(frame->format);
    if (!isHardwarePixelFormat(format)) {
        return Decoder::handleDecodedFrame(frame, serial);
    }

    AVFrame* swFrame = av_frame_alloc();
    if (!swFrame) {
        return AVERROR(ENOMEM);
    }

    int ret = av_hwframe_transfer_data(swFrame, frame, 0);
    if (ret < 0) {
        reportError(ret, "av_hwframe_transfer_data failed: " + avErrorString(ret));
        av_frame_free(&swFrame);
        return ret;
    }

    ret = av_frame_copy_props(swFrame, frame);
    if (ret < 0) {
        reportError(ret, "av_frame_copy_props failed: " + avErrorString(ret));
        av_frame_free(&swFrame);
        return ret;
    }

    ret = Decoder::handleDecodedFrame(swFrame, serial);
    av_frame_free(&swFrame);
    return ret;
}

void VideoDecoder::handleCodecDrained()
{
    Decoder::handleCodecDrained();
}

AVPixelFormat VideoDecoder::getHardwareFormat(AVCodecContext* ctx, const AVPixelFormat* formats)
{
    auto* decoder = ctx ? static_cast<VideoDecoder*>(ctx->opaque) : nullptr;
    if (decoder && decoder->hwPixelFormat_ != AV_PIX_FMT_NONE) {
        for (const AVPixelFormat* p = formats; p && *p != AV_PIX_FMT_NONE; ++p) {
            if (*p == decoder->hwPixelFormat_) {
                return *p;
            }
        }
        VP_WARN("requested hardware pixel format {} was not offered by decoder; falling back to software format",
                static_cast<int>(decoder->hwPixelFormat_));
    }

    return firstSoftwarePixelFormat(formats);
}

void VideoDecoder::resetHardwareContext()
{
    if (hwDeviceCtx_) {
        av_buffer_unref(&hwDeviceCtx_);
    }
    hwPixelFormat_ = AV_PIX_FMT_NONE;
    hwDeviceType_ = AV_HWDEVICE_TYPE_NONE;
}

AudioDecoder::AudioDecoder()
    : Decoder(AVMEDIA_TYPE_AUDIO)
{
}

int AudioDecoder::configureCodecContext(AVCodecContext* ctx,
                                        const AVCodec* codec,
                                        const Options& options)
{
    int ret = Decoder::configureCodecContext(ctx, codec, options);
    if (ret < 0) {
        return ret;
    }

    if (!ctx) {
        return AVERROR(EINVAL);
    }

    return 0;
}

int AudioDecoder::handleDecodedFrame(AVFrame* frame, int serial)
{
    return Decoder::handleDecodedFrame(frame, serial);
}

void AudioDecoder::handleCodecDrained()
{
    Decoder::handleCodecDrained();
}
