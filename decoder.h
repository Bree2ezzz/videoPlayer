#ifndef DECODER_H
#define DECODER_H

#include "FrameQueue.h"
#include "PacketQueue.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/rational.h>
}

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class D3D11Context;

/*
 * Decoder 只负责一条已选中的媒体流：
 *   PacketQueue -> avcodec_send_packet / avcodec_receive_frame -> FrameQueue
 *
 * 生命周期：
 *   open(stream) -> setQueues(packetQueue, frameQueue) -> start()
 *                                                    |
 *                                                    v
 *                                             内部线程 decodeLoop
 *                                                    |
 *   stop() ------------------------------------------+
 *   close()
 *
 * 设计边界：
 *   - Decoder 不拥有 PacketQueue / FrameQueue，它只保存非拥有指针。
 *     后续queue失效后这里也不进行清理只进行指针置空，清理由上层完成
 *   - Decoder 不决定 Demuxer 路由，也不负责播放器级切流流程。
 *   - stop() 会关闭 input queue 消费端和 output queue 生产端来唤醒阻塞线程；队列复用前由上层
 *     在所有相关线程 join 后统一 flush/reset。
 *   - input queue 返回 EndOfStream 时，decodeLoop 会向 FFmpeg 发送 nullptr packet
 *     来 drain 解码器。
 */

class Decoder
{
public:
    struct Options {
        Options()
            : threadCount(0),
              enableHardware(false),
              hwDeviceType(AV_HWDEVICE_TYPE_NONE),
              sharedD3D11(nullptr)
        {}

        // 0 表示交给 FFmpeg 自己决定线程数。
        int threadCount;

        // 后续任务 8.x 接硬解时使用；当前软解阶段保持 false。
        bool enableHardware;
        AVHWDeviceType hwDeviceType;

        // D3D11 profile supplies the application's device; Decoder never owns it.
        D3D11Context* sharedD3D11;
    };

    using EofCallback = std::function<void(int streamIndex)>;
    using ErrorCallback = std::function<void(int errCode, const std::string& msg)>;

    virtual ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // ---------- 生命周期 ----------
    // 用 Demuxer::stream(index) 打开对应流。只初始化 codec，不启动线程。
    int open(AVStream* stream, const Options& options = Options());

    // 更底层的打开接口，方便后续脱离 AVStream 或做单元测试。
    int open(const AVCodecParameters* codecpar,
             AVRational timeBase,
             int streamIndex,
             const Options& options = Options());

    // 关闭 codecCtx_。调用前必须已 stop()。
    virtual void close();

    // 绑定输入/输出队列。queue 生命周期由上层保证。
    void setInputQueue(PacketQueue* queue);
    void setOutputQueue(FrameQueue* queue);
    void setQueues(PacketQueue* inputQueue, FrameQueue* outputQueue);

    // 启动解码线程。要求：open() 成功，并且 input/output queue 均已绑定。
    int start();

    // 请求停止 decodeLoop，并唤醒已绑定队列上可能阻塞的线程。
    // 这里只 join decoder 自己的线程，其他组件由上层播放控制器管理。
    void stop();

    // ---------- seek / 切流辅助 ----------
    // 不再需要主动 requestFlush()。Decoder 通过 PacketQueue 上的 serial 自动
    // 感知 seek：从 packet queue pop 出的 serial 与上一轮不同，立即调用
    // avcodec_flush_buffers，再 send 新 packet。这样 flush 时机精确卡在
    // 新旧 GOP 边界，不会出现"旧 packet 串入新 codec 状态"的 mmco 错误。

    // 暂停 / 恢复 decodeLoop。
    //
    // pause(true) 同步等待 decodeLoop 进入暂停点（最长 500ms）。
    // pause(false) 立即唤醒。
    void pause(bool paused);
    bool isPaused() const;

    // ---------- 状态查询 ----------
    bool isOpen() const;
    bool isRunning() const;

    int streamIndex() const;
    AVRational timeBase() const;
    AVMediaType expectedMediaType() const;
    AVCodecContext* codecContext() const;

    // ---------- 回调设置（建议 start 前设置） ----------
    void setEofCallback(EofCallback cb);
    void setErrorCallback(ErrorCallback cb);

protected:
    explicit Decoder(AVMediaType expectedMediaType):expectedMediaType_(expectedMediaType){}

    // 子类扩展点：视频硬解、低延迟参数、音频特殊参数等可在这里配置。
    virtual int configureCodecContext(AVCodecContext* ctx,
                                      const AVCodec* codec,
                                      const Options& options);

    // 子类扩展点：默认语义是把 frame move 到 output FrameQueue，并附带 serial。
    // 后续视频可在这里做 hwframe transfer，音频可在这里接重采样。
    virtual int handleDecodedFrame(AVFrame* frame, int serial);

    // codec drain 完成后触发。默认调用 eofCb_。
    virtual void handleCodecDrained();

    int pushFrameToOutput(AVFrame* frame, int serial);
    void reportError(int errCode, const std::string& msg);

private:
    void decodeLoop();
    void abortBoundQueues();

    PacketQueue* inputQueueSnapshot() const;
    FrameQueue* outputQueueSnapshot() const;

private:
    // ---------- 解码配置 ----------
    const AVMediaType expectedMediaType_;
    AVCodecContext* codecCtx_ = nullptr;
    const AVCodec* codec_ = nullptr;
    Options options_;
    int streamIndex_ = -1;
    AVRational timeBase_ = {0, 1};

    // ---------- 队列绑定 ----------
    mutable std::mutex queueMutex_;
    PacketQueue* inputQueue_ = nullptr;
    FrameQueue* outputQueue_ = nullptr;

    // ---------- 线程与控制状态 ----------
    std::thread thread_;
    std::atomic_bool abort_{false};
    std::atomic_bool started_{false};
    // 当前正在处理的 packet 流的 serial。pop 出新 serial 时与之比较，
    // 不一致就 avcodec_flush_buffers。初始 -1 保证第一个 packet 一定不
    // 触发 flush（codec 刚 open，本就是干净状态）。
    int pktSerial_ = -1;

    // ---------- 暂停协议 ----------
    mutable std::mutex pauseMutex_;
    std::condition_variable pauseCv_;
    std::condition_variable pausedAckCv_;
    std::atomic_bool pauseRequested_{false};
    std::atomic_bool paused_{false};

    // codecCtx_ 只应由 decodeLoop 持续访问；open/close/flush 控制路径持锁。
    mutable std::mutex codecMutex_;

    // ---------- 回调 ----------
    EofCallback eofCb_;
    ErrorCallback errorCb_;
};

class VideoDecoder : public Decoder
{
public:
    VideoDecoder();
    ~VideoDecoder() override;

    void close() override;

protected:
    int configureCodecContext(AVCodecContext* ctx,
                              const AVCodec* codec,
                              const Options& options) override;
    int handleDecodedFrame(AVFrame* frame, int serial) override;
    void handleCodecDrained() override;

private:
    static AVPixelFormat getHardwareFormat(AVCodecContext* ctx, const AVPixelFormat* formats);
    void resetHardwareContext();

    AVBufferRef* hwDeviceCtx_ = nullptr;
    AVPixelFormat hwPixelFormat_ = AV_PIX_FMT_NONE;
    AVHWDeviceType hwDeviceType_ = AV_HWDEVICE_TYPE_NONE;
    bool requiresD3D11Output_ = false;
};

class AudioDecoder : public Decoder
{
public:
    AudioDecoder();

protected:
    int configureCodecContext(AVCodecContext* ctx,
                              const AVCodec* codec,
                              const Options& options) override;
    int handleDecodedFrame(AVFrame* frame, int serial) override;
    void handleCodecDrained() override;
};

#endif // DECODER_H
