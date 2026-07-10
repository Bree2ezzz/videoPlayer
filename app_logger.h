#ifndef VIDEOPLAYER_APP_LOGGER_H
#define VIDEOPLAYER_APP_LOGGER_H

#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG
#endif

#include <spdlog/spdlog.h>

namespace AppLogger {
void initialize();
void shutdown();
}

#define VP_TRACE(...) SPDLOG_LOGGER_CALL(spdlog::default_logger_raw(), spdlog::level::trace, __VA_ARGS__)
#define VP_DEBUG(...) SPDLOG_LOGGER_CALL(spdlog::default_logger_raw(), spdlog::level::debug, __VA_ARGS__)
#define VP_INFO(...)  SPDLOG_LOGGER_CALL(spdlog::default_logger_raw(), spdlog::level::info, __VA_ARGS__)
#define VP_WARN(...)  SPDLOG_LOGGER_CALL(spdlog::default_logger_raw(), spdlog::level::warn, __VA_ARGS__)
#define VP_ERROR(...) SPDLOG_LOGGER_CALL(spdlog::default_logger_raw(), spdlog::level::err, __VA_ARGS__)

#endif // VIDEOPLAYER_APP_LOGGER_H
