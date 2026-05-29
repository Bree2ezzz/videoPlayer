#ifndef SOFTWARERENDERER_H
#define SOFTWARERENDERER_H

#include "videorendererbase.h"

#include <QColor>
#include <QImage>
#include <QRect>
#include <QSize>
#include <QWidget>

#include <atomic>
#include <mutex>

extern "C" {
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
}

/*
 * SoftwareRenderer：基于 CPU 的软件渲染实现。
 *
 * 工作流程：
 *   renderFrame(AVFrame*)   任意线程调用
 *     ├─ sws_scale 将 YUV/其他 → RGBA8888（此步在调用线程执行，避开 GUI 线程）
 *     ├─ 构造成 QImage 并用 currentImage_ 替换（持 imageMutex_）
 *     └─ 通过 QMetaObject::invokeMethod 投递到 GUI 线程调 update()
 *
 *   paintEvent()              GUI 线程
 *     ├─ 拷贝出 currentImage_ 的引用（持锁，共享数据，QImage 是 COW）
 *     └─ 按保持宽高比的矩形 drawImage 到 widget
 *
 * 设计说明：
 *   - 转换放在调用线程（通常是 RenderScheduler 独立线程），GUI 线程只做
 *     drawImage，不阻塞事件循环
 *   - 保留 SwsContext 复用，源分辨率/像素格式变化时惰性重建
 *   - 源格式固定转成 AV_PIX_FMT_RGBA（QImage::Format_RGBA8888）以避免字节序问题
 *     （若后续想用 Format_RGB32，需要在大小端机器上分别处理 BGRA/ARGB）
 */

class SoftwareRenderer : public QWidget, public VideoRendererBase
{
    Q_OBJECT
public:
    explicit SoftwareRenderer(QWidget* parent = nullptr);
    ~SoftwareRenderer() override;

    // ---------- VideoRendererBase ----------
    void renderFrame(AVFrame* frame) override;
    void clear() override;
    QWidget* asWidget() override { return this; }
    int preferredPixelFormat() const override; // 返回 AV_PIX_FMT_YUV420P

    // 设置画面背景色（未覆盖区域，默认黑）
    void setBackgroundColor(const QColor& color);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    // 将 frame 转换为 QImage，返回空 QImage 表示失败
    QImage convertFrameToImage(AVFrame* frame);

    // 根据当前 widget 尺寸和图像尺寸，计算保持宽高比的目标矩形
    QRect computeTargetRect(const QSize& imageSize) const;

    // 释放 sws 上下文及缓存
    void releaseSwsResources();

private:
    // ---------- sws 缓存 ----------
    SwsContext* swsCtx_ = nullptr;
    int srcWidth_ = 0;
    int srcHeight_ = 0;
    AVPixelFormat srcPixFmt_ = AV_PIX_FMT_NONE;
    // ---------- 当前显示帧 ----------
    mutable std::mutex imageMutex_;
    QImage currentImage_;              // 已被转换好的 QImage，paintEvent 直接用
    QColor backgroundColor_{Qt::black};

    // 防止 renderFrame 被多线程并发调用时同时写 sws（SwsContext 非线程安全）。
    // 与 imageMutex_ 拆开是因为 paintEvent 只需要 imageMutex_，不想被长时间的
    // sws_scale 阻塞
    std::mutex convertMutex_;

    // update() 合流：多个 renderFrame 之间如果 GUI 线程还没响应上一次 update，
    // 不必重复投递。原子标志位避免 invokeMethod 调用堆积
    std::atomic_bool updatePending_{false};
};

#endif // SOFTWARERENDERER_H
