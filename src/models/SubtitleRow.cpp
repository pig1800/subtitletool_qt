#include "SubtitleRow.h"
#include "helpers/CharCountHelper.h"
#include <QRegularExpression>
#include <cmath>

void SubtitleRow::recalcAll()
{
    recalcDuration();
    recalcTargetMetrics();
}

void SubtitleRow::recalcDuration()
{
    Decimal s, e;
    if (tryParseTime(start, s) && tryParseTime(end, e) && e > s)
        durationSeconds = e - s;
    else
        durationSeconds = 0.0;
    recalcCPS();
}

void SubtitleRow::recalcTargetMetrics()
{
    maxLineLenValue = CharCountHelper::maxDisplayLineLength(target);
    recalcCPS();
}

void SubtitleRow::recalcCPS()
{
    if (durationSeconds <= Decimal(0.0)) {
        cpsValue = 0.0;
        return;
    }
    double count = CharCountHelper::countForCPS(target);
    cpsValue = count / durationSeconds.toDouble();
}

bool SubtitleRow::isDurationWarning() const
{
    if (durationSeconds <= Decimal(0.0)) return false;
    auto& g = globalSettings();
    if (g.minDuration > Decimal(0.0) && durationSeconds < g.minDuration) return true;
    if (g.maxDuration > Decimal(0.0) && durationSeconds > g.maxDuration) return true;
    return false;
}

bool SubtitleRow::isCPSWarning() const
{
    auto& g = globalSettings();
    Decimal rounded = std::round(cpsValue.toDouble() * 10.0) / 10.0;
    return g.maxCPS > Decimal(0.0) && rounded > g.maxCPS;
}

bool SubtitleRow::isMaxLineLenWarning() const
{
    return maxLineLenValue > globalSettings().maxLineLength;
}

QString SubtitleRow::durationStr() const
{
    if (durationSeconds <= Decimal(0.0)) return {};
    return durationSeconds.toString(3);
}

QString SubtitleRow::cpsStr() const
{
    return cpsValue.toString(1);
}

QString SubtitleRow::maxLineLenStr() const
{
    return maxLineLenValue.toString(1);
}

QString SubtitleRow::gapStr() const
{
    return gapSeconds.toString(3);
}

SubtitleRow SubtitleRow::clone() const
{
    return *this;
}

bool SubtitleRow::tryParseTime(const QString& text, Decimal& outSeconds)
{
    if (text.trimmed().isEmpty()) return false;
    QString t = text.trimmed();

    // Try HH:MM:SS,mmm or HH:MM:SS.mmm
    static QRegularExpression re(R"(^(\d{1,2}):(\d{2}):(\d{2})[.,](\d{1,3})$)");
    auto m = re.match(t);
    if (m.hasMatch()) {
        int h = m.captured(1).toInt();
        int min = m.captured(2).toInt();
        int sec = m.captured(3).toInt();
        QString msStr = m.captured(4).leftJustified(3, '0');
        int ms = msStr.toInt();
        outSeconds = Decimal::fromRaw(
            static_cast<int64_t>(h) * 3600000 + min * 60000 + sec * 1000 + ms);
        return true;
    }

    // Try HH:MM:SS
    static QRegularExpression re2(R"(^(\d{1,2}):(\d{2}):(\d{2})$)");
    auto m2 = re2.match(t);
    if (m2.hasMatch()) {
        int h = m2.captured(1).toInt();
        int min = m2.captured(2).toInt();
        int sec = m2.captured(3).toInt();
        outSeconds = Decimal::fromRaw(
            static_cast<int64_t>(h) * 3600000 + min * 60000 + sec * 1000);
        return true;
    }

    // Try MM:SS.mmm or MM:SS
    static QRegularExpression re3(R"(^(\d{1,2}):(\d{2})(?:[.,](\d{1,3}))?$)");
    auto m3 = re3.match(t);
    if (m3.hasMatch()) {
        int min = m3.captured(1).toInt();
        int sec = m3.captured(2).toInt();
        int ms = 0;
        if (!m3.captured(3).isEmpty())
            ms = m3.captured(3).leftJustified(3, '0').toInt();
        outSeconds = Decimal::fromRaw(
            static_cast<int64_t>(min) * 60000 + sec * 1000 + ms);
        return true;
    }

    // Try pure number (seconds)
    bool ok = false;
    double secs = t.toDouble(&ok);
    if (ok && secs >= 0) {
        outSeconds = secs;
        return true;
    }

    return false;
}

QString SubtitleRow::formatTime(double seconds)
{
    double fps = globalSettings().frameRate;
    if (fps > 0) {
        double frameDuration = 1.0 / fps;
        double frames = std::round(seconds / frameDuration);
        seconds = frames * frameDuration;
    }

    // Round to millisecond
    long long totalMs = std::llround(seconds * 1000.0);
    if (totalMs < 0) totalMs = 0;

    int h = static_cast<int>(totalMs / 3600000);
    totalMs %= 3600000;
    int mn = static_cast<int>(totalMs / 60000);
    totalMs %= 60000;
    int s = static_cast<int>(totalMs / 1000);
    int ms = static_cast<int>(totalMs % 1000);

    return QStringLiteral("%1:%2:%3,%4")
        .arg(h, 2, 10, QChar('0'))
        .arg(mn, 2, 10, QChar('0'))
        .arg(s, 2, 10, QChar('0'))
        .arg(ms, 3, 10, QChar('0'));
}

double SubtitleRow::snapToFrame(double seconds)
{
    double fps = globalSettings().frameRate;
    if (fps <= 0) return seconds;
    double frameDuration = 1.0 / fps;
    double frames = std::round(seconds / frameDuration);
    return frames * frameDuration;
}
