#ifndef NETWORKOPTIONS_H
#define NETWORKOPTIONS_H

#include <QString>

#include <cstdint>

/*
 * NetworkOptions：网络流（RTSP / RTMP / HTTP-FLV / HLS 等）的连接参数。
 *
 * 设计动机：
 *   - 这些参数最终会被翻译成 FFmpeg 的 AVDictionary 传给
 *     avformat_open_input。把它们集中成一个 struct 而不是散落在 Demuxer
 *     接口签名里，可以让 PlaybackController 一次性配置，也方便
 *     单元测试构造各种场景。
 *   - 默认值对常见场景（监控 RTSP、抖音/B 站 RTMP 推流）都足够安全；
 *     但每个字段都给注释，方便后续按需调
 *
 * 用法：
 *   PlaybackController::open(url, NetworkOptions{...})
 *   Demuxer::open(url, NetworkOptions{...})
 *
 * 与 Demuxer 的关系：
 *   Demuxer 内部有一个 toAvDictionary() 工具方法把本结构翻译成
 *   AVDictionary*，再传给 avformat_open_input。本头文件不依赖 FFmpeg，
 *   方便在 UI 层使用。
 */

struct NetworkOptions {
    // 是否启用网络层超时与重连。本地文件路径忽略本结构。
    // PlaybackController 会根据 url scheme 自动判断是否传入。
    bool networkStream = false;

    // ---------- RTSP 专属 ----------
    // RTSP 传输层："tcp" / "udp" / "udp_multicast" / "http"
    // 默认 "tcp"：监控场景下绝大多数路由器/网关都拒绝 UDP 直传。
    QString rtspTransport = QStringLiteral("tcp");

    // RTSP 收包超时（微秒）。底层翻译为 "stimeout" / "timeout"。
    // 30e6 = 30s，超过该时间仍无网络响应时交给上层进入重连流程。
    int64_t rtspStimeoutUs = 30'000'000;

    // ---------- RTMP 专属 ----------
    // RTMP live mode：true 时 ffmpeg 会按低延迟方式读流；点播 false。
    // rtmp / rtmps 都会翻译为 "rtmp_live" = "live" / "any"。
    bool rtmpLive = true;

    // ---------- HTTP / HTTPS / HLS / HTTP-FLV ----------
    // 让 FFmpeg 的 HTTP 协议层在网络断开、EOF 或直播分片读取失败时尝试重连。
    // 对 HLS(m3u8)、HTTP-FLV、HTTP MP4 直链都适用；播放器上层仍有整体超时兜底。
    bool httpReconnect = true;
    int httpReconnectDelayMaxSec = 5;

    // ---------- 通用网络 ----------
    // 接收 socket buffer 大小（字节）。0 表示不设置，沿用系统默认。
    // 高码率场景（4K HEVC > 20Mbps）建议提高到 2-4MB，避免内核 buffer 溢出丢包。
    int recvBufferBytes = 0;

    // 整体连接 / 读取超时（微秒）。底层使用 interrupt_callback 实现。
    // 这是给应用层兜底的硬超时，避开某些 FFmpeg 内部不响应 stimeout 的路径。
    int64_t openTimeoutUs = 30'000'000;   // 30s
    int64_t readTimeoutUs = 30'000'000;   // 30s

    // 起播 buffer：达到该字节数前不开始播放，避免抖动期间画面卡顿。
    // 0 表示禁用起播 buffer（连上即播）。
    int prebufferBytes = 0;

    // ---------- 鉴权 / Header ----------
    // HTTP/HTTPS/HTTP-FLV 用的 user-agent。空表示不显式设置
    QString userAgent;

    // 自定义 HTTP headers，每行一条，"Key: Value\r\n" 格式按 ffmpeg 约定
    // 例：Authorization 头、Referer
    QString extraHeaders;

    // RTSP 鉴权（如有）
    QString username;
    QString password;

    // ---------- 调试 ----------
    // 启用时，打开输入前会把 AVDictionary 内容打印到日志（不含 password）
    bool logOptions = false;
};

#endif // NETWORKOPTIONS_H
