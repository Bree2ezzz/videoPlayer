#ifndef STREAMOUTPUTOPTIONS_H
#define STREAMOUTPUTOPTIONS_H

#include <QString>

// Parameters for the local-file software streaming pipeline.
struct StreamOutputOptions {
    QString rtmpUrl;
    QString videoEncoderName = QStringLiteral("libx264");
    QString audioEncoderName = QStringLiteral("aac");

    int videoBitRate = 2'500'000;
    int videoFrameRate = 30;
    int gopSeconds = 2;
    QString x264Preset = QStringLiteral("veryfast");
    QString x264Tune = QStringLiteral("zerolatency");

    int audioBitRate = 128'000;
    int audioSampleRate = 48'000;
    int audioChannels = 2;
};

#endif // STREAMOUTPUTOPTIONS_H
// diff --git a/muxer.h b/muxer.h
