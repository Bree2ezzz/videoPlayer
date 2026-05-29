#ifndef PACKETQUEUE_H
#define PACKETQUEUE_H

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>

/*
 * PacketQueue 引入 serial（仿 ffplay）：
 *   - 每个入队的 packet 携带当时 producer 的 serial
 *   - flush() 清队列后让 serial++，下一个入队的 packet 就带上递增后的 serial
 *   - 下游（Decoder/RenderScheduler/AudioOutput）通过 pop 时拿到的 serial 与
 *     PacketQueue::currentSerial() 比对，serial 跳变即代表 seek 边界，
 *     需要做 avcodec_flush_buffers 或丢弃旧 frame
 *
 * 这种设计相比"独立 flush 标志位 + minPts 过滤"的优势：
 *   - flush 时机精确卡在新旧 GOP 边界（decoder 看到 serial 跳变那一刻）
 *   - 跨线程不需要额外同步，serial 是单调递增的原子量
 *   - 旧 serial 的残留 frame 在渲染前会被自动丢弃；精确 seek 仍需在
 *     新 serial 内丢弃落在目标时间之前的输出 frame
 */

class PacketQueue
{
public:
    enum Result {
        Ok = 0,
        Timeout = 1,
        Interrupted = 2,
        Aborted = -1,
        EndOfStream = -2,
    };

    explicit PacketQueue(size_t maxSize = 100) : maxSize_(maxSize) {}
    ~PacketQueue() { flush(); }

    PacketQueue(const PacketQueue&) = delete;
    PacketQueue& operator=(const PacketQueue&) = delete;

    int push(AVPacket* pkt, const std::atomic_bool* interrupt = nullptr)
    {
        if (!pkt) return Aborted;

        std::unique_lock<std::mutex> lock(mutex_);
        notFull_.wait(lock, [this, interrupt] {
            return queue_.size() < maxSize_ || producerClosed_ || consumerClosed_ ||
                   (interrupt && interrupt->load());
        });

        if (interrupt && interrupt->load()) return Interrupted;
        if (producerClosed_ || consumerClosed_) return Aborted;

        AVPacket* packet = av_packet_alloc();
        if (!packet) return Aborted;

        av_packet_move_ref(packet, pkt);
        queue_.push(Entry{packet, serial_});
        notEmpty_.notify_one();
        return Ok;
    }

    // pop 一个 packet 并返回它入队时的 serial。serial 可传 nullptr 表示忽略。
    int pop(AVPacket** pkt, int* serial, int timeout_ms)
    {
        if (!pkt) return Aborted;
        *pkt = nullptr;
        if (serial) *serial = -1;

        std::unique_lock<std::mutex> lock(mutex_);
        notEmpty_.wait_for(
            lock,
            std::chrono::milliseconds(timeout_ms),
            [this] {
                return !queue_.empty() || producerClosed_ || consumerClosed_;
            });

        if (consumerClosed_) return Aborted;

        if (!queue_.empty()) {
            Entry entry = queue_.front();
            queue_.pop();
            *pkt = entry.pkt;
            if (serial) *serial = entry.serial;
            notFull_.notify_one();
            return Ok;
        }

        if (producerClosed_) return EndOfStream;
        return Timeout;
    }

    // 清空队列并让 serial 递增。后续 push 的 packet 带新 serial，
    // 下游据此识别 seek 边界。
    void flush()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) {
            Entry entry = queue_.front();
            queue_.pop();
            av_packet_free(&entry.pkt);
        }
        ++serial_;
        notFull_.notify_all();
    }

    // 当前 producer 的 serial（即下一个 push 的 packet 会带的 serial）。
    // 渲染器/音频输出据此与 frame 上携带的 serial 比对。
    int currentSerial() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return serial_;
    }

    // 唤醒正在等待队列容量的生产者，使其有机会观察外部 seek/abort 条件。
    void notifyProducer()
    {
        notFull_.notify_all();
    }

    void closeProducer()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        producerClosed_ = true;
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    void closeConsumer()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        consumerClosed_ = true;
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    void abortAll()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        producerClosed_ = true;
        consumerClosed_ = true;
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    void abort() { abortAll(); }

    void reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        producerClosed_ = false;
        consumerClosed_ = false;
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    struct Entry {
        AVPacket* pkt;
        int serial;
    };

    std::queue<Entry> queue_;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    size_t maxSize_;
    int serial_ = 0;
    bool producerClosed_ = false;
    bool consumerClosed_ = false;
};

#endif // PACKETQUEUE_H
