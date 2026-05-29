#ifndef RECONNECTSTRATEGY_H
#define RECONNECTSTRATEGY_H

#include <chrono>
#include <cstdint>

/*
 * ReconnectStrategy 职责边界：
 *   决定网络流断开后是否、何时、以何种间隔尝试重连。
 *   纯策略对象，不知道 Demuxer 的存在。PlaybackController 在收到
 *   网络错误时询问该对象"下一次该等多久 / 是否放弃"，再驱动 Demuxer
 *   重新 open。
 *
 * 设计动机：
 *   - 把"重连策略"和"重连执行"分离：策略可以单独测试（喂入序列化的
 *     失败时间点，断言下一次延迟），执行逻辑放在 PlaybackController
 *     的 Reconnecting 状态分支里。
 *   - 默认实现是指数退避 + 上限 + 总次数限制；后续要换成"基于带宽"
 *     "基于服务器返回码"等策略时，只换实现不改调用方
 *
 * 默认行为（DefaultReconnectStrategy）：
 *   第 1 次：1s
 *   第 2 次：2s
 *   第 3 次：4s
 *   第 4 次：8s
 *   第 5 次起：每次 16s，封顶
 *   总尝试次数：默认 8 次，到上限后 shouldRetry 返回 false
 *   连接成功后调 reset()，下次断开重新从 1s 起步
 *
 * 线程模型：
 *   所有方法只在 PlaybackController 所在线程调用，不需要锁
 */

class ReconnectStrategy
{
public:
    virtual ~ReconnectStrategy() = default;

    // 是否还应该再尝试一次。返回 false 时 PlaybackController 放弃并
    // 进入 Error 状态。
    virtual bool shouldRetry() const = 0;

    // 下一次重连之前应当等待的延迟（毫秒）。
    // 调用之后内部的尝试计数 +1（注意：不是在 onSuccess 之后，而是在
    // 实际发起 open 前调）
    virtual std::chrono::milliseconds nextDelay() = 0;

    // 重连成功后调用，重置内部状态。下一次断开从初始值起步
    virtual void onSuccess() = 0;

    // 当前已尝试次数（用于 UI 显示 "Reconnecting (3/8)…"）
    virtual int attemptCount() const = 0;

    // 上限尝试次数（用于 UI 文案；返回 0 表示无上限）
    virtual int maxAttempts() const = 0;
};

/*
 * 指数退避默认实现：1s, 2s, 4s, 8s, 16s, 16s, ...，最多 maxAttempts 次。
 *
 * 参数：
 *   initialDelayMs   首次重连延迟，默认 1000
 *   maxDelayMs       延迟封顶，默认 16000
 *   maxAttempts      最大尝试次数，0 表示无上限；默认 8
 *   backoffFactor    每次延迟相对上一次的倍数，默认 2.0
 *
 * 实现位于 reconnectstrategy.cpp。
 */
class DefaultReconnectStrategy : public ReconnectStrategy
{
public:
    explicit DefaultReconnectStrategy(int initialDelayMs = 1000,
                                      int maxDelayMs = 16000,
                                      int maxAttempts = 8,
                                      double backoffFactor = 2.0);

    bool shouldRetry() const override;
    std::chrono::milliseconds nextDelay() override;
    void onSuccess() override;
    int attemptCount() const override;
    int maxAttempts() const override;

private:
    const int initialDelayMs_;
    const int maxDelayMs_;
    const int maxAttempts_;
    const double backoffFactor_;

    int attempt_ = 0;
    int currentDelayMs_;
};

#endif // RECONNECTSTRATEGY_H
