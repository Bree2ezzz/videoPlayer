#include "renderscheduler.h"

#include "avsync.h"
#include "logging.h"
#include "videorendererbase.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>
#include <utility>

namespace {

double quietNaN()
{
    return std::numeric_limits<double>::quiet_NaN();
}

double steadySeconds()
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

double sourceFrameInterval(int frameRateNum, int frameRateDen)
{
    constexpr double kFallbackFps = 25.0;

    if (frameRateNum > 0 && frameRateDen > 0) {
        const double fps =
            static_cast<double>(frameRateNum) / static_cast<double>(frameRateDen);
        if (std::isfinite(fps) && fps > 0.0 && fps < 1000.0) {
            return 1.0 / fps;
        }
    }

    return 1.0 / kFallbackFps;
}

bool interruptibleSleep(double seconds,
                        const std::atomic_bool& abortFlag,
                        const std::atomic_bool* wakeFlag = nullptr)
{
    if (!std::isfinite(seconds) || seconds <= 0.0) {
        return !abortFlag.load() && !(wakeFlag && wakeFlag->load());
    }

    constexpr double kSleepSliceSec = 0.01;
    double remaining = seconds;
    while (remaining > 0.0) {
        if (abortFlag.load() || (wakeFlag && wakeFlag->load())) {
            return false;
        }

        const double slice = std::min(remaining, kSleepSliceSec);
        std::this_thread::sleep_for(std::chrono::duration<double>(slice));
        remaining -= slice;
    }

    return !abortFlag.load() && !(wakeFlag && wakeFlag->load());
}

} // namespace

RenderScheduler::RenderScheduler() = default;

RenderScheduler::~RenderScheduler()
{
    stop();
}

void RenderScheduler::setFrameQueue(FrameQueue* queue)
{
    std::lock_guard<std::mutex> lock(configMutex_);
    frameQueue_ = queue;
}

void RenderScheduler::setPacketQueue(PacketQueue* queue)
{
    std::lock_guard<std::mutex> lock(configMutex_);
    packetQueue_ = queue;
}

void RenderScheduler::setRenderer(VideoRendererBase* renderer)
{
    std::lock_guard<std::mutex> lock(configMutex_);
    renderer_ = renderer;
}

void RenderScheduler::setTimeBase(AVRational timeBase,
                                  int64_t sourceStartTime,
                                  bool normalizeTimestamps)
{
    timeBaseNum_.store(timeBase.num);
    timeBaseDen_.store(timeBase.den);
    sourceStartTime_.store(sourceStartTime);
    normalizeTimestamps_.store(normalizeTimestamps);
    timestampOriginPts_ = normalizeTimestamps ? sourceStartTime : AV_NOPTS_VALUE;
}

void RenderScheduler::setSourceFrameRate(AVRational frameRate)
{
    frameRateNum_.store(frameRate.num);
    frameRateDen_.store(frameRate.den);
}

void RenderScheduler::setSync(AVSync* sync)
{
    std::lock_guard<std::mutex> lock(configMutex_);
    sync_ = sync;
}

void RenderScheduler::setMode(Mode mode)
{
    mode_.store(mode);
    flush();
}

RenderScheduler::Mode RenderScheduler::mode() const
{
    return mode_.load();
}

int RenderScheduler::start()
{
    if (started_.load()) {
        return 0;
    }

    if (thread_.joinable()) {
        return AVERROR(EALREADY);
    }

    if (!frameQueueSnapshot() || !rendererSnapshot()) {
        return AVERROR(EINVAL);
    }

    if (timeBaseNum_.load() <= 0 || timeBaseDen_.load() <= 0) {
        return AVERROR(EINVAL);
    }

    abort_ = false;
    paused_ = false;
    flushRequested_ = false;
    eofReported_ = false;
    lastDisplayedPts_.store(0.0);
    lastPtsSec_ = quietNaN();
    frameTimer_ = steadySeconds();
    timingSerial_ = -1;
    timestampOriginPts_ = normalizeTimestamps_.load()
                              ? sourceStartTime_.load()
                              : AV_NOPTS_VALUE;
    stepForwardRequested_.store(false);
    seekTargetPtsSec_.store(-1.0);
    seekSerial_.store(-1);

    started_ = true;
    thread_ = std::thread(&RenderScheduler::scheduleLoop, this);
    return 0;
}

void RenderScheduler::stop()
{
    abort_ = true;

    FrameQueue* queue = frameQueueSnapshot();
    if (queue) {
        queue->closeConsumer();
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    started_ = false;
}

void RenderScheduler::pause(bool paused)
{
    paused_ = paused;
}

bool RenderScheduler::isPaused() const
{
    return paused_.load();
}

void RenderScheduler::requestStepForward()
{
    if (!started_.load() || abort_.load()) {
        return;
    }

    stepForwardRequested_.store(true);
}

void RenderScheduler::flush()
{
    flushRequested_ = true;

    if (!started_.load()) {
        lastPtsSec_ = quietNaN();
        frameTimer_ = steadySeconds();
        lastDisplayedPts_.store(0.0);
        timingSerial_ = -1;
    }
}

void RenderScheduler::seekTo(double targetPtsSec, int serial)
{
    if (!std::isfinite(targetPtsSec) || targetPtsSec < 0.0) {
        return;
    }

    seekTargetPtsSec_.store(targetPtsSec);
    seekSerial_.store(serial);
    flushRequested_.store(true);
    VP_LOG_DEBUG() << "video seek boundary target=" << targetPtsSec
                   << " serial=" << serial;
}

bool RenderScheduler::isRunning() const
{
    return started_.load() && !abort_.load();
}

double RenderScheduler::lastDisplayedPts() const
{
    return lastDisplayedPts_.load();
}

double RenderScheduler::sourceFrameIntervalSec() const
{
    return sourceFrameInterval(frameRateNum_.load(), frameRateDen_.load());
}

void RenderScheduler::setPlaybackRate(float rate)
{
    playbackRate_.store(std::clamp(rate, 0.5f, 2.0f));
}

float RenderScheduler::playbackRate() const
{
    return playbackRate_.load();
}

void RenderScheduler::setEofCallback(EofCallback cb)
{
    std::lock_guard<std::mutex> lock(callbackMutex_);
    eofCb_ = std::move(cb);
}

void RenderScheduler::setErrorCallback(ErrorCallback cb)
{
    std::lock_guard<std::mutex> lock(callbackMutex_);
    errorCb_ = std::move(cb);
}

void RenderScheduler::scheduleLoop()
{
    frameTimer_ = steadySeconds();
    lastPtsSec_ = quietNaN();

    while (!abort_.load()) {
        if (flushRequested_.exchange(false)) {
            lastPtsSec_ = quietNaN();
            frameTimer_ = steadySeconds();
            lastDisplayedPts_.store(0.0);
            timingSerial_ = -1;
            timestampOriginPts_ = normalizeTimestamps_.load()
                                      ? sourceStartTime_.load()
                                      : AV_NOPTS_VALUE;
        }

        const bool stepping = paused_.load() && stepForwardRequested_.load();
        if (paused_.load() && !stepping) {
            interruptibleSleep(0.01, abort_);
            frameTimer_ = steadySeconds();
            continue;
        }

        FrameQueue* queue = frameQueueSnapshot();
        VideoRendererBase* renderer = rendererSnapshot();
        if (!queue || !renderer) {
            reportError(AVERROR(EINVAL), "RenderScheduler missing queue or renderer");
            break;
        }
        PacketQueue* pktQueue = packetQueueSnapshot();

        AVFrame* frame = nullptr;
        int frameSerial = -1;
        const int ret = queue->pop(&frame, &frameSerial, 100);
        if (ret == FrameQueue::Timeout) {
            continue;
        }
        if (ret == FrameQueue::EndOfStream) {
            stepForwardRequested_.store(false);
            EofCallback cb;
            if (!eofReported_.exchange(true)) {
                std::lock_guard<std::mutex> lock(callbackMutex_);
                cb = eofCb_;
            }
            if (cb) {
                cb();
            }
            break;
        }
        if (ret == FrameQueue::Aborted) {
            break;
        }
        if (ret != FrameQueue::Ok || !frame) {
            reportError(AVERROR(EINVAL), "FrameQueue pop failed");
            break;
        }

        // serial 过滤：seek 后 PacketQueue::serial_ 已 ++，但解码线程可能
        // 还在处理旧 serial 的 packet（解码过程中产出的 frame 被打上旧
        // serial）。这些旧 frame 直接丢弃，不渲染、不等待，也不更新
        // lastPtsSec_/frameTimer_，避免污染下一帧的同步计算。
        if (pktQueue && frameSerial != pktQueue->currentSerial()) {
            VP_LOG_DEBUG() << "drop stale video frame serial=" << frameSerial
                           << " current=" << pktQueue->currentSerial();
            av_frame_free(&frame);
            continue;
        }

        double pts = framePtsToSeconds(frame);
        const bool hasFramePts = std::isfinite(pts);
        if (!hasFramePts) {
            const double interval = sourceFrameInterval(
                frameRateNum_.load(), frameRateDen_.load());
            pts = std::isfinite(lastPtsSec_)
                      ? lastPtsSec_ + interval
                      : 0.0;
        }

        // packet epoch 是视频时序的边界。任何 serial 变化都必须在 scheduler
        // 自己的线程内重置定时器，不能依赖 controller 提前发出的 flush。
        if (frameSerial != timingSerial_) {
            timingSerial_ = frameSerial;
            lastPtsSec_ = quietNaN();
            frameTimer_ = steadySeconds();
            VP_LOG_DEBUG() << "first frame for serial=" << frameSerial
                           << " pts=" << pts;
        }

        const double seekTarget = seekTargetPtsSec_.load();
        if (frameSerial == seekSerial_.load() && seekTarget >= 0.0) {
            if (hasFramePts && pts < seekTarget) {
                VP_LOG_DEBUG() << "drop pre-target video frame serial=" << frameSerial
                               << " pts=" << pts
                               << " target=" << seekTarget;
                av_frame_free(&frame);
                continue;
            }
            seekTargetPtsSec_.store(-1.0);
            seekSerial_.store(-1);
        }

        if (!stepping && mode_.load() == Mode::ClockSync && std::isfinite(lastPtsSec_)) {
            AVSync* sync = syncSnapshot();
            if (sync && sync->isVideoLate(pts, lastPtsSec_) && queue->size() > 0) {
                VP_LOG_DEBUG() << "drop late video frame serial=" << frameSerial
                               << " pts=" << pts
                               << " master=" << sync->masterClock();
                av_frame_free(&frame);
                continue;
            }
        }

        const bool firstScheduledFrame = !std::isfinite(lastPtsSec_);
        const double delay = stepping ? 0.0 : computeDelaySec(pts, lastPtsSec_);
        if (stepping) {
            frameTimer_ = steadySeconds();
        } else {
            frameTimer_ += delay;
        }

        // 节流日志：每 30 帧（约 1s）打一次同步参数，方便观察 seek 后节奏
        static thread_local int s_logCounter = 0;
        if ((++s_logCounter % 30) == 0) {
            AVSync* sync = syncSnapshot();
            const double master = sync ? sync->masterClock() : 0.0;
            const double diff = sync ? sync->lastVideoDiff() : 0.0;
            VP_LOG_DEBUG() << "render pts=" << pts
                           << " lastPts=" << lastPtsSec_
                           << " delay=" << delay
                           << " master=" << master
                           << " diff=" << diff;
        }

        const double now = steadySeconds();
        double waitSec = stepping ? 0.0 : frameTimer_ - now;
        constexpr double kMaxFrameDelay = 0.1;
        if (waitSec < -kMaxFrameDelay) {
            frameTimer_ = now;
            waitSec = 0.0;
        }
        if (firstScheduledFrame) {
            AVSync* sync = syncSnapshot();
            VP_LOG_DEBUG() << "first scheduled video frame serial=" << frameSerial
                           << " pts=" << pts
                           << " master=" << (sync ? sync->masterClock() : 0.0)
                           << " delay=" << delay
                           << " wait=" << waitSec;
        }

        if (waitSec > 0.0 && !interruptibleSleep(waitSec, abort_, &flushRequested_)) {
            av_frame_free(&frame);
            if (abort_.load()) {
                break;
            }
            continue;
        }

        if (abort_.load()) {
            av_frame_free(&frame);
            break;
        }

        // seek 可能发生在等待显示期间；此时这张帧已属于旧 epoch。
        if ((pktQueue && frameSerial != pktQueue->currentSerial()) ||
            flushRequested_.load()) {
            VP_LOG_DEBUG() << "drop video frame after scheduling boundary serial="
                           << frameSerial;
            av_frame_free(&frame);
            continue;
        }

        renderer->renderFrame(frame);
        lastPtsSec_ = pts;
        lastDisplayedPts_.store(pts);
        if (stepping) {
            stepForwardRequested_.store(false);
        }
        av_frame_free(&frame);
    }

    started_ = false;
}

double RenderScheduler::computeDelaySec(double framePtsSec, double lastFramePtsSec)
{
    if (!std::isfinite(lastFramePtsSec)) {
        return 0.0;
    }

    const double rate = std::clamp(playbackRate_.load(), 0.5f, 2.0f);
    const double fallbackDelay = sourceFrameInterval(
        frameRateNum_.load(), frameRateDen_.load()) / rate;

    if (mode_.load() == Mode::ClockSync) {
        AVSync* sync = syncSnapshot();
        if (sync) {
            const double delay =
                sync->computeVideoTargetDelay(framePtsSec, lastFramePtsSec);
            return std::isfinite(delay) && delay >= 0.0 ? delay / rate : 0.0;
        }
    }

    return fallbackDelay;
}

double RenderScheduler::framePtsToSeconds(const AVFrame* frame)
{
    if (!frame) {
        return quietNaN();
    }

    int64_t pts = frame->pts;
    if (pts == AV_NOPTS_VALUE) {
        pts = frame->best_effort_timestamp;
    }
    if (pts == AV_NOPTS_VALUE) {
        return quietNaN();
    }

    const int num = timeBaseNum_.load();
    const int den = timeBaseDen_.load();
    if (num <= 0 || den <= 0) {
        return quietNaN();
    }

    int64_t normalizedPts = pts;
    if (normalizeTimestamps_.load()) {
        if (timestampOriginPts_ == AV_NOPTS_VALUE) {
            timestampOriginPts_ = pts;
        }
        normalizedPts = pts - timestampOriginPts_;
    }

    return static_cast<double>(normalizedPts) * static_cast<double>(num) /
           static_cast<double>(den);
}

FrameQueue* RenderScheduler::frameQueueSnapshot() const
{
    std::lock_guard<std::mutex> lock(configMutex_);
    return frameQueue_;
}

PacketQueue* RenderScheduler::packetQueueSnapshot() const
{
    std::lock_guard<std::mutex> lock(configMutex_);
    return packetQueue_;
}

VideoRendererBase* RenderScheduler::rendererSnapshot() const
{
    std::lock_guard<std::mutex> lock(configMutex_);
    return renderer_;
}

AVSync* RenderScheduler::syncSnapshot() const
{
    std::lock_guard<std::mutex> lock(configMutex_);
    return sync_;
}

void RenderScheduler::reportError(int errCode, const std::string& msg)
{
    ErrorCallback cb;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        cb = errorCb_;
    }

    if (cb) {
        cb(errCode, msg);
    }
}
