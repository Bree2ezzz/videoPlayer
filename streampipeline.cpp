#include "streampipeline.h"

#include "decoder.h"
#include "demuxer.h"
#include "encoder.h"
#include "muxer.h"

#include <QMetaObject>
#include <QPointer>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

#include <algorithm>
#include <cerrno>
#include <string>
#include <utility>

namespace {

std::string avErrorString(int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, buffer, sizeof(buffer));
    return buffer;
}

bool isRtmpUrl(const QString& text)
{
    const QUrl url = QUrl::fromUserInput(text);
    const QString scheme = url.scheme().toLower();
    return url.isValid() && (scheme == "rtmp" || scheme == "rtmps");
}

class FileSource final : public ISource
{
public:
    ~FileSource() override { close(); }

    int open(const QUrl& input) override
    {
        close();
        if (!input.isLocalFile()) {
            return AVERROR(EINVAL);
        }

        demuxer_ = std::make_unique<Demuxer>();
        int ret = demuxer_->open(input);
        if (ret < 0) {
            return ret;
        }
        const int videoIndex = demuxer_->bestVideoStreamIndex();
        const int audioIndex = demuxer_->bestAudioStreamIndex();
        AVStream* videoStream = demuxer_->stream(videoIndex);
        AVStream* audioStream = demuxer_->stream(audioIndex);
        if (!videoStream || !videoStream->codecpar || !audioStream || !audioStream->codecpar) {
            return AVERROR_STREAM_NOT_FOUND;
        }

        videoPackets_ = std::make_unique<PacketQueue>();
        audioPackets_ = std::make_unique<PacketQueue>();
        videoFrames_ = std::make_unique<FrameQueue>();
        audioFrames_ = std::make_unique<FrameQueue>();
        videoDecoder_ = std::make_unique<VideoDecoder>();
        audioDecoder_ = std::make_unique<AudioDecoder>();
        Decoder::Options softwareOptions;
        softwareOptions.enableHardware = false;
        ret = videoDecoder_->open(videoStream, softwareOptions);
        if (ret < 0) {
            return ret;
        }
        ret = audioDecoder_->open(audioStream, softwareOptions);
        if (ret < 0) {
            return ret;
        }

        videoDecoder_->setQueues(videoPackets_.get(), videoFrames_.get());
        audioDecoder_->setQueues(audioPackets_.get(), audioFrames_.get());
        demuxer_->setPacketQueue(videoIndex, videoPackets_.get());
        demuxer_->setPacketQueue(audioIndex, audioPackets_.get());
        videoInfo_.width = videoStream->codecpar->width;
        videoInfo_.height = videoStream->codecpar->height;
        videoInfo_.timeBase = videoStream->time_base;
        videoInfo_.frameRate = videoStream->avg_frame_rate.num > 0
                                   ? videoStream->avg_frame_rate
                                   : videoStream->r_frame_rate;
        audioInfo_.timeBase = audioStream->time_base;
        commonStartTimeUs_ = AV_NOPTS_VALUE;
        const AVStream* streams[] = {videoStream, audioStream};
        for (const AVStream* stream : streams) {
            if (stream->start_time == AV_NOPTS_VALUE) {
                continue;
            }
            const int64_t start = av_rescale_q(stream->start_time,
                                                stream->time_base,
                                                AV_TIME_BASE_Q);
            commonStartTimeUs_ = commonStartTimeUs_ == AV_NOPTS_VALUE
                                     ? start
                                     : std::min(commonStartTimeUs_, start);
        }

        auto report = [this](int error, const std::string& message) {
            if (errorCb_) {
                errorCb_(error, message);
            }
        };
        demuxer_->setEofCallback([this] {
            if (eofCb_) {
                eofCb_();
            }
        });
        demuxer_->setErrorCallback(report);
        videoDecoder_->setErrorCallback(report);
        audioDecoder_->setErrorCallback(report);
        return 0;
    }

    int start() override
    {
        if (!demuxer_ || !videoDecoder_ || !audioDecoder_) {
            return AVERROR(EINVAL);
        }
        int ret = videoDecoder_->start();
        if (ret < 0) {
            return ret;
        }
        ret = audioDecoder_->start();
        if (ret < 0) {
            stop();
            return ret;
        }
        ret = demuxer_->start();
        if (ret < 0) {
            stop();
        }
        return ret;
    }

    void stop() override
    {
        // Encoders close the frame-queue consumer first. Decoder stop then wakes
        // any remaining packet/frame waits before the demuxer is joined.
        if (videoDecoder_) {
            videoDecoder_->stop();
        }
        if (audioDecoder_) {
            audioDecoder_->stop();
        }
        if (demuxer_) {
            demuxer_->stop();
        }
    }

    void close() override
    {
        stop();
        if (videoDecoder_) {
            videoDecoder_->close();
        }
        if (audioDecoder_) {
            audioDecoder_->close();
        }
        if (demuxer_) {
            demuxer_->close();
        }
        videoDecoder_.reset();
        audioDecoder_.reset();
        demuxer_.reset();
        videoFrames_.reset();
        audioFrames_.reset();
        videoPackets_.reset();
        audioPackets_.reset();
        videoInfo_ = {};
        audioInfo_ = {};
        commonStartTimeUs_ = AV_NOPTS_VALUE;
    }

    FrameQueue* videoFrames() const override { return videoFrames_.get(); }
    FrameQueue* audioFrames() const override { return audioFrames_.get(); }
    VideoInfo videoInfo() const override { return videoInfo_; }
    AudioInfo audioInfo() const override { return audioInfo_; }
    int64_t commonStartTimeUs() const override { return commonStartTimeUs_; }
    void setEofCallback(EofCallback cb) override { eofCb_ = std::move(cb); }
    void setErrorCallback(ErrorCallback cb) override { errorCb_ = std::move(cb); }

private:
    std::unique_ptr<Demuxer> demuxer_;
    std::unique_ptr<VideoDecoder> videoDecoder_;
    std::unique_ptr<AudioDecoder> audioDecoder_;
    std::unique_ptr<PacketQueue> videoPackets_;
    std::unique_ptr<PacketQueue> audioPackets_;
    std::unique_ptr<FrameQueue> videoFrames_;
    std::unique_ptr<FrameQueue> audioFrames_;
    VideoInfo videoInfo_;
    AudioInfo audioInfo_;
    int64_t commonStartTimeUs_ = AV_NOPTS_VALUE;
    EofCallback eofCb_;
    ErrorCallback errorCb_;
};

} // namespace

struct StreamPipeline::StartJob {
    void setActiveSource(ISource* source)
    {
        std::lock_guard<std::mutex> lock(mutex);
        activeSource = source;
        if (cancelled && activeSource) {
            activeSource->stop();
        }
    }

    void setActiveMuxer(Muxer* muxer)
    {
        std::lock_guard<std::mutex> lock(mutex);
        activeMuxer = muxer;
        if (cancelled && activeMuxer) {
            activeMuxer->abort();
        }
    }

    void cancel()
    {
        std::lock_guard<std::mutex> lock(mutex);
        cancelled = true;
        if (activeSource) {
            activeSource->stop();
        }
        if (activeMuxer) {
            activeMuxer->abort();
        }
    }

    bool isCancelled() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return cancelled;
    }

    mutable std::mutex mutex;
    bool cancelled = false;
    ISource* activeSource = nullptr;
    Muxer* activeMuxer = nullptr;
};

struct StreamPipeline::StartResult {
    int error = 0;
    std::string message;
    std::unique_ptr<ISource> source;
    std::unique_ptr<VideoEncoder> videoEncoder;
    std::unique_ptr<AudioEncoder> audioEncoder;
    std::unique_ptr<Muxer> muxer;
    int videoMuxStream = -1;
    int audioMuxStream = -1;
};

StreamPipeline::StreamPipeline(QObject* parent)
    : QObject(parent),
      sourceFactory_([] { return std::make_unique<FileSource>(); })
{
}

StreamPipeline::~StreamPipeline()
{
    stop();
}

void StreamPipeline::setSourceFactory(SourceFactory factory)
{
    if (state() == State::Idle && factory) {
        sourceFactory_ = std::move(factory);
    }
}

void StreamPipeline::start(const QUrl& inputFile, const StreamOutputOptions& options)
{
    stop();
    if (!inputFile.isLocalFile() || !isRtmpUrl(options.rtmpUrl)) {
        transitionTo(State::Error);
        emit errorOccurred(AVERROR(EINVAL), QStringLiteral("A local input file and rtmp:// output URL are required"));
        return;
    }

    abort_ = false;
    const unsigned long long generation = ++generation_;
    auto job = std::make_shared<StartJob>();
    startJob_ = job;
    transitionTo(State::Connecting);
    startSetupWorker(inputFile, options, generation, job);
}

void StreamPipeline::stop()
{
    ++generation_;
    abort_ = true;
    if (startJob_) {
        startJob_->cancel();
    }
    joinSetupWorker();
    startJob_.reset();
    teardownActivePipeline();
    transitionTo(State::Idle);
}

StreamPipeline::State StreamPipeline::state() const
{
    return state_.load();
}

bool StreamPipeline::isStreaming() const
{
    return state() == State::Streaming;
}

void StreamPipeline::transitionTo(State state)
{
    const State previous = state_.exchange(state);
    if (previous != state) {
        emit stateChanged(state);
    }
}

void StreamPipeline::startSetupWorker(const QUrl& inputFile,
                                      const StreamOutputOptions& options,
                                      unsigned long long generation,
                                      const std::shared_ptr<StartJob>& job)
{
    const SourceFactory factory = sourceFactory_;
    QPointer<StreamPipeline> guard(this);
    setupThread_ = std::thread([guard, inputFile, options, generation, job, factory] {
        auto result = std::make_shared<StartResult>();
        std::unique_ptr<ISource> source = factory ? factory() : nullptr;
        if (!source) {
            result->error = AVERROR(ENOMEM);
            result->message = "stream source factory returned null";
        } else {
            job->setActiveSource(source.get());
            result->error = source->open(inputFile);
            if (result->error < 0) {
                result->message = "opening input source failed: " + avErrorString(result->error);
            }
        }

        if (result->error >= 0 && !job->isCancelled()) {
            result->muxer = std::make_unique<Muxer>();
            job->setActiveMuxer(result->muxer.get());
            result->error = result->muxer->open(options.rtmpUrl);
            if (result->error < 0) {
                result->message = "opening RTMP output " + options.rtmpUrl.toStdString() +
                                  " failed: " + avErrorString(result->error) +
                                  ". Ensure an RTMP receiver is listening on the target host and port";
            }
        }

        if (result->error >= 0 && !job->isCancelled()) {
            const ISource::VideoInfo videoInfo = source->videoInfo();
            const ISource::AudioInfo audioInfo = source->audioInfo();
            result->videoEncoder = std::make_unique<VideoEncoder>();
            result->audioEncoder = std::make_unique<AudioEncoder>();
            result->error = result->videoEncoder->open(
                VideoEncoder::Input{videoInfo.width, videoInfo.height, videoInfo.timeBase, videoInfo.frameRate},
                options);
            if (result->error >= 0) {
                result->error = result->audioEncoder->open(AudioEncoder::Input{audioInfo.timeBase}, options);
            }
            if (result->error < 0) {
                result->message = "opening encoders failed: " + avErrorString(result->error);
            }
        }

        if (result->error >= 0 && !job->isCancelled()) {
            result->videoMuxStream = result->muxer->addStream(result->videoEncoder->codecContext());
            result->error = result->videoMuxStream < 0 ? result->videoMuxStream : 0;
            if (result->error >= 0) {
                result->audioMuxStream = result->muxer->addStream(result->audioEncoder->codecContext());
                result->error = result->audioMuxStream < 0 ? result->audioMuxStream : 0;
            }
            if (result->error >= 0) {
                result->error = result->muxer->writeHeader();
            }
            if (result->error < 0) {
                result->message = "initializing FLV streams failed: " + avErrorString(result->error);
            }
        }

        if (job->isCancelled() && result->error >= 0) {
            result->error = AVERROR_EXIT;
            result->message = "stream startup cancelled";
        }
        if (result->error >= 0 && !job->isCancelled()) {
            result->source = std::move(source);
        }

        if (guard) {
            QMetaObject::invokeMethod(
                guard,
                [guard, generation, result] {
                    if (guard) {
                        guard->finishSetup(generation, result);
                    }
                },
                Qt::QueuedConnection);
        }
    });
}

void StreamPipeline::finishSetup(unsigned long long generation,
                                 const std::shared_ptr<StartResult>& result)
{
    joinSetupWorker();
    if (generation == generation_.load()) {
        startJob_.reset();
    }
    if (generation != generation_.load() || abort_ || !result || result->error < 0) {
        if (generation == generation_.load() && !abort_ && result && result->error < 0) {
            transitionTo(State::Error);
            emit errorOccurred(result->error, QString::fromLocal8Bit(result->message.c_str()));
        }
        return;
    }

    if (!result->source || !result->videoEncoder || !result->audioEncoder || !result->muxer ||
        result->videoMuxStream < 0 || result->audioMuxStream < 0) {
        transitionTo(State::Error);
        emit errorOccurred(AVERROR_BUG, QStringLiteral("Stream setup returned an incomplete pipeline"));
        return;
    }

    source_ = std::move(result->source);
    videoEncoder_ = std::move(result->videoEncoder);
    audioEncoder_ = std::move(result->audioEncoder);
    muxer_ = std::move(result->muxer);
    videoMuxStream_ = result->videoMuxStream;
    audioMuxStream_ = result->audioMuxStream;
    timelineStartUs_ = source_->commonStartTimeUs();
    syntheticVideoPts_ = -1;
    videoEncoderEof_ = false;
    audioEncoderEof_ = false;
    finalizingEof_ = false;

    source_->setErrorCallback([this, generation](int error, const std::string& message) {
        postError(generation, error, message);
    });
    videoEncoder_->setInputQueue(source_->videoFrames());
    audioEncoder_->setInputQueue(source_->audioFrames());
    videoEncoder_->setPtsMapper([this](const AVFrame* frame, AVRational timeBase) {
        return mapFramePts(frame, timeBase, videoEncoder_->timeBase(), true);
    });
    audioEncoder_->setPtsMapper([this](const AVFrame* frame, AVRational timeBase) {
        return mapFramePts(frame, timeBase, audioEncoder_->timeBase(), false);
    });
    videoEncoder_->setPacketCallback([this, generation](const AVPacket* packet, AVRational timeBase) {
        if (generation != generation_.load() || abort_ || !muxer_) {
            return AVERROR_EXIT;
        }
        return muxer_->writePacket(videoMuxStream_, packet, timeBase);
    });
    audioEncoder_->setPacketCallback([this, generation](const AVPacket* packet, AVRational timeBase) {
        if (generation != generation_.load() || abort_ || !muxer_) {
            return AVERROR_EXIT;
        }
        return muxer_->writePacket(audioMuxStream_, packet, timeBase);
    });
    videoEncoder_->setErrorCallback([this, generation](int error, const std::string& message) {
        postError(generation, error, message);
    });
    audioEncoder_->setErrorCallback([this, generation](int error, const std::string& message) {
        postError(generation, error, message);
    });
    videoEncoder_->setEofCallback([this, generation] { onEncoderEof(generation, true); });
    audioEncoder_->setEofCallback([this, generation] { onEncoderEof(generation, false); });

    int ret = videoEncoder_->start();
    if (ret >= 0) {
        ret = audioEncoder_->start();
    }
    if (ret >= 0) {
        ret = source_->start();
    }
    if (ret < 0) {
        handleError(generation, ret, QStringLiteral("starting streaming pipeline failed"));
        return;
    }
    transitionTo(State::Streaming);
}

void StreamPipeline::joinSetupWorker()
{
    if (setupThread_.joinable()) {
        setupThread_.join();
    }
}

void StreamPipeline::teardownActivePipeline()
{
    if (muxer_) {
        muxer_->abort();
    }
    if (videoEncoder_) {
        videoEncoder_->stop();
    }
    if (audioEncoder_) {
        audioEncoder_->stop();
    }
    if (source_) {
        source_->stop();
    }
    if (muxer_) {
        muxer_->writeTrailer();
    }
    if (videoEncoder_) {
        videoEncoder_->close();
    }
    if (audioEncoder_) {
        audioEncoder_->close();
    }
    if (source_) {
        source_->close();
    }
    if (muxer_) {
        muxer_->close();
    }
    videoEncoder_.reset();
    audioEncoder_.reset();
    source_.reset();
    muxer_.reset();
    videoMuxStream_ = -1;
    audioMuxStream_ = -1;
}

void StreamPipeline::postError(unsigned long long generation,
                               int errCode,
                               const std::string& message)
{
    QPointer<StreamPipeline> guard(this);
    QMetaObject::invokeMethod(
        this,
        [guard, generation, errCode, message] {
            if (guard) {
                guard->handleError(generation,
                                   errCode,
                                   QString::fromLocal8Bit(message.c_str()));
            }
        },
        Qt::QueuedConnection);
}
void StreamPipeline::handleError(unsigned long long generation,
                                 int errCode,
                                 const QString& message)
{
    if (generation != generation_.load() || abort_) {
        return;
    }
    abort_ = true;
    teardownActivePipeline();
    transitionTo(State::Error);
    emit errorOccurred(errCode, message);
}

void StreamPipeline::onEncoderEof(unsigned long long generation, bool video)
{
    bool finalize = false;
    {
        std::lock_guard<std::mutex> lock(eofMutex_);
        if (generation != generation_.load() || abort_) {
            return;
        }
        if (video) {
            videoEncoderEof_ = true;
        } else {
            audioEncoderEof_ = true;
        }
        if (videoEncoderEof_ && audioEncoderEof_ && !finalizingEof_) {
            finalizingEof_ = true;
            finalize = true;
        }
    }
    if (!finalize) {
        return;
    }
    const int ret = muxer_ ? muxer_->writeTrailer() : AVERROR(EINVAL);
    if (ret < 0) {
        postError(generation, ret, "writing RTMP trailer failed: " + avErrorString(ret));
        return;
    }
    QPointer<StreamPipeline> guard(this);
    QMetaObject::invokeMethod(
        this,
        [guard, generation] {
            if (guard) {
                guard->handleFinished(generation);
            }
        },
        Qt::QueuedConnection);
}
void StreamPipeline::handleFinished(unsigned long long generation)
{
    if (generation != generation_.load() || abort_) {
        return;
    }
    abort_ = true;
    teardownActivePipeline();
    transitionTo(State::Idle);
    emit finished();
}

int64_t StreamPipeline::mapFramePts(const AVFrame* frame,
                                    AVRational inputTimeBase,
                                    AVRational encoderTimeBase,
                                    bool video)
{
    int64_t sourcePts = frame ? frame->pts : AV_NOPTS_VALUE;
    if (sourcePts == AV_NOPTS_VALUE && frame) {
        sourcePts = frame->best_effort_timestamp;
    }
    if (sourcePts == AV_NOPTS_VALUE || inputTimeBase.num <= 0 || inputTimeBase.den <= 0) {
        if (video) {
            return syntheticVideoPts_ < 0 ? (syntheticVideoPts_ = 0) : ++syntheticVideoPts_;
        }
        return 0;
    }

    const int64_t sourceUs = av_rescale_q(sourcePts, inputTimeBase, AV_TIME_BASE_Q);
    int64_t normalizedUs = 0;
    {
        std::lock_guard<std::mutex> lock(timelineMutex_);
        if (timelineStartUs_ == AV_NOPTS_VALUE) {
            timelineStartUs_ = sourceUs;
        }
        normalizedUs = std::max<int64_t>(0, sourceUs - timelineStartUs_);
    }
    const int64_t mapped = av_rescale_q(normalizedUs, AV_TIME_BASE_Q, encoderTimeBase);
    if (video) {
        syntheticVideoPts_ = mapped;
    }
    return mapped;
}
