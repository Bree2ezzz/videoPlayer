#include "softwarerenderer.h"

#include "app_logger.h"

#include <QMetaObject>
#include <QPainter>
#include <QPaintEvent>
#include <QThread>

extern "C" {
#include <libavutil/pixdesc.h>
}

#include <limits>
#include <utility>

namespace {

const char* pixelFormatName(AVPixelFormat format)
{
    const char* name = av_get_pix_fmt_name(format);
    return name ? name : "unknown";
}
void requestQueuedUpdate(QWidget* widget, std::atomic_bool& updatePending)
{
    if (!widget) {
        return;
    }

    if (QThread::currentThread() == widget->thread()) {
        widget->update();
        return;
    }

    if (!updatePending.exchange(true)) {
        QMetaObject::invokeMethod(
            widget,
            [widget, &updatePending] {
                widget->update();
                updatePending.store(false);
            },
            Qt::QueuedConnection);
    }
}

} // namespace

SoftwareRenderer::SoftwareRenderer(QWidget* parent)
    : QWidget(parent)
{
    VP_INFO("SoftwareRenderer created this={} parent={}", static_cast<const void*>(this), static_cast<const void*>(parent));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
}

SoftwareRenderer::~SoftwareRenderer()
{
    VP_INFO("SoftwareRenderer destroyed this={}", static_cast<const void*>(this));
    std::lock_guard<std::mutex> lock(convertMutex_);
    releaseSwsResources();
}

void SoftwareRenderer::renderFrame(AVFrame* frame)
{
    if (!frame) {
        VP_WARN("SoftwareRenderer::renderFrame ignored null frame this={}", static_cast<const void*>(this));
        return;
    }

    VP_DEBUG("SoftwareRenderer::renderFrame this={} frame={} size={}x{} format={} pts={} best_effort={} gui_thread={} widget_size={}x{}",
             static_cast<const void*>(this),
             static_cast<const void*>(frame),
             frame->width,
             frame->height,
             pixelFormatName(static_cast<AVPixelFormat>(frame->format)),
             frame->pts,
             frame->best_effort_timestamp,
             QThread::currentThread() == thread(),
             width(),
             height());

    {
        std::lock_guard<std::mutex> convertLock(convertMutex_);
        QImage image = convertFrameToImage(frame);
        if (image.isNull()) {
            VP_WARN("SoftwareRenderer::renderFrame conversion returned null image this={} frame={} format={} size={}x{}",
                    static_cast<const void*>(this),
                    static_cast<const void*>(frame),
                    pixelFormatName(static_cast<AVPixelFormat>(frame->format)),
                    frame->width,
                    frame->height);
            return;
        }

        VP_DEBUG("SoftwareRenderer converted frame this={} image={}x{} bytes_per_line={}",
                 static_cast<const void*>(this), image.width(), image.height(), image.bytesPerLine());
        std::lock_guard<std::mutex> imageLock(imageMutex_);
        currentImage_ = std::move(image);
    }

    requestQueuedUpdate(this, updatePending_);
    VP_DEBUG("SoftwareRenderer requested update this={} pending={}", static_cast<const void*>(this), updatePending_.load());
}

void SoftwareRenderer::clear()
{
    VP_INFO("SoftwareRenderer::clear this={}", static_cast<const void*>(this));
    {
        std::lock_guard<std::mutex> lock(imageMutex_);
        currentImage_ = QImage();
    }

    requestQueuedUpdate(this, updatePending_);
}

int SoftwareRenderer::preferredPixelFormat() const
{
    return AV_PIX_FMT_YUV420P;
}

void SoftwareRenderer::setBackgroundColor(const QColor& color)
{
    {
        std::lock_guard<std::mutex> lock(imageMutex_);
        backgroundColor_ = color;
    }

    requestQueuedUpdate(this, updatePending_);
}

void SoftwareRenderer::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QImage image;
    QColor backgroundColor;
    {
        std::lock_guard<std::mutex> lock(imageMutex_);
        image = currentImage_;
        backgroundColor = backgroundColor_;
    }

    QPainter painter(this);
    painter.fillRect(rect(), backgroundColor);

    if (image.isNull()) {
        VP_DEBUG("SoftwareRenderer::paintEvent this={} widget={}x{} image=null",
                 static_cast<const void*>(this), width(), height());
        return;
    }

    const QRect target = computeTargetRect(image.size());
    VP_DEBUG("SoftwareRenderer::paintEvent this={} widget={}x{} image={}x{} target=({},{} {}x{})",
             static_cast<const void*>(this),
             width(),
             height(),
             image.width(),
             image.height(),
             target.x(),
             target.y(),
             target.width(),
             target.height());
    painter.drawImage(target, image);
}

QImage SoftwareRenderer::convertFrameToImage(AVFrame* frame)
{
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        VP_WARN("SoftwareRenderer::convertFrameToImage invalid frame this={} frame={} size={}x{}",
                static_cast<const void*>(this),
                static_cast<const void*>(frame),
                frame ? frame->width : -1,
                frame ? frame->height : -1);
        return QImage();
    }

    const AVPixelFormat framePixFmt = static_cast<AVPixelFormat>(frame->format);
    if (framePixFmt == AV_PIX_FMT_NONE) {
        VP_WARN("SoftwareRenderer::convertFrameToImage frame has AV_PIX_FMT_NONE this={} frame={}",
                static_cast<const void*>(this), static_cast<const void*>(frame));
        return QImage();
    }

    const bool sourceChanged =
        !swsCtx_ ||
        srcWidth_ != frame->width ||
        srcHeight_ != frame->height ||
        srcPixFmt_ != framePixFmt;

    if (sourceChanged) {
        VP_INFO("SoftwareRenderer rebuilding sws this={} source={}x{} format={} old={}x{} old_format={}",
                static_cast<const void*>(this),
                frame->width,
                frame->height,
                pixelFormatName(framePixFmt),
                srcWidth_,
                srcHeight_,
                pixelFormatName(srcPixFmt_));
        releaseSwsResources();

        swsCtx_ = sws_getContext(frame->width,
                                 frame->height,
                                 framePixFmt,
                                 frame->width,
                                 frame->height,
                                 AV_PIX_FMT_RGBA,
                                 SWS_BILINEAR,
                                 nullptr,
                                 nullptr,
                                 nullptr);
        if (!swsCtx_) {
            VP_ERROR("SoftwareRenderer sws_getContext failed this={} source={}x{} format={}",
                     static_cast<const void*>(this),
                     frame->width,
                     frame->height,
                     pixelFormatName(framePixFmt));
            return QImage();
        }

        srcWidth_ = frame->width;
        srcHeight_ = frame->height;
        srcPixFmt_ = framePixFmt;

    }

    QImage image(frame->width, frame->height, QImage::Format_RGBA8888);
    if (image.isNull()) {
        VP_ERROR("SoftwareRenderer QImage allocation failed this={} size={}x{}",
                 static_cast<const void*>(this), frame->width, frame->height);
        return QImage();
    }

    const auto bytesPerLine = image.bytesPerLine();
    if (bytesPerLine <= 0 ||
        bytesPerLine > std::numeric_limits<int>::max()) {
        VP_ERROR("SoftwareRenderer invalid QImage bytesPerLine this={} bytes_per_line={}",
                 static_cast<const void*>(this), bytesPerLine);
        return QImage();
    }

    uint8_t* dstData[4] = {image.bits(), nullptr, nullptr, nullptr};
    int dstLineSizes[4] = {static_cast<int>(bytesPerLine), 0, 0, 0};

    const int scaledHeight = sws_scale(swsCtx_,
                                       frame->data,
                                       frame->linesize,
                                       0,
                                       frame->height,
                                       dstData,
                                       dstLineSizes);
    if (scaledHeight != frame->height) {
        VP_ERROR("SoftwareRenderer sws_scale returned unexpected height this={} converted={} expected={}",
                 static_cast<const void*>(this), scaledHeight, frame->height);
        return QImage();
    }

    return image;
}

QRect SoftwareRenderer::computeTargetRect(const QSize& imageSize) const
{
    if (imageSize.isEmpty() || size().isEmpty()) {
        return QRect();
    }

    const QSize scaled = imageSize.scaled(size(), Qt::KeepAspectRatio);
    return QRect((width() - scaled.width()) / 2,
                 (height() - scaled.height()) / 2,
                 scaled.width(),
                 scaled.height());
}

void SoftwareRenderer::releaseSwsResources()
{
    if (swsCtx_) {
        VP_DEBUG("SoftwareRenderer releasing sws this={} source={}x{} format={}",
                 static_cast<const void*>(this), srcWidth_, srcHeight_, pixelFormatName(srcPixFmt_));
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }

    srcWidth_ = 0;
    srcHeight_ = 0;
    srcPixFmt_ = AV_PIX_FMT_NONE;
}
