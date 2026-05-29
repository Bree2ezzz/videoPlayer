#ifndef AVSYNC_H
#define AVSYNC_H

#include <atomic>
#include <functional>
#include <mutex>

/*
 * AVSync 职责边界：
 *   音视频同步的策略中心。本项目固定采用 Audio Master：
 *     - 音频按声卡节奏自然推进，由 AudioOutput::audioClock() 给出
 *     - 视频（以及未来的字幕等）作为"从动"轨道，询问 masterClock()
 *       并据此调整自身显示时机
 *
 * 设计说明：
 *   - 不拥有 AudioOutput，只通过两个回调拿值：
 *       audioClockFn 返回当前 audio clock（秒）
 *       audioDriftFn 返回 audio clock 自上次更新以来流逝的墙钟时间（秒）
 *     这种间接方式让单元测试可以注入 fake clock，未来也方便扩展到
 *     "外部时钟"（NTP / 多端协同播放）等场景
 *   - 同步算法（compute_target_delay）参考 ffplay 实现，集中在本类内部，
 *     RenderScheduler 只调度不计算策略，单一职责
 *   - 阈值以 static constexpr 形式公开，供调试和未来运行期调参参考
 *
 * 线程模型：
 *   - 所有 public 接口可在任意线程调用
 *   - audioClockFn_ / audioDriftFn_ 替换通过 sourceMutex_ 保护
 *   - lastVideoDiff_ 用 atomic，UI/日志线程可无锁读取
 */

class AVSync
{
public:
    using ClockFn = std::function<double()>;
    using DriftFn = std::function<double()>;

    // ---------- ffplay 同步常量 ----------
    // 视频与主时钟偏差小于该值时不做修正（认为已经"对齐"）
    static constexpr double kSyncThresholdMin = 0.04;   // 40 ms
    // 偏差超过该值就用最大力度修正（防止单次抖动放大延迟）
    static constexpr double kSyncThresholdMax = 0.10;   // 100 ms
    // 帧间隔大于该值时，视频偏早不再"加倍延后"，而是按偏差精确等待
    static constexpr double kFrameDupThreshold = 0.10;
    // 偏差超过该值视为时钟跳变（seek 后未对齐 / 流断），不强行修正
    static constexpr double kNoSyncThreshold = 10.0;    // 10 s

    AVSync();
    ~AVSync();

    AVSync(const AVSync&) = delete;
    AVSync& operator=(const AVSync&) = delete;

    // ---------- 时钟源绑定 ----------
    // 绑定音频时钟。clockFn 必须，driftFn 可选（传 nullptr 表示不做漂移补偿）。
    // 传两个 nullptr 表示解绑，此后 masterClock() 返回 NaN。
    void setAudioClockSource(ClockFn clockFn, DriftFn driftFn = nullptr);

    // 是否已绑定有效时钟源
    bool hasMasterClock() const;

    // ---------- 主时钟查询 ----------
    // 当前主时钟值（秒）。
    // 实现 = audioClockFn_() + audioDriftFn_()（若提供）
    // 漂移补偿对应 ffplay 中读取 audio clock 时的 "now - last_updated" 修正项。
    // 无 source 绑定时返回 NaN，调用方应回退到 SourceFps 行为。
    double masterClock() const;

    // ---------- 同步算法 ----------
    // 计算视频帧应当等待的时间（秒）。
    // 输入：
    //   framePtsSec      即将显示的帧的 pts
    //   lastFramePtsSec  上一帧已显示的 pts（作为天然帧间隔参考）
    // 输出：
    //   >= 0 的等待时长。0 表示立即显示。
    //
    // 算法（对齐 ffplay compute_target_delay）：
    //   delay = framePtsSec - lastFramePtsSec   // 帧间隔基准
    //   diff  = framePtsSec - masterClock
    //   sync_threshold = clamp(delay, kSyncThresholdMin, kSyncThresholdMax)
    //   if (|diff| < kNoSyncThreshold) {
    //       if (diff <= -sync_threshold)        delay = max(0, delay + diff)  // 视频晚了，立即追
    //       else if (diff >= sync_threshold &&
    //                delay > kFrameDupThreshold) delay = delay + diff          // 帧间隔大，按偏差等
    //       else if (diff >= sync_threshold)    delay = delay * 2              // 帧间隔小，加倍延后
    //   }
    // 无主时钟时退化为 delay = framePtsSec - lastFramePtsSec，与 SourceFps 等价。
    //
    // 副作用：把本次 (framePtsSec - masterClock) 写入 lastVideoDiff_，便于 UI/日志查询。
    double computeVideoTargetDelay(double framePtsSec, double lastFramePtsSec) const;

    // 视频是否已经"太晚"——主时钟领先 framePtsSec 超过帧间隔。
    // 调度器若发现 isVideoLate==true 且 FrameQueue 还有更新的帧，可选择丢帧追赶。
    // 仅做判断，是否丢帧由调度器决定。
    bool isVideoLate(double framePtsSec, double lastFramePtsSec) const;

    // ---------- 调试 / 统计 ----------
    // 最近一次 computeVideoTargetDelay 计算时的 diff = framePts - masterClock
    double lastVideoDiff() const;

private:
    // 取一次时钟源快照，避免回调期间被 setAudioClockSource 替换
    struct SourceSnapshot {
        ClockFn clockFn;
        DriftFn driftFn;
    };
    SourceSnapshot snapshotSource() const;

private:
    mutable std::mutex sourceMutex_;
    ClockFn audioClockFn_;
    DriftFn audioDriftFn_;

    mutable std::atomic<double> lastVideoDiff_{0.0};
};

#endif // AVSYNC_H
