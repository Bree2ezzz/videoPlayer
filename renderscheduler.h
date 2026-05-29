#ifndef RENDERSCHEDULER_H
#define RENDERSCHEDULER_H

#include "FrameQueue.h"
#include "PacketQueue.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

extern "C" {
extern "C" {
#include <libavutil/rational.h>
}
}

class VideoRendererBase;
class AVSync;

/*
 * RenderScheduler 职责边界：
 *   FrameQueue(AVFrame) -> 依据时钟计算显示时机 -> VideoRendererBase::renderFrame
 *
 * 设计说明：
 *   - 独立线程运行 scheduleLoop，拥有自己的 thread_
 *   - 不拥有 FrameQueue / VideoRendererBase / AVSync，只持有非拥有指针
 *   - 两种工作模式：
 *       SourceFps：按源帧率定时驱动，不做音视频同步（任务 4.3 使用）
 *       ClockSync：把 (framePts, lastFramePts) 交给 AVSync 计算等待时长
 *                  （任务 5.x 接入 AVSync 后使用）
 *   - 同步策略集中在 AVSync 内，本类只做"取帧 → 等待 → 渲染"，
 *     不知道 ffplay 同步公式细节
 *   - 切换模式时对外接口不变，上层代码无需修改
 *   - 时基（timeBase）由上层在 start 前通过 setTimeBase 传入；scheduleLoop
 *     用它把 frame->pts 转换为秒
 *
 * 线程模型：
 *   - 所有 public 接口在控制线程调用
 *   - scheduleLoop 在内部线程；对共享状态通过原子变量 / mutex 保护
 */

class RenderScheduler
{
public:
    enum class Mode {
        SourceFps,   // 按源帧率定时驱动（不做音视频同步，任务 4.3 使用）
        ClockSync,   // 通过 AVSync 与外部时钟同步（任务 5.x 接入 AVSync 后使用）
    };

    using EofCallback = std::function<void()>;
    using ErrorCallback = std::function<void(int errCode, const std::string& msg)>;

    RenderScheduler();
    ~RenderScheduler();

    RenderScheduler(const RenderScheduler&) = delete;
    RenderScheduler& operator=(const RenderScheduler&) = delete;

    // ---------- 配置 ----------
    // 绑定帧源和渲染器。允许在 stop 状态下重新绑定。
    // stop() 会关闭 queue 的消费端；复用同一个 FrameQueue 前，上层需要在
    // 相关线程都停止后对 queue 执行 flush/reset，使其处于未 close 状态。
    void setFrameQueue(FrameQueue* queue);
    // 绑定 PacketQueue 用于查询 currentSerial。pop 出的 frame.serial !=
    // packetQueue->currentSerial() 即代表 seek 后的旧帧，立即丢弃。
    // 不绑定时（如纯视频回放）退化为不做 serial 过滤。
    void setPacketQueue(PacketQueue* queue);
    void setRenderer(VideoRendererBase* renderer);

    // 源流的 pts 时基。来自 AVStream::time_base。start() 前必须设置
    void setTimeBase(AVRational timeBase);

    // 源帧率，用于 SourceFps 模式计算定时间隔。
    // 通常取 AVStream::avg_frame_rate。未知时传 {0,1}，调度器回退到 25fps
    void setSourceFrameRate(AVRational frameRate);

    // 绑定同步策略对象（ClockSync 模式必需）。SourceFps 模式下可不设置。
    // 允许传 nullptr 解绑。运行期切换 Mode 前需保证调用过本接口。
    void setSync(AVSync* sync);

    // 显示模式切换。运行期切换安全（内部加锁）。
    // 切到 ClockSync 时若未绑定 AVSync，调度器自动回退到 SourceFps 行为
    // 直到 sync 被绑定，避免崩溃。
    void setMode(Mode mode);
    Mode mode() const;

    // ---------- 生命周期 ----------
    // 启动调度线程。要求：FrameQueue / Renderer 已绑定，TimeBase 已设置。
    int start();

    // 请求停止：abort_ = true，关闭 FrameQueue 消费端唤醒阻塞 pop；join 线程
    void stop();

    // ---------- 运行期控制 ----------
    // 暂停：scheduleLoop 停止从 queue pop，最后一帧保持显示
    void pause(bool paused);
    bool isPaused() const;

    // 清除内部时序状态（frameTimer_、上一帧 pts 等）。
    // 用于 seek 后让调度器重新对齐时钟。不清 FrameQueue 本身。
    void flush();

    // demuxer 已完成 seek 且 packet serial 已推进后设置新的精确播放起点。
    // 目标前的视频帧仍由 decoder 产生以建立参考图像，但不会提交给 renderer。
    void seekTo(double targetPtsSec, int serial);

    // ---------- 状态查询 ----------
    bool isRunning() const;
    // 最近一帧显示的 pts（秒）。供上层做进度查询的次要参考（音频时钟更准）
    double lastDisplayedPts() const;

    // ---------- 回调 ----------
    // 可在运行期替换；内部会快照 std::function 后再调用，避免回调里重入死锁。
    void setEofCallback(EofCallback cb);
    void setErrorCallback(ErrorCallback cb);

private:
    void scheduleLoop();

    // 计算下一帧实际显示前应当等待的时间（秒）
    // SourceFps：返回理论帧间隔
    // ClockSync：转交给 AVSync::computeVideoTargetDelay；
    //            未绑定 AVSync 时回退到 SourceFps 行为
    double computeDelaySec(double framePtsSec, double lastFramePtsSec);

    // 根据 timeBase_ 把 AVFrame 的 pts 转成秒；无效 pts 返回 NaN
    double framePtsToSeconds(const AVFrame* frame) const;

    // 快照取值，避免调用方长时间持锁
    FrameQueue* frameQueueSnapshot() const;
    PacketQueue* packetQueueSnapshot() const;
    VideoRendererBase* rendererSnapshot() const;
    AVSync* syncSnapshot() const;

    void reportError(int errCode, const std::string& msg);

private:
    // ---------- 外部对象（非拥有） ----------
    mutable std::mutex configMutex_;
    FrameQueue* frameQueue_ = nullptr;
    PacketQueue* packetQueue_ = nullptr;  // 仅用于查询 currentSerial 做 seek 边界过滤
    VideoRendererBase* renderer_ = nullptr;
    AVSync* sync_ = nullptr;

    // ---------- 时基 / 帧率 ----------
    std::atomic<int> timeBaseNum_{0};
    std::atomic<int> timeBaseDen_{1};
    std::atomic<int> frameRateNum_{0};
    std::atomic<int> frameRateDen_{1};
    // SourceFps 模式默认帧率（frameRate 无效时使用）
    static constexpr double kFallbackFps = 25.0;

    // ---------- 模式 ----------
    std::atomic<Mode> mode_{Mode::SourceFps};

    // ---------- 调度内部状态（仅 scheduleLoop 访问） ----------
    // 上一帧的 pts（秒），用于计算帧间隔
    double lastPtsSec_ = 0.0;
    // 下一帧理论应当显示的墙钟时间点（秒，steady_clock）
    double frameTimer_ = 0.0;
    // lastPtsSec_ 所属的 packet epoch。serial 变化时禁止跨 epoch 计算 delay。
    int timingSerial_ = -1;

    // ---------- 线程 / 状态 ----------
    std::thread thread_;
    std::atomic_bool abort_{false};
    std::atomic_bool started_{false};
    std::atomic_bool paused_{false};
    std::atomic_bool flushRequested_{false};
    std::atomic_bool eofReported_{false};
    std::atomic<double> seekTargetPtsSec_{-1.0};
    std::atomic<int> seekSerial_{-1};

    // 最近显示帧的 pts（秒）。isSeeking 后的 UI 反馈可用
    std::atomic<double> lastDisplayedPts_{0.0};

    // ---------- 回调 ----------
    mutable std::mutex callbackMutex_;
    EofCallback eofCb_;
    ErrorCallback errorCb_;
};

#endif // RENDERSCHEDULER_H
