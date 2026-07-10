#include "openglrenderer.h"

#include "app_logger.h"

#include <QMetaObject>
#include <QOpenGLShaderProgram>

extern "C" {
#include <libavutil/pixdesc.h>
}
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>

namespace {

const char* pixelFormatName(AVPixelFormat format)
{
    const char* name = av_get_pix_fmt_name(format);
    return name ? name : "unknown";
}

const char* uploadFormatName(int format)
{
    switch (format) {
    case 0:
        return "None";
    case 1:
        return "Yuv420p";
    case 2:
        return "Nv12";
    }
    return "Unknown";
}
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
    VP_INFO("OpenGLRenderer created this={} parent={}", static_cast<const void*>(this), static_cast<const void*>(parent));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
}

OpenGLRenderer::~OpenGLRenderer()
{
    VP_INFO("OpenGLRenderer destroyed this={} context={}", static_cast<const void*>(this), static_cast<const void*>(context()));
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
        VP_WARN("OpenGLRenderer::renderFrame ignored null frame this={}", static_cast<const void*>(this));
        return;
    }

    VP_DEBUG("OpenGLRenderer::renderFrame this={} frame={} size={}x{} format={} pts={} best_effort={} gui_thread={} widget_size={}x{}",
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

    FrameData copied;
    if (!copyFrame(frame, copied)) {
        VP_WARN("OpenGLRenderer::renderFrame copyFrame failed this={} frame={} size={}x{} format={}",
                static_cast<const void*>(this),
                static_cast<const void*>(frame),
                frame->width,
                frame->height,
                pixelFormatName(static_cast<AVPixelFormat>(frame->format)));
        return;
    }

    VP_DEBUG("OpenGLRenderer copied frame this={} upload_format={} size={}x{} planes=({}, {}, {})",
             static_cast<const void*>(this),
             uploadFormatName(static_cast<int>(copied.format)),
             copied.width,
             copied.height,
             copied.planes[0].size(),
             copied.planes[1].size(),
             copied.planes[2].size());

    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        pendingFrame_ = std::move(copied);
        clearRequested_ = false;
    }

    requestUpdate();
    VP_DEBUG("OpenGLRenderer requested update this={} pending={}", static_cast<const void*>(this), updatePending_.load());
}

void OpenGLRenderer::clear()
{
    VP_INFO("OpenGLRenderer::clear this={}", static_cast<const void*>(this));
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
    VP_INFO("OpenGLRenderer::initializeGL this={} widget={}x{} dpr={} context={}",
            static_cast<const void*>(this),
            width(),
            height(),
            devicePixelRatioF(),
            static_cast<const void*>(context()));
    initializeOpenGLFunctions();

    glClearColor(backgroundColor_.redF(),
                 backgroundColor_.greenF(),
                 backgroundColor_.blueF(),
                 backgroundColor_.alphaF());

    program_ = new QOpenGLShaderProgram();
    if (!program_->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShaderSource()) ||
        !program_->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShaderSource())) {
        VP_ERROR("OpenGLRenderer shader compile failed this={} log={}",
                 static_cast<const void*>(this), program_->log().toStdString());
        return;
    }

    program_->bindAttributeLocation("position", 0);
    program_->bindAttributeLocation("texCoordIn", 1);
    if (!program_->link()) {
        VP_ERROR("OpenGLRenderer shader link failed this={} log={}",
                 static_cast<const void*>(this), program_->log().toStdString());
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
    VP_INFO("OpenGLRenderer::initializeGL done this={} textures_initialized={}",
            static_cast<const void*>(this), texturesInitialized_);
}

void OpenGLRenderer::paintGL()
{
    VP_DEBUG("OpenGLRenderer::paintGL begin this={} widget={}x{} dpr={} texture_ready={} program={}",
             static_cast<const void*>(this),
             width(),
             height(),
             devicePixelRatioF(),
             textureReady_,
             static_cast<const void*>(program_));
    glClearColor(backgroundColor_.redF(),
                 backgroundColor_.greenF(),
                 backgroundColor_.blueF(),
                 backgroundColor_.alphaF());
    glClear(GL_COLOR_BUFFER_BIT);

    FrameData frame;
    bool clearRequested = false;
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        const bool hadPendingFrame = pendingFrame_.isValid();
        clearRequested = clearRequested_;
        VP_DEBUG("OpenGLRenderer::paintGL locked state this={} pending_valid={} clear_requested={}",
                 static_cast<const void*>(this), hadPendingFrame, clearRequested);
        clearRequested_ = false;
        if (pendingFrame_.isValid()) {
            frame = std::move(pendingFrame_);
            pendingFrame_.reset();
        }
    }

    if (clearRequested) {
        VP_INFO("OpenGLRenderer::paintGL applying clear this={}", static_cast<const void*>(this));
        textureReady_ = false;
        displayedFrameSize_ = QSize();
    }

    if (frame.isValid()) {
        VP_DEBUG("OpenGLRenderer::paintGL uploading pending frame this={} format={} size={}x{}",
                 static_cast<const void*>(this), uploadFormatName(static_cast<int>(frame.format)), frame.width, frame.height);
        uploadFrame(frame);
    }

    if (!textureReady_ || !program_ || !program_->isLinked()) {
        VP_DEBUG("OpenGLRenderer::paintGL skip draw this={} texture_ready={} program={} linked={}",
                 static_cast<const void*>(this),
                 textureReady_,
                 static_cast<const void*>(program_),
                 program_ ? program_->isLinked() : false);
        return;
    }

    const QRect target = viewportRectForFrame(displayedFrameSize_);
    if (target.isEmpty()) {
        VP_WARN("OpenGLRenderer::paintGL empty viewport target this={} frame={}x{} widget={}x{}",
                static_cast<const void*>(this),
                displayedFrameSize_.width(),
                displayedFrameSize_.height(),
                width(),
                height());
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
    const GLenum drawError = glGetError();
    VP_DEBUG("OpenGLRenderer::paintGL draw this={} target=({},{} {}x{}) frame={}x{} upload_format={} gl_error={}",
             static_cast<const void*>(this),
             target.x(),
             target.y(),
             target.width(),
             target.height(),
             displayedFrameSize_.width(),
             displayedFrameSize_.height(),
             uploadFormatName(static_cast<int>(textureFormat_)),
             static_cast<unsigned int>(drawError));

    glBindTexture(GL_TEXTURE_2D, 0);
    program_->release();

    const qreal dpr = devicePixelRatioF();
    glViewport(0, 0, static_cast<GLsizei>(width() * dpr), static_cast<GLsizei>(height() * dpr));
}

void OpenGLRenderer::resizeGL(int width, int height)
{
    VP_INFO("OpenGLRenderer::resizeGL this={} size={}x{} dpr={}",
            static_cast<const void*>(this), width, height, devicePixelRatioF());
    const qreal dpr = devicePixelRatioF();
    glViewport(0, 0, static_cast<GLsizei>(width * dpr), static_cast<GLsizei>(height * dpr));
}

bool OpenGLRenderer::copyFrame(AVFrame* frame, FrameData& out)
{
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        VP_WARN("OpenGLRenderer::copyFrame invalid frame this={} frame={} size={}x{}",
                static_cast<const void*>(this),
                static_cast<const void*>(frame),
                frame ? frame->width : -1,
                frame ? frame->height : -1);
        return false;
    }

    const AVPixelFormat format = static_cast<AVPixelFormat>(frame->format);
    if (isPlanar420Format(format)) {
        VP_DEBUG("OpenGLRenderer::copyFrame direct planar420 this={} format={} size={}x{}",
                 static_cast<const void*>(this), pixelFormatName(format), frame->width, frame->height);
        return copyPlanar420Frame(frame, out);
    }
    if (format == AV_PIX_FMT_NV12) {
        VP_DEBUG("OpenGLRenderer::copyFrame direct NV12 this={} size={}x{}",
                 static_cast<const void*>(this), frame->width, frame->height);
        return copyNv12Frame(frame, out);
    }

    VP_INFO("OpenGLRenderer::copyFrame converting unsupported source format this={} format={} size={}x{}",
            static_cast<const void*>(this), pixelFormatName(format), frame->width, frame->height);
    std::lock_guard<std::mutex> lock(convertMutex_);
    return convertFrameToYuv420p(frame, out);
}

bool OpenGLRenderer::copyPlanar420Frame(AVFrame* frame, FrameData& out)
{
    if (!frame->data[0] || !frame->data[1] || !frame->data[2]) {
        VP_WARN("OpenGLRenderer::copyPlanar420Frame missing planes this={} data=({}, {}, {})",
                static_cast<const void*>(this),
                static_cast<const void*>(frame->data[0]),
                static_cast<const void*>(frame->data[1]),
                static_cast<const void*>(frame->data[2]));
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
        VP_WARN("OpenGLRenderer::copyNv12Frame missing planes this={} data=({}, {})",
                static_cast<const void*>(this),
                static_cast<const void*>(frame->data[0]),
                static_cast<const void*>(frame->data[1]));
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
        VP_WARN("OpenGLRenderer::convertFrameToYuv420p frame has AV_PIX_FMT_NONE this={} frame={}",
                static_cast<const void*>(this), static_cast<const void*>(frame));
        return false;
    }

    const bool sourceChanged =
        !swsCtx_ ||
        swsSrcWidth_ != frame->width ||
        swsSrcHeight_ != frame->height ||
        swsSrcFormat_ != format;

    if (sourceChanged) {
        VP_INFO("OpenGLRenderer rebuilding sws this={} source={}x{} format={} old={}x{} old_format={}",
                static_cast<const void*>(this),
                frame->width,
                frame->height,
                pixelFormatName(format),
                swsSrcWidth_,
                swsSrcHeight_,
                pixelFormatName(swsSrcFormat_));
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
            VP_ERROR("OpenGLRenderer sws_getContext failed this={} source={}x{} format={}",
                     static_cast<const void*>(this),
                     frame->width,
                     frame->height,
                     pixelFormatName(format));
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
        VP_ERROR("OpenGLRenderer sws_scale returned unexpected height this={} converted={} expected={}",
                 static_cast<const void*>(this), converted, frame->height);
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
    VP_DEBUG("OpenGLRenderer::uploadFrame this={} format={} size={}x{}",
             static_cast<const void*>(this), uploadFormatName(static_cast<int>(frame.format)), frame.width, frame.height);
    ensureTextureStorage(frame);
    if (!texturesInitialized_) {
        VP_ERROR("OpenGLRenderer textures are not initialized this={}",
                 static_cast<const void*>(this));
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
    VP_DEBUG("OpenGLRenderer::uploadFrame done this={} texture_ready={} displayed={}x{}",
             static_cast<const void*>(this), textureReady_, displayedFrameSize_.width(), displayedFrameSize_.height());
}

void OpenGLRenderer::ensureTextureStorage(const FrameData& frame)
{
    if (!texturesInitialized_) {
        initializeTextures();
    }
    if (!texturesInitialized_) {
        VP_ERROR("OpenGLRenderer textures are not initialized this={}",
                 static_cast<const void*>(this));
        return;
    }

    if (textureWidth_ == frame.width &&
        textureHeight_ == frame.height &&
        textureFormat_ == frame.format) {
        return;
    }

    VP_INFO("OpenGLRenderer allocating texture storage this={} old={}x{} old_format={} new={}x{} new_format={}",
            static_cast<const void*>(this),
            textureWidth_,
            textureHeight_,
            uploadFormatName(static_cast<int>(textureFormat_)),
            frame.width,
            frame.height,
            uploadFormatName(static_cast<int>(frame.format)));

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
    VP_INFO("OpenGLRenderer textures initialized this={} ids=({}, {}, {})",
            static_cast<const void*>(this),
            static_cast<unsigned int>(textures_[0]),
            static_cast<unsigned int>(textures_[1]),
            static_cast<unsigned int>(textures_[2]));
}

void OpenGLRenderer::releaseGlResources()
{
    if (texturesInitialized_) {
        VP_INFO("OpenGLRenderer deleting textures this={} ids=({}, {}, {})",
                static_cast<const void*>(this),
                static_cast<unsigned int>(textures_[0]),
                static_cast<unsigned int>(textures_[1]),
                static_cast<unsigned int>(textures_[2]));
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
        VP_DEBUG("OpenGLRenderer releasing sws this={} source={}x{} format={}",
                 static_cast<const void*>(this), swsSrcWidth_, swsSrcHeight_, pixelFormatName(swsSrcFormat_));
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
