#include "logging.h"

// videoplayer.* 这一类的所有日志都用这个 category。
// 默认 debug 关闭、info/warning/critical 开启。
// main.cpp 里通过 setFilterRules 切换。
Q_LOGGING_CATEGORY(videoplayerLog, "videoplayer", QtInfoMsg)
