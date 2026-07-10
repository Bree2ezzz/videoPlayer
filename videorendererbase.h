#ifndef VIDEORENDERERBASE_H
#define VIDEORENDERERBASE_H

extern "C" {
extern "C" {
#include <libavutil/frame.h>
}
}

class QWidget;

/*
 * VideoRendererBase 是渲染层的抽象接口。
 *
 * 设计动机：
 *   支持 SoftwareRenderer（QImage / CPU）和 D3D11Renderer（DXGI / GPU）。
 *   实现不需要继承同一个 Qt 基类，因此通过 asWidget() 暴露实际承载画面的
 *   QWidget。
 *
 * 职责边界：
 *   - 接收 AVFrame 并显示（可能被任意线程调用，实现必须线程安全）
 *   - 不负责解码、格式协商；传入的 frame 若不是期望的像素格式，由实现
 *     内部用 sws_scale 转换
 *   - 不负责显示时机调度，这由 RenderScheduler 决定；渲染器只管"现在
 *     拿到这一帧，尽快显示出去"
 *   - 不持有 FrameQueue
 */

class VideoRendererBase
{
public:
    virtual ~VideoRendererBase() = default;

    // 接收一帧并显示。可以在任意线程调用。
    // 实现应当快速返回：拷贝/转换后触发异步刷新，不阻塞调用线程。
    // 传入的 frame 由调用方持有，返回后调用方可以立即 av_frame_free/unref；
    // 实现需要长期持有时自己做 av_frame_ref 或数据拷贝。
    virtual void renderFrame(AVFrame* frame) = 0;

    // 清空当前显示内容，用于 seek / 切流时避免残留画面。
    virtual void clear() = 0;

    // 暴露底层 Qt widget，供 MainWindow / 布局使用。
    // 子类返回 this（向下转型到 QWidget*）。
    virtual QWidget* asWidget() = 0;

    // 返回当前渲染器能直接处理的像素格式首选项；
    // Software profile 返回 YUV420P；D3D11 profile 返回 AV_PIX_FMT_D3D11。
    virtual int preferredPixelFormat() const = 0;
};

#endif // VIDEORENDERERBASE_H
