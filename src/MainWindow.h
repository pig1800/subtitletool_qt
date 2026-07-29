#pragma once
#include <QMainWindow>
#include <QTableView>
#include <QListWidget>
#include <QLineEdit>
#include <QLabel>
#include <QScrollArea>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QStyledItemDelegate>
#include <QTimer>
#include <memory>
#include <vector>
#include "models/FileTab.h"
#include "models/AudioData.h"
#include "widgets/WaveformWidget.h"

class FindReplaceDialog;
class RuleCheckDialog;
class SubtitleTableView;

// Delegate that trims whitespace on commit and supports multiline for Source/Target
class SubtitleDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const override;
    void setEditorData(QWidget* editor, const QModelIndex& index) const override;
    void setModelData(QWidget* editor, QAbstractItemModel* model,
                      const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
};

struct SpeakerShortcut {
    QString key;
    QString label;
    QLineEdit* nameEdit = nullptr; // widget in the right panel
    QString name() const { return nameEdit ? nameEdit->text() : QString(); }
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

    // Public access for dialogs
    std::vector<std::unique_ptr<FileTab>>& files() { return m_files; }
    FileTab* activeFile() const { return m_activeFile; }
    void focusFileAndCell(FileTab* file, int rowIndex, const QString& columnHeader);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void openFile();
    void saveFile();
    void saveAllFiles();
    void saveAsFile();
    void exportSrtFile();
    void exportSrtAll();
    void closeFile();
    void closeAllFiles();

    void findReplace();
    void ruleCheck();
    void shiftTime();
    void projectSettings();
    void autoFixDuration();

    void deleteCurrentRow();
    void addBlankNext();
    void splitLine();
    void mergeLine();
    void mergeLineNoSpace();
    void undoStructural();

    void playSelection();
    void toggleView();

    void setStartTime();
    void setEndTime();
    void connectToPrevious();
    void connectToNext();
    void extendStartToCPS();
    void extendEndToCPS();
    void minimizeStartToCPS();
    void minimizeEndToCPS();
    void extendStartToCPSOverwrite();
    void extendEndToCPSOverwrite();

    void saveConfig();
    void loadConfig();

    void onFileListSelectionChanged();
    void onCellClicked(const QModelIndex& index);
    void onWaveformCursorChanged(double seconds);

private:
    void setupUI();
    void setupShortcuts();
    void setActiveFile(FileTab* tab);
    void doOpenFile(const QString& path);
    void doSaveFile(FileTab* tab, const QString& path);
    void doExportSrt(FileTab* tab, const QString& path);
    void updateTitle();
    void focusCell(int row, int col);
    void refreshFileList();
    void applySpeakerShortcut(const QString& key);
    void switchToPreviousFile();
    void switchToNextFile();
    void fixSingleRow(SubtitleRow& row, bool forceMinGap, const QString& nextStartStr);
    void tryLoadAudioForTab(FileTab* tab);
    void loadAudioFile(const QString& audioPath);
    void updateTimeIndicator();
    bool handleMouseAction(Qt::MouseButton btn);

    // Widgets
    QListWidget* m_fileList;
    SubtitleTableView* m_tableView;
    WaveformWidget* m_waveform;
    QLabel* m_timeIndicator;
    QLabel* m_deltaIndicator;

    // State
    std::vector<std::unique_ptr<FileTab>> m_files;
    FileTab* m_activeFile = nullptr;
    bool m_switchingFile = false;
    bool m_bulkLoading = false;
    QString m_lastFileDir;
    QString m_lastConfigDir;
    double m_audioOffsetSeconds = 0.0;
    double m_audioCursor = 0.0;

    // Audio playback
    QMediaPlayer* m_player = nullptr;
    QAudioOutput* m_audioOutput = nullptr;
    QTimer* m_playTimer = nullptr;

    bool m_suppressDirty = false;

    // Dialogs
    FindReplaceDialog* m_findDialog = nullptr;
    RuleCheckDialog* m_ruleCheckDialog = nullptr;

    // Speaker shortcuts
    std::vector<SpeakerShortcut> m_speakerShortcuts;
};
