#pragma once
#include <QString>
#include <QFileInfo>
#include <vector>
#include <stack>
#include "SubtitleRow.h"
#include "SubtitleModel.h"

class AudioData;

class FileTab {
public:
    FileTab() : model(new SubtitleModel) {}
    ~FileTab();

    // Non-copyable, movable
    FileTab(const FileTab&) = delete;
    FileTab& operator=(const FileTab&) = delete;
    FileTab(FileTab&& o) noexcept
        : filePath(std::move(o.filePath)), dirty(o.dirty),
          model(o.model), undoStack(std::move(o.undoStack)),
          cachedAudioData(o.cachedAudioData), cachedAudioPath(std::move(o.cachedAudioPath))
    {
        o.model = nullptr;
        o.cachedAudioData = nullptr;
    }

    QString filePath;
    bool dirty = false;
    SubtitleModel* model = nullptr;
    std::stack<std::vector<SubtitleRow>> undoStack;

    // Audio cache per tab
    AudioData* cachedAudioData = nullptr;
    QString cachedAudioPath;

    QString displayName() const {
        QString name = QFileInfo(filePath).fileName();
        return dirty ? name + "*" : name;
    }

    void saveSnapshot() {
        std::vector<SubtitleRow> snap;
        snap.reserve(model->rows().size());
        for (const auto& r : model->rows())
            snap.push_back(r.clone());
        undoStack.push(std::move(snap));
    }

    void undo() {
        if (undoStack.empty()) return;
        auto snap = std::move(undoStack.top());
        undoStack.pop();
        model->setRows(std::move(snap));
        dirty = true;
    }

    void markDirty() { dirty = true; }
};
