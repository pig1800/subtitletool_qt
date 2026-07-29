#include "WaveformWidget.h"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <algorithm>
#include <cmath>

WaveformWidget::WaveformWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(120);
    setMaximumHeight(120);
    setAcceptDrops(true);
    initColorMap();
}

void WaveformWidget::initColorMap()
{
    // Simple black -> blue -> red -> yellow -> green -> white
    struct RGB { double r, g, b; };
    RGB stops[] = {
        {0, 0, 0}, {0, 0, 160}, {255, 0, 0}, {255, 255, 0}, {0, 255, 0}, {255, 255, 255}
    };
    int numStops = 6;
    int segments = numStops - 1;
    for (int seg = 0; seg < segments; ++seg) {
        int startIdx = seg * (256 / segments);
        int endIdx = (seg == segments - 1) ? 256 : (seg + 1) * (256 / segments);
        int steps = endIdx - startIdx;
        for (int j = 0; j < steps; ++j) {
            double p = (steps == 1) ? 1.0 : static_cast<double>(j) / (steps - 1);
            int r = static_cast<int>(stops[seg].r * (1 - p) + stops[seg + 1].r * p);
            int g = static_cast<int>(stops[seg].g * (1 - p) + stops[seg + 1].g * p);
            int b = static_cast<int>(stops[seg].b * (1 - p) + stops[seg + 1].b * p);
            m_colorMap[startIdx + j] = QColor(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255));
        }
    }
}

void WaveformWidget::setAudioData(AudioData* data)
{
    m_audioData = data;
    update();
}

void WaveformWidget::setCursor(double seconds)
{
    m_cursorSeconds = seconds;
    update();
}

void WaveformWidget::setPlaybackCursor(double seconds)
{
    m_playbackCursor = seconds;
    update();
}

void WaveformWidget::clearPlaybackCursor()
{
    m_playbackCursor = -1.0;
    update();
}

void WaveformWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(30, 30, 30));

    if (!m_audioData) {
        p.setPen(Qt::gray);
        p.drawText(10, 20, "No audio loaded");
        return;
    }

    if (m_spectrogramView)
        renderSpectrogram(p);
    else
        renderWaveform(p);

    renderSubtitleBlocks(p);
    renderCursor(p);
}

void WaveformWidget::renderWaveform(QPainter& p)
{
    if (!m_audioData || m_audioData->peaks.empty()) return;

    double pps = width() / m_zoomSeconds;
    double startSec = m_cursorSeconds - m_zoomSeconds / 2.0;
    double audioStart = startSec + m_audioOffset;

    int startIdx = static_cast<int>(audioStart * m_audioData->samplesPerSecond);
    int endIdx = static_cast<int>((audioStart + m_zoomSeconds) * m_audioData->samplesPerSecond);

    double midY = height() / 2.0;
    double scaleY = midY * 0.9;

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 150, 255));

    for (int i = startIdx; i < endIdx; ++i) {
        if (i >= 0 && i < static_cast<int>(m_audioData->peaks.size())) {
            auto& peak = m_audioData->peaks[i];
            double x = (i / m_audioData->samplesPerSecond - m_audioOffset - startSec) * pps;
            double w = pps / m_audioData->samplesPerSecond;
            if (w < 1) w = 1;

            double yTop = midY - peak.max * scaleY;
            double yBot = midY - peak.min * scaleY;

            p.drawRect(QRectF(x, yTop, w, std::max(1.0, yBot - yTop)));
        }
    }
}

void WaveformWidget::renderSpectrogram(QPainter& p)
{
    if (!m_audioData || m_audioData->spectrogram.empty()) return;

    double pps = width() / m_zoomSeconds;
    double startSec = m_cursorSeconds - m_zoomSeconds / 2.0;
    double audioStart = startSec + m_audioOffset;

    int startSlice = static_cast<int>(audioStart * m_audioData->spectrogramSlicesPerSecond);
    int endSlice = static_cast<int>((audioStart + m_zoomSeconds) * m_audioData->spectrogramSlicesPerSecond);

    double sliceWidth = pps / m_audioData->spectrogramSlicesPerSecond;
    if (sliceWidth < 1) sliceWidth = 1;

    int numBins = static_cast<int>(m_audioData->spectrogram[0].size());
    double binHeight = static_cast<double>(height()) / numBins;

    for (int i = startSlice; i < endSlice; ++i) {
        if (i >= 0 && i < static_cast<int>(m_audioData->spectrogram.size())) {
            auto& slice = m_audioData->spectrogram[i];
            double x = (i / m_audioData->spectrogramSlicesPerSecond - m_audioOffset - startSec) * pps;

            for (int j = 0; j < numBins; ++j) {
                float dB = slice[j];
                if (dB < -90) dB = -90;
                if (dB > 0) dB = 0;
                int colorIdx = std::clamp(static_cast<int>(255 * (dB + 90) / 90.0), 0, 255);

                double y = height() - (j + 1) * binHeight;
                p.fillRect(QRectF(x, y, sliceWidth, binHeight + 1), m_colorMap[colorIdx]);
            }
        }
    }
}

void WaveformWidget::renderSubtitleBlocks(QPainter& p)
{
    if (!m_rows) return;

    double pps = width() / m_zoomSeconds;
    double startSec = m_cursorSeconds - m_zoomSeconds / 2.0;

    p.setPen(QPen(QColor(255, 255, 255, 255), 1));
    p.setBrush(QColor(255, 255, 255, 180));

    for (const auto& row : *m_rows) {
        Decimal st, en;
        if (SubtitleRow::tryParseTime(row.start, st) && SubtitleRow::tryParseTime(row.end, en)) {
            if (en > startSec && st < startSec + m_zoomSeconds) {
                double x1 = (st - startSec) * pps;
                double x2 = (en - startSec) * pps;
                double w = std::max(x2 - x1, 1.0);
                p.drawRect(QRectF(x1, height() - 15, w, 15));
            }
        }
    }
}

void WaveformWidget::renderCursor(QPainter& p)
{
    // Center cursor (red)
    double centerX = width() / 2.0;
    p.setPen(QPen(Qt::red, 2));
    p.drawLine(QPointF(centerX, 0), QPointF(centerX, height()));

    // Playback cursor (yellow)
    if (m_playbackCursor >= 0) {
        double pps = width() / m_zoomSeconds;
        double startSec = m_cursorSeconds - m_zoomSeconds / 2.0;
        double playX = (m_playbackCursor - startSec) * pps;
        p.setPen(QPen(Qt::yellow, 2));
        p.drawLine(QPointF(playX, 0), QPointF(playX, height()));
    }
}

void WaveformWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        emit mouseAction(event->button());
        return;
    }
    if (!m_audioData) return;

    double pps = width() / m_zoomSeconds;
    double startSec = m_cursorSeconds - m_zoomSeconds / 2.0;
    double clickedSec = startSec + event->position().x() / pps;

    if (clickedSec < 0) clickedSec = 0;
    double maxSec = m_audioData->totalDurationSeconds - m_audioOffset;
    if (clickedSec > maxSec) clickedSec = std::max(0.0, maxSec);

    m_cursorSeconds = clickedSec;
    update();
    emit cursorChanged(m_cursorSeconds);
}

void WaveformWidget::wheelEvent(QWheelEvent* event)
{
    if (!m_audioData) return;

    bool ctrl = event->modifiers() & Qt::ControlModifier;
    if (ctrl) {
        double factor = event->angleDelta().y() > 0 ? 0.8 : 1.25;
        m_zoomSeconds *= factor;
        m_zoomSeconds = std::clamp(m_zoomSeconds, 0.5, 60.0);
    } else {
        double shift = event->angleDelta().y() > 0 ? -1.0 : 1.0;
        double newSec = m_cursorSeconds + shift;
        double maxSec = m_audioData->totalDurationSeconds - m_audioOffset;
        newSec = std::clamp(newSec, 0.0, std::max(0.0, maxSec));
        m_cursorSeconds = newSec;
    }
    update();
    emit cursorChanged(m_cursorSeconds);
    event->accept();
}

void WaveformWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls()) {
        for (const auto& url : event->mimeData()->urls()) {
            QString path = url.toLocalFile().toLower();
            if (path.endsWith(".m4a") || path.endsWith(".mp3") || path.endsWith(".wav")) {
                event->acceptProposedAction();
                return;
            }
        }
    }
}

void WaveformWidget::dropEvent(QDropEvent* event)
{
    for (const auto& url : event->mimeData()->urls()) {
        QString path = url.toLocalFile();
        QString lower = path.toLower();
        if (lower.endsWith(".m4a") || lower.endsWith(".mp3") || lower.endsWith(".wav")) {
            emit audioFileDropped(path);
            break;
        }
    }
}
