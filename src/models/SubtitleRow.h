#pragma once
#include <QString>
#include <QTime>
#include <cmath>
#include "helpers/Decimal.h"

struct GlobalSettings {
    Decimal maxLineLength = 12.0;
    Decimal maxDuration = 0.0;
    Decimal minDuration = 0.8;
    double frameRate = 0.0;      // double: inherently floating-point, no exact-comparison need
    Decimal minGapSeconds = 0.0;
    Decimal maxCPS = 6.9;
    bool highlightConsecutiveSpeaker = false;
};

inline GlobalSettings& globalSettings() {
    static GlobalSettings s;
    return s;
}

struct SubtitleRow {
    QString start;
    QString end;
    QString speaker;
    QString source;
    QString target;

    // Computed (cached)
    Decimal durationSeconds;
    Decimal cpsValue;
    Decimal maxLineLenValue;
    Decimal gapSeconds;
    bool gapWarning = false;

    void recalcAll();
    void recalcDuration();
    void recalcTargetMetrics();
    void recalcCPS();

    bool isDurationWarning() const;
    bool isCPSWarning() const;
    bool isMaxLineLenWarning() const;

    QString durationStr() const;
    QString cpsStr() const;
    QString maxLineLenStr() const;
    QString gapStr() const;

    SubtitleRow clone() const;

    // Time helpers
    static bool tryParseTime(const QString& text, Decimal& outSeconds);
    static QString formatTime(double seconds);
    static double snapToFrame(double seconds);
};
