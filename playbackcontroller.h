#ifndef PLAYBACKCONTROLLER_H
#define PLAYBACKCONTROLLER_H

#include <QObject>
#include <QString>
#include <QUrl>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include "videorendererbase.h"
class Demuxer;
class Decoder;
class VideoDecoder;
class AudioDecoder;
class AudioOutput;
class RenderScheduler;
class AVSync;
class VideoRendererBase;
class PacketQueue;
class FrameQueue;
class D3D11Context;

class QTimer;

enum class PlaybackProfile {
    Software,
    D3D11,
};

/*
 * PlaybackController 职责边界：
 *   把数据流模块（Demuxer / Decoder / AudioOutput / RenderScheduler / AVSync）
 *   和 Qt UI 粘起来，集中管理：
 *     - 模块对象的生命周期与依赖装配
 *     - 播放状态机
 *     - 暂停 / 跳转 / 音量等运行期控制
 *     - 把模块层的 std::function 错误/EOF 回调转成 Qt signal 给 UI
 *     - 周期性广播播放进度
 *
 * 设计说明：
 *   - 不渲染、不解码、不解封装；这些工作交给被持有的子模块。
 *   - 不拥有 VideoRendererBase（由 UI/MainWindow 注入），便于切换软件 / D3D11 渲染。
 *   - open / seek 都是异步：UI 立即返回，结果通过 signal 通知。状态机串行
 *     执行，避免半开状态。
 *   - 所有模块层回调都在内部 trampoline 中通过 QMetaObject::invokeMethod
 *     marshal 到 PlaybackController 所在线程（通常是 GUI 线程），UI 不需要
 *     考虑跨线程问题。
 *   - Demuxer / Decoder / AudioOutput / RenderScheduler / AVSync 用 unique_ptr
 *     持有，open() 时构造、close() 时按反向顺序拆除。
 *   - 进度更新由内部 QTimer 触发，频率默认 100ms。
 *
 * 状态机：
 *   Idle ──open──> Opening ──成功──> Ready ──play──> Playing
 *                              失败──> Error            │
 *                                                       │ pause / play
 *                                                       ▼
 *                                                     Paused
 *                                                       │
 *                              ┌──── seek ─────────────┘
 *                              ▼
 *                            Seeking ── 完成 ──> Playing/Paused（恢复 seek 前态）
 *
 *   close() / stop() 从任何状态回到 Idle。
 *   遇错从任何状态进入 Error，需要再次 open() 才能恢复。
 *
 * 线程模型：
 *   - 所有 public 接口必须在创建该对象的线程（通常是 GUI 线程）调用。
 *   - 模块内部线程（demuxer 读线程、decoder 线程、scheduler 线程、SDL 音频线程）
 *     由各子模块自管。
 *   - 内部 trampoline 保证 UI 收到的 signal 都是 GUI 线程。
 */

class PlaybackController : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Idle,       // 未打开
        Opening,    // open 进行中（解封装、探测流、构造解码器）
        Ready,      // 已就绪但尚未开始播放
        Playing,
        Paused,
        Seeking,    // seek 进行中
        Stopped,    // close 后回到 Idle 之前的瞬态
        Error,
    };
    Q_ENUM(State)

    explicit PlaybackController(QObject* parent = nullptr);
    ~PlaybackController() override;

    PlaybackController(const PlaybackController&) = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;

    // ---------- 渲染器注入 ----------
    // 在 open() 之前调用。允许 nullptr（无视频输出，只放音频）。
    // 切换渲染器需要先 close()。
    void setRenderer(VideoRendererBase* renderer);
    // Profile is fixed for one open cycle. MainWindow must close before changing it.
    void setPlaybackProfile(PlaybackProfile profile, D3D11Context* sharedD3D11 = nullptr);
    PlaybackProfile playbackProfile() const;
    unsigned long long openSessionId() const;

    // ---------- 生命周期 ----------
    // 异步打开。UI 立即返回，最终结果通过 stateChanged() / errorOccurred() 通知。
    // 已有播放时内部先 close()。
    Q_INVOKABLE void open(const QUrl& url);

    // 同步关闭：停止全部线程、释放模块对象。返回后 state==Idle。
    Q_INVOKABLE void close();

    // ---------- 播放控制 ----------
    // play() 仅在 Ready / Paused 下生效。Playing 下重复调用是 no-op。
    Q_INVOKABLE void play();
    // pause() 仅在 Playing 下生效。其他状态忽略。
    Q_INVOKABLE void pause();
    // 在 Playing/Paused 之间切换，便于 UI 单按钮绑定。
    Q_INVOKABLE void togglePause();

    // 异步 seek：positionSec 为绝对秒数，会被 clamp 到 [0, durationSec]。
    // 实时流（isRealtime() 为 true）忽略本次调用，并通过 errorOccurred 提示。
    Q_INVOKABLE void seek(double positionSec);

    // 暂停态逐帧步进。内部复用精确 seek 链路，并在 seek 后渲染一帧预览。
    Q_INVOKABLE void stepForward();
    Q_INVOKABLE void stepBackward();

    // ---------- 音量 ----------
    // [0.0, 1.0]，越界会被 clamp。即时生效。
    Q_INVOKABLE void setVolume(float volume);
    Q_INVOKABLE float volume() const;

    Q_INVOKABLE void setMuted(bool muted);
    Q_INVOKABLE bool isMuted() const;

    Q_INVOKABLE void setPlaybackRate(float rate);
    Q_INVOKABLE float playbackRate() const;

    // ---------- 状态查询 ----------
    Q_INVOKABLE State state() const;
    Q_INVOKABLE bool isOpen() const;        // 非 Idle / Error / Opening
    Q_INVOKABLE bool isPlaying() const;     // 仅 Playing
    Q_INVOKABLE bool isRealtime() const;    // 网络流，UI 用来禁用进度条/seek
    Q_INVOKABLE bool hasVideo() const;
    Q_INVOKABLE bool hasAudio() const;

    // 当前播放位置（秒）。优先取 audioClock；无音轨时取视频 lastDisplayedPts。
    Q_INVOKABLE double positionSec() const;
    // 总时长（秒）。未知（实时流）返回 -1.0。
    Q_INVOKABLE double durationSec() const;

    // 当前媒体的 URL（open 成功后有效）
    Q_INVOKABLE QUrl currentUrl() const;

signals:
    // 状态机变更。UI 用来切换按钮文案、禁用控件等。
    void stateChanged(State newState);

    // 媒体已加载，可读取 duration / 流信息。state 在此之前已经进入 Ready。
    void mediaLoaded();

    // 周期性进度广播。频率由 setPositionUpdateInterval() 决定，默认 100ms。
    // duration 为 -1 表示未知。
    void positionChanged(double positionSec, double durationSec);

    // 播放自然结束（流读完 + 队列排空 + 渲染完最后一帧）
    void endOfStream();

    // 任意子模块上报的错误。errCode 是 AVERROR / errno；msg 已是用户可读形式
    void errorOccurred(int errCode, const QString& msg);

    // 音量 / 静音变化（其他控件可同步显示）
    void volumeChanged(float volume);
    void mutedChanged(bool muted);
    void playbackRateChanged(float rate);

    // seek 完成（state 已经回到 seek 前的 Playing/Paused）
    void seekFinished();

    // Emitted for every new open session, including automatic network reconnects.
    void playbackSessionChanged(unsigned long long sessionId, const QUrl& url);

    // D3D11 device setup or codec format negotiation failed; UI rebuilds the Software profile.
    // Session and URL make queued fallback notifications harmless after close/reopen/profile changes.
    void d3d11FallbackRequested(unsigned long long sessionId,
                                const QUrl& url,
                                const QString& reason);

public slots:
    // UI 拖动进度条结束时调用：等价于 seek(positionSec)
    void requestSeek(double positionSec);

private slots:
    // 进度定时器触发，发出 positionChanged
    void onPositionTick();

    // 子模块回调 marshal 到本线程的入口
    void handleVideoEofFromModule();
    void handleAudioEofFromModule();
    void handleEofFromModule();
    void handleErrorFromModule(int errCode, const QString& msg);

private:
    struct OpenJob;
    struct OpenResult;

    // ---------- 状态机辅助 ----------
    void transitionTo(State newState);
    void enterError(int errCode, const QString& msg);

    // 装配 / 拆除子模块。失败回退到 Idle 并 emit error。
    bool buildPipeline(const QUrl& url);
    bool finishPipelineAfterDemuxerOpen();
    void readDemuxerInfo();
    void teardownPipeline();
    void startOpenWorker(const QUrl& url, unsigned long long serial, bool autoPlay);
    void handleOpenWorkerResult(unsigned long long serial,
                                const std::shared_ptr<OpenResult>& result);
    void cancelOpenWorker();
    void joinOpenWorker();

    // 装配阶段细分，便于失败定位
    bool openDemuxer(const QUrl& url);
    bool openDecoders();           // 视频 + 音频
    void requestD3D11Fallback(const QString& reason);
    bool openAudioOutput();        // 音频流不存在时跳过
    bool wireScheduler();          // 绑定 timeBase / sync / renderer

    // seek 流程细分
    void doSeek(double positionSec);
    double stepBasePositionSec() const;

    bool restartLivePipeline();
    bool scheduleReconnect(int errCode, const QString& msg);
    void performReconnect();

    // 音量应用：把 volume_ * (muted_ ? 0 : 1) 推给 AudioOutput
    void applyVolumeToOutput();
    void applyPlaybackRateToOutputs();

    // 把模块的 std::function 回调统一中转为 invokeMethod
    void installModuleCallbacks();

private:
    // ---------- 子模块（unique_ptr 拥有） ----------
    std::unique_ptr<Demuxer> demuxer_;
    std::unique_ptr<VideoDecoder> videoDecoder_;
    std::unique_ptr<AudioDecoder> audioDecoder_;
    std::unique_ptr<AudioOutput> audioOutput_;
    std::unique_ptr<RenderScheduler> renderScheduler_;
    std::unique_ptr<AVSync> avSync_;

    // ---------- 异步 open ----------
    std::thread openThread_;
    std::shared_ptr<OpenJob> openJob_;

    // 队列：解封装→解码、解码→输出
    std::unique_ptr<PacketQueue> videoPacketQueue_;
    std::unique_ptr<PacketQueue> audioPacketQueue_;
    std::unique_ptr<FrameQueue> videoFrameQueue_;
    std::unique_ptr<FrameQueue> audioFrameQueue_;

    // ---------- 渲染器（不拥有） ----------
    VideoRendererBase* renderer_ = nullptr;
    PlaybackProfile playbackProfile_ = PlaybackProfile::Software;
    D3D11Context* sharedD3D11_ = nullptr; // owned by MainWindow; outlives this pipeline

    // ---------- 媒体描述（open 成功后填充） ----------
    QUrl currentUrl_;
    int videoStreamIndex_ = -1;
    int audioStreamIndex_ = -1;
    double durationSec_ = -1.0;
    bool realtime_ = false;
    bool hasAudio_ = false;
    bool hasVideo_ = false;
    bool videoEofReceived_ = true;
    bool audioEofReceived_ = true;
    double videoFrameIntervalSec_ = 1.0 / 25.0;

    // ---------- 状态 ----------
    std::atomic<State> state_{State::Idle};
    std::atomic<unsigned long long> openSerial_{0};
    // seek 前的目标恢复态：seek 完成后回到 Playing 或 Paused
    State preSeekState_ = State::Paused;
    // seek 目标位置（秒）。在 demuxer 真正处理请求并让新时间轴输出抵达前，
    // 用此值作为 positionSec() 的返回值，避免 UI 临时跳回旧位置。
    // -1.0 表示无效（非 seek 后状态）。
    mutable std::atomic<double> lastSeekTargetSec_{-1.0};

    // ---------- 音量 ----------
    std::atomic<float> volume_{1.0f};
    std::atomic_bool muted_{false};
    std::atomic<float> playbackRate_{1.0f};
    std::atomic_bool renderStepAfterSeek_{false};
    bool livePausedByStop_ = false;
    int reconnectAttempts_ = 0;

    // ---------- 进度推送 ----------
    QTimer* positionTimer_ = nullptr;
    QTimer* reconnectTimer_ = nullptr;

    // ---------- 模块层回调中转所需 ----------
    // 防止子模块回调在对象析构后被触发：模块层只持有弱引用般的 lambda，
    // lambda 通过 QPointer 投递 invokeMethod 到本对象。
    // （具体在 .cpp 中用 QPointer<PlaybackController> 配合实现。）
};

#endif // PLAYBACKCONTROLLER_H
