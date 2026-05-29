#include "audiooutput.h"

#include "logging.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace {

std::string avErrorString(int errCode)
{
    char errBuf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errCode, errBuf, sizeof(errBuf));
    return errBuf;
}

double steadySeconds()
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

int sdlErrorCode()
{
    return AVERROR_EXTERNAL;
}

} // namespace

AudioOutput::AudioOutput() = default;

AudioOutput::~AudioOutput()
{
    close();
}

int AudioOutput::open(const AVCodecParameters* sourceCodecpar,
                      AVRational sourceTimeBase,
                      const AudioOutputParams& targetParams)
{

    if (!sourceCodecpar || sourceCodecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        return AVERROR(EINVAL);
    }

    if (targetParams.sampleRate <= 0 ||
        targetParams.channels <= 0 ||
        targetParams.channels > std::numeric_limits<Uint8>::max() ||
        targetParams.samplesPerCallback <= 0 ||
        targetParams.samplesPerCallback > std::numeric_limits<Uint16>::max()) {
        return AVERROR(EINVAL);
    }

    if (targetParams.sampleFormat != AV_SAMPLE_FMT_S16) {
        return AVERROR(EINVAL);
    }

    if (sourceCodecpar->sample_rate <= 0 ||
        sourceCodecpar->format == AV_SAMPLE_FMT_NONE ||
        sourceCodecpar->ch_layout.nb_channels <= 0) {
        return AVERROR(EINVAL);
    }

    close();

    requestedParams_ = targetParams;
    //记录源音频的元数据，从avstream中得到
    srcSampleFormat_ = static_cast<AVSampleFormat>(sourceCodecpar->format);
    srcSampleRate_ = sourceCodecpar->sample_rate;
    srcTimeBase_ = sourceTimeBase;

    int ret = av_channel_layout_copy(&srcChLayout_, &sourceCodecpar->ch_layout);
    if (ret < 0) {
        reportError(ret, "av_channel_layout_copy failed: " + avErrorString(ret));
        return ret;
    }
    //配置并打开sdl音频设备
    SDL_AudioSpec desired{};
    SDL_AudioSpec obtained{};
    desired.freq = requestedParams_.sampleRate;
    desired.format = AUDIO_S16SYS;
    desired.channels = static_cast<Uint8>(requestedParams_.channels);
    desired.samples = static_cast<Uint16>(requestedParams_.samplesPerCallback);
    //readloop，当sdl中音频缓存不足就会调用这个callback，这个是静态全局函数，传个this
    desired.callback = &AudioOutput::sdlAudioCallback;
    desired.userdata = this;

    //obtained是经过sdl分析本机是否支持后，把最后用于实际播放的格式保存在内。
    device_ = SDL_OpenAudioDevice(
        nullptr,
        0,
        &desired,
        &obtained,
        SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);

    if (!device_) {
        const int err = sdlErrorCode();
        reportError(err, std::string("SDL_OpenAudioDevice failed: ") + SDL_GetError());
        av_channel_layout_uninit(&srcChLayout_);
        return err;
    }

    actualParams_.sampleRate = obtained.freq;
    actualParams_.channels = obtained.channels;
    actualParams_.sampleFormat = AV_SAMPLE_FMT_S16;
    actualParams_.samplesPerCallback = obtained.samples;

    bytesPerSample_ = av_get_bytes_per_sample(actualParams_.sampleFormat);
    bytesPerSecond_ = actualParams_.sampleRate * actualParams_.channels * bytesPerSample_;
    callbackBufferBytes_ = static_cast<int>(obtained.size);

    if (bytesPerSample_ <= 0 || bytesPerSecond_ <= 0) {
        const int err = AVERROR(EINVAL);
        reportError(err, "Invalid SDL audio format");
        close();
        return err;
    }

    opened_ = true;
    running_ = false;
    paused_ = false;
    eofReported_ = false;
    resampleBufferSize_ = 0;
    resampleBufferOffset_ = 0;
    currentFramePtsSec_ = 0.0;
    seekTargetPtsSec_ = -1.0;
    seekSerial_ = -1;
    updateAudioClock(0.0);
    return 0;
}

void AudioOutput::close()
{
    stop();

    if (device_) {
        SDL_CloseAudioDevice(device_);
        device_ = 0;
    }

    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (swrCtx_) {
            swr_free(&swrCtx_);
        }
        resampleBuffer_.clear();
        resampleBufferSize_ = 0;
        resampleBufferOffset_ = 0;
        currentFramePtsSec_ = 0.0;
        seekTargetPtsSec_ = -1.0;
        seekSerial_ = -1;
    }

    av_channel_layout_uninit(&srcChLayout_);
    srcSampleFormat_ = AV_SAMPLE_FMT_NONE;
    srcSampleRate_ = 0;
    srcTimeBase_ = {0, 1};

    requestedParams_ = AudioOutputParams{};
    actualParams_ = AudioOutputParams{};
    bytesPerSecond_ = 0;
    bytesPerSample_ = 0;
    callbackBufferBytes_ = 0;

    opened_ = false;
    running_ = false;
    paused_ = false;
    eofReported_ = false;
    updateAudioClock(0.0);
}

void AudioOutput::setFrameQueue(FrameQueue* queue)
{
    std::lock_guard<std::mutex> callbackLock(callbackMutex_);
    std::lock_guard<std::mutex> queueLock(queueMutex_);
    frameQueue_ = queue;
}

void AudioOutput::setPacketQueue(PacketQueue* queue)
{
    std::lock_guard<std::mutex> queueLock(queueMutex_);
    packetQueue_ = queue;
}

int AudioOutput::start()
{
    if (!opened_.load() || !device_ || !frameQueueSnapshot()) {
        return AVERROR(EINVAL);
    }

    eofReported_ = false;
    running_ = true;
    paused_ = false;
    SDL_PauseAudioDevice(device_, 0);
    return 0;
}

void AudioOutput::stop()
{
    const bool shouldCloseConsumer = opened_.load() || running_.load();

    if (device_) {
        SDL_PauseAudioDevice(device_, 1);
    }

    running_ = false;

    FrameQueue* queue = shouldCloseConsumer ? frameQueueSnapshot() : nullptr;
    if (queue) {
        queue->closeConsumer();
    }
}

void AudioOutput::pause(bool paused)
{
    if (!opened_.load() || !device_) {
        return;
    }

    paused_ = paused;

    if (!running_.load()) {
        return;
    }

    SDL_PauseAudioDevice(device_, paused ? 1 : 0);
}

bool AudioOutput::isPaused() const
{
    return paused_.load();
}

void AudioOutput::flush()
{
    std::lock_guard<std::mutex> lock(callbackMutex_);
    resampleBufferSize_ = 0;
    resampleBufferOffset_ = 0;
    eofReported_ = false;

    if (device_) {
        SDL_ClearQueuedAudio(device_);
    }
}

void AudioOutput::seekTo(double targetPtsSec, int serial)
{
    if (!std::isfinite(targetPtsSec) || targetPtsSec < 0.0) {
        return;
    }

    std::lock_guard<std::mutex> lock(callbackMutex_);
    resampleBufferSize_ = 0;
    resampleBufferOffset_ = 0;
    eofReported_ = false;
    seekTargetPtsSec_ = targetPtsSec;
    seekSerial_ = serial;

    if (swrCtx_) {
        swr_close(swrCtx_);
        const int ret = swr_init(swrCtx_);
        if (ret < 0) {
            reportError(ret, "swr_init after seek failed: " + avErrorString(ret));
            swr_free(&swrCtx_);
        }
    }

    if (device_) {
        SDL_ClearQueuedAudio(device_);
    }

    VP_LOG_DEBUG() << "audio seek boundary target=" << targetPtsSec
                   << " serial=" << serial
                   << " prevAudioClock=" << audioClock_.load();
    currentFramePtsSec_ = targetPtsSec;
    updateAudioClock(targetPtsSec);
}

void AudioOutput::setVolume(float volume)
{
    volume_.store(std::clamp(volume, 0.0f, 1.0f));
}

float AudioOutput::volume() const
{
    return volume_.load();
}

double AudioOutput::audioClock() const
{
    return audioClock_.load();
}

double AudioOutput::audioClockDrift() const
{
    const double updateTime = audioClockUpdateTime_.load();
    if (updateTime <= 0.0 || !isRunning()) {
        return 0.0;
    }
    const double drift = steadySeconds() - updateTime;
    // 正常播放时 drift 不超过一个回调周期（~23ms）。seek 后音频帧尚未到达时
    // drift 会持续累积，导致 masterClock 超前真实播放位置。限制 drift 上限
    // 防止 AVSync 算法误判视频"落后"而快速跳帧。
    constexpr double kMaxDrift = 0.1;
    return std::min(drift, kMaxDrift);
}

bool AudioOutput::isOpen() const
{
    return opened_.load();
}

bool AudioOutput::isRunning() const
{
    return opened_.load() && running_.load() && !paused_.load();
}

AudioOutputParams AudioOutput::actualParams() const
{
    return actualParams_;
}

void AudioOutput::setEofCallback(EofCallback cb)
{
    eofCb_ = std::move(cb);
}

void AudioOutput::setErrorCallback(ErrorCallback cb)
{
    errorCb_ = std::move(cb);
}

void AudioOutput::sdlAudioCallback(void* userdata, uint8_t* stream, int len)
{
    AudioOutput* output = static_cast<AudioOutput*>(userdata);
    if (!output) {
        std::memset(stream, 0, len);
        return;
    }

    output->fillStream(stream, len);
}

void AudioOutput::fillStream(uint8_t* stream, int len)
{
    std::lock_guard<std::mutex> lock(callbackMutex_);
    //stream需要在回调内部填充而不是在外部准备好
    std::memset(stream, 0, len);

    int written = 0;
    while (written < len) {
        //resampleBufferOffset_是以及被回调使用的字节数，resampleBufferSize_是已经读到缓冲区的字节数
        //>=判断成功代表写入缓存区的数据已经被消耗完毕，需要再读一次
        if (resampleBufferOffset_ >= resampleBufferSize_) {
            const int ret = refillResampleBuffer();
            if (ret <= 0) {
                break;
            }
        }

        const int remain = resampleBufferSize_ - resampleBufferOffset_;
        const int copyLen = std::min(remain, len - written);
        const int volume = static_cast<int>(
            std::clamp(volume_.load(), 0.0f, 1.0f) * SDL_MIX_MAXVOLUME);

        SDL_MixAudioFormat(stream + written,
                           resampleBuffer_.data() + resampleBufferOffset_,
                           AUDIO_S16SYS,
                           copyLen,
                           volume);

        resampleBufferOffset_ += copyLen;
        written += copyLen;
    }

    if (written > 0 && bytesPerSecond_ > 0 && actualParams_.sampleRate > 0) {
        const int frameBytes = actualParams_.channels * bytesPerSample_;
        const int consumedSamples =
            frameBytes > 0 ? resampleBufferOffset_ / frameBytes : 0;
        const int pendingBytes = device_ ? static_cast<int>(SDL_GetQueuedAudioSize(device_)) : 0;
        const double bufferedSec =
            static_cast<double>(pendingBytes + callbackBufferBytes_) / bytesPerSecond_;
        const double pts =
            currentFramePtsSec_ +
            static_cast<double>(consumedSamples) / actualParams_.sampleRate -
            bufferedSec;
        // 高频路径，节流：每 50 次回调（约 1s）打一次
        static thread_local int s_logCounter = 0;
        if ((++s_logCounter % 50) == 0) {
            VP_LOG_DEBUG() << "fillStream pts=" << pts
                           << " currentFramePts=" << currentFramePtsSec_
                           << " bufferedSec=" << bufferedSec
                           << " prevAudioClock=" << audioClock_.load();
        }
        updateAudioClock(pts);
    }
}

int AudioOutput::refillResampleBuffer()
{
    FrameQueue* queue = nullptr;
    PacketQueue* pktQueue = nullptr;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queue = frameQueue_;
        pktQueue = packetQueue_;
    }
    if (!queue) {
        return 0;
    }

    // 在单次 SDL 回调内一次性吃掉旧 serial 帧以及精确 seek 目标前的帧。
    AVFrame* frame = nullptr;
    int frameSerial = -1;
    int skipSamples = 0;
    double framePtsSec = 0.0;
    int frameSampleRate = 0;
    while (true) {
        const int popRet = queue->pop(&frame, &frameSerial, 0);
        if (popRet == FrameQueue::Timeout) {
            return 0;
        }
        if (popRet == FrameQueue::EndOfStream) {
            if (!eofReported_.exchange(true) && eofCb_) {
                eofCb_();
            }
            return 0;
        }
        if (popRet != FrameQueue::Ok || !frame) {
            return 0;
        }

        if (pktQueue && frameSerial != pktQueue->currentSerial()) {
            VP_LOG_DEBUG() << "drop stale audio frame serial=" << frameSerial
                           << " current=" << pktQueue->currentSerial();
            av_frame_free(&frame);
            frame = nullptr;
            continue;
        }

        int64_t framePts = frame->pts;
        if (framePts == AV_NOPTS_VALUE) {
            framePts = frame->best_effort_timestamp;
        }
        framePtsSec =
            framePts != AV_NOPTS_VALUE
                ? static_cast<double>(framePts) * av_q2d(srcTimeBase_)
                : audioClock_.load();
        frameSampleRate = frame->sample_rate > 0 ? frame->sample_rate : srcSampleRate_;
        skipSamples = 0;

        if (frameSerial == seekSerial_ && seekTargetPtsSec_ >= 0.0 &&
            framePts != AV_NOPTS_VALUE && frameSampleRate > 0) {
            const double endPts =
                framePtsSec + static_cast<double>(frame->nb_samples) / frameSampleRate;
            if (endPts <= seekTargetPtsSec_) {
                VP_LOG_DEBUG() << "drop pre-target audio frame serial=" << frameSerial
                               << " pts=" << framePtsSec
                               << " target=" << seekTargetPtsSec_;
                av_frame_free(&frame);
                frame = nullptr;
                continue;
            }

            if (framePtsSec < seekTargetPtsSec_) {
                const double samplesToSkip =
                    (seekTargetPtsSec_ - framePtsSec) * frameSampleRate;
                skipSamples = std::clamp(
                    static_cast<int>(std::ceil(samplesToSkip)), 0, frame->nb_samples);
            }
            seekTargetPtsSec_ = -1.0;
            seekSerial_ = -1;
        } else if (frameSerial == seekSerial_ && seekTargetPtsSec_ >= 0.0) {
            // 没有可用 PTS 时不能安全裁剪；消费第一帧并停止过滤，避免静音卡死。
            seekTargetPtsSec_ = -1.0;
            seekSerial_ = -1;
        }

        if (skipSamples >= frame->nb_samples) {
            av_frame_free(&frame);
            frame = nullptr;
            continue;
        }
        break;
    }

    int ret = ensureSwrContextLocked(frame);
    if (ret < 0) {
        av_frame_free(&frame);
        return -1;
    }
    const int inputSamples = frame->nb_samples - skipSamples;
    //预估输出buffer需要多大,为了防止swr_convert写爆内存
    const int outSamples = swr_get_out_samples(swrCtx_, inputSamples);
    if (outSamples <= 0) {
        //小于等于0出现直接return，不去处理这一frame
        av_frame_free(&frame);
        return 0;
    }
    //单通道输出样本数*通道数*每个样本多少字节  是一个安全上限
    const int outBytes = outSamples * actualParams_.channels * bytesPerSample_;
    if (static_cast<int>(resampleBuffer_.size()) < outBytes) {
        resampleBuffer_.resize(outBytes);
    }

    const AVSampleFormat inputFormat = static_cast<AVSampleFormat>(frame->format);
    const int inputBytesPerSample = av_get_bytes_per_sample(inputFormat);
    const bool planar = av_sample_fmt_is_planar(inputFormat) != 0;
    const int channels = frame->ch_layout.nb_channels > 0
                             ? frame->ch_layout.nb_channels
                             : srcChLayout_.nb_channels;
    const int planes = planar ? channels : 1;
    if (inputBytesPerSample <= 0 || channels <= 0 || planes <= 0) {
        av_frame_free(&frame);
        return -1;
    }

    std::vector<const uint8_t*> inputData(planes);
    const int step = inputBytesPerSample * (planar ? 1 : channels);
    for (int plane = 0; plane < planes; ++plane) {
        inputData[plane] = frame->extended_data[plane] + skipSamples * step;
    }

    uint8_t* outPtr = resampleBuffer_.data();
    ret = swr_convert(swrCtx_,
                      &outPtr,
                      outSamples,
                      inputData.data(),
                      inputSamples);
    if (ret < 0) {
        reportError(ret, "swr_convert failed: " + avErrorString(ret));
        av_frame_free(&frame);
        return -1;
    }
    //这里计算的是实际产生的数据量
    resampleBufferSize_ = ret * actualParams_.channels * bytesPerSample_;
    resampleBufferOffset_ = 0;
    const double newFramePts =
        framePtsSec + (frameSampleRate > 0
                           ? static_cast<double>(skipSamples) / frameSampleRate
                           : 0.0);
    VP_LOG_DEBUG() << "refill new frame pts=" << newFramePts
                   << " prevCurrentFramePts=" << currentFramePtsSec_
                   << " serial=" << frameSerial;
    currentFramePtsSec_ = newFramePts;

    av_frame_free(&frame);
    return resampleBufferSize_;
}

int AudioOutput::ensureSwrContextLocked(const AVFrame* frame)
{
    //swrcontext是音频重采样/格式转换器.负责采样率转换、采样格式转换（float-》int16）、声道转换（5.1-》stereo）、planar/interleaved转换（FLTP->S16）
    //frame的格式理论上可以发生变化，例如直播流切换。这里通过判断保存的frame格式与新传入的frame格式是否相同来进行决定是否需要复用旧转换器还是重新创建
    if (!frame) {
        return AVERROR(EINVAL);
    }

    const AVSampleFormat frameFormat = static_cast<AVSampleFormat>(frame->format);
    const int frameSampleRate = frame->sample_rate > 0 ? frame->sample_rate : srcSampleRate_;
    if (frameFormat == AV_SAMPLE_FMT_NONE || frameSampleRate <= 0) {
        return AVERROR(EINVAL);
    }

    AVChannelLayout frameLayout{};
    int ret = 0;
    if (frame->ch_layout.nb_channels > 0) {
        //拷贝声道布局
        ret = av_channel_layout_copy(&frameLayout, &frame->ch_layout);
    } else if (srcChLayout_.nb_channels > 0) {
        ret = av_channel_layout_copy(&frameLayout, &srcChLayout_);
    } else {
        return AVERROR(EINVAL);
    }

    if (ret < 0) {
        reportError(ret, "av_channel_layout_copy failed: " + avErrorString(ret));
        return ret;
    }

    const bool sameSource =
        swrCtx_ &&
        srcSampleFormat_ == frameFormat &&
        srcSampleRate_ == frameSampleRate &&
        av_channel_layout_compare(&srcChLayout_, &frameLayout) == 0;

    if (sameSource) {
        //相同说明目前已有的swr_convert可以继续使用，将临时变量frameLayout释放
        av_channel_layout_uninit(&frameLayout);
        return 0;
    }
    //走到这说明不相同，把在类中保存的swrCtx释放
    if (swrCtx_) {
        swr_free(&swrCtx_);
    }
    //保存的srcChLayout释放,并将新值拷贝进去
    av_channel_layout_uninit(&srcChLayout_);
    ret = av_channel_layout_copy(&srcChLayout_, &frameLayout);
    av_channel_layout_uninit(&frameLayout);
    if (ret < 0) {
        reportError(ret, "av_channel_layout_copy failed: " + avErrorString(ret));
        return ret;
    }

    srcSampleFormat_ = frameFormat;
    srcSampleRate_ = frameSampleRate;

    AVChannelLayout dstLayout{};
    av_channel_layout_default(&dstLayout, actualParams_.channels);
    //格式转换协议   2. 3. 4.希望转换成什么样子去给到声卡  5. 6. 7.输入的音频长什么样
    ret = swr_alloc_set_opts2(&swrCtx_,
                              &dstLayout,
                              actualParams_.sampleFormat,
                              actualParams_.sampleRate,
                              &srcChLayout_,
                              srcSampleFormat_,
                              srcSampleRate_,
                              0,
                              nullptr);
    av_channel_layout_uninit(&dstLayout);
    if (ret < 0) {
        reportError(ret, "swr_alloc_set_opts2 failed: " + avErrorString(ret));
        swr_free(&swrCtx_);
        return ret;
    }
    //初始化重采样上下文，让它准备好工作但是本身没有数据转换。
    ret = swr_init(swrCtx_);
    if (ret < 0) {
        reportError(ret, "swr_init failed: " + avErrorString(ret));
        swr_free(&swrCtx_);
        return ret;
    }

    return 0;
}

void AudioOutput::updateAudioClock(double pts)
{
    audioClock_.store(pts);
    audioClockUpdateTime_.store(steadySeconds());
}

FrameQueue* AudioOutput::frameQueueSnapshot() const
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    return frameQueue_;
}

void AudioOutput::reportError(int errCode, const std::string& msg)
{
    if (errorCb_) {
        errorCb_(errCode, msg);
    }
}
