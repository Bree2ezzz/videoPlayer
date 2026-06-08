#ifndef DEMUXER_H
#define DEMUXER_H

#include <QUrl>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include "networkoptions.h"
#include "PacketQueue.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/*
 * 生命周期：
 *   open() -> setPacketQueue()... -> start()
 *              |                        |
 *              v                        v
 *         运行期可动态          内部线程执行 readLoop
 *         切换订阅 / seek            |
 *              |                     v
 *         stop() ----------- 线程 join、关闭 queue 生产端
 *         close()
 *
 * 线程模型：
 *   - Demuxer 自己持有并管理 thread_，不接受外部传入线程
 *   - readLoop 为唯一访问 fmtCtx_ 的线程，外部对 fmtCtx_ 的所有操作
 *     都要转换成"设置请求标志 + 唤醒 readLoop"的异步形式
 */

class Demuxer
{
public:
    // EOF 回调：readLoop 读到末尾时触发（已关闭订阅 queue 的生产端之后）
    using EofCallback = std::function<void()>;
    // 错误回调：打开失败、读取错误等异常路径通知上层
    using ErrorCallback = std::function<void(int errCode, const std::string& msg)>;
    // seek 完成回调：在 demuxer 线程内部、av_seek_frame 成功且 packet queue
    // 已 flush（serial 已递增）后立即触发。回调内可同步重置音频时钟、
    // 通知 UI 等。timestampUs 为本次 seek 的目标位置（微秒）。
    // 回调在 demuxer 线程上调用，实现需自行处理跨线程同步。
    using SeekCompletedCallback = std::function<void(int64_t timestampUs)>;

    Demuxer();
    ~Demuxer();

    Demuxer(const Demuxer&) = delete;
    Demuxer& operator=(const Demuxer&) = delete;

    // ---------- 生命周期 ----------
    // 打开输入（本地文件或 RTSP/RTMP URL），探测流信息。
    // 不启动线程、不读取 packet。返回 0 成功，负值为 AVERROR
    int open(const QUrl& url, const NetworkOptions& options = NetworkOptions());

    // 关闭输入，释放 fmtCtx_。调用前必须已 stop()
    void close();

    // 启动 readLoop 线程。要求：open() 成功、至少已经绑定一个 PacketQueue
    int start();

    // 请求停止：abort_ = true，并关闭所有已订阅 queue 的生产端以唤醒阻塞的 push/pop；
    // 然后 join 线程。该函数可重入，已停止时直接返回
    void stop();

    // ---------- 流信息查询（open 之后即可调用，只读） ----------
    int streamCount() const;
    AVStream* stream(int index) const;

    std::vector<int> audioStreamIndices() const;
    std::vector<int> videoStreamIndices() const;

    // 基于 av_find_best_stream，无匹配时返回 -1
    int bestAudioStreamIndex() const;
    int bestVideoStreamIndex() const;

    // 总时长（微秒，AV_TIME_BASE 单位）。未知返回 -1（如实时流）
    int64_t durationUs() const;

    // 是否为实时流（RTSP/RTMP 等，无 duration、不可 seek）。供 UI 禁用进度条
    bool isRealtime() const;

    // ---------- 订阅管理（可在运行期调用，内部加锁） ----------
    // 绑定某个 stream_index 到一个 PacketQueue，readLoop 会把该流的 packet push 进去。
    // 若该 index 已有绑定则覆盖。queue 的生命周期由调用方保证。
    void setPacketQueue(int streamIndex, PacketQueue* queue);

    // 取消订阅。readLoop 读到该流 packet 时会直接 av_packet_unref 丢弃
    void clearPacketQueue(int streamIndex);

    // 原子流切换：一步完成 "取消 oldIdx + 绑定 newIdx"，避免中间态丢包。
    // 内部会触发一次 seek 到当前播放位置附近的关键帧，保证 newIdx 从 I 帧开始。
    void switchStream(int oldStreamIndex, int newStreamIndex,
                      PacketQueue* queue, int64_t currentPtsUs);

    // ---------- 运行期控制 ----------
    // 异步 seek：只记录请求，真正的 av_seek_frame 在 readLoop 中执行。
    // timestampUs 为 AV_TIME_BASE 单位（微秒）。
    // seek 触发时 readLoop 会：av_seek_frame → flush 所有订阅 queue/推进 serial
    // → 回调消费者设置新 epoch → 继续读
    void seek(int64_t timestampUs);

    // 判断当前是否有未处理的 seek 请求（供上层做 UI 反馈）
    bool isSeeking() const;

    // 暂停 / 恢复 readLoop。
    // pause(true) 同步等待 readLoop 进入暂停点后返回（最长 500ms）。
    // pause(false) 唤醒 readLoop 继续。
    // 暂停期间 seek() 仍可调用，恢复后会立即处理。
    void pause(bool paused);
    bool isPaused() const;

    // ---------- 回调设置（start 之前设置） ----------
    void setEofCallback(EofCallback cb);
    void setErrorCallback(ErrorCallback cb);
    void setSeekCompletedCallback(SeekCompletedCallback cb);

private:
    void readLoop();

    // readLoop 内部使用，不对外暴露
    // 执行实际的 seek，并对所有订阅 queue 执行 flush。调用时持有 seekMutex_
    void doSeekLocked(int64_t timestampUs);

    // 关闭所有订阅 queue 的生产端，通知下游排空后结束
    void closeProducerForAllQueues();

    // 根据 stream_index 找到 queue，未订阅返回 nullptr（内部加锁）
    PacketQueue* findQueue(int streamIndex);

    static int interruptCb(void* opaque);

private:
    // ---------- FFmpeg 上下文 ----------
    AVFormatContext* fmtCtx_ = nullptr;
    bool realtime_ = false;
    std::atomic<int64_t> ioDeadlineUs_{0};
    std::atomic<int64_t> readTimeoutUs_{0};

    // ---------- 订阅表：stream_index -> queue ----------
    // 被 readLoop 和外部线程共享，所有读写都要持 queueMapMutex_
    mutable std::mutex queueMapMutex_;
    std::unordered_map<int, PacketQueue*> packetQueues_;

    // ---------- 线程与退出 ----------
    std::thread thread_;
    std::atomic_bool abort_{false};
    std::atomic_bool started_{false};

    // ---------- seek 异步化 ----------
    // seek() 只写 seekReqUs_ 并置 seekPending_，唤醒 readLoop 处理。
    // readLoop 在每轮循环开头检查
    mutable std::mutex seekMutex_;
    std::condition_variable seekCond_;
    std::atomic_bool seekPending_{false};
    int64_t seekReqUs_ = 0;

    // ---------- 暂停协议 ----------
    // pauseRequested_ 由控制线程置位；readLoop 在循环顶部观察到后置 paused_
    // 并 cv 等待。控制线程通过 pausedCv_ 等到 paused_ 真的为 true 才返回。
    mutable std::mutex pauseMutex_;
    std::condition_variable pauseCv_;       // readLoop 等在这个 cv 上恢复
    std::condition_variable pausedAckCv_;   // 控制线程等 readLoop 真暂停的 ack
    std::atomic_bool pauseRequested_{false};
    std::atomic_bool paused_{false};

    // ---------- 回调 ----------
    EofCallback eofCb_;
    ErrorCallback errorCb_;
    SeekCompletedCallback seekCompletedCb_;
};

#endif // DEMUXER_H
