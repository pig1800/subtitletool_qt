#pragma once
#include <QString>

namespace CharCountHelper {

inline double countForCPS(const QString& text)
{
    // Excluded from CPS (but still count toward maxlen):
    //   U+0020 half-width space
    //   U+FF1F ？ fullwidth question mark
    //   U+FF01 ！ fullwidth exclamation
    //   U+2026 … horizontal ellipsis
    // Japanese middle dot (U+30FB ・) still counts.
    int count = 0;
    for (int i = 0; i < text.length(); ++i) {
        ushort u = text.at(i).unicode();
        if (u == 0x0020 || u == 0xFF1F || u == 0xFF01 || u == 0x2026)
            continue;
        ++count;
    }
    return static_cast<double>(count);
}

inline double maxDisplayLineLength(const QString& text)
{
    if (text.isEmpty()) return 0.0;
    double maxLen = 0.0;
    const auto lines = text.split('\n');
    for (const auto& line : lines) {
        QString clean = line.trimmed();
        // Remove trailing \r if present
        if (clean.endsWith('\r'))
            clean.chop(1);
        double len = static_cast<double>(clean.length());
        if (len > maxLen) maxLen = len;
    }
    return maxLen;
}

} // namespace CharCountHelper
