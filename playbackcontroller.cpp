#include "playbackcontroller.h"

#include "PacketQueue.h"
#include "FrameQueue.h"
#include "audiooutput.h"
#include "avsync.h"
#include "decoder.h"
#include "demuxer.h"
#include "logging.h"
#include "networkoptions.h"
#include "renderscheduler.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavformat/avformat.h>
}

#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <thread>
#include <utility>

namespace {

constexpr int kMaxReconnectAttempts = 5;
constexpr int kInitialReconnectDelayMs = 1000;
constexpr int kMaxReconnectDelayMs = 8000;

QString avErrorString(int errCode)
{
    char errBuf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errCode, errBuf, sizeof(errBuf));
    return QString::fromLocal8Bit(errBuf);
}

QString moduleErrorMessage(const QString& module, const QString& msg)
{
    return module.isEmpty() ? msg : module + QStringLiteral(": ") + msg;
}

double clampSeekPosition(double positionSec, double durationSec)
{
    if (!std::isfinite(positionSec)) {
        return 0.0;
    }
    if (durationSec >= 0.0 && std::isfinite(durationSec)) {
        return std::clamp(positionSec, 0.0, durationSec);
    }
    return std::max(0.0, positionSec);
}

double frameIntervalFromRate(AVRational rate)
{
    if (rate.num > 0 && rate.den > 0) {
        const double fps =
            static_cast<double>(rate.num) / static_cast<double>(rate.den);
        if (std::isfinite(fps) && fps > 0.0 && fps < 1000.0) {
            return 1.0 / fps;
        }
    }
    return 1.0 / 25.0;
}

double frameStepSeekTolerance(double frameIntervalSec)
{
    if (!std::isfinite(frameIntervalSec) || frameIntervalSec <= 0.0) {
        return 0.001;
    }
    return std::clamp(frameIntervalSec * 0.05, 0.0001, 0.005);
}

bool isNetworkUrl(const QUrl& url)
{
    const QString scheme = url.scheme().toLower();
    return scheme == "rtsp" ||
           scheme == "rtmp" ||
           scheme == "rtmps" ||
           scheme == "rtp" ||
           scheme == "udp" ||
           scheme == "http" ||
           scheme == "https" ||
           scheme == "srt";
}

NetworkOptions networkOptionsForUrl(const QUrl& url)
{
    NetworkOptions options;
    options.networkStream = isNetworkUrl(url);
    if (url.scheme().toLower() == "rtsp") {
        options.rtspTransport = QStringLiteral("tcp");
    }
    return options;
}

AVHWDeviceType defaultHardwareDeviceType()
{
#if defined(_WIN32)
    return AV_HWDEVICE_TYPE_D3D11VA;
#elif defined(__APPLE__)
    return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#else
    return AV_HWDEVICE_TYPE_VAAPI;
#endif
}

int reconnectDelayMs(int attempt)
{
    if (attempt <= 1) {
        return kInitialReconnectDelayMs;
    }
    int delay = kInitialReconnectDelayMs;
    for (int i = 1; i < attempt; ++i) {
        delay = std::min(delay * 2, kMaxReconnectDelayMs);
    }
    return delay;
}

} // namespace

struct PlaybackController::OpenJob {
    void setActiveDemuxer(Demuxer* demuxer)
    {
        std::lock_guard<std::mutex> lock(mutex);
        activeDemuxer = demuxer;
        if (cancelled.load() && activeDemuxer) {
            activeDemuxer->stop();
        }
    }

    void cancel()
    {
        cancelled.store(true);
        std::lock_guard<std::mutex> lock(mutex);
        if (activeDemuxer) {
            activeDemuxer->stop();
        }
    }

    bool isCancelled() const
    {
        return cancelled.load();
    }

    std::atomic_bool cancelled{false};
    std::mutex mutex;
    Demuxer* activeDemuxer = nullptr;
};

struct PlaybackController::OpenResult {
    QUrl url;
    bool autoPlay = false;
    int errCode = 0;
    QString errorMessage;
    std::unique_ptr<Demuxer> demuxer;
};

PlaybackController::PlaybackController(QObject* parent)
    : QObject(parent),
      positionTimer_(new QTimer(this)),
      reconnectTimer_(new QTimer(this))
{
    positionTimer_->setInterval(100);
    connect(positionTimer_, &QTimer::timeout,
            this, &PlaybackController::onPositionTick);

    reconnectTimer_->setSingleShot(true);
    connect(reconnectTimer_, &QTimer::timeout,
            this, &PlaybackController::performReconnect);
}

PlaybackController::~PlaybackController()
{
    close();
    joinOpenWorker();
}

void PlaybackController::setRenderer(VideoRendererBase* renderer)
{
    renderer_ = renderer;
    if (renderScheduler_) {
        renderScheduler_->setRenderer(renderer_);
    }
}

void PlaybackController::setHardwareDecodingEnabled(bool enabled)
{
    hardwareDecodingEnabled_.store(enabled);
}

bool PlaybackController::hardwareDecodingEnabled() const
{
    return hardwareDecodingEnabled_.load();
}

void PlaybackController::open(const QUrl& url)
{
    close();
    transitionTo(State::Opening);
    currentUrl_ = url;

    const unsigned long long serial = openSerial_.load();
    startOpenWorker(url, serial, false);
}

void PlaybackController::close()
{
    ++openSerial_;
    cancelOpenWorker();
    joinOpenWorker();
    if (reconnectTimer_) {
        reconnectTimer_->stop();
    }
    positionTimer_->stop();
    teardownPipeline();

    currentUrl_ = QUrl();
    videoStreamIndex_ = -1;
    audioStreamIndex_ = -1;
    durationSec_ = -1.0;
    realtime_ = false;
    hasAudio_ = false;
    hasVideo_ = false;
    videoEofReceived_ = true;
    audioEofReceived_ = true;
    preSeekState_ = State::Paused;
    lastSeekTargetSec_.store(-1.0);
    renderStepAfterSeek_.store(false);
    videoFrameIntervalSec_ = 1.0 / 25.0;
    livePausedByStop_ = false;
    reconnectAttempts_ = 0;

    transitionTo(State::Idle);
}

void PlaybackController::play()
{
    State s = state();
    if (s == State::Playing || s == State::Opening || s == State::Seeking ||
        s == State::Idle || s == State::Stopped || s == State::Error) {
        return;
    }

    if (s == State::Paused && realtime_ && livePausedByStop_) {
        livePausedByStop_ = false;
        transitionTo(State::Opening);
        if (!restartLivePipeline()) {
            return;
        }
        s = State::Ready;
    }

    if (s == State::Ready) {
        if (renderScheduler_) {
            const int ret = renderScheduler_->start();
            if (ret < 0) {
                enterError(ret, QStringLiteral("RenderScheduler start failed: ") + avErrorString(ret));
                return;
            }
        }

        if (audioOutput_) {
            const int ret = audioOutput_->start();
            if (ret < 0) {
                enterError(ret, QStringLiteral("AudioOutput start failed: ") + avErrorString(ret));
                return;
            }
        }

        if (videoDecoder_) {
            const int ret = videoDecoder_->start();
            if (ret < 0) {
                enterError(ret, QStringLiteral("VideoDecoder start failed: ") + avErrorString(ret));
                return;
            }
        }

        if (audioDecoder_) {
            const int ret = audioDecoder_->start();
            if (ret < 0) {
                enterError(ret, QStringLiteral("AudioDecoder start failed: ") + avErrorString(ret));
                return;
            }
        }

        if (demuxer_) {
            const int ret = demuxer_->start();
            if (ret < 0) {
                enterError(ret, QStringLiteral("Demuxer start failed: ") + avErrorString(ret));
                return;
            }
        }
    } else if (s == State::Paused) {
        if (audioOutput_) {
            audioOutput_->pause(false);
        }
        if (renderScheduler_) {
            renderScheduler_->pause(false);
        }
    } else {
        return;
    }

    positionTimer_->start();
    transitionTo(State::Playing);
}

void PlaybackController::pause()
{
    if (state() != State::Playing) {
        return;
    }

    if (realtime_) {
        ++openSerial_;
        livePausedByStop_ = true;
        positionTimer_->stop();
        teardownPipeline();
        transitionTo(State::Paused);
        onPositionTick();
        return;
    }

    if (audioOutput_) {
        audioOutput_->pause(true);
    }
    if (renderScheduler_) {
        renderScheduler_->pause(true);
    }

    transitionTo(State::Paused);
}

void PlaybackController::togglePause()
{
    if (state() == State::Playing) {
        pause();
    } else {
        play();
    }
}

void PlaybackController::seek(double positionSec)
{
    if (state() != State::Playing && state() != State::Paused &&
        state() != State::Ready) {
        return;
    }

    if (realtime_ || durationSec_ < 0.0) {
        emit errorOccurred(AVERROR(ENOSYS), QStringLiteral("Realtime streams do not support seeking"));
        return;
    }

    doSeek(positionSec);
}

void PlaybackController::stepForward()
{
    if (state() != State::Paused || !hasVideo_ || !renderScheduler_ ||
        realtime_ || durationSec_ < 0.0) {
        return;
    }

    const double currentPts = std::max(0.0, stepBasePositionSec());
    const bool hasPendingTarget = lastSeekTargetSec_.load() >= 0.0;
    const double epsilon = std::max(0.0001, videoFrameIntervalSec_ * 0.1);
    const double delta = hasPendingTarget ? videoFrameIntervalSec_ : epsilon;
    renderStepAfterSeek_.store(true);
    doSeek(currentPts + delta);
}

void PlaybackController::stepBackward()
{
    if (state() != State::Paused || !hasVideo_ || !renderScheduler_ ||
        realtime_ || durationSec_ < 0.0) {
        return;
    }

    const double currentPts = std::max(0.0, stepBasePositionSec());
    const bool hasPendingTarget = lastSeekTargetSec_.load() >= 0.0;
    const double epsilon = std::max(0.0001, videoFrameIntervalSec_ * 0.1);
    const double delta = hasPendingTarget
                             ? videoFrameIntervalSec_
                             : videoFrameIntervalSec_ + epsilon;
    renderStepAfterSeek_.store(true);
    doSeek(std::max(0.0, currentPts - delta));
}

void PlaybackController::setVolume(float volume)
{
    const float clamped = std::clamp(volume, 0.0f, 1.0f);
    const float old = volume_.exchange(clamped);
    applyVolumeToOutput();

    if (old != clamped) {
        emit volumeChanged(clamped);
    }
}

float PlaybackController::volume() const
{
    return volume_.load();
}

void PlaybackController::setMuted(bool muted)
{
    const bool old = muted_.exchange(muted);
    applyVolumeToOutput();

    if (old != muted) {
        emit mutedChanged(muted);
    }
}

bool PlaybackController::isMuted() const
{
    return muted_.load();
}

void PlaybackController::setPlaybackRate(float rate)
{
    const float clamped = std::clamp(rate, 0.5f, 2.0f);
    const float old = playbackRate_.exchange(clamped);
    applyPlaybackRateToOutputs();

    if (std::fabs(old - clamped) >= 0.0001f) {
        emit playbackRateChanged(clamped);
    }
}

float PlaybackController::playbackRate() const
{
    return playbackRate_.load();
}

PlaybackController::State PlaybackController::state() const
{
    return state_.load();
}

bool PlaybackController::isOpen() const
{
    const State s = state();
    return s != State::Idle && s != State::Error && s != State::Opening;
}

bool PlaybackController::isPlaying() const
{
    return state() == State::Playing;
}

bool PlaybackController::isRealtime() const
{
    return realtime_;
}

bool PlaybackController::hasVideo() const
{
    return hasVideo_;
}

bool PlaybackController::hasAudio() const
{
    return hasAudio_;
}

double PlaybackController::positionSec() const
{
    const double seekTarget = lastSeekTargetSec_.load();
    if (seekTarget >= 0.0 && state() == State::Seeking) {
        return seekTarget;
    }

    if (state() == State::Paused && hasVideo_ && renderScheduler_) {
        const double pts = renderScheduler_->lastDisplayedPts();
        const double tolerance = frameStepSeekTolerance(videoFrameIntervalSec_);
        if (seekTarget >= 0.0 &&
            (!std::isfinite(pts) || pts + tolerance < seekTarget)) {
            return seekTarget;
        }
        if (seekTarget >= 0.0) {
            lastSeekTargetSec_.store(-1.0);
        }
        return std::max(0.0, pts);
    }

    if (hasAudio_ && audioOutput_) {
        const double clock = audioOutput_->audioClock();
        // demuxer seekCompleted 回调内会把 audioClock seekTo 到 target，
        // 但 controller 触发 seek 到 demuxer 真正执行之间存在短暂窗口
        // （demuxer 线程要先看到 seekPending_）。窗口期 audioClock 还是
        // 旧值，明显小于 target 时用 target 兜底。
        if (seekTarget >= 0.0 && clock < seekTarget - 0.1) {
            return seekTarget;
        }
        if (seekTarget >= 0.0) {
            lastSeekTargetSec_.store(-1.0);
        }
        return std::max(0.0, clock);
    }
    if (hasVideo_ && renderScheduler_) {
        const double pts = renderScheduler_->lastDisplayedPts();
        if (seekTarget >= 0.0 && pts < seekTarget - 0.1) {
            return seekTarget;
        }
        if (seekTarget >= 0.0) {
            lastSeekTargetSec_.store(-1.0);
        }
        return std::max(0.0, pts);
    }
    return seekTarget >= 0.0 ? seekTarget : 0.0;
}

double PlaybackController::durationSec() const
{
    return durationSec_;
}

QUrl PlaybackController::currentUrl() const
{
    return currentUrl_;
}

void PlaybackController::requestSeek(double positionSec)
{
    seek(positionSec);
}

void PlaybackController::onPositionTick()
{
    emit positionChanged(positionSec(), durationSec_);
}

void PlaybackController::handleEofFromModule()
{
    if (state() == State::Idle || state() == State::Error ||
        state() == State::Stopped) {
        return;
    }
    if (!videoEofReceived_ || !audioEofReceived_) {
        return;
    }

    if (realtime_ && scheduleReconnect(AVERROR_EOF, QStringLiteral("Network stream ended"))) {
        return;
    }

    positionTimer_->stop();
    transitionTo(State::Stopped);
    emit endOfStream();
}

void PlaybackController::handleVideoEofFromModule()
{
    videoEofReceived_ = true;
    handleEofFromModule();
}

void PlaybackController::handleAudioEofFromModule()
{
    audioEofReceived_ = true;
    handleEofFromModule();
}

void PlaybackController::handleErrorFromModule(int errCode, const QString& msg)
{
    if (scheduleReconnect(errCode, msg)) {
        return;
    }
    enterError(errCode, msg);
}

void PlaybackController::transitionTo(State newState)
{
    const State old = state_.exchange(newState);
    if (old != newState) {
        emit stateChanged(newState);
    }
}

void PlaybackController::enterError(int errCode, const QString& msg)
{
    if (scheduleReconnect(errCode, msg)) {
        return;
    }

    if (state() == State::Error) {
        emit errorOccurred(errCode, msg);
        return;
    }

    ++openSerial_;
    positionTimer_->stop();
    teardownPipeline();
    transitionTo(State::Error);
    emit errorOccurred(errCode, msg);
}

bool PlaybackController::buildPipeline(const QUrl& url)
{
    teardownPipeline();

    demuxer_ = std::make_unique<Demuxer>();
    avSync_ = std::make_unique<AVSync>();

    if (!openDemuxer(url) || !finishPipelineAfterDemuxerOpen()) {
        teardownPipeline();
        const bool reconnectPending = reconnectTimer_ && reconnectTimer_->isActive();
        if (!reconnectPending && state() != State::Error) {
            transitionTo(State::Idle);
        }
        return false;
    }

    return true;
}

bool PlaybackController::finishPipelineAfterDemuxerOpen()
{
    if (!demuxer_) {
        enterError(AVERROR(EINVAL), QStringLiteral("Demuxer is not open"));
        return false;
    }

    if (!avSync_) {
        avSync_ = std::make_unique<AVSync>();
    }

    readDemuxerInfo();

    if (!openDecoders() ||
        !openAudioOutput() ||
        !wireScheduler()) {
        return false;
    }

    videoEofReceived_ = !hasVideo_;
    audioEofReceived_ = !hasAudio_;
    installModuleCallbacks();
    applyVolumeToOutput();
    applyPlaybackRateToOutputs();
    return true;
}

void PlaybackController::readDemuxerInfo()
{
    videoStreamIndex_ = demuxer_ ? demuxer_->bestVideoStreamIndex() : -1;
    audioStreamIndex_ = demuxer_ ? demuxer_->bestAudioStreamIndex() : -1;
    realtime_ = demuxer_ ? demuxer_->isRealtime() : false;
    videoFrameIntervalSec_ = 1.0 / 25.0;

    const int64_t durationUs = demuxer_ ? demuxer_->durationUs() : -1;
    durationSec_ = durationUs >= 0
                       ? static_cast<double>(durationUs) / static_cast<double>(AV_TIME_BASE)
                       : -1.0;
}

void PlaybackController::teardownPipeline()
{
    if (renderScheduler_) {
        renderScheduler_->stop();
    }
    if (audioOutput_) {
        audioOutput_->stop();
    }
    if (videoDecoder_) {
        videoDecoder_->stop();
    }
    if (audioDecoder_) {
        audioDecoder_->stop();
    }
    if (demuxer_) {
        demuxer_->stop();
    }

    if (renderer_) {
        renderer_->clear();
    }

    renderScheduler_.reset();
    audioOutput_.reset();
    videoDecoder_.reset();
    audioDecoder_.reset();
    demuxer_.reset();
    avSync_.reset();

    videoFrameQueue_.reset();
    audioFrameQueue_.reset();
    videoPacketQueue_.reset();
    audioPacketQueue_.reset();
}

void PlaybackController::startOpenWorker(const QUrl& url,
                                         unsigned long long serial,
                                         bool autoPlay)
{
    joinOpenWorker();

    auto job = std::make_shared<OpenJob>();
    openJob_ = job;

    PlaybackController* controller = this;
    openThread_ = std::thread(
        [controller, url, serial, autoPlay, job] {
            auto result = std::make_shared<OpenResult>();
            result->url = url;
            result->autoPlay = autoPlay;
            result->demuxer = std::make_unique<Demuxer>();

            if (job->isCancelled()) {
                return;
            }

            job->setActiveDemuxer(result->demuxer.get());
            if (job->isCancelled()) {
                job->setActiveDemuxer(nullptr);
                return;
            }

            const int ret = result->demuxer->open(url, networkOptionsForUrl(url));
            job->setActiveDemuxer(nullptr);

            if (job->isCancelled()) {
                return;
            }

            if (ret < 0) {
                result->errCode = ret;
                result->errorMessage =
                    QStringLiteral("Demuxer open failed: ") + avErrorString(ret);
                result->demuxer.reset();
            }

            QMetaObject::invokeMethod(
                controller,
                [controller, serial, result] {
                    controller->handleOpenWorkerResult(serial, result);
                },
                Qt::QueuedConnection);
        });
}

void PlaybackController::handleOpenWorkerResult(
    unsigned long long serial,
    const std::shared_ptr<OpenResult>& result)
{
    if (!result ||
        openSerial_.load() != serial ||
        state() != State::Opening) {
        return;
    }

    joinOpenWorker();
    openJob_.reset();

    currentUrl_ = result->url;

    if (result->errCode < 0) {
        enterError(result->errCode, result->errorMessage);
        return;
    }

    teardownPipeline();
    demuxer_ = std::move(result->demuxer);
    avSync_ = std::make_unique<AVSync>();

    if (!finishPipelineAfterDemuxerOpen()) {
        teardownPipeline();
        const bool reconnectPending = reconnectTimer_ && reconnectTimer_->isActive();
        if (!reconnectPending && state() != State::Error) {
            transitionTo(State::Idle);
        }
        return;
    }

    reconnectAttempts_ = 0;
    livePausedByStop_ = false;
    transitionTo(State::Ready);
    emit mediaLoaded();
    onPositionTick();

    if (result->autoPlay) {
        play();
    }
}

void PlaybackController::cancelOpenWorker()
{
    if (openJob_) {
        openJob_->cancel();
        openJob_.reset();
    }
}

void PlaybackController::joinOpenWorker()
{
    if (!openThread_.joinable()) {
        return;
    }

    if (openThread_.get_id() == std::this_thread::get_id()) {
        return;
    }

    openThread_.join();
}

bool PlaybackController::openDemuxer(const QUrl& url)
{
    const int ret = demuxer_->open(url, networkOptionsForUrl(url));
    if (ret < 0) {
        enterError(ret, QStringLiteral("Demuxer open failed: ") + avErrorString(ret));
        return false;
    }

    readDemuxerInfo();
    return true;
}

bool PlaybackController::openDecoders()
{
    if (videoStreamIndex_ >= 0 && renderer_) {
        AVStream* stream = demuxer_->stream(videoStreamIndex_);
        if (!stream) {
            enterError(AVERROR(EINVAL), QStringLiteral("Invalid video stream"));
            return false;
        }

        videoPacketQueue_ = std::make_unique<PacketQueue>();
        videoFrameQueue_ = std::make_unique<FrameQueue>();
        videoDecoder_ = std::make_unique<VideoDecoder>();

        Decoder::Options videoOptions;
        videoOptions.enableHardware = hardwareDecodingEnabled_.load();
        videoOptions.hwDeviceType = videoOptions.enableHardware
                                        ? defaultHardwareDeviceType()
                                        : AV_HWDEVICE_TYPE_NONE;

        int ret = videoDecoder_->open(stream, videoOptions);
        if (ret < 0) {
            enterError(ret, QStringLiteral("VideoDecoder open failed: ") + avErrorString(ret));
            return false;
        }

        videoDecoder_->setQueues(videoPacketQueue_.get(), videoFrameQueue_.get());
        demuxer_->setPacketQueue(videoStreamIndex_, videoPacketQueue_.get());
        hasVideo_ = true;
    } else {
        hasVideo_ = false;
    }

    if (audioStreamIndex_ >= 0) {
        AVStream* stream = demuxer_->stream(audioStreamIndex_);
        if (!stream) {
            enterError(AVERROR(EINVAL), QStringLiteral("Invalid audio stream"));
            return false;
        }

        audioPacketQueue_ = std::make_unique<PacketQueue>();
        audioFrameQueue_ = std::make_unique<FrameQueue>();
        audioDecoder_ = std::make_unique<AudioDecoder>();

        int ret = audioDecoder_->open(stream);
        if (ret < 0) {
            enterError(ret, QStringLiteral("AudioDecoder open failed: ") + avErrorString(ret));
            return false;
        }

        audioDecoder_->setQueues(audioPacketQueue_.get(), audioFrameQueue_.get());
        demuxer_->setPacketQueue(audioStreamIndex_, audioPacketQueue_.get());
        hasAudio_ = true;
    } else {
        hasAudio_ = false;
    }

    if (!hasAudio_ && !hasVideo_) {
        enterError(AVERROR_STREAM_NOT_FOUND, QStringLiteral("No playable audio or video stream"));
        return false;
    }

    return true;
}

bool PlaybackController::openAudioOutput()
{
    if (!hasAudio_) {
        return true;
    }

    AVStream* stream = demuxer_->stream(audioStreamIndex_);
    if (!stream) {
        enterError(AVERROR(EINVAL), QStringLiteral("Invalid audio stream"));
        return false;
    }

    audioOutput_ = std::make_unique<AudioOutput>();
    audioOutput_->setFrameQueue(audioFrameQueue_.get());
    // 把 packet queue 同时传给 audio output，供其在 pop frame 时通过
    // currentSerial 判断"这个 frame 是否还属于当前 serial"。
    audioOutput_->setPacketQueue(audioPacketQueue_.get());

    const int ret = audioOutput_->open(stream->codecpar, stream->time_base);
    if (ret < 0) {
        enterError(ret, QStringLiteral("AudioOutput open failed: ") + avErrorString(ret));
        return false;
    }

    avSync_->setAudioClockSource(
        [this] {
            return audioOutput_ ? audioOutput_->audioClock() : 0.0;
        },
        [this] {
            return audioOutput_ ? audioOutput_->audioClockDrift() : 0.0;
        });

    return true;
}

bool PlaybackController::wireScheduler()
{
    if (!hasVideo_) {
        return true;
    }

    AVStream* stream = demuxer_->stream(videoStreamIndex_);
    if (!stream) {
        enterError(AVERROR(EINVAL), QStringLiteral("Invalid video stream"));
        return false;
    }

    renderScheduler_ = std::make_unique<RenderScheduler>();
    renderScheduler_->setFrameQueue(videoFrameQueue_.get());
    renderScheduler_->setPacketQueue(videoPacketQueue_.get());
    renderScheduler_->setRenderer(renderer_);
    renderScheduler_->setTimeBase(stream->time_base);
    renderScheduler_->setSourceFrameRate(stream->avg_frame_rate);
    videoFrameIntervalSec_ = frameIntervalFromRate(stream->avg_frame_rate);
    renderScheduler_->setSync(avSync_.get());
    renderScheduler_->setMode(hasAudio_ ? RenderScheduler::Mode::ClockSync
                                        : RenderScheduler::Mode::SourceFps);
    return true;
}

void PlaybackController::doSeek(double positionSec)
{
    const State currentState = state();
    const State restoreState =
        currentState == State::Playing
            ? State::Playing
            : currentState == State::Ready ? State::Ready : State::Paused;
    preSeekState_ = restoreState;

    const double targetSec = clampSeekPosition(positionSec, durationSec_);
    const int64_t targetUs = static_cast<int64_t>(targetSec * AV_TIME_BASE);

    VP_LOG_INFO() << "doSeek targetSec=" << targetSec
                  << " currentState=" << static_cast<int>(currentState);

    lastSeekTargetSec_.store(targetSec);
    if (currentState != State::Ready) {
        transitionTo(State::Seeking);
    }

    videoEofReceived_ = !hasVideo_;
    audioEofReceived_ = !hasAudio_;

    // 实际清缓存、重置 clock 和 scheduler epoch 必须等 demuxer 成功 seek
    // 且 packet serial 已推进后执行，否则旧帧可能重新污染视频定时状态。
    if (demuxer_) {
        demuxer_->seek(targetUs);
    }

    onPositionTick();
}

double PlaybackController::stepBasePositionSec() const
{
    const double position = positionSec();
    const double pendingTarget = lastSeekTargetSec_.load();
    if (pendingTarget >= 0.0 && std::isfinite(pendingTarget)) {
        return pendingTarget;
    }
    return position;
}

bool PlaybackController::restartLivePipeline()
{
    if (!currentUrl_.isValid() || currentUrl_.isEmpty()) {
        enterError(AVERROR(EINVAL), QStringLiteral("Invalid live URL"));
        return false;
    }

    if (!buildPipeline(currentUrl_)) {
        return false;
    }

    reconnectAttempts_ = 0;
    transitionTo(State::Ready);
    emit mediaLoaded();
    onPositionTick();
    return true;
}

bool PlaybackController::scheduleReconnect(int errCode, const QString& msg)
{
    if (livePausedByStop_ ||
        !isNetworkUrl(currentUrl_) ||
        currentUrl_.isEmpty() ||
        state() == State::Idle ||
        state() == State::Stopped) {
        return false;
    }

    if (reconnectAttempts_ >= kMaxReconnectAttempts) {
        return false;
    }

    ++reconnectAttempts_;
    ++openSerial_;
    positionTimer_->stop();
    teardownPipeline();
    transitionTo(State::Opening);

    const int delayMs = reconnectDelayMs(reconnectAttempts_);
    emit errorOccurred(
        errCode,
        QStringLiteral("%1; reconnecting %2/%3 in %4s")
            .arg(msg)
            .arg(reconnectAttempts_)
            .arg(kMaxReconnectAttempts)
            .arg(delayMs / 1000.0, 0, 'f', 1));

    if (reconnectTimer_) {
        reconnectTimer_->start(delayMs);
    }
    return true;
}

void PlaybackController::performReconnect()
{
    if (state() != State::Opening ||
        currentUrl_.isEmpty() ||
        !isNetworkUrl(currentUrl_)) {
        return;
    }

    const unsigned long long serial = openSerial_.load();
    startOpenWorker(currentUrl_, serial, true);
}

void PlaybackController::applyVolumeToOutput()
{
    if (audioOutput_) {
        audioOutput_->setVolume(muted_.load() ? 0.0f : volume_.load());
    }
}

void PlaybackController::applyPlaybackRateToOutputs()
{
    const float rate = playbackRate_.load();
    if (audioOutput_) {
        audioOutput_->setPlaybackRate(rate);
    }
    if (renderScheduler_) {
        renderScheduler_->setPlaybackRate(rate);
    }
}

void PlaybackController::installModuleCallbacks()
{
    QPointer<PlaybackController> guard(this);
    const unsigned long long serial = openSerial_.load();

    auto postError = [guard, serial](const QString& module, int errCode, const std::string& msg) {
        if (!guard) {
            return;
        }
        if (guard->openSerial_.load() != serial) {
            return;
        }

        const QString qmsg = moduleErrorMessage(module, QString::fromStdString(msg));
        QMetaObject::invokeMethod(
            guard.data(),
            [guard, serial, errCode, qmsg] {
                if (guard && guard->openSerial_.load() == serial) {
                    guard->handleErrorFromModule(errCode, qmsg);
                }
            },
            Qt::QueuedConnection);
    };

    auto postEof = [guard, serial](bool video) {
        if (!guard) {
            return;
        }
        if (guard->openSerial_.load() != serial) {
            return;
        }

        QMetaObject::invokeMethod(
            guard.data(),
            [guard, serial, video] {
                if (guard && guard->openSerial_.load() == serial) {
                    if (video) {
                        guard->handleVideoEofFromModule();
                    } else {
                        guard->handleAudioEofFromModule();
                    }
                }
            },
            Qt::QueuedConnection);
    };

    if (demuxer_) {
        demuxer_->setErrorCallback(
            [postError](int errCode, const std::string& msg) {
                postError(QStringLiteral("Demuxer"), errCode, msg);
            });

        // 此回调仍在 demuxer 线程：它发生在 av_seek_frame 成功且 packet
        // serial 已推进之后，并且在任何新 packet 被读取之前。先同步配置
        // 消费端的精确 seek 边界，再把状态变更投递回 GUI 线程。
        demuxer_->setSeekCompletedCallback(
            [guard, serial](int64_t timestampUs) {
                if (!guard) return;
                if (guard->openSerial_.load() != serial) return;
                const double sec =
                    static_cast<double>(timestampUs) /
                    static_cast<double>(AV_TIME_BASE);

                if (guard->videoFrameQueue_) {
                    guard->videoFrameQueue_->flush();
                }
                if (guard->audioFrameQueue_) {
                    guard->audioFrameQueue_->flush();
                }
                if (guard->renderScheduler_ && guard->videoPacketQueue_) {
                    guard->renderScheduler_->seekTo(
                        sec, guard->videoPacketQueue_->currentSerial());
                }
                if (guard->audioOutput_) {
                    const int audioSerial = guard->audioPacketQueue_
                                                ? guard->audioPacketQueue_->currentSerial()
                                                : -1;
                    guard->audioOutput_->seekTo(sec, audioSerial);
                }

                QMetaObject::invokeMethod(
                    guard.data(),
                    [guard, serial] {
                        if (!guard || guard->openSerial_.load() != serial) {
                            return;
                        }
                        if (guard->state() == PlaybackController::State::Seeking) {
                            guard->transitionTo(guard->preSeekState_);
                        }
                        if (guard->renderStepAfterSeek_.exchange(false) &&
                            guard->state() == PlaybackController::State::Paused &&
                            guard->renderScheduler_) {
                            guard->renderScheduler_->requestStepForward();
                        }
                        emit guard->seekFinished();
                        guard->onPositionTick();
                    },
                    Qt::QueuedConnection);
            });
    }
    if (videoDecoder_) {
        videoDecoder_->setErrorCallback(
            [postError](int errCode, const std::string& msg) {
                postError(QStringLiteral("VideoDecoder"), errCode, msg);
            });
    }
    if (audioDecoder_) {
        audioDecoder_->setErrorCallback(
            [postError](int errCode, const std::string& msg) {
                postError(QStringLiteral("AudioDecoder"), errCode, msg);
            });
    }
    if (audioOutput_) {
        audioOutput_->setErrorCallback(
            [postError](int errCode, const std::string& msg) {
                postError(QStringLiteral("AudioOutput"), errCode, msg);
            });
    }
    if (renderScheduler_) {
        renderScheduler_->setErrorCallback(
            [postError](int errCode, const std::string& msg) {
                postError(QStringLiteral("RenderScheduler"), errCode, msg);
            });
    }

    if (renderScheduler_) {
        renderScheduler_->setEofCallback(
            [postEof] {
                postEof(true);
            });
    }
    if (audioOutput_) {
        audioOutput_->setEofCallback(
            [postEof] {
                postEof(false);
            });
    }
}
