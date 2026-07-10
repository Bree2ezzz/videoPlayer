#ifndef OPENGLRENDERER_H
#define OPENGLRENDERER_H

#include "videorendererbase.h"

#include <QColor>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QRect>
#include <QSize>

#include <array>
#include <atomic>
#include <mutex>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

class QOpenGLShaderProgram;

class OpenGLRenderer : public QOpenGLWidget, protected QOpenGLFunctions, public VideoRendererBase
{
    Q_OBJECT
public:
    explicit OpenGLRenderer(QWidget* parent = nullptr);
    ~OpenGLRenderer() override;

    void renderFrame(AVFrame* frame) override;
    void clear() override;
    QWidget* asWidget() override { return this; }
    int preferredPixelFormat() const override;

    void setBackgroundColor(const QColor& color);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;

private:
    enum class UploadFormat {
        None,
        Yuv420p,
        Nv12,
    };

    struct FrameData {
        int width = 0;
        int height = 0;
        UploadFormat format = UploadFormat::None;
        std::array<std::vector<uint8_t>, 3> planes;

        bool isValid() const { return width > 0 && height > 0 && format != UploadFormat::None; }
        void reset();
    };

    bool copyFrame(AVFrame* frame, FrameData& out);
    bool copyPlanar420Frame(AVFrame* frame, FrameData& out);
    bool copyNv12Frame(AVFrame* frame, FrameData& out);
    bool convertFrameToYuv420p(AVFrame* frame, FrameData& out);

    void requestUpdate();
    void uploadFrame(const FrameData& frame);
    void ensureTextureStorage(const FrameData& frame);
    void initializeTextures();
    void releaseGlResources();
    void releaseSwsResources();
    QRect viewportRectForFrame(const QSize& frameSize) const;

    mutable std::mutex frameMutex_;
    FrameData pendingFrame_;
    bool clearRequested_ = false;

    std::mutex convertMutex_;
    SwsContext* swsCtx_ = nullptr;
    int swsSrcWidth_ = 0;
    int swsSrcHeight_ = 0;
    AVPixelFormat swsSrcFormat_ = AV_PIX_FMT_NONE;

    QOpenGLShaderProgram* program_ = nullptr;
    QOpenGLBuffer vertexBuffer_;
    QOpenGLVertexArrayObject vao_;
    GLuint textures_[3] = {0, 0, 0};
    bool texturesInitialized_ = false;
    bool textureReady_ = false;
    int textureWidth_ = 0;
    int textureHeight_ = 0;
    UploadFormat textureFormat_ = UploadFormat::None;
    QSize displayedFrameSize_;

    QColor backgroundColor_{Qt::black};
    std::atomic_bool updatePending_{false};
};

#endif // OPENGLRENDERER_H
