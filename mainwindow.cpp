#include "mainwindow.h"
#include "./ui_mainwindow.h"

#include "softwarerenderer.h"
#include "videorendererbase.h"

#include <QAction>
#include <QCloseEvent>
#include <QFileDialog>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLayout>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QObject>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWidget>
#include <QStackedLayout>
#include <QStatusBar>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      ui_(new Ui::MainWindow),
      controller_(new PlaybackController(this))
{
    ui_->setupUi(this);
    setWindowTitle(QStringLiteral("VideoPlayer"));
    setMouseTracking(true);

    videoContainer_ = ui_->centralwidget;
    videoContainer_->setMouseTracking(true);
    videoContainer_->setAutoFillBackground(false);
    if (videoContainer_->layout()) {
        delete videoContainer_->layout();
    }

    overlayLayout_ = new QStackedLayout(videoContainer_);
    overlayLayout_->setContentsMargins(0, 0, 0, 0);
    overlayLayout_->setSpacing(0);
    overlayLayout_->setStackingMode(QStackedLayout::StackAll);

    installRenderer(RendererKind::Software);
    setupControlsQml();

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

    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("Help"));
    QAction* aboutAction = helpMenu->addAction(QStringLiteral("About"));

    connect(openFileAction, &QAction::triggered, this, &MainWindow::onOpenFile);
    connect(openUrlAction, &QAction::triggered, this, &MainWindow::onOpenUrl);
    connect(exitAction, &QAction::triggered, this, &QWidget::close);
    connect(playPauseAction, &QAction::triggered, controller_, &PlaybackController::togglePause);
    connect(fullscreenAction, &QAction::triggered, this, &MainWindow::onToggleFullscreen);
    connect(switchRendererAction, &QAction::triggered, this, &MainWindow::onSwitchRenderer);
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

    statusLabel_ = new QLabel(statusBar());
    statusLabel_->setMinimumWidth(260);
    statusBar()->addPermanentWidget(statusLabel_, 1);
    updateStatusBar();
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
        QStringLiteral("URL:"),
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

    statusBar()->showMessage(QStringLiteral("OpenGL renderer is not implemented yet"), 3000);
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
        QStringLiteral("Qt / FFmpeg / SDL video player"));
}

void MainWindow::onStateChanged(PlaybackController::State)
{
    updateStatusBar();
}

void MainWindow::onMediaLoaded()
{
    updateStatusBar();
    statusBar()->showMessage(QStringLiteral("Media loaded"), 2000);
}

void MainWindow::onPositionChanged(double, double)
{
    updateStatusBar();
}

void MainWindow::onErrorOccurred(int, const QString& msg)
{
    statusBar()->showMessage(msg, 5000);
}

void MainWindow::onEndOfStream()
{
    statusBar()->showMessage(QStringLiteral("End of stream"), 3000);
    updateStatusBar();
}

void MainWindow::installRenderer(RendererKind kind)
{
    if (kind == RendererKind::OpenGL) {
        statusBar()->showMessage(QStringLiteral("OpenGL renderer is not implemented yet"), 3000);
        return;
    }

    removeCurrentRenderer();

    rendererKind_ = RendererKind::Software;
    auto* renderer = new SoftwareRenderer(videoContainer_);
    renderer_ = renderer;

    if (overlayLayout_) {
        overlayLayout_->insertWidget(0, renderer->asWidget());
    }

    if (controller_) {
        controller_->setRenderer(renderer_);
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
    if (overlayLayout_ && widget) {
        overlayLayout_->removeWidget(widget);
    }
    delete widget;
    renderer_ = nullptr;
}

void MainWindow::setupControlsQml()
{
    controlsQml_ = new QQuickWidget(videoContainer_);
    controlsQml_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    controlsQml_->setClearColor(Qt::transparent);
    controlsQml_->setAttribute(Qt::WA_AlwaysStackOnTop);
    controlsQml_->setAttribute(Qt::WA_TranslucentBackground);
    controlsQml_->setMouseTracking(true);
    controlsQml_->rootContext()->setContextProperty(QStringLiteral("controller"), controller_);
    controlsQml_->setSource(QUrl(QStringLiteral("qrc:/qml/PlayerControls.qml")));

    if (overlayLayout_) {
        overlayLayout_->addWidget(controlsQml_);
    }

    QObject* root = controlsQml_->rootObject();
    if (root) {
        connect(root, SIGNAL(fullscreenClicked()), this, SLOT(onToggleFullscreen()));
        connect(root, SIGNAL(rendererBadgeClicked()), this, SLOT(onSwitchRenderer()));
    }
}

void MainWindow::updateStatusBar()
{
    const QString rendererName =
        rendererKind_ == RendererKind::Software ? QStringLiteral("SW") : QStringLiteral("OpenGL");
    const QString durationText = controller_->durationSec() < 0.0
                                     ? QStringLiteral("LIVE")
                                     : QStringLiteral("%1s").arg(controller_->durationSec(), 0, 'f', 1);
    const QString text =
        QStringLiteral("%1 | Renderer: %2 | %3s / %4")
            .arg(stateText(controller_->state()))
            .arg(rendererName)
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
