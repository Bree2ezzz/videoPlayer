#ifndef STREAMPIPELINE_H
#define STREAMPIPELINE_H

#include "FrameQueue.h"
#include "streamoutputoptions.h"

#include <QObject>
#include <QUrl>

extern "C" {
#include <libavutil/rational.h>
}

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <mutex>
#include <thread>
#include <chrono>

class Encoder;
class VideoEncoder;
class AudioEncoder;
class Muxer;

class ISource
{
public:
    struct VideoInfo {
        int width = 0;
        int height = 0;
        AVRational timeBase = {0, 1};
        AVRational frameRate = {0, 1};
    };
    struct AudioInfo {
        AVRational timeBase = {0, 1};
    };

    using EofCallback = std::function<void()>;
    using ErrorCallback = std::function<void(int errCode, const std::string& message)>;

    virtual ~ISource() = default;
    virtual int open(const QUrl& input) = 0;
    virtual int start() = 0;
    virtual void stop() = 0;
    virtual void close() = 0;
    virtual FrameQueue* videoFrames() const = 0;
    virtual FrameQueue* audioFrames() const = 0;
    virtual VideoInfo videoInfo() const = 0;
    virtual AudioInfo audioInfo() const = 0;
    virtual int64_t commonStartTimeUs() const = 0;
    virtual void setEofCallback(EofCallback cb) = 0;
    virtual void setErrorCallback(ErrorCallback cb) = 0;
};

class StreamPipeline final : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Idle,
        Connecting,
        Streaming,
        Error,
    };
    Q_ENUM(State)

    using SourceFactory = std::function<std::unique_ptr<ISource>()>;

    explicit StreamPipeline(QObject* parent = nullptr);
    ~StreamPipeline() override;

    StreamPipeline(const StreamPipeline&) = delete;
    StreamPipeline& operator=(const StreamPipeline&) = delete;

    void setSourceFactory(SourceFactory factory);
    void start(const QUrl& inputFile, const StreamOutputOptions& options);
    Q_INVOKABLE void stop();
    State state() const;
    bool isStreaming() const;

signals:
    void stateChanged(StreamPipeline::State state);
    void errorOccurred(int errCode, const QString& message);
    void finished();

private:
    struct StartJob;
    struct StartResult;

    void transitionTo(State state);
    void startSetupWorker(const QUrl& inputFile,
                          const StreamOutputOptions& options,
                          unsigned long long generation,
                          const std::shared_ptr<StartJob>& job);
    void finishSetup(unsigned long long generation, const std::shared_ptr<StartResult>& result);
    void joinSetupWorker();
    void teardownActivePipeline();
    void postError(unsigned long long generation, int errCode, const std::string& message);
    void handleError(unsigned long long generation, int errCode, const QString& message);
    void onEncoderEof(unsigned long long generation, bool video);
    void handleFinished(unsigned long long generation);
    int writeTimedPacket(unsigned long long generation,
                         int muxStreamIndex,
                         const AVPacket* packet,
                         AVRational encoderTimeBase);
    int waitUntilPacketDeadline(unsigned long long generation, int64_t packetTimeUs);
    int64_t mapFramePts(const AVFrame* frame,
                        AVRational inputTimeBase,
                        AVRational encoderTimeBase,
                        bool video);

private:
    std::unique_ptr<ISource> source_;
    std::unique_ptr<VideoEncoder> videoEncoder_;
    std::unique_ptr<AudioEncoder> audioEncoder_;
    std::unique_ptr<Muxer> muxer_;
    int videoMuxStream_ = -1;
    int audioMuxStream_ = -1;

    std::thread setupThread_;
    std::shared_ptr<StartJob> startJob_;
    SourceFactory sourceFactory_;
    std::atomic<State> state_{State::Idle};
    std::atomic_bool abort_{false};
    std::atomic<unsigned long long> generation_{0};
    std::mutex timelineMutex_;
    int64_t timelineStartUs_ = AV_NOPTS_VALUE;
    int64_t syntheticVideoPts_ = -1;
    std::mutex pacingMutex_;
    bool pacingAnchorInitialized_ = false;
    std::chrono::steady_clock::time_point pacingAnchorWallTime_{};
    int64_t pacingAnchorMediaUs_ = 0;
    std::mutex eofMutex_;
    bool videoEncoderEof_ = false;
    bool audioEncoderEof_ = false;
    bool finalizingEof_ = false;
};

#endif // STREAMPIPELINE_H
