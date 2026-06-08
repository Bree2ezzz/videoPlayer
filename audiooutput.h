#ifndef AUDIOOUTPUT_H
#define AUDIOOUTPUT_H

#include "FrameQueue.h"
#include "PacketQueue.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <SDL2/SDL.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct AudioOutputParams {
    int sampleRate = 44100;
    int channels = 2;
    // 当前 AudioOutput 只支持输出 S16，对应 SDL 的 AUDIO_S16SYS。
    // 字段保留给后续扩展；传入其他格式时 open() 会返回 AVERROR(EINVAL)。
    AVSampleFormat sampleFormat = AV_SAMPLE_FMT_S16;
    // SDL callback 每次请求的采样数（单声道计数）。越小延迟越低，
    // 但 CPU 中断越频繁。1024 是一个比较稳的值。
    int samplesPerCallback = 1024;
};

/*
 * AudioOutput 职责边界：
 *   FrameQueue(AVFrame) -> SwrContext 重采样 -> SDL 音频回调 -> 声卡
 *
 * 设计说明：
 *   - AudioOutput 不拥有 FrameQueue，只持有非拥有指针。队列生命周期由
 *     上层（播放控制器）统一管理。
 *   - 重采样放在 AudioOutput 而不是 AudioDecoder：目标格式由 SDL 设备
 *     协商决定，Decoder 不应当感知下游输出。
 *   - 暂停 / 恢复直接走 SDL_PauseAudioDevice；paused_ 只表示用户暂停意图，
 *     stop() 不把"停止输出"伪装成"用户暂停"。
 *   - audioClock() 是 AVSync 的基准：表示"声卡当前实际正在播放的 pts"，
 *     需要扣除 SDL 已缓存但尚未播放的数据带来的延迟，才能得到精确值。
 *   - seekTo() 在 demuxer 完成 queue serial 切换后清空输出余量、重置
 *     audio clock，并丢弃/裁剪新 serial 中位于目标之前的音频。
 *
 * 线程模型：
 *   - 所有 public 接口在控制线程（主线程/播放控制器）调用
 *   - SDL 在独立的音频回调线程上调用 sdlAudioCallback，回调内部会
 *     pop 帧、重采样、更新 audioClock_，因此需要考虑与控制线程并发
 *   - FrameQueue 指针改动、flush、暂停切换期间，回调线程可能正在执行，
 *     通过 callbackMutex_ 保护关键临界区
 */

class AudioOutput
{
public:
    using EofCallback = std::function<void()>;
    using ErrorCallback = std::function<void(int errCode, const std::string& msg)>;

    AudioOutput();
    ~AudioOutput();

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // ---------- 生命周期 ----------
    // 打开 SDL 设备并初始化 SwrContext。当前只支持 targetParams.sampleFormat = AV_SAMPLE_FMT_S16。
    // sourceCodecpar / sourceTimeBase 来自 AudioDecoder 打开的 stream，
    // 用来推断源格式（sample_fmt / sample_rate / ch_layout）和 pts 单位。
    // 设备默认处于暂停态；调用 start() 之后才会真正开始播放。
    int open(const AVCodecParameters* sourceCodecpar,
             AVRational sourceTimeBase,
             const AudioOutputParams& targetParams = AudioOutputParams());

    // 关闭 SDL 设备、释放 SwrContext。调用前建议已 stop()。
    void close();

    // 绑定输入 FrameQueue。可在运行期调用（与回调并发安全）。
    void setFrameQueue(FrameQueue* queue);

    // 绑定 PacketQueue 用于查询 currentSerial。pop 出的 frame.serial !=
    // packetQueue->currentSerial() 即代表 seek 后的旧帧，立即丢弃。
    void setPacketQueue(PacketQueue* queue);

    // 开始播放：SDL_PauseAudioDevice(0)。要求 open() 成功、FrameQueue 已绑定。
    int start();

    // 停止播放：SDL_PauseAudioDevice(1)，并通过 frameQueue->closeConsumer()
    // 唤醒可能阻塞在 pop 上的等待。不释放 SDL 设备本身（close 才释放）。
    // 不修改用户暂停状态，isPaused() 只反映 pause(true/false) 的意图。
    void stop();

    // ---------- 运行期控制 ----------
    // 暂停 / 恢复。暂停期间 SDL 回调停止调用，audioClock_ 保持不变。
    void pause(bool paused);
    bool isPaused() const;

    // 清空内部 resample 残留缓冲。
    // 内部同时调用 SDL_ClearQueuedAudio 避免残余 PCM 播出。
    // 不重置 audioClock_；seek 流程应调用 seekTo() 以同时设置过滤边界。
    void flush();

    // 精确 seek 边界：由 demuxer 成功 seek 并推进 packet serial 后调用。
    // serial 对应这次 seek 的新音频 epoch；输出侧会丢弃目标前的整帧，
    // 并裁剪跨越 targetPtsSec 的第一帧，避免播放关键帧回退区间的声音。
    void seekTo(double targetPtsSec, int serial);

    // 音量 [0.0, 1.0]。在回调中通过 SDL_MixAudioFormat 对输出做缩放。
    void setVolume(float volume);
    float volume() const;

    // 基础变速播放：通过重采样比例实现变速变调。
    void setPlaybackRate(float rate);
    float playbackRate() const;

    // ---------- 同步接口（供 AVSync 使用） ----------
    // 当前音频时钟（秒）。精确定义：下一刻扬声器将要发出的声音对应的 pts。
    // 实现上 = 最近 pop 出的 frame 的 pts + 该 frame 已消耗的采样数/采样率
    //       - SDL 底层尚未播放的字节数 / 每秒字节数
    double audioClock() const;

    // audioClock_ 最近一次更新以来流逝的墙钟时间（秒）。
    // 供 AVSync 做一阶外推（更精确的同步）。实现可选，先返回 0 也能 work。
    double audioClockDrift() const;

    // ---------- 状态查询 ----------
    bool isOpen() const;
    bool isRunning() const;  // 已 start 且没有用户暂停
    AudioOutputParams actualParams() const; // SDL 协商后的真实格式

    // ---------- 回调 ----------
    void setEofCallback(EofCallback cb);
    void setErrorCallback(ErrorCallback cb);

private:
    // SDL 音频回调（静态入口）。userdata 指向 this。
    static void sdlAudioCallback(void* userdata, uint8_t* stream, int len);

    // 回调实现：把 len 字节 PCM 填入 stream（含音量缩放）。
    void fillStream(uint8_t* stream, int len);

    // 从 FrameQueue 取一帧并重采样到 resampleBuffer_ 中。
    // 返回本次重采样后可用的字节数；EOF 或 Abort 返回 0；失败返回 -1。
    int refillResampleBuffer();

    // 根据源 frame 惰性重建 SwrContext（源格式可能在流切换时变化）。
    // 调用者需已持有 callbackMutex_。
    int ensureSwrContextLocked(const AVFrame* frame);

    // 把 audioClock_ 和 audioClockUpdateTime_ 一起更新（供 AVSync 读取）。
    void updateAudioClock(double pts);

    FrameQueue* frameQueueSnapshot() const;

    void reportError(int errCode, const std::string& msg);

private:
    // ---------- 源格式（open 时由上层指定） ----------
    AVSampleFormat srcSampleFormat_ = AV_SAMPLE_FMT_NONE;
    int srcSampleRate_ = 0;
    AVChannelLayout srcChLayout_{}; // 用 av_channel_layout_copy / uninit 管理
    AVRational srcTimeBase_ = {0, 1};

    // ---------- 目标格式 ----------
    AudioOutputParams requestedParams_{};     // 应用请求的
    AudioOutputParams actualParams_{};        // SDL 协商后的
    int bytesPerSecond_ = 0;       // actual.sampleRate * channels * bytesPerSample
    int bytesPerSample_ = 0;       // 单声道单个采样的字节数
    int callbackBufferBytes_ = 0;  // SDL obtained.size，用于 callback 模式下估算输出延迟

    // ---------- 重采样 ----------
    SwrContext* swrCtx_ = nullptr;
    float swrPlaybackRate_ = 1.0f;      // swrCtx_ 创建时使用的播放速率
    std::vector<uint8_t> resampleBuffer_; // 一帧重采样输出的临时缓冲
    int resampleBufferSize_ = 0;          // 缓冲中可用字节数
    int resampleBufferOffset_ = 0;        // 已经被回调消费的字节数
    double currentFramePtsSec_ = 0.0;     // 当前 buffer 对应源 frame 的 pts（秒）
    double seekTargetPtsSec_ = -1.0;      // >= 0 时过滤 seekSerial_ 的目标前音频
    int seekSerial_ = -1;

    // ---------- SDL 设备 ----------
    SDL_AudioDeviceID device_ = 0;

    // ---------- 队列 ----------
    mutable std::mutex queueMutex_;
    FrameQueue* frameQueue_ = nullptr;
    PacketQueue* packetQueue_ = nullptr;  // 仅用于查询 currentSerial 做 seek 边界过滤

    // ---------- 回调并发保护 ----------
    // 保护 swrCtx_ / resampleBuffer_ / currentFramePtsSec_ 等
    // "仅在音频回调线程使用但会被 flush/close 打断"的资源。
    mutable std::mutex callbackMutex_;

    // ---------- 状态 ----------
    std::atomic_bool opened_{false};
    std::atomic_bool running_{false};
    std::atomic_bool paused_{false};
    std::atomic<float> volume_{1.0f};
    std::atomic<float> playbackRate_{1.0f};

    // audioClock_ 用原子存储以便 AVSync 在其他线程无锁读取
    std::atomic<double> audioClock_{0.0};
    // 最近一次 audioClock_ 更新时的系统时间（秒，单调时钟）
    std::atomic<double> audioClockUpdateTime_{0.0};

    std::atomic_bool eofReported_{false};

    EofCallback eofCb_;
    ErrorCallback errorCb_;
};

#endif // AUDIOOUTPUT_H
