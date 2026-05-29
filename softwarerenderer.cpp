#include "softwarerenderer.h"

#include <QMetaObject>
#include <QPainter>
#include <QPaintEvent>
#include <QThread>

#include <limits>
#include <utility>

namespace {

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
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
}

SoftwareRenderer::~SoftwareRenderer()
{
    std::lock_guard<std::mutex> lock(convertMutex_);
    releaseSwsResources();
}

void SoftwareRenderer::renderFrame(AVFrame* frame)
{
    if (!frame) {
        return;
    }

    {
        std::lock_guard<std::mutex> convertLock(convertMutex_);
        QImage image = convertFrameToImage(frame);
        if (image.isNull()) {
            return;
        }

        std::lock_guard<std::mutex> imageLock(imageMutex_);
        currentImage_ = std::move(image);
    }

    requestQueuedUpdate(this, updatePending_);
}

void SoftwareRenderer::clear()
{
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
        return;
    }

    painter.drawImage(computeTargetRect(image.size()), image);
}

QImage SoftwareRenderer::convertFrameToImage(AVFrame* frame)
{
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        return QImage();
    }

    const AVPixelFormat framePixFmt = static_cast<AVPixelFormat>(frame->format);
    if (framePixFmt == AV_PIX_FMT_NONE) {
        return QImage();
    }

    const bool sourceChanged =
        !swsCtx_ ||
        srcWidth_ != frame->width ||
        srcHeight_ != frame->height ||
        srcPixFmt_ != framePixFmt;

    if (sourceChanged) {
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
            return QImage();
        }

        srcWidth_ = frame->width;
        srcHeight_ = frame->height;
        srcPixFmt_ = framePixFmt;

    }

    QImage image(frame->width, frame->height, QImage::Format_RGBA8888);
    if (image.isNull()) {
        return QImage();
    }

    const auto bytesPerLine = image.bytesPerLine();
    if (bytesPerLine <= 0 ||
        bytesPerLine > std::numeric_limits<int>::max()) {
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
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }

    srcWidth_ = 0;
    srcHeight_ = 0;
    srcPixFmt_ = AV_PIX_FMT_NONE;
}
