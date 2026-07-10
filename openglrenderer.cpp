#include "openglrenderer.h"

#include <QMetaObject>
#include <QOpenGLShaderProgram>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>

namespace {

constexpr GLfloat kQuadVertices[] = {
    // position     // tex coord
    -1.0f, -1.0f,   0.0f, 1.0f,
     1.0f, -1.0f,   1.0f, 1.0f,
    -1.0f,  1.0f,   0.0f, 0.0f,
     1.0f,  1.0f,   1.0f, 0.0f,
};

int chromaSize(int value)
{
    return (value + 1) / 2;
}

void copyPlane(const uint8_t* src,
               int srcStride,
               uint8_t* dst,
               int dstStride,
               int rowBytes,
               int rows)
{
    if (!src || !dst || rowBytes <= 0 || rows <= 0) {
        return;
    }

    const uint8_t* srcRow = src;
    if (srcStride < 0) {
        srcRow = src + static_cast<ptrdiff_t>(rows - 1) * srcStride;
    }

    for (int y = 0; y < rows; ++y) {
        std::memcpy(dst + static_cast<ptrdiff_t>(y) * dstStride,
                    srcRow + static_cast<ptrdiff_t>(y) * srcStride,
                    static_cast<size_t>(rowBytes));
    }
}

bool isPlanar420Format(AVPixelFormat format)
{
    return format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P;
}

const char* vertexShaderSource()
{
    return R"(
#ifdef GL_ES
precision mediump float;
#endif
attribute vec2 position;
attribute vec2 texCoordIn;
varying vec2 texCoord;
void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
    texCoord = texCoordIn;
}
)";
}

const char* fragmentShaderSource()
{
    return R"(
#ifdef GL_ES
precision mediump float;
#endif
uniform sampler2D texY;
uniform sampler2D texU;
uniform sampler2D texV;
uniform sampler2D texUV;
uniform int frameFormat;
varying vec2 texCoord;
void main()
{
    float y = texture2D(texY, texCoord).r;
    float u = 0.0;
    float v = 0.0;

    if (frameFormat == 1) {
        vec2 uv = texture2D(texUV, texCoord).rg;
        u = uv.r - 0.5;
        v = uv.g - 0.5;
    } else {
        u = texture2D(texU, texCoord).r - 0.5;
        v = texture2D(texV, texCoord).r - 0.5;
    }

    vec3 rgb = vec3(
        y + 1.402 * v,
        y - 0.344136 * u - 0.714136 * v,
        y + 1.772 * u);
    gl_FragColor = vec4(rgb, 1.0);
}
)";
}

} // namespace

void OpenGLRenderer::FrameData::reset()
{
    width = 0;
    height = 0;
    format = UploadFormat::None;
    for (auto& plane : planes) {
        plane.clear();
    }
}

OpenGLRenderer::OpenGLRenderer(QWidget* parent)
    : QOpenGLWidget(parent),
      vertexBuffer_(QOpenGLBuffer::VertexBuffer)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
}

OpenGLRenderer::~OpenGLRenderer()
{
    if (context()) {
        makeCurrent();
        releaseGlResources();
        doneCurrent();
    }

    std::lock_guard<std::mutex> lock(convertMutex_);
    releaseSwsResources();
}

void OpenGLRenderer::renderFrame(AVFrame* frame)
{
    if (!frame) {
        return;
    }

    FrameData copied;
    if (!copyFrame(frame, copied)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        pendingFrame_ = std::move(copied);
        clearRequested_ = false;
    }

    requestUpdate();
}

void OpenGLRenderer::clear()
{
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        pendingFrame_.reset();
        clearRequested_ = true;
    }

    requestUpdate();
}

int OpenGLRenderer::preferredPixelFormat() const
{
    return AV_PIX_FMT_YUV420P;
}

void OpenGLRenderer::setBackgroundColor(const QColor& color)
{
    backgroundColor_ = color;
    requestUpdate();
}

void OpenGLRenderer::initializeGL()
{
    initializeOpenGLFunctions();

    glClearColor(backgroundColor_.redF(),
                 backgroundColor_.greenF(),
                 backgroundColor_.blueF(),
                 backgroundColor_.alphaF());

    program_ = new QOpenGLShaderProgram();
    if (!program_->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShaderSource()) ||
        !program_->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShaderSource())) {
        return;
    }

    program_->bindAttributeLocation("position", 0);
    program_->bindAttributeLocation("texCoordIn", 1);
    if (!program_->link()) {
        return;
    }

    vao_.create();
    QOpenGLVertexArrayObject::Binder vaoBinder(&vao_);

    vertexBuffer_.create();
    vertexBuffer_.bind();
    vertexBuffer_.allocate(kQuadVertices, static_cast<int>(sizeof(kQuadVertices)));

    program_->bind();
    program_->enableAttributeArray(0);
    program_->setAttributeBuffer(0, GL_FLOAT, 0, 2, 4 * static_cast<int>(sizeof(GLfloat)));
    program_->enableAttributeArray(1);
    program_->setAttributeBuffer(1,
                                 GL_FLOAT,
                                 2 * static_cast<int>(sizeof(GLfloat)),
                                 2,
                                 4 * static_cast<int>(sizeof(GLfloat)));
    program_->release();
    vertexBuffer_.release();

    initializeTextures();
}

void OpenGLRenderer::paintGL()
{
    glClearColor(backgroundColor_.redF(),
                 backgroundColor_.greenF(),
                 backgroundColor_.blueF(),
                 backgroundColor_.alphaF());
    glClear(GL_COLOR_BUFFER_BIT);

    FrameData frame;
    bool clearRequested = false;
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        clearRequested = clearRequested_;
        clearRequested_ = false;
        if (pendingFrame_.isValid()) {
            frame = std::move(pendingFrame_);
            pendingFrame_.reset();
        }
    }

    if (clearRequested) {
        textureReady_ = false;
        displayedFrameSize_ = QSize();
    }

    if (frame.isValid()) {
        uploadFrame(frame);
    }

    if (!textureReady_ || !program_ || !program_->isLinked()) {
        return;
    }

    const QRect target = viewportRectForFrame(displayedFrameSize_);
    if (target.isEmpty()) {
        return;
    }

    glViewport(target.x(), target.y(), target.width(), target.height());

    program_->bind();
    program_->setUniformValue("texY", 0);
    program_->setUniformValue("texU", 1);
    program_->setUniformValue("texV", 2);
    program_->setUniformValue("texUV", 1);
    program_->setUniformValue("frameFormat", textureFormat_ == UploadFormat::Nv12 ? 1 : 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures_[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, textures_[1]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, textures_[2]);

    QOpenGLVertexArrayObject::Binder vaoBinder(&vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindTexture(GL_TEXTURE_2D, 0);
    program_->release();

    const qreal dpr = devicePixelRatioF();
    glViewport(0, 0, static_cast<GLsizei>(width() * dpr), static_cast<GLsizei>(height() * dpr));
}

void OpenGLRenderer::resizeGL(int width, int height)
{
    const qreal dpr = devicePixelRatioF();
    glViewport(0, 0, static_cast<GLsizei>(width * dpr), static_cast<GLsizei>(height * dpr));
}

bool OpenGLRenderer::copyFrame(AVFrame* frame, FrameData& out)
{
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        return false;
    }

    const AVPixelFormat format = static_cast<AVPixelFormat>(frame->format);
    if (isPlanar420Format(format)) {
        return copyPlanar420Frame(frame, out);
    }
    if (format == AV_PIX_FMT_NV12) {
        return copyNv12Frame(frame, out);
    }

    std::lock_guard<std::mutex> lock(convertMutex_);
    return convertFrameToYuv420p(frame, out);
}

bool OpenGLRenderer::copyPlanar420Frame(AVFrame* frame, FrameData& out)
{
    if (!frame->data[0] || !frame->data[1] || !frame->data[2]) {
        return false;
    }

    const int width = frame->width;
    const int height = frame->height;
    const int chromaWidth = chromaSize(width);
    const int chromaHeight = chromaSize(height);

    out.reset();
    out.width = width;
    out.height = height;
    out.format = UploadFormat::Yuv420p;
    out.planes[0].resize(static_cast<size_t>(width) * height);
    out.planes[1].resize(static_cast<size_t>(chromaWidth) * chromaHeight);
    out.planes[2].resize(static_cast<size_t>(chromaWidth) * chromaHeight);

    copyPlane(frame->data[0], frame->linesize[0], out.planes[0].data(), width, width, height);
    copyPlane(frame->data[1], frame->linesize[1], out.planes[1].data(), chromaWidth, chromaWidth, chromaHeight);
    copyPlane(frame->data[2], frame->linesize[2], out.planes[2].data(), chromaWidth, chromaWidth, chromaHeight);
    return true;
}

bool OpenGLRenderer::copyNv12Frame(AVFrame* frame, FrameData& out)
{
    if (!frame->data[0] || !frame->data[1]) {
        return false;
    }

    const int width = frame->width;
    const int height = frame->height;
    const int chromaWidth = chromaSize(width);
    const int chromaHeight = chromaSize(height);
    const int uvRowBytes = chromaWidth * 2;

    out.reset();
    out.width = width;
    out.height = height;
    out.format = UploadFormat::Nv12;
    out.planes[0].resize(static_cast<size_t>(width) * height);
    out.planes[1].resize(static_cast<size_t>(uvRowBytes) * chromaHeight);

    copyPlane(frame->data[0], frame->linesize[0], out.planes[0].data(), width, width, height);
    copyPlane(frame->data[1], frame->linesize[1], out.planes[1].data(), uvRowBytes, uvRowBytes, chromaHeight);
    return true;
}

bool OpenGLRenderer::convertFrameToYuv420p(AVFrame* frame, FrameData& out)
{
    const AVPixelFormat format = static_cast<AVPixelFormat>(frame->format);
    if (format == AV_PIX_FMT_NONE) {
        return false;
    }

    const bool sourceChanged =
        !swsCtx_ ||
        swsSrcWidth_ != frame->width ||
        swsSrcHeight_ != frame->height ||
        swsSrcFormat_ != format;

    if (sourceChanged) {
        releaseSwsResources();
        swsCtx_ = sws_getContext(frame->width,
                                 frame->height,
                                 format,
                                 frame->width,
                                 frame->height,
                                 AV_PIX_FMT_YUV420P,
                                 SWS_BILINEAR,
                                 nullptr,
                                 nullptr,
                                 nullptr);
        if (!swsCtx_) {
            return false;
        }
        swsSrcWidth_ = frame->width;
        swsSrcHeight_ = frame->height;
        swsSrcFormat_ = format;
    }

    const int width = frame->width;
    const int height = frame->height;
    const int chromaWidth = chromaSize(width);
    const int chromaHeight = chromaSize(height);

    out.reset();
    out.width = width;
    out.height = height;
    out.format = UploadFormat::Yuv420p;
    out.planes[0].resize(static_cast<size_t>(width) * height);
    out.planes[1].resize(static_cast<size_t>(chromaWidth) * chromaHeight);
    out.planes[2].resize(static_cast<size_t>(chromaWidth) * chromaHeight);

    uint8_t* dstData[4] = {
        out.planes[0].data(),
        out.planes[1].data(),
        out.planes[2].data(),
        nullptr,
    };
    int dstLinesize[4] = {width, chromaWidth, chromaWidth, 0};

    const int converted = sws_scale(swsCtx_,
                                    frame->data,
                                    frame->linesize,
                                    0,
                                    frame->height,
                                    dstData,
                                    dstLinesize);
    if (converted != frame->height) {
        out.reset();
        return false;
    }

    return true;
}

void OpenGLRenderer::requestUpdate()
{
    if (QThread::currentThread() == thread()) {
        update();
        return;
    }

    if (!updatePending_.exchange(true)) {
        QMetaObject::invokeMethod(
            this,
            [this] {
                update();
                updatePending_.store(false);
            },
            Qt::QueuedConnection);
    }
}

void OpenGLRenderer::uploadFrame(const FrameData& frame)
{
    ensureTextureStorage(frame);
    if (!texturesInitialized_) {
        return;
    }

    const int chromaWidth = chromaSize(frame.width);
    const int chromaHeight = chromaSize(frame.height);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures_[0]);
    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    0,
                    0,
                    frame.width,
                    frame.height,
                    GL_RED,
                    GL_UNSIGNED_BYTE,
                    frame.planes[0].data());

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, textures_[1]);
    if (frame.format == UploadFormat::Nv12) {
        glTexSubImage2D(GL_TEXTURE_2D,
                        0,
                        0,
                        0,
                        chromaWidth,
                        chromaHeight,
                        GL_RG,
                        GL_UNSIGNED_BYTE,
                        frame.planes[1].data());
    } else {
        glTexSubImage2D(GL_TEXTURE_2D,
                        0,
                        0,
                        0,
                        chromaWidth,
                        chromaHeight,
                        GL_RED,
                        GL_UNSIGNED_BYTE,
                        frame.planes[1].data());
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, textures_[2]);
        glTexSubImage2D(GL_TEXTURE_2D,
                        0,
                        0,
                        0,
                        chromaWidth,
                        chromaHeight,
                        GL_RED,
                        GL_UNSIGNED_BYTE,
                        frame.planes[2].data());
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    textureReady_ = true;
    displayedFrameSize_ = QSize(frame.width, frame.height);
}

void OpenGLRenderer::ensureTextureStorage(const FrameData& frame)
{
    if (!texturesInitialized_) {
        initializeTextures();
    }
    if (!texturesInitialized_) {
        return;
    }

    if (textureWidth_ == frame.width &&
        textureHeight_ == frame.height &&
        textureFormat_ == frame.format) {
        return;
    }

    const int chromaWidth = chromaSize(frame.width);
    const int chromaHeight = chromaSize(frame.height);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures_[0]);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_R8,
                 frame.width,
                 frame.height,
                 0,
                 GL_RED,
                 GL_UNSIGNED_BYTE,
                 nullptr);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, textures_[1]);
    if (frame.format == UploadFormat::Nv12) {
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RG8,
                     chromaWidth,
                     chromaHeight,
                     0,
                     GL_RG,
                     GL_UNSIGNED_BYTE,
                     nullptr);
    } else {
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_R8,
                     chromaWidth,
                     chromaHeight,
                     0,
                     GL_RED,
                     GL_UNSIGNED_BYTE,
                     nullptr);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, textures_[2]);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_R8,
                     chromaWidth,
                     chromaHeight,
                     0,
                     GL_RED,
                     GL_UNSIGNED_BYTE,
                     nullptr);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    textureWidth_ = frame.width;
    textureHeight_ = frame.height;
    textureFormat_ = frame.format;
}

void OpenGLRenderer::initializeTextures()
{
    if (texturesInitialized_) {
        return;
    }

    glGenTextures(3, textures_);
    for (GLuint texture : textures_) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    texturesInitialized_ = true;
}

void OpenGLRenderer::releaseGlResources()
{
    if (texturesInitialized_) {
        glDeleteTextures(3, textures_);
        textures_[0] = textures_[1] = textures_[2] = 0;
        texturesInitialized_ = false;
    }

    textureReady_ = false;
    textureWidth_ = 0;
    textureHeight_ = 0;
    textureFormat_ = UploadFormat::None;
    displayedFrameSize_ = QSize();

    vao_.destroy();
    vertexBuffer_.destroy();

    delete program_;
    program_ = nullptr;
}

void OpenGLRenderer::releaseSwsResources()
{
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
    swsSrcWidth_ = 0;
    swsSrcHeight_ = 0;
    swsSrcFormat_ = AV_PIX_FMT_NONE;
}

QRect OpenGLRenderer::viewportRectForFrame(const QSize& frameSize) const
{
    if (frameSize.isEmpty() || size().isEmpty()) {
        return QRect();
    }

    const qreal dpr = devicePixelRatioF();
    const int widgetWidth = std::max(1, static_cast<int>(std::lround(width() * dpr)));
    const int widgetHeight = std::max(1, static_cast<int>(std::lround(height() * dpr)));

    const QSize scaled = frameSize.scaled(QSize(widgetWidth, widgetHeight), Qt::KeepAspectRatio);
    return QRect((widgetWidth - scaled.width()) / 2,
                 (widgetHeight - scaled.height()) / 2,
                 scaled.width(),
                 scaled.height());
}
