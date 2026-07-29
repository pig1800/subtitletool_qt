#pragma once
#include <QWidget>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QColor>
#include <vector>
#include <cmath>
#include "models/AudioData.h"
#include "models/SubtitleRow.h"

class WaveformWidget : public QWidget {
    Q_OBJECT
public:
    explicit WaveformWidget(QWidget* parent = nullptr);

    void setAudioData(AudioData* data);
    AudioData* audioData() const { return m_audioData; }

    void setCursor(double seconds);
    double cursor() const { return m_cursorSeconds; }

    void setPlaybackCursor(double seconds);
    void clearPlaybackCursor();

    void setSpectrogramView(bool on) { m_spectrogramView = on; update(); }
    bool isSpectrogramView() const { return m_spectrogramView; }
    void toggleView() { m_spectrogramView = !m_spectrogramView; update(); }

    void setAudioOffset(double seconds) { m_audioOffset = seconds; update(); }
    double audioOffset() const { return m_audioOffset; }

    // Subtitle rows for overlay
    void setSubtitleRows(const std::vector<SubtitleRow>* rows) { m_rows = rows; update(); }

signals:
    void cursorChanged(double seconds);
    void audioFileDropped(const QString& path);
    void mouseAction(Qt::MouseButton button);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void renderWaveform(QPainter& p);
    void renderSpectrogram(QPainter& p);
    void renderSubtitleBlocks(QPainter& p);
    void renderCursor(QPainter& p);
    void initColorMap();

    AudioData* m_audioData = nullptr;
    const std::vector<SubtitleRow>* m_rows = nullptr;
    double m_cursorSeconds = 0.0;
    double m_playbackCursor = -1.0;
    double m_zoomSeconds = 10.0;
    double m_audioOffset = 0.0;
    bool m_spectrogramView = false;
    QColor m_colorMap[256];
};
