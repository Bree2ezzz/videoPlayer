#ifndef VIDEOPLAYER_LOGGING_H
#define VIDEOPLAYER_LOGGING_H

#include <QDebug>
#include <QLoggingCategory>

/*
 * 统一日志入口。
 *
 * Qt 的 QLoggingCategory 已经支持按类别开关日志，配合
 * QtDebugMsg / QtInfoMsg / QtWarningMsg / QtCriticalMsg 四级。
 *
 * 用法：
 *   VP_LOG_INFO()  << "playback opened" << url;
 *   VP_LOG_WARN()  << "decode failed" << ret;
 *   VP_LOG_DEBUG() << "fillStream" << audioClock;
 *
 * 输出格式（由 main.cpp 配置 qSetMessagePattern 控制）：
 *   [INFO ] playbackcontroller.cpp:73 PlaybackController::open  - playback opened ...
 *
 * 发布版本控制：
 *   开发期：在 main 里  QLoggingCategory::setFilterRules("videoplayer.debug=true")
 *   发布期：              QLoggingCategory::setFilterRules("videoplayer.debug=false")
 *   warning / critical 始终保留。info 默认开，可在发布版关闭。
 */

Q_DECLARE_LOGGING_CATEGORY(videoplayerLog)

#define VP_LOG_DEBUG()  qCDebug(videoplayerLog).nospace().noquote()    << "[" << __FUNCTION__ << ":" << __LINE__ << "] "
#define VP_LOG_INFO()   qCInfo(videoplayerLog).nospace().noquote()     << "[" << __FUNCTION__ << ":" << __LINE__ << "] "
#define VP_LOG_WARN()   qCWarning(videoplayerLog).nospace().noquote()  << "[" << __FUNCTION__ << ":" << __LINE__ << "] "
#define VP_LOG_ERROR()  qCCritical(videoplayerLog).nospace().noquote() << "[" << __FUNCTION__ << ":" << __LINE__ << "] "

#endif // VIDEOPLAYER_LOGGING_H
