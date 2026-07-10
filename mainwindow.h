#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "playbackcontroller.h"

#include <QMainWindow>
#include <QString>
#include <QUrl>

#include <memory>

class QAction;
class QVBoxLayout;
class QWidget;
class QCloseEvent;
class QEvent;
class QKeyEvent;
class QLabel;
class QMenu;
class QPushButton;
class QSlider;

class D3D11Context;
class VideoRendererBase;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

/*
 * MainWindow owns the renderer/profile pair. Software is CPU decode + QWidget/QImage;
 * D3D11 is D3D11VA decode + D3D11 swap-chain rendering on one shared device. Changing
 * profile always tears down and rebuilds that pair before a media URL is reopened.
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void openMedia(const QUrl& url);

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    void onOpenFile();
    void onOpenUrl();
    void onSwitchRenderer();
    void onToggleFullscreen();
    void onAbout();
    void onD3D11FallbackRequested(const QString& reason);

    void onStateChanged(PlaybackController::State newState);
    void onMediaLoaded();
    void onPositionChanged(double positionSec, double durationSec);
    void onErrorOccurred(int errCode, const QString& msg);
    void onEndOfStream();

private:
    bool installRenderer(PlaybackProfile profile);
    void removeCurrentRenderer();
    void activateProfile(PlaybackProfile profile, bool reopenMedia);
    void updateProfileActions();

    void setupControlsWidget();
    void updateControlsState();
    void updatePositionControls(double positionSec, double durationSec);
    void updateVolumeControls(float volume, bool muted);
    void updatePlaybackRateControl(float rate);
    void updateOpeningIndicator();
    void updateStatusBar();

    static QString stateText(PlaybackController::State s);
    static QString formatTime(double seconds);

private:
    Ui::MainWindow* ui_ = nullptr;

    PlaybackController* controller_ = nullptr;
    VideoRendererBase* renderer_ = nullptr;
    PlaybackProfile playbackProfile_ = PlaybackProfile::Software;
    std::unique_ptr<D3D11Context> d3d11Context_;
    QUrl requestedMediaUrl_;

    QWidget* videoContainer_ = nullptr;
    QVBoxLayout* videoLayout_ = nullptr;
    QLabel* openingLabel_ = nullptr;
    QWidget* controlsWidget_ = nullptr;
    QSlider* progressSlider_ = nullptr;
    QSlider* volumeSlider_ = nullptr;
    QLabel* currentTimeLabel_ = nullptr;
    QLabel* durationLabel_ = nullptr;
    QPushButton* stepBackwardButton_ = nullptr;
    QPushButton* playPauseButton_ = nullptr;
    QPushButton* stepForwardButton_ = nullptr;
    QPushButton* muteButton_ = nullptr;
    QPushButton* speedButton_ = nullptr;
    QMenu* speedMenu_ = nullptr;
    QPushButton* fullscreenButton_ = nullptr;
    QPushButton* rendererButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QAction* softwareProfileAction_ = nullptr;
    QAction* d3d11ProfileAction_ = nullptr;
};

#endif // MAINWINDOW_H
