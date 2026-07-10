#include "audiooutput.h"

#include "app_logger.h"


extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
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

const char* sampleFormatName(AVSampleFormat format)
{
    const char* name = av_get_sample_fmt_name(format);
    return name ? name : "unknown";
}

std::string channelLayoutString(const AVChannelLayout& layout, int fallbackChannels = 0)
{
    AVChannelLayout tmp{};
    const AVChannelLayout* usedLayout = &layout;
    if (layout.nb_channels <= 0 && fallbackChannels > 0) {
        av_channel_layout_default(&tmp, fallbackChannels);
        usedLayout = &tmp;
    }

    char desc[128] = {};
    if (usedLayout->nb_channels > 0 &&
        av_channel_layout_describe(usedLayout, desc, sizeof(desc)) >= 0 &&
        desc[0] != '\0') {
        av_channel_layout_uninit(&tmp);
        return desc;
    }

    const int channels = usedLayout->nb_channels > 0 ? usedLayout->nb_channels : fallbackChannels;
    av_channel_layout_uninit(&tmp);
    return channels > 0 ? std::to_string(channels) + "c" : std::string("unknown");
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
        VP_ERROR("AudioOutput::open rejected invalid source codec parameters ptr={} type={}",
                 static_cast<const void*>(sourceCodecpar),
                 sourceCodecpar ? static_cast<int>(sourceCodecpar->codec_type) : -1);
        return AVERROR(EINVAL);
    }

    VP_INFO("AudioOutput::open source codec_id={} sample_rate={} sample_fmt={} channels={} channel_layout={} target_sample_rate={} target_channels={} target_samples={} target_sample_fmt={}",
            static_cast<int>(sourceCodecpar->codec_id),
            sourceCodecpar->sample_rate,
            sampleFormatName(static_cast<AVSampleFormat>(sourceCodecpar->format)),
            sourceCodecpar->ch_layout.nb_channels,
            channelLayoutString(sourceCodecpar->ch_layout),
            targetParams.sampleRate,
            targetParams.channels,
            targetParams.samplesPerCallback,
            sampleFormatName(targetParams.sampleFormat));

    if (targetParams.sampleRate <= 0 ||
        targetParams.channels <= 0 ||
        targetParams.channels > std::numeric_limits<Uint8>::max() ||
        targetParams.samplesPerCallback <= 0 ||
        targetParams.samplesPerCallback > std::numeric_limits<Uint16>::max()) {
        VP_ERROR("AudioOutput::open rejected invalid target params sample_rate={} channels={} samples={}",
                 targetParams.sampleRate, targetParams.channels, targetParams.samplesPerCallback);
        return AVERROR(EINVAL);
    }

    if (targetParams.sampleFormat != AV_SAMPLE_FMT_S16) {
        VP_ERROR("AudioOutput::open rejected unsupported target sample format {}",
                 sampleFormatName(targetParams.sampleFormat));
        return AVERROR(EINVAL);
    }

    if (sourceCodecpar->sample_rate <= 0 ||
        sourceCodecpar->format == AV_SAMPLE_FMT_NONE ||
        sourceCodecpar->ch_layout.nb_channels <= 0) {
        VP_ERROR("AudioOutput::open rejected incomplete source params sample_rate={} sample_fmt={} channels={} layout={}",
                 sourceCodecpar->sample_rate,
                 sampleFormatName(static_cast<AVSampleFormat>(sourceCodecpar->format)),
                 sourceCodecpar->ch_layout.nb_channels,
                 channelLayoutString(sourceCodecpar->ch_layout));
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

    VP_INFO("AudioOutput SDL device opened id={} requested freq={} channels={} samples={} obtained freq={} channels={} samples={} size={}",
            static_cast<unsigned int>(device_),
            desired.freq,
            static_cast<int>(desired.channels),
            static_cast<int>(desired.samples),
            obtained.freq,
            static_cast<int>(obtained.channels),
            static_cast<int>(obtained.samples),
            static_cast<int>(obtained.size));

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
    tempoInputEof_ = false;
    tempoInputPtsSamples_ = 0;
    tempoPlaybackRate_ = 0.0f;
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
        releaseTempoFilterLocked();
        resampleBuffer_.clear();
        resampleBufferSize_ = 0;
        resampleBufferOffset_ = 0;
        currentFramePtsSec_ = 0.0;
        seekTargetPtsSec_ = -1.0;
        seekSerial_ = -1;
        tempoInputEof_ = false;
        tempoInputPtsSamples_ = 0;
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
    FrameQueue* queue = frameQueueSnapshot();
    if (!opened_.load() || !device_ || !queue) {
        VP_ERROR("AudioOutput::start rejected opened={} device={} frame_queue={}",
                 opened_.load(), static_cast<unsigned int>(device_), static_cast<const void*>(queue));
        return AVERROR(EINVAL);
    }

    VP_INFO("AudioOutput::start device={} sample_rate={} channels={} callback_bytes={} playback_rate={}",
            static_cast<unsigned int>(device_),
            actualParams_.sampleRate,
            actualParams_.channels,
            callbackBufferBytes_,
            playbackRate_.load());

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
    releaseTempoFilterLocked();

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
    releaseTempoFilterLocked();
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

void AudioOutput::setPlaybackRate(float rate)
{
    const float clamped = std::clamp(rate, 0.5f, 2.0f);
    std::lock_guard<std::mutex> lock(callbackMutex_);
    const float old = playbackRate_.load();
    if (std::fabs(old - clamped) < 0.0001f) {
        return;
    }

    playbackRate_.store(clamped);
    VP_INFO("AudioOutput playback rate changed old={} new={}", old, clamped);
    resampleBufferSize_ = 0;
    resampleBufferOffset_ = 0;
    releaseTempoFilterLocked();

    updateAudioClock(audioClock_.load());
}

float AudioOutput::playbackRate() const
{
    return playbackRate_.load();
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
    const double mediaDrift = drift * std::clamp(playbackRate_.load(), 0.5f, 2.0f);
    return std::min(mediaDrift, kMaxDrift);
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
        const double rate = std::clamp(playbackRate_.load(), 0.5f, 2.0f);
        const double pts =
            currentFramePtsSec_ +
            static_cast<double>(consumedSamples) / actualParams_.sampleRate * rate -
            bufferedSec * rate;
        // 高频路径，节流：每 50 次回调（约 1s）打一次
        updateAudioClock(pts);
    }
}

int AudioOutput::refillResampleBuffer()
{
    while (true) {
        const int outputRet = receiveTempoOutputLocked();
        if (outputRet != 0) {
            return outputRet;
        }

        const int feedRet = feedTempoInputLocked();
        if (feedRet <= 0) {
            return feedRet;
        }
    }
}

int AudioOutput::receiveTempoOutputLocked()
{
    if (!tempoSinkCtx_) {
        return 0;
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        return -1;
    }

    const int ret = av_buffersink_get_frame(tempoSinkCtx_, frame);
    if (ret == AVERROR(EAGAIN)) {
        av_frame_free(&frame);
        return 0;
    }
    if (ret == AVERROR_EOF) {
        av_frame_free(&frame);
        if (!eofReported_.exchange(true) && eofCb_) {
            eofCb_();
        }
        return 0;
    }
    if (ret < 0) {
        reportError(ret, "av_buffersink_get_frame failed: " + avErrorString(ret));
        av_frame_free(&frame);
        return -1;
    }

    const AVSampleFormat format = static_cast<AVSampleFormat>(frame->format);
    const int channels = frame->ch_layout.nb_channels > 0
                             ? frame->ch_layout.nb_channels
                             : actualParams_.channels;
    if (frame->sample_rate != actualParams_.sampleRate ||
        channels != actualParams_.channels ||
        frame->nb_samples <= 0) {
        av_frame_free(&frame);
        return 0;
    }

    const int outputBytes = frame->nb_samples * actualParams_.channels * bytesPerSample_;
    if (outputBytes <= 0) {
        av_frame_free(&frame);
        return 0;
    }
    if (static_cast<int>(resampleBuffer_.size()) < outputBytes) {
        resampleBuffer_.resize(outputBytes);
    }

    if (format == AV_SAMPLE_FMT_S16) {
        std::memcpy(resampleBuffer_.data(), frame->data[0], static_cast<size_t>(outputBytes));
    } else if (format == AV_SAMPLE_FMT_S16P) {
        auto* dst = reinterpret_cast<int16_t*>(resampleBuffer_.data());
        for (int sample = 0; sample < frame->nb_samples; ++sample) {
            for (int ch = 0; ch < channels; ++ch) {
                const auto* src = reinterpret_cast<const int16_t*>(frame->extended_data[ch]);
                dst[sample * channels + ch] = src[sample];
            }
        }
    } else {
        const int err = AVERROR(EINVAL);
        reportError(err, "atempo produced unsupported sample format");
        av_frame_free(&frame);
        return -1;
    }

    resampleBufferSize_ = outputBytes;
    resampleBufferOffset_ = 0;
    av_frame_free(&frame);
    return resampleBufferSize_;
}

int AudioOutput::feedTempoInputLocked()
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
            if (tempoSrcCtx_ && !tempoInputEof_) {
                const int closeRet = av_buffersrc_close(tempoSrcCtx_, AV_NOPTS_VALUE, 0);
                if (closeRet < 0) {
                    reportError(closeRet, "av_buffersrc_close failed: " + avErrorString(closeRet));
                    return -1;
                }
                tempoInputEof_ = true;
                return 1;
            }
            if (!eofReported_.exchange(true) && eofCb_) {
                eofCb_();
            }
            return 0;
        }
        if (popRet != FrameQueue::Ok || !frame) {
            return 0;
        }

        if (pktQueue && frameSerial != pktQueue->currentSerial()) {
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

    ret = ensureTempoFilterLocked();
    if (ret < 0) {
        reportError(ret, "ensureTempoFilterLocked failed: " + avErrorString(ret));
        av_frame_free(&frame);
        return -1;
    }

    const int inputSamples = frame->nb_samples - skipSamples;
    const int outSamples = swr_get_out_samples(swrCtx_, inputSamples);
    if (outSamples <= 0) {
        av_frame_free(&frame);
        return 1;
    }

    AVFrame* pcmFrame = av_frame_alloc();
    if (!pcmFrame) {
        av_frame_free(&frame);
        return -1;
    }

    pcmFrame->format = AV_SAMPLE_FMT_S16;
    pcmFrame->sample_rate = actualParams_.sampleRate;
    pcmFrame->nb_samples = outSamples;
    av_channel_layout_default(&pcmFrame->ch_layout, actualParams_.channels);
    ret = av_frame_get_buffer(pcmFrame, 0);
    if (ret < 0) {
        reportError(ret, "av_frame_get_buffer failed: " + avErrorString(ret));
        av_frame_free(&pcmFrame);
        av_frame_free(&frame);
        return -1;
    }

    const AVSampleFormat inputFormat = static_cast<AVSampleFormat>(frame->format);
    const int inputBytesPerSample = av_get_bytes_per_sample(inputFormat);
    const bool planar = av_sample_fmt_is_planar(inputFormat) != 0;
    const int channels = frame->ch_layout.nb_channels > 0
                             ? frame->ch_layout.nb_channels
                             : srcChLayout_.nb_channels;
    const int planes = planar ? channels : 1;
    if (inputBytesPerSample <= 0 || channels <= 0 || planes <= 0) {
        av_frame_free(&pcmFrame);
        av_frame_free(&frame);
        return -1;
    }

    std::vector<const uint8_t*> inputData(planes);
    const int step = inputBytesPerSample * (planar ? 1 : channels);
    for (int plane = 0; plane < planes; ++plane) {
        inputData[plane] = frame->extended_data[plane] + skipSamples * step;
    }

    ret = swr_convert(swrCtx_,
                      pcmFrame->data,
                      outSamples,
                      inputData.data(),
                      inputSamples);
    if (ret < 0) {
        reportError(ret, "swr_convert failed: " + avErrorString(ret));
        av_frame_free(&pcmFrame);
        av_frame_free(&frame);
        return -1;
    }

    const double newFramePts =
        framePtsSec + (frameSampleRate > 0
                           ? static_cast<double>(skipSamples) / frameSampleRate
                           : 0.0);
    if (ret > 0) {
        pcmFrame->nb_samples = ret;
        pcmFrame->pts = tempoInputPtsSamples_;
        const bool firstTempoInput = tempoInputPtsSamples_ == 0;
        tempoInputPtsSamples_ += ret;
        const int addRet = av_buffersrc_add_frame_flags(tempoSrcCtx_,
                                                        pcmFrame,
                                                        AV_BUFFERSRC_FLAG_KEEP_REF);
        if (addRet < 0) {
            reportError(addRet, "av_buffersrc_add_frame_flags failed: " + avErrorString(addRet));
            av_frame_free(&pcmFrame);
            av_frame_free(&frame);
            return -1;
        }

        if (firstTempoInput) {
            VP_DEBUG("feeding first atempo input samples={} pts_samples={} source_pts_sec={} skip_samples={} source_sample_rate={}",
                     ret, pcmFrame->pts, newFramePts, skipSamples, frameSampleRate);
        }

        // atempo may emit samples from a short mixed window; using the newest
        // fed source PTS keeps the audio master clock bounded to that delay.
        currentFramePtsSec_ = newFramePts;
        tempoInputEof_ = false;
        eofReported_ = false;
    }

    av_frame_free(&pcmFrame);
    av_frame_free(&frame);
    return 1;
}

int AudioOutput::ensureTempoFilterLocked()
{
    const float rate = std::clamp(playbackRate_.load(), 0.5f, 2.0f);
    if (tempoGraph_ && std::fabs(tempoPlaybackRate_ - rate) < 0.0001f) {
        return 0;
    }

    releaseTempoFilterLocked();

    if (actualParams_.sampleRate <= 0 || actualParams_.channels <= 0) {
        const int ret = AVERROR(EINVAL);
        VP_ERROR("cannot create atempo graph: invalid actual audio params sample_rate={} channels={}",
                 actualParams_.sampleRate, actualParams_.channels);
        return ret;
    }

    const AVFilter* abuffer = avfilter_get_by_name("abuffer");
    const AVFilter* atempo = avfilter_get_by_name("atempo");
    const AVFilter* abuffersink = avfilter_get_by_name("abuffersink");
    if (!abuffer || !atempo || !abuffersink) {
        const int ret = AVERROR(EINVAL);
        VP_ERROR("cannot create atempo graph: missing filter abuffer={} atempo={} abuffersink={}",
                 static_cast<const void*>(abuffer),
                 static_cast<const void*>(atempo),
                 static_cast<const void*>(abuffersink));
        return ret;
    }

    AVFilterGraph* graph = avfilter_graph_alloc();
    if (!graph) {
        const int ret = AVERROR(ENOMEM);
        VP_ERROR("avfilter_graph_alloc failed ret={} msg={}", ret, avErrorString(ret));
        return ret;
    }

    AVChannelLayout srcLayout{};
    av_channel_layout_default(&srcLayout, actualParams_.channels);
    const std::string srcLayoutDesc = channelLayoutString(srcLayout, actualParams_.channels);
    av_channel_layout_uninit(&srcLayout);

    char srcArgs[256] = {};
    std::snprintf(srcArgs,
                  sizeof(srcArgs),
                  "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
                  actualParams_.sampleRate,
                  actualParams_.sampleRate,
                  sampleFormatName(AV_SAMPLE_FMT_S16),
                  srcLayoutDesc.c_str());

    char tempoArgs[64] = {};
    std::snprintf(tempoArgs, sizeof(tempoArgs), "tempo=%0.6f", static_cast<double>(rate));

    VP_INFO("creating atempo graph rate={} src_args={} tempo_args={}", rate, srcArgs, tempoArgs);

    AVFilterContext* srcCtx = nullptr;
    AVFilterContext* tempoCtx = nullptr;
    AVFilterContext* sinkCtx = nullptr;

    int ret = avfilter_graph_create_filter(&srcCtx, abuffer, "tempo_src", srcArgs, nullptr, graph);
    if (ret < 0) {
        VP_ERROR("avfilter_graph_create_filter tempo_src failed ret={} msg={} args={}",
                 ret, avErrorString(ret), srcArgs);
        avfilter_graph_free(&graph);
        return ret;
    }

    ret = avfilter_graph_create_filter(&tempoCtx, atempo, "tempo", tempoArgs, nullptr, graph);
    if (ret < 0) {
        VP_ERROR("avfilter_graph_create_filter tempo failed ret={} msg={} args={}",
                 ret, avErrorString(ret), tempoArgs);
        avfilter_graph_free(&graph);
        return ret;
    }

    ret = avfilter_graph_create_filter(&sinkCtx, abuffersink, "tempo_sink", nullptr, nullptr, graph);
    if (ret < 0) {
        VP_ERROR("avfilter_graph_create_filter tempo_sink failed ret={} msg={}",
                 ret, avErrorString(ret));
        avfilter_graph_free(&graph);
        return ret;
    }

    ret = avfilter_link(srcCtx, 0, tempoCtx, 0);
    if (ret < 0) {
        VP_ERROR("avfilter_link tempo_src->tempo failed ret={} msg={}", ret, avErrorString(ret));
        avfilter_graph_free(&graph);
        return ret;
    }

    ret = avfilter_link(tempoCtx, 0, sinkCtx, 0);
    if (ret < 0) {
        VP_ERROR("avfilter_link tempo->tempo_sink failed ret={} msg={}", ret, avErrorString(ret));
        avfilter_graph_free(&graph);
        return ret;
    }

    ret = avfilter_graph_config(graph, nullptr);
    if (ret < 0) {
        VP_ERROR("avfilter_graph_config atempo graph failed ret={} msg={} src_args={} tempo_args={}",
                 ret, avErrorString(ret), srcArgs, tempoArgs);
        avfilter_graph_free(&graph);
        return ret;
    }

    tempoGraph_ = graph;
    tempoSrcCtx_ = srcCtx;
    tempoSinkCtx_ = sinkCtx;
    tempoPlaybackRate_ = rate;
    tempoInputEof_ = false;
    tempoInputPtsSamples_ = 0;
    VP_INFO("atempo graph ready rate={} sample_rate={} channels={} sample_fmt={} channel_layout={}",
            rate,
            actualParams_.sampleRate,
            actualParams_.channels,
            sampleFormatName(AV_SAMPLE_FMT_S16),
            srcLayoutDesc);
    return 0;
}

void AudioOutput::releaseTempoFilterLocked()
{
    if (tempoGraph_) {
        VP_DEBUG("releasing atempo graph rate={}", tempoPlaybackRate_);
        avfilter_graph_free(&tempoGraph_);
    }
    tempoSrcCtx_ = nullptr;
    tempoSinkCtx_ = nullptr;
    tempoPlaybackRate_ = 0.0f;
    tempoInputEof_ = false;
    tempoInputPtsSamples_ = 0;
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
    const int outputSampleRate = actualParams_.sampleRate;
    //格式转换协议   2. 3. 4.希望转换成什么样子去给到声卡  5. 6. 7.输入的音频长什么样
    ret = swr_alloc_set_opts2(&swrCtx_,
                              &dstLayout,
                              actualParams_.sampleFormat,
                              outputSampleRate,
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
    VP_ERROR("AudioOutput error code={} msg={}", errCode, msg);
    if (errorCb_) {
        errorCb_(errCode, msg);
    }
}
