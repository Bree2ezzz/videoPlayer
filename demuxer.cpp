#include "demuxer.h"

#include "logging.h"

#include <QByteArray>
#include <QString>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <utility>

namespace {

std::string avErrorString(int errCode)
{
    char errBuf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errCode, errBuf, sizeof(errBuf));
    return errBuf;
}

std::string inputStringFromUrl(const QUrl& url)
{
    if (url.isLocalFile()) {
        return url.toLocalFile().toStdString();
    }

    if (url.scheme().isEmpty()) {
        return url.toString().toStdString();
    }

    return url.toEncoded().toStdString();
}

bool isRealtimeScheme(const QUrl& url)
{
    const QString scheme = url.scheme().toLower();
    return scheme == "rtsp" ||
           scheme == "rtmp" ||
           scheme == "rtp" ||
           scheme == "udp";
}

} // namespace

Demuxer::Demuxer() = default;

Demuxer::~Demuxer()
{
    close();
}

int Demuxer::open(const QUrl& url)
{
    close();
    //将url初步检测是本地文件还是网络流
    const std::string input = inputStringFromUrl(url);
    AVFormatContext* ctx = avformat_alloc_context();
    if (!ctx) {
        const int ret = AVERROR(ENOMEM);
        if (errorCb_) {
            errorCb_(ret, "avformat_alloc_context failed: " + avErrorString(ret));
        }
        return ret;
    }

    abort_ = false;
    //设置中断轮询，在阻塞io的时候会不断调用
    ctx->interrupt_callback.callback = &Demuxer::interruptCb;
    ctx->interrupt_callback.opaque = this;

    int ret = avformat_open_input(&ctx, input.c_str(), nullptr, nullptr);
    if (ret < 0) {
        if (errorCb_) {
            errorCb_(ret, "avformat_open_input failed: " + avErrorString(ret));
        }
        avformat_close_input(&ctx);
        return ret;
    }

    ret = avformat_find_stream_info(ctx, nullptr);
    if (ret < 0) {
        if (errorCb_) {
            errorCb_(ret, "avformat_find_stream_info failed: " + avErrorString(ret));
        }
        avformat_close_input(&ctx);
        return ret;
    }

    fmtCtx_ = ctx;
    realtime_ = isRealtimeScheme(url) || fmtCtx_->duration == AV_NOPTS_VALUE;
    abort_ = false;
    seekPending_ = false;
    seekReqUs_ = 0;

    return 0;
}

void Demuxer::close()
{
    //ffmpeg上下文释放  内部packetqueue清理  状态重置
    stop();

    if (fmtCtx_) {
        avformat_close_input(&fmtCtx_);
    }

    {
        std::lock_guard<std::mutex> lock(queueMapMutex_);
        packetQueues_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(seekMutex_);
        seekPending_ = false;
        seekReqUs_ = 0;
    }

    realtime_ = false;
}

int Demuxer::start()
{
    if (started_) {
        return 0;
    }
    //joinable为真代表这个线程已经传入了一个函数，已经拥有一个线程了。
    if (thread_.joinable()) {
        return AVERROR(EALREADY);
    }

    if (!fmtCtx_) {
        return AVERROR(EINVAL);
    }

    {
        std::lock_guard<std::mutex> lock(queueMapMutex_);
        if (packetQueues_.empty()) {
            return AVERROR(EINVAL);
        }

        for (auto& item : packetQueues_) {
            if (item.second) {
                //如果传入的queue是之前使用过的，这里需要给它打开
                //如果这个queue里面有数据或者被使用中那么上层调用的时候有问题，这里不做过多保护
                item.second->reset();
            }
        }
    }

    abort_ = false;
    started_ = true;
    thread_ = std::thread(&Demuxer::readLoop, this);
    return 0;
}

void Demuxer::stop()
{
    abort_ = true;
    seekCond_.notify_all();
    // 唤醒可能挂在 pauseCv_ 上的 readLoop，让它走 abort 分支退出
    pauseRequested_.store(false);
    pauseCv_.notify_all();
    //将demuxer使用的queue全都关闭生产端
    //没有进行清理，上层需要复用queue需要手动清理。清理需要等待所有使用这个queue的线程都停止了（例如decoder线程）
    std::vector<PacketQueue*> queues;
    {
        std::lock_guard<std::mutex> lock(queueMapMutex_);
        queues.reserve(packetQueues_.size());
        for (const auto& item : packetQueues_) {
            if (item.second) {
                queues.push_back(item.second);
            }
        }
    }
    for (PacketQueue* queue : queues) {
        queue->closeProducer();
    }
    //请求 readLoop 退出，并关闭已订阅 PacketQueue 的生产端，唤醒可能阻塞的 push/pop。
    //这里只 join demuxer 自己的线程；decoder 线程会通过 PacketQueue::EndOfStream
    //感知生产端结束，但其 join/资源释放由上层播放控制器负责。
    if (thread_.joinable()) {
        thread_.join();
    }

    started_ = false;
}

int Demuxer::streamCount() const
{
    return fmtCtx_ ? static_cast<int>(fmtCtx_->nb_streams) : 0;
}

AVStream* Demuxer::stream(int index) const
{
    if (!fmtCtx_ || index < 0 || index >= static_cast<int>(fmtCtx_->nb_streams)) {
        return nullptr;
    }

    return fmtCtx_->streams[index];
}

std::vector<int> Demuxer::audioStreamIndices() const
{
    //返回所有的音频流序号
    std::vector<int> indices;
    if (!fmtCtx_) {
        return indices;
    }

    for (unsigned int i = 0; i < fmtCtx_->nb_streams; ++i) {
        const AVStream* stream = fmtCtx_->streams[i];
        if (stream && stream->codecpar &&
            stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            indices.push_back(static_cast<int>(i));
        }
    }

    return indices;
}

std::vector<int> Demuxer::videoStreamIndices() const
{
    //返回所有视频流序号
    std::vector<int> indices;
    if (!fmtCtx_) {
        return indices;
    }

    for (unsigned int i = 0; i < fmtCtx_->nb_streams; ++i) {
        const AVStream* stream = fmtCtx_->streams[i];
        if (stream && stream->codecpar &&
            stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            indices.push_back(static_cast<int>(i));
        }
    }

    return indices;
}

int Demuxer::bestAudioStreamIndex() const
{
    if (!fmtCtx_) {
        return -1;
    }

    const int ret = av_find_best_stream(
        fmtCtx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    return ret >= 0 ? ret : -1;
}

int Demuxer::bestVideoStreamIndex() const
{
    if (!fmtCtx_) {
        return -1;
    }

    const int ret = av_find_best_stream(
        fmtCtx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    return ret >= 0 ? ret : -1;
}

int64_t Demuxer::durationUs() const
{
    //这个文件的总时长
    if (!fmtCtx_ || fmtCtx_->duration == AV_NOPTS_VALUE) {
        return -1;
    }

    return fmtCtx_->duration;
}

bool Demuxer::isRealtime() const
{
    return realtime_;
}

void Demuxer::setPacketQueue(int streamIndex, PacketQueue* queue)
{
    if (!fmtCtx_ || streamIndex < 0 ||
        streamIndex >= static_cast<int>(fmtCtx_->nb_streams)) {
        return;
    }

    std::lock_guard<std::mutex> lock(queueMapMutex_);
    if (queue) {
        packetQueues_[streamIndex] = queue;
    } else {
        packetQueues_.erase(streamIndex);
    }
}

void Demuxer::clearPacketQueue(int streamIndex)
{
    std::lock_guard<std::mutex> lock(queueMapMutex_);
    packetQueues_.erase(streamIndex);
}
// 仅切换 demuxer 的 packet 路由
//oldStreamIndex 不再接收 packet，newStreamIndex 的 packet 写入 queue。
//这里没有管理decoder、旧queue相关的线程等等的生命周期，也没有abort旧queue
//调用方去保证旧queue的消费者的下一步以及新queue的消费者的设置。
void Demuxer::switchStream(int oldStreamIndex, int newStreamIndex,
                           PacketQueue* queue, int64_t currentPtsUs)
{
    if (!fmtCtx_ || newStreamIndex < 0 ||
        newStreamIndex >= static_cast<int>(fmtCtx_->nb_streams)) {
        return;
    }

    PacketQueue* oldQueue = nullptr;
    {
        std::lock_guard<std::mutex> lock(queueMapMutex_);
        auto oldIt = packetQueues_.find(oldStreamIndex);
        if (oldIt != packetQueues_.end()) {
            oldQueue = oldIt->second;
            packetQueues_.erase(oldIt);
        }

        if (queue) {
            packetQueues_[newStreamIndex] = queue;
        }
    }

    if (oldQueue) {
        oldQueue->flush();
    }
    if (queue && queue != oldQueue) {
        queue->flush();
    }

    seek(currentPtsUs);
}

void Demuxer::seek(int64_t timestampUs)
{
    // seek 只提交请求；readLoop 在下一次写 packet 前完成定位并推进
    // queue serial，确保新旧时间轴可由消费者严格区分。
    if (!fmtCtx_) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(seekMutex_);
        seekReqUs_ = std::max<int64_t>(0, timestampUs);
        seekPending_ = true;
    }

    // readLoop 可能正在等待某个已满 PacketQueue 的容量。唤醒 push，
    // 让它根据 seekPending_ 放弃旧时间轴的 packet 并回到循环顶部处理 seek。
    std::vector<PacketQueue*> queues;
    {
        std::lock_guard<std::mutex> lock(queueMapMutex_);
        queues.reserve(packetQueues_.size());
        for (const auto& item : packetQueues_) {
            if (item.second) {
                queues.push_back(item.second);
            }
        }
    }
    for (PacketQueue* queue : queues) {
        queue->notifyProducer();
    }

    seekCond_.notify_all();
}

bool Demuxer::isSeeking() const
{
    return seekPending_;
}

void Demuxer::pause(bool paused)
{
    if (paused) {
        if (!started_.load() || abort_.load()) {
            return;
        }
        pauseRequested_.store(true);

        // 等 readLoop 真的进入暂停点（最长 500ms 防止上层卡死）
        std::unique_lock<std::mutex> lock(pauseMutex_);
        pausedAckCv_.wait_for(lock, std::chrono::milliseconds(500), [this] {
            return paused_.load() || !started_.load() || abort_.load();
        });
    } else {
        pauseRequested_.store(false);
        {
            std::lock_guard<std::mutex> lock(pauseMutex_);
            paused_.store(false);
        }
        pauseCv_.notify_all();
    }
}

bool Demuxer::isPaused() const
{
    return paused_.load();
}

void Demuxer::setEofCallback(EofCallback cb)
{
    eofCb_ = std::move(cb);
}

void Demuxer::setErrorCallback(ErrorCallback cb)
{
    errorCb_ = std::move(cb);
}

void Demuxer::setSeekCompletedCallback(SeekCompletedCallback cb)
{
    seekCompletedCb_ = std::move(cb);
}

void Demuxer::readLoop()
{
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        const int ret = AVERROR(ENOMEM);
        if (errorCb_) {
            errorCb_(ret, "av_packet_alloc failed: " + avErrorString(ret));
        }
        started_ = false;
        return;
    }

    while (!abort_) {
        // 暂停协议：控制线程置 pauseRequested_ 后，readLoop 在循环顶部
        // 把自己挂到 pauseCv_ 上等。挂入前发 ack，让 pause(true) 能同步返回。
        if (pauseRequested_.load()) {
            std::unique_lock<std::mutex> lock(pauseMutex_);
            paused_.store(true);
            pausedAckCv_.notify_all();
            pauseCv_.wait(lock, [this] {
                return !pauseRequested_.load() || abort_.load();
            });
            paused_.store(false);
            if (abort_.load()) {
                break;
            }
        }

        if (seekPending_) {
            std::unique_lock<std::mutex> lock(seekMutex_);
            if (seekPending_) {
                doSeekLocked(seekReqUs_);
                seekPending_ = false;
            }
        }

        const int ret = av_read_frame(fmtCtx_, pkt);
        if (ret == 0) {
            if (seekPending_) {
                av_packet_unref(pkt);
                continue;
            }

            PacketQueue* queue = findQueue(pkt->stream_index);
            if (queue) {
                const int pushRet = queue->push(pkt, &seekPending_);
                if (pushRet == PacketQueue::Interrupted) {
                    av_packet_unref(pkt);
                    continue;
                }
                if (pushRet != PacketQueue::Ok) {
                    av_packet_unref(pkt);
                    break;
                }
            } else {
                av_packet_unref(pkt);
            }
            continue;
        }

        if (ret == AVERROR_EOF) {
            closeProducerForAllQueues();
            if (eofCb_) {
                eofCb_();
            }
            break;
        }

        if (ret == AVERROR(EAGAIN)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (!abort_ && errorCb_) {
            errorCb_(ret, "av_read_frame failed: " + avErrorString(ret));
        }
        break;
    }

    closeProducerForAllQueues();
    av_packet_free(&pkt);
    started_ = false;
}

void Demuxer::doSeekLocked(int64_t timestampUs)
{
    std::vector<PacketQueue*> queues;
    {
        std::lock_guard<std::mutex> lock(queueMapMutex_);
        queues.reserve(packetQueues_.size());
        for (const auto& item : packetQueues_) {
            if (item.second) {
                queues.push_back(item.second);
            }
        }
    }

    if (!fmtCtx_ || realtime_) {
        // 实时流不支持 seek，但仍然清队列以恢复一致状态
        for (PacketQueue* queue : queues) {
            queue->flush();
        }
        return;
    }

    // av_seek_frame 的 stream_index 决定 ffmpeg 用哪条流的关键帧做锚。
    // 显式取视频流索引可以保证 seek 后第一帧落在视频 I 帧上。
    int seekStreamIndex = -1;
    int64_t seekTimestamp = timestampUs;
    const int videoIdx = av_find_best_stream(
        fmtCtx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIdx >= 0 && fmtCtx_->streams[videoIdx]) {
        seekStreamIndex = videoIdx;
        const AVRational& tb = fmtCtx_->streams[videoIdx]->time_base;
        seekTimestamp = av_rescale_q(timestampUs,
                                     AVRational{1, AV_TIME_BASE},
                                     tb);
    }

    const int ret = av_seek_frame(
        fmtCtx_, seekStreamIndex, seekTimestamp, AVSEEK_FLAG_BACKWARD);
    VP_LOG_INFO() << "av_seek_frame target=" << timestampUs
                  << "us streamIndex=" << seekStreamIndex
                  << " ret=" << ret;
    if (ret < 0) {
        if (errorCb_) {
            errorCb_(ret, "av_seek_frame failed: " + avErrorString(ret));
        }
        // seek 失败也要清队列、推进 serial，否则下游会一直处理无效旧包
        for (PacketQueue* queue : queues) {
            queue->flush();
        }
        return;
    }

    // ★ 顺序很重要：先 flush 队列让 serial 递增，再触发 seekCompleted 回调。
    // 回调内（PlaybackController）会重置 audioClock 到 timestampUs，
    // 之后 demuxer 继续 av_read_frame push 新 packet 时，已经是新 serial。
    // decoder 看到 serial 跳变会 avcodec_flush_buffers，所有路径汇合。
    for (PacketQueue* queue : queues) {
        queue->flush();
    }
    if (!queues.empty()) {
        VP_LOG_DEBUG() << "post-seek flush, new serial="
                       << queues.front()->currentSerial();
    }

    if (seekCompletedCb_) {
        seekCompletedCb_(timestampUs);
    }
}

void Demuxer::closeProducerForAllQueues()
{
    std::vector<PacketQueue*> queues;
    {
        std::lock_guard<std::mutex> lock(queueMapMutex_);
        queues.reserve(packetQueues_.size());
        for (const auto& item : packetQueues_) {
            if (item.second) {
                queues.push_back(item.second);
            }
        }
    }

    for (PacketQueue* queue : queues) {
        queue->closeProducer();
    }
}

PacketQueue* Demuxer::findQueue(int streamIndex)
{
    std::lock_guard<std::mutex> lock(queueMapMutex_);
    auto it = packetQueues_.find(streamIndex);
    if (it == packetQueues_.end()) {
        return nullptr;
    }

    return it->second;
}

int Demuxer::interruptCb(void* opaque)
{
    Demuxer* demuxer = static_cast<Demuxer*>(opaque);
    return demuxer && demuxer->abort_ ? 1 : 0;
}
