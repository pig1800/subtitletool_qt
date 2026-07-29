#pragma once
#include <QString>
#include <vector>
#include <functional>

struct Peak {
    float min = 0.0f;
    float max = 0.0f;
};

class AudioData {
public:
    QString filePath;
    double totalDurationSeconds = 0.0;
    std::vector<Peak> peaks;
    double samplesPerSecond = 100.0; // 100 peaks per second

    std::vector<std::vector<float>> spectrogram;
    double spectrogramSlicesPerSecond = 0.0;

    static AudioData* loadFromFile(const QString& path, std::function<void(double)> progress = {});
};
