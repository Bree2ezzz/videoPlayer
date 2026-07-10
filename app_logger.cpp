#include "app_logger.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>
#include <vector>

namespace AppLogger {

void initialize()
{
    try {
        auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            "logs/videoplayer.log", 5 * 1024 * 1024, 3, true);

        std::vector<spdlog::sink_ptr> sinks{consoleSink, fileSink};
        auto logger = std::make_shared<spdlog::logger>("videoplayer", sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::debug);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%s:%#] %v");
        logger->flush_on(spdlog::level::debug);
        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::debug);
    } catch (const spdlog::spdlog_ex&) {
        auto logger = spdlog::stderr_color_mt("videoplayer-fallback");
        logger->set_level(spdlog::level::debug);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%s:%#] %v");
        logger->flush_on(spdlog::level::debug);
        spdlog::set_default_logger(logger);
    }
}

void shutdown()
{
    spdlog::shutdown();
}

} // namespace AppLogger
