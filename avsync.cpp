#include "avsync.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

double quietNaN()
{
    return std::numeric_limits<double>::quiet_NaN();
}

bool isUsableClock(double value)
{
    return std::isfinite(value);
}

double sanitizeDelay(double delay)
{
    if (!std::isfinite(delay) || delay < 0.0) {
        return 0.0;
    }
    return delay;
}

} // namespace

AVSync::AVSync() = default;

AVSync::~AVSync() = default;

void AVSync::setAudioClockSource(ClockFn clockFn, DriftFn driftFn)
{
    std::lock_guard<std::mutex> lock(sourceMutex_);
    audioClockFn_ = std::move(clockFn);
    audioDriftFn_ = std::move(driftFn);
}

bool AVSync::hasMasterClock() const
{
    return static_cast<bool>(snapshotSource().clockFn);
}

double AVSync::masterClock() const
{
    const SourceSnapshot source = snapshotSource();
    if (!source.clockFn) {
        return quietNaN();
    }

    const double clock = source.clockFn();
    if (!isUsableClock(clock)) {
        return quietNaN();
    }

    if (!source.driftFn) {
        return clock;
    }

    const double drift = source.driftFn();
    return std::isfinite(drift) ? clock + drift : clock;
}

double AVSync::computeVideoTargetDelay(double framePtsSec, double lastFramePtsSec) const
{
    double delay = sanitizeDelay(framePtsSec - lastFramePtsSec);

    if (!std::isfinite(framePtsSec) || !std::isfinite(lastFramePtsSec)) {
        lastVideoDiff_.store(0.0);
        return delay;
    }

    const double master = masterClock();
    if (!isUsableClock(master)) {
        lastVideoDiff_.store(0.0);
        return delay;
    }

    const double diff = framePtsSec - master;
    lastVideoDiff_.store(diff);

    if (!std::isfinite(diff) || std::fabs(diff) >= kNoSyncThreshold) {
        return delay;
    }

    const double syncThreshold =
        std::clamp(delay, kSyncThresholdMin, kSyncThresholdMax);

    if (diff <= -syncThreshold) {
        delay = std::max(0.0, delay + diff);
    } else if (diff >= syncThreshold && delay > kFrameDupThreshold) {
        delay = delay + diff;
    } else if (diff >= syncThreshold) {
        delay = delay * 2.0;
    }

    return sanitizeDelay(delay);
}

bool AVSync::isVideoLate(double framePtsSec, double lastFramePtsSec) const
{
    if (!std::isfinite(framePtsSec) || !std::isfinite(lastFramePtsSec)) {
        return false;
    }

    const double master = masterClock();
    if (!isUsableClock(master)) {
        return false;
    }

    const double delay = sanitizeDelay(framePtsSec - lastFramePtsSec);
    if (delay <= 0.0) {
        return framePtsSec < master;
    }

    return master - framePtsSec > delay;
}

double AVSync::lastVideoDiff() const
{
    return lastVideoDiff_.load();
}

AVSync::SourceSnapshot AVSync::snapshotSource() const
{
    std::lock_guard<std::mutex> lock(sourceMutex_);
    return SourceSnapshot{audioClockFn_, audioDriftFn_};
}
