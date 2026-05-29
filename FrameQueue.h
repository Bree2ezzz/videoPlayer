#ifndef FRAMEQUEUE_H
#define FRAMEQUEUE_H

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>

/*
 * FrameQueue 上的 serial 由 Decoder 推入时显式指定，对应当时 packet 的 serial。
 * 下游消费者（RenderScheduler / AudioOutput）pop 时取出 serial，与
 * PacketQueue::currentSerial() 比对：不一致即代表 seek 后的旧 frame，丢弃即可。
 */

class FrameQueue
{
public:
    enum Result {
        Ok = 0,
        Timeout = 1,
        Aborted = -1,
        EndOfStream = -2,
    };

    explicit FrameQueue(size_t maxSize = 16) : maxSize_(maxSize) {}
    ~FrameQueue() { flush(); }

    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    int push(AVFrame* frame, int serial)
    {
        if (!frame) return Aborted;

        std::unique_lock<std::mutex> lock(mutex_);
        notFull_.wait(lock, [this] {
            return queue_.size() < maxSize_ || producerClosed_ || consumerClosed_;
        });

        if (producerClosed_ || consumerClosed_) return Aborted;

        AVFrame* queuedFrame = av_frame_alloc();
        if (!queuedFrame) return Aborted;

        av_frame_move_ref(queuedFrame, frame);
        queue_.push(Entry{queuedFrame, serial});
        notEmpty_.notify_one();
        return Ok;
    }

    int pop(AVFrame** frame, int* serial, int timeout_ms)
    {
        if (!frame) return Aborted;
        *frame = nullptr;
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
            *frame = entry.frame;
            if (serial) *serial = entry.serial;
            notFull_.notify_one();
            return Ok;
        }

        if (producerClosed_) return EndOfStream;
        return Timeout;
    }

    void flush()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) {
            Entry entry = queue_.front();
            queue_.pop();
            av_frame_free(&entry.frame);
        }
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
        AVFrame* frame;
        int serial;
    };

    std::queue<Entry> queue_;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    size_t maxSize_;
    bool producerClosed_ = false;
    bool consumerClosed_ = false;
};

#endif // FRAMEQUEUE_H
