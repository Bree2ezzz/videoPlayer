#include "mainwindow.h"
#include "./ui_mainwindow.h"

#include "softwarerenderer.h"
#include "openglrenderer.h"
#include "videorendererbase.h"

#include <QAction>
#include <QCloseEvent>
#include <QEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLayout>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr int kProgressSliderScale = 1000;

int progressSliderMax(double durationSec)
{
    if (!std::isfinite(durationSec) || durationSec <= 0.0) {
        return kProgressSliderScale;
    }

    const double scaled = durationSec * kProgressSliderScale;
    const double maxInt = static_cast<double>(std::numeric_limits<int>::max());
    return static_cast<int>(std::clamp(scaled, 1.0, maxInt));
}

int progressSliderValue(double positionSec)
{
    if (!std::isfinite(positionSec) || positionSec <= 0.0) {
        return 0;
    }

    const double scaled = positionSec * kProgressSliderScale;
    const double maxInt = static_cast<double>(std::numeric_limits<int>::max());
    return static_cast<int>(std::clamp(scaled, 0.0, maxInt));
}

double progressSliderSeconds(int value)
{
    return static_cast<double>(value) / kProgressSliderScale;
}

QString playbackRateText(float rate)
{
    return QStringLiteral("%1x").arg(static_cast<double>(rate), 0, 'g', 3);
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      ui_(new Ui::MainWindow),
      controller_(new PlaybackController(this))
{
    ui_->setupUi(this);
    setWindowTitle(QStringLiteral("VideoPlayer"));
    setMouseTracking(true);

    QWidget* central = ui_->centralwidget;
    central->setMouseTracking(true);
    central->setAutoFillBackground(false);
    if (central->layout()) {
        delete central->layout();
    }

    auto* mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    videoContainer_ = new QWidget(central);
    videoContainer_->setMouseTracking(true);
    videoContainer_->setAutoFillBackground(false);
    videoContainer_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    videoContainer_->installEventFilter(this);
    mainLayout->addWidget(videoContainer_, 1);

    videoLayout_ = new QVBoxLayout(videoContainer_);
    videoLayout_->setContentsMargins(0, 0, 0, 0);
    videoLayout_->setSpacing(0);

    installRenderer(RendererKind::Software);

    openingLabel_ = new QLabel(QStringLiteral("Opening media..."), videoContainer_);
    openingLabel_->setAlignment(Qt::AlignCenter);
    openingLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    openingLabel_->setStyleSheet(QStringLiteral(
        "background-color: rgba(0, 0, 0, 150);"
        "color: #f2f2f2;"
        "font-size: 18px;"
        "font-weight: 600;"));
    openingLabel_->hide();

    setupControlsWidget();

    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("File"));
    QAction* openFileAction = fileMenu->addAction(QStringLiteral("Open File..."));
    QAction* openUrlAction = fileMenu->addAction(QStringLiteral("Open URL..."));
    fileMenu->addSeparator();
    QAction* exitAction = fileMenu->addAction(QStringLiteral("Exit"));

    QMenu* playbackMenu = menuBar()->addMenu(QStringLiteral("Playback"));
    QAction* playPauseAction = playbackMenu->addAction(QStringLiteral("Play / Pause"));
    QAction* fullscreenAction = playbackMenu->addAction(QStringLiteral("Fullscreen"));

    QMenu* rendererMenu = menuBar()->addMenu(QStringLiteral("Renderer"));
    QAction* switchRendererAction = rendererMenu->addAction(QStringLiteral("Switch Renderer"));
    QAction* hardwareDecodeAction = rendererMenu->addAction(QStringLiteral("Hardware Decoding"));
    hardwareDecodeAction->setCheckable(true);

    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("Help"));
    QAction* aboutAction = helpMenu->addAction(QStringLiteral("About"));

    connect(openFileAction, &QAction::triggered, this, &MainWindow::onOpenFile);
    connect(openUrlAction, &QAction::triggered, this, &MainWindow::onOpenUrl);
    connect(exitAction, &QAction::triggered, this, &QWidget::close);
    connect(playPauseAction, &QAction::triggered, controller_, &PlaybackController::togglePause);
    connect(fullscreenAction, &QAction::triggered, this, &MainWindow::onToggleFullscreen);
    connect(switchRendererAction, &QAction::triggered, this, &MainWindow::onSwitchRenderer);
    connect(hardwareDecodeAction, &QAction::toggled, this, [this, hardwareDecodeAction](bool enabled) {
        if (controller_->isOpen()) {
            statusBar()->showMessage(QStringLiteral("Close the current media before changing hardware decoding"), 3000);
            QSignalBlocker blocker(hardwareDecodeAction);
            hardwareDecodeAction->setChecked(hardwareDecodingEnabled_);
            return;
        }

        hardwareDecodingEnabled_ = enabled;
        controller_->setHardwareDecodingEnabled(enabled);
        updateStatusBar();
        statusBar()->showMessage(enabled
                                     ? QStringLiteral("Hardware decoding enabled")
                                     : QStringLiteral("Hardware decoding disabled"),
                                 2000);
    });
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAbout);

    connect(controller_, &PlaybackController::stateChanged,
            this, &MainWindow::onStateChanged);
    connect(controller_, &PlaybackController::mediaLoaded,
            this, &MainWindow::onMediaLoaded);
    connect(controller_, &PlaybackController::positionChanged,
            this, &MainWindow::onPositionChanged);
    connect(controller_, &PlaybackController::errorOccurred,
            this, &MainWindow::onErrorOccurred);
    connect(controller_, &PlaybackController::endOfStream,
            this, &MainWindow::onEndOfStream);
    connect(controller_, &PlaybackController::volumeChanged,
            this, [this](float volume) {
                updateVolumeControls(volume, controller_->isMuted());
            });
    connect(controller_, &PlaybackController::mutedChanged,
            this, [this](bool muted) {
                updateVolumeControls(controller_->volume(), muted);
            });
    connect(controller_, &PlaybackController::playbackRateChanged,
            this, &MainWindow::updatePlaybackRateControl);

    statusLabel_ = new QLabel(statusBar());
    statusLabel_->setMinimumWidth(260);
    statusBar()->addPermanentWidget(statusLabel_, 1);
    updateStatusBar();
    updateOpeningIndicator();
}

MainWindow::~MainWindow()
{
    if (controller_) {
        controller_->close();
    }
    removeCurrentRenderer();
    delete ui_;
}

void MainWindow::openMedia(const QUrl& url)
{
    if (!url.isValid() || url.isEmpty()) {
        return;
    }

    controller_->open(url);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    controller_->close();
    QMainWindow::closeEvent(event);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == videoContainer_ &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        updateOpeningIndicator();
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->isAutoRepeat()) {
        QMainWindow::keyPressEvent(event);
        return;
    }

    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    switch (event->key()) {
    case Qt::Key_Space:
        controller_->togglePause();
        event->accept();
        return;
    case Qt::Key_Left:
        controller_->seek(controller_->positionSec() - (shift ? 30.0 : 5.0));
        event->accept();
        return;
    case Qt::Key_Right:
        controller_->seek(controller_->positionSec() + (shift ? 30.0 : 5.0));
        event->accept();
        return;
    case Qt::Key_Up:
        controller_->setVolume(controller_->volume() + 0.05f);
        event->accept();
        return;
    case Qt::Key_Down:
        controller_->setVolume(controller_->volume() - 0.05f);
        event->accept();
        return;
    case Qt::Key_M:
        controller_->setMuted(!controller_->isMuted());
        event->accept();
        return;
    case Qt::Key_Comma:
        controller_->stepBackward();
        event->accept();
        return;
    case Qt::Key_Period:
        controller_->stepForward();
        event->accept();
        return;
    case Qt::Key_F:
        onToggleFullscreen();
        event->accept();
        return;
    case Qt::Key_Escape:
        if (isFullScreen()) {
            showNormal();
            event->accept();
            return;
        }
        break;
    default:
        break;
    }

    QMainWindow::keyPressEvent(event);
}

void MainWindow::onOpenFile()
{
    const QString fileName = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Open Media"),
        QString(),
        QStringLiteral("Media Files (*.*)"));
    if (fileName.isEmpty()) {
        return;
    }

    openMedia(QUrl::fromLocalFile(fileName));
}

void MainWindow::onOpenUrl()
{
    bool ok = false;
    const QString text = QInputDialog::getText(
        this,
        QStringLiteral("Open URL"),
        QStringLiteral("URL (RTSP / RTMP / HLS .m3u8 / HTTP-FLV / HTTP MP4):"),
        QLineEdit::Normal,
        QString(),
        &ok);
    if (!ok || text.trimmed().isEmpty()) {
        return;
    }

    openMedia(QUrl::fromUserInput(text.trimmed()));
}

void MainWindow::onSwitchRenderer()
{
    if (controller_->isOpen()) {
        statusBar()->showMessage(QStringLiteral("Close the current media before switching renderer"), 3000);
        return;
    }

    const RendererKind nextKind = rendererKind_ == RendererKind::Software
                                      ? RendererKind::OpenGL
                                      : RendererKind::Software;
    installRenderer(nextKind);
    updateStatusBar();
    statusBar()->showMessage(nextKind == RendererKind::OpenGL
                                 ? QStringLiteral("OpenGL renderer enabled")
                                 : QStringLiteral("Software renderer enabled"),
                             2000);
}

void MainWindow::onToggleFullscreen()
{
    if (isFullScreen()) {
        showNormal();
    } else {
        showFullScreen();
    }
}

void MainWindow::onAbout()
{
    QMessageBox::about(
        this,
        QStringLiteral("About VideoPlayer"),
        QStringLiteral("Qt / FFmpeg / SDL video player\n\n"
                       "Network URL support: RTSP, RTMP/RTMPS, HLS(m3u8), "
                       "HTTP-FLV, and HTTP/HTTPS media files."));
}

void MainWindow::onStateChanged(PlaybackController::State)
{
    updateStatusBar();
    updateControlsState();
    updateOpeningIndicator();
}

void MainWindow::onMediaLoaded()
{
    updateStatusBar();
    updatePositionControls(controller_->positionSec(), controller_->durationSec());
    updateVolumeControls(controller_->volume(), controller_->isMuted());
    updateControlsState();
    updateOpeningIndicator();
    statusBar()->showMessage(QStringLiteral("Media loaded"), 2000);
}

void MainWindow::onPositionChanged(double positionSec, double durationSec)
{
    updateStatusBar();
    updatePositionControls(positionSec, durationSec);
}

void MainWindow::onErrorOccurred(int, const QString& msg)
{
    updateOpeningIndicator();
    statusBar()->showMessage(msg, 5000);
}

void MainWindow::onEndOfStream()
{
    statusBar()->showMessage(QStringLiteral("End of stream"), 3000);
    updateStatusBar();
    updateControlsState();
    updateOpeningIndicator();
}

void MainWindow::installRenderer(RendererKind kind)
{
    removeCurrentRenderer();

    rendererKind_ = kind;
    if (kind == RendererKind::OpenGL) {
        renderer_ = new OpenGLRenderer(videoContainer_);
    } else {
        renderer_ = new SoftwareRenderer(videoContainer_);
    }

    QWidget* widget = renderer_ ? renderer_->asWidget() : nullptr;
    if (widget) {
        widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    if (videoLayout_ && widget) {
        videoLayout_->addWidget(widget);
    }

    if (controller_) {
        controller_->setRenderer(renderer_);
    }

    if (rendererButton_) {
        rendererButton_->setText(kind == RendererKind::OpenGL
                                     ? QStringLiteral("GL")
                                     : QStringLiteral("SW"));
    }
}

void MainWindow::removeCurrentRenderer()
{
    if (!renderer_) {
        return;
    }

    if (controller_) {
        controller_->setRenderer(nullptr);
    }

    QWidget* widget = renderer_->asWidget();
    if (videoLayout_ && widget) {
        videoLayout_->removeWidget(widget);
    }
    delete widget;
    renderer_ = nullptr;
}

void MainWindow::setupControlsWidget()
{
    controlsWidget_ = new QWidget(ui_->centralwidget);
    controlsWidget_->setObjectName(QStringLiteral("controlsWidget"));
    controlsWidget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    controlsWidget_->setFocusPolicy(Qt::NoFocus);
    controlsWidget_->setFixedHeight(76);
    controlsWidget_->setStyleSheet(QStringLiteral(
        "#controlsWidget { background-color: #111111; }"
        "QLabel { color: #dddddd; }"
        "QPushButton { color: #eeeeee; background-color: #242424; border: 1px solid #444444; border-radius: 4px; padding: 3px 8px; }"
        "QPushButton:disabled { color: #777777; background-color: #1a1a1a; }"
        "QPushButton:hover:!disabled { background-color: #303030; }"
        "QSlider::groove:horizontal { height: 4px; background: #444444; border-radius: 2px; }"
        "QSlider::sub-page:horizontal { background: #4fc3f7; border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 12px; margin: -5px 0; border-radius: 6px; background: #f5f5f5; }"
    ));

    auto* outerLayout = new QVBoxLayout(controlsWidget_);
    outerLayout->setContentsMargins(12, 6, 12, 6);
    outerLayout->setSpacing(4);

    auto* progressRow = new QHBoxLayout();
    progressRow->setContentsMargins(0, 0, 0, 0);
    progressRow->setSpacing(8);

    currentTimeLabel_ = new QLabel(formatTime(0.0), controlsWidget_);
    currentTimeLabel_->setMinimumWidth(52);
    currentTimeLabel_->setAlignment(Qt::AlignCenter);

    progressSlider_ = new QSlider(Qt::Horizontal, controlsWidget_);
    progressSlider_->setRange(0, kProgressSliderScale);
    progressSlider_->setEnabled(false);
    progressSlider_->setFocusPolicy(Qt::NoFocus);

    durationLabel_ = new QLabel(formatTime(0.0), controlsWidget_);
    durationLabel_->setMinimumWidth(52);
    durationLabel_->setAlignment(Qt::AlignCenter);

    progressRow->addWidget(currentTimeLabel_);
    progressRow->addWidget(progressSlider_, 1);
    progressRow->addWidget(durationLabel_);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(8);

    muteButton_ = new QPushButton(QStringLiteral("Mute"), controlsWidget_);
    muteButton_->setEnabled(false);
    muteButton_->setFocusPolicy(Qt::NoFocus);

    volumeSlider_ = new QSlider(Qt::Horizontal, controlsWidget_);
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setValue(100);
    volumeSlider_->setFixedWidth(110);
    volumeSlider_->setEnabled(false);
    volumeSlider_->setFocusPolicy(Qt::NoFocus);

    playPauseButton_ = new QPushButton(QStringLiteral("Play"), controlsWidget_);
    playPauseButton_->setEnabled(false);
    playPauseButton_->setFocusPolicy(Qt::NoFocus);

    stepBackwardButton_ = new QPushButton(QStringLiteral("-1帧"), controlsWidget_);
    stepBackwardButton_->setFixedWidth(52);
    stepBackwardButton_->setEnabled(false);
    stepBackwardButton_->setFocusPolicy(Qt::NoFocus);

    stepForwardButton_ = new QPushButton(QStringLiteral("+1帧"), controlsWidget_);
    stepForwardButton_->setFixedWidth(52);
    stepForwardButton_->setEnabled(false);
    stepForwardButton_->setFocusPolicy(Qt::NoFocus);

    speedButton_ = new QPushButton(playbackRateText(controller_->playbackRate()), controlsWidget_);
    speedButton_->setFixedWidth(58);
    speedButton_->setFocusPolicy(Qt::NoFocus);
    speedMenu_ = new QMenu(speedButton_);
    const float speeds[] = {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f};
    for (const float speed : speeds) {
        QAction* action = speedMenu_->addAction(playbackRateText(speed));
        connect(action, &QAction::triggered, this, [this, speed] {
            controller_->setPlaybackRate(speed);
        });
    }
    speedButton_->setMenu(speedMenu_);

    rendererButton_ = new QPushButton(QStringLiteral("SW"), controlsWidget_);
    rendererButton_->setFixedWidth(46);
    rendererButton_->setFocusPolicy(Qt::NoFocus);

    fullscreenButton_ = new QPushButton(QStringLiteral("Fullscreen"), controlsWidget_);
    fullscreenButton_->setFocusPolicy(Qt::NoFocus);

    buttonRow->addWidget(muteButton_);
    buttonRow->addWidget(volumeSlider_);
    buttonRow->addStretch(1);
    buttonRow->addWidget(stepBackwardButton_);
    buttonRow->addWidget(playPauseButton_);
    buttonRow->addWidget(stepForwardButton_);
    buttonRow->addStretch(1);
    buttonRow->addWidget(speedButton_);
    buttonRow->addWidget(rendererButton_);
    buttonRow->addWidget(fullscreenButton_);

    outerLayout->addLayout(progressRow);
    outerLayout->addLayout(buttonRow);

    if (QLayout* layout = ui_->centralwidget->layout()) {
        static_cast<QVBoxLayout*>(layout)->addWidget(controlsWidget_, 0);
    }

    connect(playPauseButton_, &QPushButton::clicked,
            controller_, &PlaybackController::togglePause);
    connect(stepBackwardButton_, &QPushButton::clicked,
            controller_, &PlaybackController::stepBackward);
    connect(stepForwardButton_, &QPushButton::clicked,
            controller_, &PlaybackController::stepForward);
    connect(muteButton_, &QPushButton::clicked, this, [this] {
        controller_->setMuted(!controller_->isMuted());
    });
    connect(fullscreenButton_, &QPushButton::clicked,
            this, &MainWindow::onToggleFullscreen);
    connect(rendererButton_, &QPushButton::clicked,
            this, &MainWindow::onSwitchRenderer);
    connect(progressSlider_, &QSlider::sliderReleased, this, [this] {
        if (!progressSlider_ || !progressSlider_->isEnabled()) {
            return;
        }
        controller_->requestSeek(progressSliderSeconds(progressSlider_->value()));
    });
    connect(progressSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (progressSlider_ && progressSlider_->isSliderDown() && currentTimeLabel_) {
            currentTimeLabel_->setText(formatTime(progressSliderSeconds(value)));
        }
    });
    connect(volumeSlider_, &QSlider::sliderMoved, this, [this](int value) {
        controller_->setVolume(static_cast<float>(value) / 100.0f);
    });
    connect(volumeSlider_, &QSlider::sliderReleased, this, [this] {
        if (volumeSlider_) {
            controller_->setVolume(static_cast<float>(volumeSlider_->value()) / 100.0f);
        }
    });

    updatePositionControls(controller_->positionSec(), controller_->durationSec());
    updateVolumeControls(controller_->volume(), controller_->isMuted());
    updatePlaybackRateControl(controller_->playbackRate());
    updateControlsState();
}

void MainWindow::updateControlsState()
{
    const PlaybackController::State state = controller_->state();
    const bool isOpen =
        state != PlaybackController::State::Idle &&
        state != PlaybackController::State::Opening &&
        state != PlaybackController::State::Error;
    const bool canControl = isOpen && state != PlaybackController::State::Seeking;
    const bool canSeek = canControl && !controller_->isRealtime() && controller_->durationSec() >= 0.0;
    const bool canStep =
        state == PlaybackController::State::Paused &&
        controller_->hasVideo() &&
        !controller_->isRealtime() &&
        controller_->durationSec() >= 0.0;

    if (progressSlider_) {
        progressSlider_->setEnabled(canSeek);
    }
    if (volumeSlider_) {
        volumeSlider_->setEnabled(isOpen);
    }
    if (muteButton_) {
        muteButton_->setEnabled(isOpen);
    }
    if (playPauseButton_) {
        playPauseButton_->setEnabled(canControl);
        playPauseButton_->setText(state == PlaybackController::State::Playing
                                      ? QStringLiteral("Pause")
                                      : QStringLiteral("Play"));
    }
    if (stepBackwardButton_) {
        stepBackwardButton_->setEnabled(canStep);
    }
    if (stepForwardButton_) {
        stepForwardButton_->setEnabled(canStep);
    }
    if (speedButton_) {
        speedButton_->setEnabled(true);
    }
}

void MainWindow::updatePositionControls(double positionSec, double durationSec)
{
    if (currentTimeLabel_) {
        currentTimeLabel_->setText(formatTime(positionSec));
    }
    if (durationLabel_) {
        durationLabel_->setText(durationSec < 0.0
                                    ? QStringLiteral("LIVE")
                                    : formatTime(durationSec));
    }
    if (!progressSlider_ || progressSlider_->isSliderDown()) {
        return;
    }

    progressSlider_->setMaximum(progressSliderMax(durationSec));
    progressSlider_->setValue(std::min(progressSliderValue(positionSec), progressSlider_->maximum()));
}

void MainWindow::updateVolumeControls(float volume, bool muted)
{
    if (volumeSlider_ && !volumeSlider_->isSliderDown()) {
        const int value = static_cast<int>(std::clamp(volume, 0.0f, 1.0f) * 100.0f);
        volumeSlider_->setValue(value);
    }
    if (muteButton_) {
        muteButton_->setText(muted ? QStringLiteral("Muted") : QStringLiteral("Mute"));
    }
}

void MainWindow::updatePlaybackRateControl(float rate)
{
    if (speedButton_) {
        speedButton_->setText(playbackRateText(rate));
    }
}

void MainWindow::updateOpeningIndicator()
{
    if (!openingLabel_ || !videoContainer_) {
        return;
    }

    const bool opening = controller_ &&
                         controller_->state() == PlaybackController::State::Opening;
    openingLabel_->setGeometry(videoContainer_->rect());
    openingLabel_->setVisible(opening);
    if (opening) {
        openingLabel_->raise();
        statusBar()->showMessage(QStringLiteral("Opening media..."), 0);
    } else if (statusBar()->currentMessage() == QStringLiteral("Opening media...")) {
        statusBar()->clearMessage();
    }
}

void MainWindow::updateStatusBar()
{
    const QString rendererName =
        rendererKind_ == RendererKind::Software ? QStringLiteral("SW") : QStringLiteral("OpenGL");
    const QString hwDecodeName = hardwareDecodingEnabled_ ? QStringLiteral("HW On")
                                                          : QStringLiteral("HW Off");
    const QString durationText = controller_->durationSec() < 0.0
                                     ? QStringLiteral("LIVE")
                                     : QStringLiteral("%1s").arg(controller_->durationSec(), 0, 'f', 1);
    const QString text =
        QStringLiteral("%1 | Renderer: %2 | %3 | %4s / %5")
            .arg(stateText(controller_->state()))
            .arg(rendererName)
            .arg(hwDecodeName)
            .arg(controller_->positionSec(), 0, 'f', 1)
            .arg(durationText);
    if (statusLabel_) {
        statusLabel_->setText(text);
    }
}

QString MainWindow::stateText(PlaybackController::State s)
{
    switch (s) {
    case PlaybackController::State::Idle:
        return QStringLiteral("Idle");
    case PlaybackController::State::Opening:
        return QStringLiteral("Opening");
    case PlaybackController::State::Ready:
        return QStringLiteral("Ready");
    case PlaybackController::State::Playing:
        return QStringLiteral("Playing");
    case PlaybackController::State::Paused:
        return QStringLiteral("Paused");
    case PlaybackController::State::Seeking:
        return QStringLiteral("Seeking");
    case PlaybackController::State::Stopped:
        return QStringLiteral("Stopped");
    case PlaybackController::State::Error:
        return QStringLiteral("Error");
    }

    return QStringLiteral("Unknown");
}

QString MainWindow::formatTime(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0) {
        seconds = 0.0;
    }

    const int totalSeconds = static_cast<int>(seconds);
    const int hours = totalSeconds / 3600;
    const int minutes = (totalSeconds % 3600) / 60;
    const int secs = totalSeconds % 60;

    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(secs, 2, 10, QLatin1Char('0'));
    }

    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'));
}
