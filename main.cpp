#include "mainwindow.h"
#include "app_logger.h"

#include <QApplication>
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

    AppLogger::initialize();
    VP_INFO("application startup argc={}", argc);

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
    VP_INFO("application shutdown ret={}", ret);
    AppLogger::shutdown();
    return ret;
}
