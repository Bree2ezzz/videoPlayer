#include "mainwindow.h"
#include "logging.h"

#include <QApplication>
#include <QLoggingCategory>
#include <QString>
#include <QUrl>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

int main(int argc, char *argv[])
{
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        return 1;
    }
    avformat_network_init();

    // 日志格式：[级别] 文件:行 函数 - 内容
    qSetMessagePattern(
        "[%{type}] %{file}:%{line} %{function} - %{message}");

    // 日志级别开关。开发期开 debug，发布期改成 *.debug=false 即可。
    // 也可以通过环境变量 QT_LOGGING_RULES 覆盖，方便不重编切换。
    QLoggingCategory::setFilterRules(QStringLiteral(
        "videoplayer.debug=true\n"
        "videoplayer.info=true\n"
        "videoplayer.warning=true\n"
        "videoplayer.critical=true\n"));

    QApplication a(argc, argv);
    MainWindow w;
    if (argc > 1) {
        const QString input = QString::fromLocal8Bit(argv[1]);
        const QUrl url = QUrl::fromUserInput(input);
        w.openMedia(url);
    }
    w.show();

    const int ret = a.exec();
    avformat_network_deinit();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    SDL_Quit();
    return ret;
}
