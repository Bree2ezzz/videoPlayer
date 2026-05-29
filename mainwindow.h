#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "playbackcontroller.h"

#include <QMainWindow>
#include <QString>
#include <QUrl>

class QQuickWidget;
class QStackedLayout;
class QWidget;
class QCloseEvent;
class QKeyEvent;
class QLabel;

class VideoRendererBase;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

/*
 * MainWindow 职责边界：
 *   - 窗口骨架：菜单栏（打开文件、打开 URL、切换渲染器）+ 中央视频区 +
 *     底部控制栏（QML）+ 状态栏（错误信息、当前解码模式）
 *   - 持有 PlaybackController（数据流编排）和 VideoRendererBase（视频画面）
 *   - 把 QML 控制栏的信号（play/pause/seek/volume）转发给 PlaybackController
 *   - 监听 PlaybackController 的 signal，更新 QML 的进度条 / 时间标签 /
 *     按钮状态
 *
 * 设计说明：
 *   - 中央视频区是一个容器 widget（videoContainer_），实际渲染器作为它的
 *     子 widget 填满。切换软件 / OpenGL 渲染器时，只换内部 widget，外层
 *     布局不变。
 *   - 控制栏走 QML（QQuickWidget 加载 PlayerControls.qml），叠在视频区底部
 *     （由 QStackedLayout 实现"上层透明 overlay"，鼠标移出后渐隐由 QML 端控制）。
 *   - QML 通过 contextProperty "controller" 拿到 PlaybackController 指针，
 *     直接调用 slots / 监听 signals，不再写 Q_PROPERTY 中转。
 *   - MainWindow 不持有播放业务状态——任何"是否在播放、当前位置"的判断
 *     都向 PlaybackController 询问。
 */

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // 程序启动时如果命令行带了文件/URL，main.cpp 调它直接打开
    void openMedia(const QUrl& url);

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    // ---------- 菜单动作 ----------
    void onOpenFile();
    void onOpenUrl();
    void onSwitchRenderer();          // 软件 ↔ OpenGL
    void onToggleFullscreen();
    void onAbout();

    // ---------- PlaybackController 事件 ----------
    void onStateChanged(PlaybackController::State newState);
    void onMediaLoaded();
    void onPositionChanged(double positionSec, double durationSec);
    void onErrorOccurred(int errCode, const QString& msg);
    void onEndOfStream();

private:
    // 创建/销毁视频渲染 widget。kind=Software / OpenGL
    enum class RendererKind { Software, OpenGL };
    void installRenderer(RendererKind kind);
    void removeCurrentRenderer();

    // 加载 QML 控制栏（QQuickWidget），并把 controller_ 注入 root context
    void setupControlsQml();

    // 把状态栏更新成与 controller_ 当前状态匹配
    void updateStatusBar();

    // 状态机文案 / 渲染器名称等小工具
    static QString stateText(PlaybackController::State s);

private:
    Ui::MainWindow* ui_ = nullptr;

    // ---------- 业务对象 ----------
    PlaybackController* controller_ = nullptr;
    VideoRendererBase* renderer_ = nullptr;
    RendererKind rendererKind_ = RendererKind::Software;

    // ---------- UI 节点 ----------
    QWidget* videoContainer_ = nullptr;     // 中央：视频 + overlay 控制栏的容器
    QStackedLayout* overlayLayout_ = nullptr;
    QQuickWidget* controlsQml_ = nullptr;
    QLabel* statusLabel_ = nullptr;
};

#endif // MAINWINDOW_H
