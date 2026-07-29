#include "MainWindow.h"
#include "dialogs/FindReplaceDialog.h"
#include "dialogs/ShiftTimeDialog.h"
#include "dialogs/ProjectSettingsDialog.h"
#include "dialogs/RuleCheckDialog.h"
#include "helpers/ExcelHelper.h"
#include "helpers/LexicographicComparer.h"
#include "helpers/CharCountHelper.h"
#include "helpers/Decimal.h"
#include "models/SubtitleModel.h"

#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QCloseEvent>
#include <QHeaderView>
#include <QScrollBar>
#include <QSplitter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDockWidget>
#include <QPushButton>
#include <QToolBar>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTextBlock>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>
#include <QTimer>
#include <QProgressDialog>
#include <QPlainTextEdit>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QSignalBlocker>
#include <QTextDocument>
#include <QTextStream>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>
#include <climits>

// Persist a last-used directory to QSettings immediately, so load/save
// dialogs resume there across process lifetimes even if the app is killed
// before a clean close (WinForms-style: save at the point of change).
static void persistDirSetting(const QString& key, const QString& dir)
{
    QSettings("subtitletool", "subtitletool").setValue(key, dir);
}

// ── Editor key filter: forwards Ctrl shortcuts to MainWindow ─────
// Lets editors handle Ctrl+C/V/X/A/Z (standard edit ops).
// Everything else with Ctrl goes to the main window.

// ── QLineEdit that never selects-all on focus ──────────────────
class NoSelectLineEdit : public QLineEdit {
public:
    using QLineEdit::QLineEdit;
    void setText(const QString& text) {
        QLineEdit::setText(text);
        deselect();
    }
protected:
    void focusInEvent(QFocusEvent* e) override {
        QWidget::focusInEvent(e);
    }
    void showEvent(QShowEvent* e) override {
        QLineEdit::showEvent(e);
        deselect();
    }
};

class EditorKeyFilter : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject* obj, QEvent* event) override {
        if (event->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(event);
            if (ke->modifiers() & Qt::ControlModifier) {
                int key = ke->key();
                // Let standard edit shortcuts stay in the editor
                // Ctrl+Z stays, but Ctrl+Shift+Z forwards (rows undo)
                bool shift = ke->modifiers() & Qt::ShiftModifier;
                if (key == Qt::Key_C || key == Qt::Key_V || key == Qt::Key_X ||
                    key == Qt::Key_A || (key == Qt::Key_Z && !shift))
                    return false;
                // Forward everything else to the main window
                auto* win = obj->property("mainWindow").value<QWidget*>();
                if (win) {
                    QApplication::sendEvent(win, ke);
                    return true;
                }
            }
        }
        return false;
    }
};

// ── Compact multiline editor that doesn't inflate row height ─────

class CompactTextEdit : public QPlainTextEdit {
public:
    explicit CompactTextEdit(QWidget* parent = nullptr) : QPlainTextEdit(parent) {
        setFrameShape(QFrame::NoFrame);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setTabChangesFocus(true);
        document()->setDocumentMargin(2);
        setStyleSheet("QPlainTextEdit { padding: 0px; margin: 0px; color: black; background: transparent; }");
    }
protected:
    void scrollContentsBy(int /*dx*/, int /*dy*/) override {
        // Never scroll — widget is always sized to fit all lines
    }
    void wheelEvent(QWheelEvent* e) override {
        e->ignore(); // pass to parent (table scrolls instead)
    }
    void paintEvent(QPaintEvent* event) override {
        QPlainTextEdit::paintEvent(event);
        drawLineBreakIndicators();
    }
    QSize minimumSizeHint() const override {
        return QSize(0, fontMetrics().height() + 4);
    }
    QSize sizeHint() const override {
        int lines = qMax(1, document()->blockCount());
        return QSize(100, lines * fontMetrics().lineSpacing() + 4);
    }
private:
    void drawLineBreakIndicators() {
        double maxLen = globalSettings().maxLineLength.toDouble();
        if (maxLen < 1.0) return;
        if (property("column").toInt() != ColTarget) return;
        int step = static_cast<int>(maxLen);

        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(QColor(220, 30, 30), 2));

        QFontMetrics fm(font());
        QPointF offset = contentOffset();
        int docMargin = static_cast<int>(document()->documentMargin());

        for (QTextBlock block = document()->begin(); block != document()->end(); block = block.next()) {
            QString text = block.text();
            int len = text.length();
            if (len <= step) continue;

            QRectF br = blockBoundingGeometry(block).translated(offset);
            int y0 = static_cast<int>(br.top());
            int y1 = static_cast<int>(br.bottom() - 1);

            for (int pos = step; pos < len; pos += step) {
                int x = docMargin + fm.horizontalAdvance(text.left(pos));
                painter.drawLine(x, y0, x, y1);
            }
        }
    }
};

// Helper: set up palette + key filter on any editor widget
static void setupEditor(QWidget* editor) {
    // Cell editors get +1pt font
    QFont f = editor->font();
    f.setPointSize(f.pointSize() + 1);
    editor->setFont(f);

    // Force black text to prevent blue/colored text on some themes
    QPalette pal = editor->palette();
    pal.setColor(QPalette::Text, Qt::black);
    pal.setColor(QPalette::WindowText, Qt::black);
    editor->setPalette(pal);

    // Install key filter to forward Ctrl shortcuts to MainWindow
    QWidget* mainWin = editor;
    while (mainWin && !mainWin->inherits("MainWindow"))
        mainWin = mainWin->parentWidget();
    if (mainWin) {
        editor->setProperty("mainWindow", QVariant::fromValue(mainWin));
        auto* filter = new EditorKeyFilter(editor);
        editor->installEventFilter(filter);
    }
}

// ── Delegate: trim on commit, multiline for Source/Target ────────

QWidget* SubtitleDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                                         const QModelIndex& index) const
{
    int col = index.column();
    if (col == ColSource || col == ColTarget) {
        auto* editor = new CompactTextEdit(parent);
        setupEditor(editor);
        editor->setProperty("column", col);
        // Live sync: push changes to model on every keystroke
        auto persistent = QPersistentModelIndex(index);
        connect(editor, &QPlainTextEdit::textChanged, editor, [editor, persistent]() {
            if (persistent.isValid()) {
                auto* m = const_cast<QAbstractItemModel*>(persistent.model());
                m->setData(persistent, editor->toPlainText(), Qt::EditRole);
            }
        });
        return editor;
    }
    if (col == ColSpeaker) {
        auto* editor = new NoSelectLineEdit(parent);
        editor->setFrame(false);
        editor->setStyleSheet("QLineEdit { background: transparent; }");
        setupEditor(editor);
        auto persistent = QPersistentModelIndex(index);
        connect(editor, &QLineEdit::textChanged, editor, [editor, persistent]() {
            if (persistent.isValid()) {
                auto* m = const_cast<QAbstractItemModel*>(persistent.model());
                m->setData(persistent, editor->text(), Qt::EditRole);
            }
        });
        return editor;
    }
    return QStyledItemDelegate::createEditor(parent, option, index);
}

void SubtitleDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const
{
    int col = index.column();
    if (col == ColSource || col == ColTarget) {
        auto* te = qobject_cast<QPlainTextEdit*>(editor);
        if (te) {
            QString val = index.data(Qt::EditRole).toString();
            if (te->toPlainText() != val) {
                QSignalBlocker blocker(te);
                te->setPlainText(val);
                // Move cursor to start so first line is visible (not scrolled to end)
                auto cursor = te->textCursor();
                cursor.movePosition(QTextCursor::Start);
                te->setTextCursor(cursor);
            }
            return;
        }
    }
    if (col == ColSpeaker) {
        auto* le = qobject_cast<QLineEdit*>(editor);
        if (le) {
            QString val = index.data(Qt::EditRole).toString();
            if (le->text() != val) {
                QSignalBlocker blocker(le);
                le->setText(val);
                le->deselect();
            }
            // Apply foreground color from model (e.g. blue for consecutive speaker)
            QVariant fg = index.data(Qt::ForegroundRole);
            QPalette pal = le->palette();
            pal.setColor(QPalette::Text, fg.isValid() ? fg.value<QColor>() : Qt::black);
            le->setPalette(pal);
            return;
        }
    }
    QStyledItemDelegate::setEditorData(editor, index);
}

void SubtitleDelegate::setModelData(QWidget* editor, QAbstractItemModel* model,
                                     const QModelIndex& index) const
{
    int col = index.column();
    if (col == ColSource || col == ColTarget) {
        auto* te = qobject_cast<QPlainTextEdit*>(editor);
        if (te) {
            model->setData(index, te->toPlainText().trimmed(), Qt::EditRole);
            return;
        }
    }
    auto* le = qobject_cast<QLineEdit*>(editor);
    if (le) {
        model->setData(index, le->text().trimmed(), Qt::EditRole);
        return;
    }
    QStyledItemDelegate::setModelData(editor, model, index);
}

QSize SubtitleDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    auto base = QStyledItemDelegate::sizeHint(option, index);
    int col = index.column();
    if (col == ColSource || col == ColTarget) {
        QString text = index.data(Qt::DisplayRole).toString();
        // Match CompactTextEdit exactly: +1pt font, QTextDocument layout,
        // and 4px doc margin (setDocumentMargin(2) * 2 sides).
        QFont f = option.font;
        f.setPointSize(f.pointSize() + 1);
        int availWidth = option.rect.width() - 4;
        if (availWidth < 50) availWidth = 50;
        QTextDocument doc;
        doc.setDefaultFont(f);
        doc.setTextWidth(availWidth);
        doc.setPlainText(text);
        int h = qCeil(doc.size().height()) + 4;
        return QSize(base.width(), std::max(base.height(), h));
    }
    return base;
}

void SubtitleDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                             const QModelIndex& index) const
{
    int col = index.column();
    // Columns with persistent editors: paint only background, no text
    // (the editor widget renders the text; painting it here causes double-render)
    if (col == ColSpeaker || col == ColSource || col == ColTarget) {
        QVariant bg = index.data(Qt::BackgroundRole);
        painter->fillRect(option.rect, bg.isValid() ? bg.value<QColor>() : option.palette.base().color());
        return;
    }
    QStyledItemDelegate::paint(painter, option, index);
}

// ── Frame-based arithmetic helpers ──────────────────────────────
// When fps > 0, all time calculations go through integer frame counts
// to guarantee frame-exact results with zero accumulated rounding.

static inline long long secToFrame(double seconds, double fps)
{
    return std::llround(seconds * fps);
}

static inline double frameToSec(long long frame, double fps)
{
    return static_cast<double>(frame) / fps;
}

static inline QString frameToTime(long long frame, double fps)
{
    return SubtitleRow::formatTime(frameToSec(frame, fps));
}

static inline long long timeToFrame(const QString& t, double fps)
{
    Decimal s;
    SubtitleRow::tryParseTime(t, s);
    return secToFrame(s, fps);
}

static inline long long ceilFrames(double seconds, double fps)
{
    return static_cast<long long>(std::ceil(seconds * fps));
}

static inline long long floorFrames(double seconds, double fps)
{
    return static_cast<long long>(std::floor(seconds * fps));
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("Subtitle Editor");
    setAcceptDrops(true);
    qApp->installEventFilter(this);

    m_player = new QMediaPlayer(this);
    m_audioOutput = new QAudioOutput(this);
    m_audioOutput->setVolume(1.0);
    m_player->setAudioOutput(m_audioOutput);

    // Lower update frequency to reduce UI thread pressure during playback
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        if (state == QMediaPlayer::StoppedState) {
            if (m_playTimer) { m_playTimer->stop(); }
            m_waveform->clearPlaybackCursor();
            updateTimeIndicator();
        }
    });

    setupUI();
    setupShortcuts();

    // Restore last-used directories for load/save dialogs (persisted across runs).
    QSettings settings("subtitletool", "subtitletool");
    QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    m_lastFileDir = settings.value("lastFileDir", docs).toString();
    m_lastConfigDir = settings.value("lastConfigDir", docs).toString();
}

MainWindow::~MainWindow()
{
    m_player->stop();
}

void MainWindow::setupUI()
{
    auto* central = new QWidget;
    setCentralWidget(central);

    auto* mainLayout = new QHBoxLayout(central);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Left: File list
    m_fileList = new QListWidget;
    m_fileList->setFixedWidth(160);
    m_fileList->setAcceptDrops(true);
    // Keep selection highlight visible even when list loses focus
    QPalette flPal = m_fileList->palette();
    flPal.setColor(QPalette::Inactive, QPalette::Highlight, flPal.color(QPalette::Active, QPalette::Highlight));
    flPal.setColor(QPalette::Inactive, QPalette::HighlightedText, flPal.color(QPalette::Active, QPalette::HighlightedText));
    m_fileList->setPalette(flPal);
    mainLayout->addWidget(m_fileList);
    connect(m_fileList, &QListWidget::currentRowChanged, this, &MainWindow::onFileListSelectionChanged);

    // Center: Waveform + Table + Toolbar
    auto* centerLayout = new QVBoxLayout;

    // Waveform area — indicators overlay on top-right of waveform
    m_waveform = new WaveformWidget;
    m_waveform->setFixedHeight(120);

    m_timeIndicator = new QLabel("00:00:00,000 / 00:00:00,000", m_waveform);
    m_timeIndicator->setStyleSheet("background: rgba(0,0,0,200); color: lightgray; padding: 4px; font-family: Consolas; font-size: 8pt;");
    m_timeIndicator->adjustSize();
    m_timeIndicator->move(m_waveform->width() - m_timeIndicator->width() - 4, 4);

    m_deltaIndicator = new QLabel("0.000", m_waveform);
    m_deltaIndicator->setStyleSheet("background: rgba(0,0,0,200); color: cyan; padding: 4px; font-family: Consolas; font-size: 8pt;");
    m_deltaIndicator->adjustSize();
    m_deltaIndicator->move(m_waveform->width() - m_deltaIndicator->width() - 4,
                           4 + m_timeIndicator->height() + 1);

    // Reposition indicators when waveform resizes
    connect(m_waveform, &WaveformWidget::cursorChanged, this, [this](double) {
        m_timeIndicator->adjustSize();
        m_timeIndicator->move(m_waveform->width() - m_timeIndicator->width() - 4, 4);
        m_deltaIndicator->adjustSize();
        m_deltaIndicator->move(m_waveform->width() - m_deltaIndicator->width() - 4,
                               4 + m_timeIndicator->height() + 1);
    });

    centerLayout->addWidget(m_waveform);

    connect(m_waveform, &WaveformWidget::cursorChanged, this, &MainWindow::onWaveformCursorChanged);
    connect(m_waveform, &WaveformWidget::audioFileDropped, this, &MainWindow::loadAudioFile);
    // Mouse button shortcuts are handled globally via eventFilter

    // Table view
    m_tableView = new QTableView;
    m_tableView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tableView->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_tableView->verticalHeader()->setVisible(false);
    m_tableView->horizontalHeader()->setStretchLastSection(false);
    m_tableView->setAcceptDrops(true);
    m_tableView->setWordWrap(true);
    m_tableView->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_tableView->setEditTriggers(QAbstractItemView::AllEditTriggers); // single-click edit
    m_tableView->setItemDelegate(new SubtitleDelegate(m_tableView));
    m_tableView->installEventFilter(this);
    centerLayout->addWidget(m_tableView, 1);

    connect(m_tableView, &QTableView::clicked, this, &MainWindow::onCellClicked);

    // Recalculate row heights when source/target column widths change (e.g. window resize),
    // because word-wrapping may have changed the number of visual lines.
    connect(m_tableView->horizontalHeader(), &QHeaderView::sectionResized,
            this, [this](int logicalIndex, int, int) {
        if ((logicalIndex == ColSource || logicalIndex == ColTarget) && m_activeFile && m_activeFile->model)
            for (int r = 0; r < m_activeFile->model->rowCount(); ++r)
                m_tableView->resizeRowToContents(r);
    });

    // Toolbar
    auto* toolbar = new QWidget;
    auto* tbLayout = new QHBoxLayout(toolbar);
    tbLayout->setContentsMargins(4, 2, 4, 2);
    tbLayout->setSpacing(2);

    auto addBtn = [&](const QString& text, auto slot) {
        auto* btn = new QPushButton(text);
        btn->setFont(QFont(QApplication::font().family(), 8));
        btn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
        btn->setStyleSheet("padding: 2px 4px;");
        connect(btn, &QPushButton::clicked, this, slot);
        tbLayout->addWidget(btn);
        return btn;
    };
    auto addSep = [&]() { tbLayout->addSpacing(6); };

    addBtn("Open", &MainWindow::openFile);
    addBtn("Save", &MainWindow::saveFile);
    addBtn("Save All", &MainWindow::saveAllFiles);
    addBtn("Save As", &MainWindow::saveAsFile);
    addBtn("Export SRT", &MainWindow::exportSrtFile);
    addBtn("Export SRT All", &MainWindow::exportSrtAll);
    addBtn("Close", &MainWindow::closeFile);
    addBtn("Close All", &MainWindow::closeAllFiles);
    addSep();
    addBtn("Find", &MainWindow::findReplace);
    addBtn("Auto Fix Duration", &MainWindow::autoFixDuration);
    addBtn("Shift Time", &MainWindow::shiftTime);
    addBtn("Project Settings", &MainWindow::projectSettings);
    addBtn("Rule Check", &MainWindow::ruleCheck);
    addSep();
    addBtn("Undo", &MainWindow::undoStructural);
    addBtn("Delete Line", &MainWindow::deleteCurrentRow);
    addBtn("Add Blank Next", &MainWindow::addBlankNext);
    addBtn("Split Line", &MainWindow::splitLine);
    addBtn("Merge Line", &MainWindow::mergeLine);
    addSep();
    addBtn("Play Selection", &MainWindow::playSelection);
    addBtn("Toggle View", &MainWindow::toggleView);
    addSep();
    addBtn("Set Start", &MainWindow::setStartTime);
    addBtn("Set End", &MainWindow::setEndTime);
    tbLayout->addStretch();

    centerLayout->addWidget(toolbar);
    mainLayout->addLayout(centerLayout, 1);

    // Right: Speaker shortcuts
    auto* rightPanel = new QWidget;
    rightPanel->setFixedWidth(200);
    auto* rightLayout = new QVBoxLayout(rightPanel);

    auto* scrollArea = new QScrollArea;
    auto* shortcutContainer = new QWidget;
    auto* shortcutLayout = new QVBoxLayout(shortcutContainer);
    shortcutLayout->setSpacing(0);
    shortcutLayout->setContentsMargins(4, 2, 4, 2);

    auto shortcuts = std::vector<std::pair<QString, QString>>{
        {"1", "Ctrl+1"}, {"2", "Ctrl+2"}, {"3", "Ctrl+3"},
        {"4", "Ctrl+4"}, {"5", "Ctrl+5"}, {"6", "Ctrl+6"},
        {"7", "Ctrl+7"}, {"8", "Ctrl+8"}, {"9", "Ctrl+9"},
        {"0", "Ctrl+0"}, {"Period", "Ctrl+."}, {"Slash", "Ctrl+/"},
        {"Multiply", "Ctrl+*"}, {"Minus", "Ctrl+-"}, {"Plus", "Ctrl++"},
        {"F1", "Ctrl+F1"}, {"F2", "Ctrl+F2"}, {"F3", "Ctrl+F3"},
        {"F4", "Ctrl+F4"}, {"F5", "Ctrl+F5"}, {"F6", "Ctrl+F6"},
        {"F7", "Ctrl+F7"}, {"F8", "Ctrl+F8"}, {"F9", "Ctrl+F9"},
        {"F10", "Ctrl+F10"}, {"F11", "Ctrl+F11"}, {"F12", "Ctrl+F12"}
    };

    for (auto& [key, label] : shortcuts) {
        auto* row = new QHBoxLayout;
        auto* lbl = new QLabel(label);
        lbl->setFixedWidth(55);
        lbl->setFont(QFont(QApplication::font().family(), 10));
        auto* edit = new QLineEdit;
        edit->setFont(QFont(QApplication::font().family(), 9));
        edit->setFixedHeight(edit->fontMetrics().height() + 6);
        row->addWidget(lbl);
        row->addWidget(edit);
        shortcutLayout->addLayout(row);
        m_speakerShortcuts.push_back({key, label, edit});
    }
    shortcutLayout->addStretch();

    scrollArea->setWidget(shortcutContainer);
    scrollArea->setWidgetResizable(true);
    rightLayout->addWidget(scrollArea, 1);

    // Keyboard reference
    auto* helpLabel = new QLabel(
        "Ctrl+O: Open\n"
        "Ctrl+S: Save\n"
        "Ctrl+Shift+A: Save All\n"
        "Ctrl+Shift+S: Save As\n"
        "Ctrl+E: Export SRT\n"
        "Ctrl+Shift+E: Export SRT All\n"
        "Ctrl+W: Close\n"
        "Ctrl+Shift+W: Close All\n"
        "Ctrl+PgUp/Dn: Prev/Next File\n"
        "\n"
        "Ctrl+F: Find\n"
        "Ctrl+Shift+Z: Rows Undo\n"
        "Ctrl+Del: Delete Row\n"
        "Ctrl+J: Merge with Space\n"
        "Ctrl+Shift+J: Merge No Space\n"
        "Ctrl+L: Add Blank Next\n"
        "Ctrl+K or Mouse3: Split Line\n"
        "Ctrl+I: Shift Time\n"
        "Ctrl+P or Mouse2: Play Selection\n"
        "Ctrl+G: Toggle View\n"
        "Ctrl+[ or Mouse5: Set Start Time\n"
        "Ctrl+] or Mouse4: Set End Time\n"
        "Ctrl+Shift+[: Connect Prev\n"
        "Ctrl+Shift+]: Connect Next\n"
        "Ctrl+Alt+Shift+[/Left: Extend Start\n"
        "Ctrl+Alt+Shift+]/Right: Extend End\n"
        "Ctrl+Alt+Shift+-/+: Minimize CPS\n"
        "Ctrl+Alt+Shift+;/': Extend Overwrite\n"
        "Ctrl+Up/Dn: Prev/Next Sub\n"
        "\n"
        "Ctrl+1..0/F1..F12: Speaker"
    );
    helpLabel->setFont(QFont(QApplication::font().family(), 8));
    helpLabel->setStyleSheet("color: gray;");
    rightLayout->addWidget(helpLabel);

    auto* saveCfgBtn = new QPushButton("Save Config");
    auto* loadCfgBtn = new QPushButton("Load Config");
    connect(saveCfgBtn, &QPushButton::clicked, this, &MainWindow::saveConfig);
    connect(loadCfgBtn, &QPushButton::clicked, this, &MainWindow::loadConfig);
    rightLayout->addWidget(saveCfgBtn);
    rightLayout->addWidget(loadCfgBtn);

    mainLayout->addWidget(rightPanel);
}

void MainWindow::setupShortcuts()
{
    // Shortcuts are handled in keyPressEvent
}

// ── File Operations ─────────────────────────────────────────────

void MainWindow::openFile()
{
    auto paths = QFileDialog::getOpenFileNames(this, "Open", m_lastFileDir, "Excel Files (*.xlsx)");
    if (paths.isEmpty()) return;

    m_lastFileDir = QFileInfo(paths.first()).absolutePath();
    persistDirSetting("lastFileDir", m_lastFileDir);
    m_bulkLoading = true;
    for (const auto& path : paths)
        doOpenFile(path);
    m_bulkLoading = false;

    if (!m_files.empty())
        setActiveFile(m_files.back().get());
}

void MainWindow::doOpenFile(const QString& path)
{
    QString fullPath = QFileInfo(path).absoluteFilePath();

    // Check if already open
    for (auto& f : m_files) {
        if (QFileInfo(f->filePath).absoluteFilePath() == fullPath) {
            setActiveFile(f.get());
            return;
        }
    }

    try {
        auto rows = ExcelHelper::readXlsx(fullPath);
        auto tab = std::make_unique<FileTab>();
        tab->filePath = fullPath;
        tab->model->setRows(std::move(rows));

        // Insert in lexicographic order
        int insertIdx = 0;
        while (insertIdx < static_cast<int>(m_files.size()) &&
               lexicographicCompare(tab->displayName(), m_files[insertIdx]->displayName()) > 0) {
            ++insertIdx;
        }

        auto* rawPtr = tab.get();
        m_files.insert(m_files.begin() + insertIdx, std::move(tab));
        refreshFileList();

        if (!m_bulkLoading)
            setActiveFile(rawPtr);
    } catch (const std::exception& ex) {
        QMessageBox::critical(this, "Error", QString("Failed to open file:\n%1").arg(ex.what()));
    }
}

void MainWindow::saveFile()
{
    if (!m_activeFile) return;
    if (m_activeFile->filePath.isEmpty() || !QFile::exists(m_activeFile->filePath)) {
        saveAsFile();
        return;
    }
    doSaveFile(m_activeFile, m_activeFile->filePath);
}

void MainWindow::doSaveFile(FileTab* tab, const QString& path)
{
    try {
        ExcelHelper::writeXlsx(path, tab->model->rows());
        tab->filePath = path;
        tab->dirty = false;
        refreshFileList();
        updateTitle();
    } catch (const std::exception& ex) {
        QMessageBox::critical(this, "Error", QString("Failed to save file:\n%1").arg(ex.what()));
    }
}

void MainWindow::saveAsFile()
{
    if (!m_activeFile) return;
    auto path = QFileDialog::getSaveFileName(this, "Save As", m_lastFileDir, "Excel Files (*.xlsx)");
    if (path.isEmpty()) return;
    m_lastFileDir = QFileInfo(path).absolutePath();
    persistDirSetting("lastFileDir", m_lastFileDir);
    doSaveFile(m_activeFile, path);
}

void MainWindow::doExportSrt(FileTab* tab, const QString& path)
{
    // Open in binary (no QIODevice::Text): we emit literal CRLF below, and the
    // Text flag would re-translate "\n"->"\r\n" on Windows, producing "\r\r\n".
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::critical(this, "Error",
            QString("Failed to export SRT:\n%1").arg(file.errorString()));
        return;
    }

    QTextStream out(&file);
    out.setGenerateByteOrderMark(false);

    int index = 0;
    for (const auto& row : tab->model->rows()) {
        Decimal s, e;
        if (!SubtitleRow::tryParseTime(row.start, s) ||
            !SubtitleRow::tryParseTime(row.end, e) || e <= s)
            continue;

        QString text = row.target.trimmed();
        if (text.isEmpty()) text = row.source.trimmed();
        if (text.isEmpty()) continue;

        ++index;
        out << index << "\r\n"
            << SubtitleRow::formatTime(s.toDouble()) << " --> "
            << SubtitleRow::formatTime(e.toDouble()) << "\r\n"
            << text << "\r\n\r\n";
    }
}

void MainWindow::exportSrtFile()
{
    if (!m_activeFile) return;
    // Export only works once the subtitle has a saved Excel file. For an
    // untitled file, prompt to save first; if the save is cancelled, abort
    // the export too.
    if (m_activeFile->filePath.isEmpty() || !QFile::exists(m_activeFile->filePath)) {
        saveFile();
        if (m_activeFile->filePath.isEmpty() || !QFile::exists(m_activeFile->filePath))
            return;
    }
    QString dir = QFileInfo(m_activeFile->filePath).absolutePath();
    QString baseName = QFileInfo(m_activeFile->filePath).completeBaseName();
    doExportSrt(m_activeFile, QDir(dir).filePath(baseName + ".srt"));
}

void MainWindow::exportSrtAll()
{
    if (m_files.empty()) return;

    // No destination prompt: each file exports next to its own saved Excel
    // file. Untitled files are saved first; if a save is cancelled, that file
    // is skipped. Only the final count is reported.
    int exported = 0;
    for (auto& f : m_files) {
        if (f->filePath.isEmpty() || !QFile::exists(f->filePath)) {
            setActiveFile(f.get());
            saveFile();
            if (f->filePath.isEmpty() || !QFile::exists(f->filePath))
                continue;
        }
        QString dir = QFileInfo(f->filePath).absolutePath();
        QString baseName = QFileInfo(f->filePath).completeBaseName();
        doExportSrt(f.get(), QDir(dir).filePath(baseName + ".srt"));
        ++exported;
    }
    QMessageBox::information(this, "Export SRT All",
        QString("Exported %1 file(s).").arg(exported));
}

void MainWindow::saveAllFiles()
{
    for (auto& f : m_files) {
        if (f->dirty) {
            if (f->filePath.isEmpty() || !QFile::exists(f->filePath)) {
                setActiveFile(f.get());
                saveFile();
            } else {
                doSaveFile(f.get(), f->filePath);
            }
        }
    }
}

void MainWindow::closeFile()
{
    if (!m_activeFile) return;

    if (m_activeFile->dirty) {
        auto result = QMessageBox::question(this, "Save Changes",
            QString("Save changes to %1?").arg(QFileInfo(m_activeFile->filePath).fileName()),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);

        if (result == QMessageBox::Cancel) return;
        if (result == QMessageBox::Yes) {
            saveFile();
            if (m_activeFile->dirty) return;
        }
    }

    int idx = -1;
    for (int i = 0; i < static_cast<int>(m_files.size()); ++i) {
        if (m_files[i].get() == m_activeFile) { idx = i; break; }
    }
    if (idx < 0) return;

    m_files.erase(m_files.begin() + idx);
    refreshFileList();

    if (!m_files.empty()) {
        int next = std::min(idx, static_cast<int>(m_files.size()) - 1);
        setActiveFile(m_files[next].get());
    } else {
        setActiveFile(nullptr);
    }
}

void MainWindow::closeAllFiles()
{
    // Close clean files first
    m_files.erase(std::remove_if(m_files.begin(), m_files.end(),
        [](const auto& f) { return !f->dirty; }), m_files.end());

    // Handle dirty files
    while (!m_files.empty()) {
        setActiveFile(m_files.front().get());
        auto* before = m_activeFile;
        closeFile();
        if (m_activeFile == before) return; // user cancelled
    }

    setActiveFile(nullptr);
    refreshFileList();
}

void MainWindow::setActiveFile(FileTab* tab)
{
    m_player->stop();
    m_waveform->clearPlaybackCursor();
    m_activeFile = tab;
    m_switchingFile = true;
    m_audioCursor = 0.0;

    if (tab) {
        m_tableView->setModel(tab->model);

        // Set column widths
        m_tableView->setColumnWidth(ColIndex, 40);
        m_tableView->setColumnWidth(ColStart, 90);
        m_tableView->setColumnWidth(ColEnd, 90);
        m_tableView->setColumnWidth(ColDuration, 60);
        m_tableView->setColumnWidth(ColGap, 50);
        m_tableView->setColumnWidth(ColCPS, 50);
        m_tableView->setColumnWidth(ColSpeaker, 100);
        m_tableView->setColumnWidth(ColMaxLen, 50);

        // Stretch Source and Target
        auto* header = m_tableView->horizontalHeader();
        header->setSectionResizeMode(ColSource, QHeaderView::Stretch);
        header->setSectionResizeMode(ColTarget, QHeaderView::Stretch);

        m_waveform->setSubtitleRows(&tab->model->rows());

        // Open persistent editors for editable columns
        reopenAllPersistentEditors();

        // Track current cell changes (works even when clicking persistent editors)
        connect(m_tableView->selectionModel(), &QItemSelectionModel::currentChanged,
                this, [this](const QModelIndex& current, const QModelIndex&) {
            onCellClicked(current);
        });

        // Disconnect any prior connections from this model to us
        disconnect(tab->model, &QAbstractItemModel::rowsInserted, this, nullptr);
        disconnect(tab->model, &QAbstractItemModel::modelReset, this, nullptr);

        // Reopen persistent editors when rows are inserted
        connect(tab->model, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex&, int first, int last) {
                openPersistentEditors(first, last);
            });

        // Reopen all persistent editors after model reset (e.g. undo)
        connect(tab->model, &QAbstractItemModel::modelReset, this,
            &MainWindow::reopenAllPersistentEditors);

        // Select first cell
        if (tab->model->rowCount() > 0)
            focusCell(0, ColStart);

        // Connect model changes to dirty tracking
        disconnect(tab->model, &QAbstractItemModel::dataChanged, this, nullptr);
        connect(tab->model, &QAbstractItemModel::dataChanged, this,
            [this, tab](const QModelIndex& topLeft, const QModelIndex& bottomRight, const QList<int>& roles) {
            // Don't mark dirty for display-only changes (highlight row, etc.)
            if (m_suppressDirty) return;
            if (!roles.isEmpty() && !roles.contains(Qt::EditRole) && !roles.contains(Qt::DisplayRole))
                return;
            tab->markDirty();
            refreshFileList();
            updateTitle();
            m_waveform->update();
            // Row heights may need updating when multiline source/target text changes
            for (int r = topLeft.row(); r <= bottomRight.row(); ++r)
                m_tableView->resizeRowToContents(r);
        });

        tryLoadAudioForTab(tab);
    } else {
        m_tableView->setModel(nullptr);
        m_waveform->setAudioData(nullptr);
        m_waveform->setSubtitleRows(nullptr);
    }

    // Update file list selection
    for (int i = 0; i < m_fileList->count(); ++i) {
        if (i < static_cast<int>(m_files.size()) && m_files[i].get() == tab) {
            m_fileList->setCurrentRow(i);
            break;
        }
    }

    m_switchingFile = false;
    updateTitle();
    m_waveform->update();
    updateTimeIndicator();
}

void MainWindow::tryLoadAudioForTab(FileTab* tab)
{
    if (!tab) return;

    // Restore from cache
    if (tab->cachedAudioData) {
        m_waveform->setAudioData(tab->cachedAudioData);
        if (!tab->cachedAudioPath.isEmpty())
            m_player->setSource(QUrl::fromLocalFile(tab->cachedAudioPath));
        return;
    }

    // Try to find matching audio file
    QString fileName = QFileInfo(tab->filePath).fileName();
    QRegularExpression re("^(\\d+)");
    auto match = re.match(fileName);
    if (match.hasMatch()) {
        QString num = match.captured(1);
        QString dir = QFileInfo(tab->filePath).absolutePath();
        QStringList paths = {
            QDir(dir + "/../m4a").absoluteFilePath(num + ".m4a"),
            QDir(dir + "/../m4a").absoluteFilePath(num + ".wav"),
            QDir(dir + "/../wav").absoluteFilePath(num + ".wav"),
        };
        for (const auto& audioPath : paths) {
            if (QFile::exists(audioPath)) {
                loadAudioFile(audioPath);
                return;
            }
        }
    }
    m_waveform->setAudioData(nullptr);
}

void MainWindow::loadAudioFile(const QString& audioPath)
{
    m_player->stop();

    // Dispose existing cache on active tab if loading new audio
    if (m_activeFile) {
        delete m_activeFile->cachedAudioData;
        m_activeFile->cachedAudioData = nullptr;
        m_activeFile->cachedAudioPath.clear();
    }

    m_waveform->setAudioData(nullptr);
    m_waveform->clearPlaybackCursor();

    // Show progress dialog
    QProgressDialog progress("Processing audio data...", QString(), 0, 100, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.show();

    auto* data = AudioData::loadFromFile(audioPath, [&progress](double pct) {
        progress.setValue(static_cast<int>(pct));
        QApplication::processEvents();
    });

    progress.close();

    if (data) {
        m_waveform->setAudioData(data);
        m_player->setSource(QUrl::fromLocalFile(audioPath));

        if (m_activeFile) {
            m_activeFile->cachedAudioData = data;
            m_activeFile->cachedAudioPath = audioPath;
        }
    }
}

void MainWindow::refreshFileList()
{
    m_fileList->clear();
    for (const auto& f : m_files)
        m_fileList->addItem(f->displayName());
}

void MainWindow::updateTitle()
{
    if (m_activeFile)
        setWindowTitle(QString("Subtitle Editor - %1").arg(m_activeFile->displayName()));
    else
        setWindowTitle("Subtitle Editor");
}

void MainWindow::openPersistentEditors(int firstRow, int lastRow)
{
    if (!m_activeFile || !m_activeFile->model) return;
    static const int cols[] = { ColSpeaker, ColSource, ColTarget };
    for (int r = firstRow; r <= lastRow; ++r) {
        for (int c : cols)
            m_tableView->openPersistentEditor(m_activeFile->model->index(r, c));
    }
}

void MainWindow::reopenAllPersistentEditors()
{
    if (!m_activeFile || !m_activeFile->model) return;
    int rows = m_activeFile->model->rowCount();
    if (rows > 0)
        openPersistentEditors(0, rows - 1);
}

void MainWindow::focusCell(int row, int col)
{
    if (!m_activeFile || !m_activeFile->model) return;
    if (row < 0 || row >= m_activeFile->model->rowCount()) return;

    auto idx = m_activeFile->model->index(row, col);
    m_tableView->setCurrentIndex(idx);
    m_tableView->scrollTo(idx);

    // Ensure highlight row is in sync and editor has keyboard focus
    m_activeFile->model->setHighlightRow(row);
    if (auto* editor = m_tableView->indexWidget(idx))
        editor->setFocus();

    // Update audio cursor
    auto& rows = m_activeFile->model->rows();
    if (row < static_cast<int>(rows.size())) {
        Decimal st;
        if (SubtitleRow::tryParseTime(rows[row].start, st)) {
            m_audioCursor = st;
            m_waveform->setCursor(st);
            updateTimeIndicator();
        }
    }
}

void MainWindow::focusFileAndCell(FileTab* file, int rowIndex, const QString& columnHeader)
{
    if (m_activeFile != file)
        setActiveFile(file);

    int colIdx = ColStart;
    if (columnHeader == "Start") colIdx = ColStart;
    else if (columnHeader == "End") colIdx = ColEnd;
    else if (columnHeader == "Duration") colIdx = ColDuration;
    else if (columnHeader == "Gap") colIdx = ColGap;
    else if (columnHeader == "CPS") colIdx = ColCPS;
    else if (columnHeader == "Speaker") colIdx = ColSpeaker;
    else if (columnHeader == "Source") colIdx = ColSource;
    else if (columnHeader == "Target") colIdx = ColTarget;
    else if (columnHeader == "MaxLen") colIdx = ColMaxLen;

    QTimer::singleShot(0, this, [this, rowIndex, colIdx]() {
        focusCell(rowIndex, colIdx);
    });
}

void MainWindow::onFileListSelectionChanged()
{
    if (m_switchingFile) return;
    int row = m_fileList->currentRow();
    if (row >= 0 && row < static_cast<int>(m_files.size()))
        setActiveFile(m_files[row].get());
}

void MainWindow::onCellClicked(const QModelIndex& index)
{
    if (!m_activeFile || !index.isValid()) return;

    // Highlight current row, clear selections on previous row's editors
    int prevRow = m_activeFile->model->highlightRow();
    m_activeFile->model->setHighlightRow(index.row());

    if (prevRow >= 0 && prevRow != index.row()) {
        static const int cols[] = { ColSpeaker, ColSource, ColTarget };
        for (int c : cols) {
            auto* w = m_tableView->indexWidget(m_activeFile->model->index(prevRow, c));
            if (!w) continue;
            if (auto* le = qobject_cast<QLineEdit*>(w))
                le->deselect();
            else if (auto* te = qobject_cast<QPlainTextEdit*>(w)) {
                auto cursor = te->textCursor();
                cursor.clearSelection();
                te->setTextCursor(cursor);
            }
        }
    }

    auto& rows = m_activeFile->model->rows();
    if (index.row() < static_cast<int>(rows.size())) {
        Decimal st;
        if (SubtitleRow::tryParseTime(rows[index.row()].start, st)) {
            m_audioCursor = st;
            m_waveform->setCursor(st);
            updateTimeIndicator();
        }
    }
}

void MainWindow::onWaveformCursorChanged(double seconds)
{
    m_audioCursor = seconds;
    updateTimeIndicator();
}

void MainWindow::updateTimeIndicator()
{
    auto* ad = m_waveform->audioData();
    if (!ad) {
        m_timeIndicator->setText("00:00:00,000 / 00:00:00,000");
        m_timeIndicator->adjustSize();
        m_timeIndicator->move(m_waveform->width() - m_timeIndicator->width() - 4, 4);
        m_deltaIndicator->setText("0.000");
        m_deltaIndicator->adjustSize();
        m_deltaIndicator->move(m_waveform->width() - m_deltaIndicator->width() - 4,
                               4 + m_timeIndicator->height() + 1);
        return;
    }

    double totalDur = ad->totalDurationSeconds - m_audioOffsetSeconds;
    if (totalDur < 0) totalDur = 0;
    m_timeIndicator->setText(
        SubtitleRow::formatTime(m_audioCursor) + " / " + SubtitleRow::formatTime(totalDur));
    m_timeIndicator->adjustSize();
    m_timeIndicator->move(m_waveform->width() - m_timeIndicator->width() - 4, 4);

    // Delta from current row's start
    auto idx = m_tableView->currentIndex();
    if (idx.isValid() && m_activeFile) {
        auto& rows = m_activeFile->model->rows();
        if (idx.row() < static_cast<int>(rows.size())) {
            Decimal st;
            if (SubtitleRow::tryParseTime(rows[idx.row()].start, st)) {
                double delta = m_audioCursor - st;
                m_deltaIndicator->setText(QString::number(delta, 'f', 3));
                m_deltaIndicator->adjustSize();
                m_deltaIndicator->move(m_waveform->width() - m_deltaIndicator->width() - 4,
                                       4 + m_timeIndicator->height() + 1);
                return;
            }
        }
    }
    m_deltaIndicator->setText("0.000");
    m_deltaIndicator->adjustSize();
    m_deltaIndicator->move(m_waveform->width() - m_deltaIndicator->width() - 4,
                           4 + m_timeIndicator->height() + 1);
}

// ── Row Operations ──────────────────────────────────────────────

void MainWindow::deleteCurrentRow()
{
    if (!m_activeFile) return;
    auto idx = m_tableView->currentIndex();
    if (!idx.isValid()) return;

    m_activeFile->saveSnapshot();
    int row = idx.row();
    int col = idx.column();
    m_activeFile->model->removeRow(row);
    m_activeFile->markDirty();
    refreshFileList();

    if (m_activeFile->model->rowCount() > 0)
        focusCell(std::min(row, m_activeFile->model->rowCount() - 1), col);
}

void MainWindow::addBlankNext()
{
    if (!m_activeFile) return;
    auto idx = m_tableView->currentIndex();
    if (!idx.isValid()) return;

    m_activeFile->saveSnapshot();
    auto& rows = m_activeFile->model->rows();
    int i = idx.row();
    auto& cur = rows[i];
    auto& g = globalSettings();

    SubtitleRow newRow;
    newRow.speaker = cur.speaker;

    double fps = g.frameRate;
    if (fps > 0) {
        long long curEndFrame = timeToFrame(cur.end, fps);
        long long gapFrames = ceilFrames(g.minGapSeconds, fps);
        long long newStartFrame = curEndFrame + gapFrames;

        long long durFrames = std::llround(0.5 * fps);
        long long minDurFrames = ceilFrames(g.minDuration, fps);
        if (durFrames < minDurFrames) durFrames = minDurFrames;

        long long newEndFrame = newStartFrame + durFrames;
        newRow.start = frameToTime(newStartFrame, fps);
        newRow.end = frameToTime(newEndFrame, fps);
    } else {
        Decimal endSec;
        if (SubtitleRow::tryParseTime(cur.end, endSec)) {
            double newStart = endSec + g.minGapSeconds;
            double durSec = 0.5;
            if (durSec < g.minDuration) durSec = g.minDuration;
            newRow.start = SubtitleRow::formatTime(newStart);
            newRow.end = SubtitleRow::formatTime(newStart + durSec);
        } else {
            newRow.start = cur.end;
            newRow.end = cur.end;
        }
    }

    newRow.recalcAll();
    m_activeFile->model->insertRow(i + 1, newRow);
    m_activeFile->markDirty();
    refreshFileList();
    focusCell(i + 1, idx.column());
}

void MainWindow::splitLine()
{
    if (!m_activeFile) return;
    auto idx = m_tableView->currentIndex();
    if (!idx.isValid()) return;

    int i = idx.row();
    auto& rows = m_activeFile->model->rows();
    auto& cur = rows[i];

    Decimal startSec, endSec;
    if (!SubtitleRow::tryParseTime(cur.start, startSec) || !SubtitleRow::tryParseTime(cur.end, endSec))
        return;

    if (m_audioCursor <= startSec || m_audioCursor >= endSec) {
        QMessageBox::warning(this, "Invalid Split",
            "Audio cursor must be between Start and End of the selected row.");
        return;
    }

    m_activeFile->saveSnapshot();

    // Split text at cursor position in the active editor, or at first newline
    QString targetRight, sourceRight;
    int cursorCol = idx.column();
    QWidget* focused = QApplication::focusWidget();
    auto* focusedTE = qobject_cast<QPlainTextEdit*>(focused);
    int cursorPos = -1;
    if (focusedTE)
        cursorPos = focusedTE->textCursor().position();

    if (cursorCol == ColTarget && cursorPos >= 0) {
        targetRight = cur.target.mid(cursorPos).trimmed();
        cur.target = cur.target.left(cursorPos).trimmed();
    } else {
        int lb = cur.target.indexOf('\n');
        if (lb >= 0) {
            targetRight = cur.target.mid(lb + 1).trimmed();
            cur.target = cur.target.left(lb).trimmed();
        }
    }
    if (cursorCol == ColSource && cursorPos >= 0) {
        sourceRight = cur.source.mid(cursorPos).trimmed();
        cur.source = cur.source.left(cursorPos).trimmed();
    } else {
        int lb = cur.source.indexOf('\n');
        if (lb >= 0) {
            sourceRight = cur.source.mid(lb + 1).trimmed();
            cur.source = cur.source.left(lb).trimmed();
        }
    }

    double fps = globalSettings().frameRate;
    QString splitTime;
    if (fps > 0) {
        long long splitFrame = secToFrame(m_audioCursor, fps);
        splitTime = frameToTime(splitFrame, fps);
    } else {
        splitTime = SubtitleRow::formatTime(m_audioCursor);
    }

    SubtitleRow newRow;
    newRow.start = splitTime;
    newRow.end = cur.end;
    newRow.speaker = cur.speaker;
    newRow.source = sourceRight;
    newRow.target = targetRight;
    newRow.recalcAll();

    cur.end = splitTime;
    cur.recalcAll();

    // Refresh original row so persistent editors pick up the truncated text
    m_activeFile->model->refreshRow(i);

    m_activeFile->model->insertRow(i + 1, newRow);
    m_activeFile->markDirty();
    refreshFileList();
    focusCell(i + 1, idx.column());
}

void MainWindow::mergeLine()
{
    if (!m_activeFile) return;
    auto idx = m_tableView->currentIndex();
    if (!idx.isValid() || idx.row() <= 0) return;

    m_activeFile->saveSnapshot();
    auto& rows = m_activeFile->model->rows();
    int i = idx.row();
    auto& cur = rows[i];
    auto& prev = rows[i - 1];

    prev.end = cur.end;
    if (!cur.source.isEmpty())
        prev.source = prev.source.isEmpty() ? cur.source : prev.source + " " + cur.source;
    if (!cur.target.isEmpty())
        prev.target = prev.target.isEmpty() ? cur.target : prev.target + " " + cur.target;
    prev.recalcAll();

    m_activeFile->model->removeRow(i);
    m_activeFile->model->refreshRow(i - 1);
    m_tableView->resizeRowToContents(i - 1);
    m_activeFile->markDirty();
    refreshFileList();
    focusCell(i - 1, idx.column());
}

void MainWindow::mergeLineNoSpace()
{
    if (!m_activeFile) return;
    auto idx = m_tableView->currentIndex();
    if (!idx.isValid() || idx.row() <= 0) return;

    m_activeFile->saveSnapshot();
    auto& rows = m_activeFile->model->rows();
    int i = idx.row();
    auto& cur = rows[i];
    auto& prev = rows[i - 1];

    prev.end = cur.end;
    if (!cur.source.isEmpty())
        prev.source = prev.source.isEmpty() ? cur.source : prev.source + cur.source;
    if (!cur.target.isEmpty())
        prev.target = prev.target.isEmpty() ? cur.target : prev.target + cur.target;
    prev.recalcAll();

    m_activeFile->model->removeRow(i);
    m_activeFile->model->refreshRow(i - 1);
    m_tableView->resizeRowToContents(i - 1);
    m_activeFile->markDirty();
    refreshFileList();
    focusCell(i - 1, idx.column());
}

void MainWindow::undoStructural()
{
    if (!m_activeFile) return;

    // Remember focus position before model reset
    int row = m_tableView->currentIndex().row();
    int col = m_tableView->currentIndex().column();
    if (col < 0) col = ColSource;

    m_activeFile->undo();
    refreshFileList();
    m_waveform->update();
    updateTimeIndicator();

    // Restore focus after model reset clears it
    if (m_activeFile->model->rowCount() > 0)
        focusCell(std::min(row, m_activeFile->model->rowCount() - 1), col);
}

// ── Audio Operations ─────────────────────────────────────────────

void MainWindow::playSelection()
{
    if (!m_activeFile || !m_player->source().isValid()) return;

    if (m_player->isPlaying()) {
        m_player->stop();
        m_waveform->clearPlaybackCursor();
        if (m_playTimer) { m_playTimer->stop(); delete m_playTimer; m_playTimer = nullptr; }
        return;
    }

    auto idx = m_tableView->currentIndex();
    if (!idx.isValid()) return;

    auto& rows = m_activeFile->model->rows();
    if (idx.row() >= static_cast<int>(rows.size())) return;
    auto& row = rows[idx.row()];

    Decimal st, en;
    if (!SubtitleRow::tryParseTime(row.start, st) || !SubtitleRow::tryParseTime(row.end, en))
        return;

    double seekTime = m_audioCursor + m_audioOffsetSeconds;
    double endTime = en + m_audioOffsetSeconds;

    m_player->setPosition(static_cast<qint64>(seekTime * 1000));
    m_player->play();

    // Poll playback position at ~30ms to update yellow cursor
    if (m_playTimer) { m_playTimer->stop(); delete m_playTimer; }
    m_playTimer = new QTimer(this);
    m_playTimer->setInterval(50);
    connect(m_playTimer, &QTimer::timeout, this, [this, endTime]() {
        if (!m_player->isPlaying()) {
            m_playTimer->stop();
            m_waveform->clearPlaybackCursor();
            updateTimeIndicator();
            return;
        }
        double posMs = static_cast<double>(m_player->position());
        double posSec = posMs / 1000.0 - m_audioOffsetSeconds;
        m_waveform->setPlaybackCursor(posSec);
        updateTimeIndicator();

        if (posMs / 1000.0 >= endTime) {
            m_player->stop();
            m_playTimer->stop();
            m_waveform->clearPlaybackCursor();
            updateTimeIndicator();
        }
    });
    m_playTimer->start();
}

void MainWindow::toggleView()
{
    m_waveform->toggleView();
}

// ── Time Setting Operations ──────────────────────────────────────

void MainWindow::setStartTime()
{
    if (!m_activeFile) return;
    auto idx = m_tableView->currentIndex();
    if (!idx.isValid()) return;

    m_activeFile->saveSnapshot();
    auto& rows = m_activeFile->model->rows();
    int i = idx.row();
    auto& g = globalSettings();
    double fps = g.frameRate;

    if (fps > 0) {
        long long currentFrame = secToFrame(m_audioCursor, fps);
        rows[i].start = frameToTime(currentFrame, fps);
        rows[i].recalcDuration();

        if (i > 0) {
            long long prevEndFrame = timeToFrame(rows[i - 1].end, fps);
            long long newStartFrame = currentFrame;
            long long minGapFrames = ceilFrames(g.minGapSeconds, fps);

            if (newStartFrame - prevEndFrame < minGapFrames) {
                long long maxPrevEnd = newStartFrame - minGapFrames;
                if (maxPrevEnd < 0) maxPrevEnd = 0;
                rows[i - 1].end = frameToTime(maxPrevEnd, fps);
                rows[i - 1].recalcDuration();
                m_activeFile->model->refreshRow(i - 1);
            }
        }
    } else {
        rows[i].start = SubtitleRow::formatTime(m_audioCursor);
        rows[i].recalcDuration();

        if (i > 0) {
            Decimal newStart, prevEnd;
            if (SubtitleRow::tryParseTime(rows[i].start, newStart) &&
                SubtitleRow::tryParseTime(rows[i - 1].end, prevEnd)) {
                if (newStart - prevEnd < g.minGapSeconds) {
                    double maxPrevEnd = newStart - g.minGapSeconds;
                    if (maxPrevEnd < 0) maxPrevEnd = 0;
                    rows[i - 1].end = SubtitleRow::formatTime(maxPrevEnd);
                    rows[i - 1].recalcDuration();
                    m_activeFile->model->refreshRow(i - 1);
                }
            }
        }
    }

    m_activeFile->model->refreshRow(i);
    m_activeFile->model->updateGaps();
    m_activeFile->markDirty();
    refreshFileList();
    m_waveform->update();
    updateTimeIndicator();
}

void MainWindow::setEndTime()
{
    if (!m_activeFile) return;
    auto idx = m_tableView->currentIndex();
    if (!idx.isValid()) return;

    m_activeFile->saveSnapshot();
    auto& rows = m_activeFile->model->rows();
    int i = idx.row();
    auto& g = globalSettings();
    double fps = g.frameRate;

    if (fps > 0) {
        long long currentFrame = secToFrame(m_audioCursor, fps);
        rows[i].end = frameToTime(currentFrame, fps);
        rows[i].recalcDuration();

        if (i < static_cast<int>(rows.size()) - 1) {
            long long nextStartFrame = timeToFrame(rows[i + 1].start, fps);
            long long newEndFrame = currentFrame;
            long long minGapFrames = ceilFrames(g.minGapSeconds, fps);

            if (nextStartFrame - newEndFrame < minGapFrames) {
                long long minNextStart = newEndFrame + minGapFrames;
                rows[i + 1].start = frameToTime(minNextStart, fps);
                rows[i + 1].recalcDuration();
                m_activeFile->model->refreshRow(i + 1);
            }
        }
    } else {
        rows[i].end = SubtitleRow::formatTime(m_audioCursor);
        rows[i].recalcDuration();

        if (i < static_cast<int>(rows.size()) - 1) {
            Decimal newEnd, nextStart;
            if (SubtitleRow::tryParseTime(rows[i].end, newEnd) &&
                SubtitleRow::tryParseTime(rows[i + 1].start, nextStart)) {
                if (nextStart - newEnd < g.minGapSeconds) {
                    double minNextStart = newEnd + g.minGapSeconds;
                    rows[i + 1].start = SubtitleRow::formatTime(minNextStart);
                    rows[i + 1].recalcDuration();
                    m_activeFile->model->refreshRow(i + 1);
                }
            }
        }
    }

    m_activeFile->model->refreshRow(i);
    m_activeFile->model->updateGaps();
    m_activeFile->markDirty();
    refreshFileList();
    m_waveform->update();
    updateTimeIndicator();
}

void MainWindow::connectToPrevious()
{
    if (!m_activeFile) return;
    auto idx = m_tableView->currentIndex();
    if (!idx.isValid() || idx.row() <= 0) return;

    auto& rows = m_activeFile->model->rows();
    int i = idx.row();
    auto& g = globalSettings();
    double fps = g.frameRate;

    Decimal endSec;
    if (!SubtitleRow::tryParseTime(rows[i].end, endSec)) return;

    m_activeFile->saveSnapshot();

    if (fps > 0) {
        long long endFrame = secToFrame(endSec, fps);
        long long minDurFrames = ceilFrames(g.minDuration, fps);
        long long minGapFrames = ceilFrames(g.minGapSeconds, fps);

        long long prevEndFrame = timeToFrame(rows[i - 1].end, fps);
        long long targetStartFrame = prevEndFrame + minGapFrames;

        // Apply MinDuration: Start cannot be later than End - MinDuration
        if (endFrame - targetStartFrame < minDurFrames) {
            targetStartFrame = endFrame - minDurFrames;
            if (targetStartFrame < 0) targetStartFrame = 0;
        }

        rows[i].start = frameToTime(targetStartFrame, fps);
        rows[i].recalcDuration();

        // If pushing Start back created a gap violation with prevRow
        long long pEndFrame = timeToFrame(rows[i - 1].end, fps);
        if (targetStartFrame - pEndFrame < minGapFrames) {
            long long newPrevEnd = targetStartFrame - minGapFrames;
            if (newPrevEnd < 0) newPrevEnd = 0;
            rows[i - 1].end = frameToTime(newPrevEnd, fps);
            rows[i - 1].recalcDuration();
            m_activeFile->model->refreshRow(i - 1);
        }
    } else {
        Decimal prevEnd;
        if (!SubtitleRow::tryParseTime(rows[i - 1].end, prevEnd)) return;

        double targetStart = prevEnd + g.minGapSeconds;

        if (endSec - targetStart < g.minDuration) {
            targetStart = endSec - g.minDuration;
            if (targetStart < 0) targetStart = 0;
        }

        rows[i].start = SubtitleRow::formatTime(targetStart);
        rows[i].recalcDuration();

        Decimal pEnd;
        if (SubtitleRow::tryParseTime(rows[i - 1].end, pEnd)) {
            if (targetStart - pEnd < g.minGapSeconds) {
                double newPrevEnd = targetStart - g.minGapSeconds;
                if (newPrevEnd < 0) newPrevEnd = 0;
                rows[i - 1].end = SubtitleRow::formatTime(newPrevEnd);
                rows[i - 1].recalcDuration();
                m_activeFile->model->refreshRow(i - 1);
            }
        }
    }

    m_activeFile->model->refreshRow(i);
    m_activeFile->model->updateGaps();
    m_activeFile->markDirty();
    refreshFileList();
    m_waveform->update();
    updateTimeIndicator();
}

void MainWindow::connectToNext()
{
    if (!m_activeFile) return;
    auto idx = m_tableView->currentIndex();
    if (!idx.isValid()) return;
    int i = idx.row();
    auto& rows = m_activeFile->model->rows();
    if (i >= static_cast<int>(rows.size()) - 1) return;

    Decimal startSec;
    if (!SubtitleRow::tryParseTime(rows[i].start, startSec)) return;

    m_activeFile->saveSnapshot();
    auto& g = globalSettings();
    double fps = g.frameRate;

    if (fps > 0) {
        long long startFrame = secToFrame(startSec, fps);
        long long minDurFrames = ceilFrames(g.minDuration, fps);
        long long minGapFrames = ceilFrames(g.minGapSeconds, fps);

        long long nextStartFrame = timeToFrame(rows[i + 1].start, fps);
        long long targetEndFrame = nextStartFrame - minGapFrames;

        if (targetEndFrame - startFrame < minDurFrames)
            targetEndFrame = startFrame + minDurFrames;

        rows[i].end = frameToTime(targetEndFrame, fps);
        rows[i].recalcDuration();

        long long nStartFrame = timeToFrame(rows[i + 1].start, fps);
        if (nStartFrame - targetEndFrame < minGapFrames) {
            long long newNextStart = targetEndFrame + minGapFrames;
            rows[i + 1].start = frameToTime(newNextStart, fps);
            rows[i + 1].recalcDuration();
            m_activeFile->model->refreshRow(i + 1);
        }
    } else {
        Decimal nextStart;
        if (!SubtitleRow::tryParseTime(rows[i + 1].start, nextStart)) return;

        double targetEnd = nextStart - g.minGapSeconds;

        if (targetEnd - startSec < g.minDuration)
            targetEnd = startSec + g.minDuration;

        rows[i].end = SubtitleRow::formatTime(targetEnd);
        rows[i].recalcDuration();

        Decimal nStart;
        if (SubtitleRow::tryParseTime(rows[i + 1].start, nStart)) {
            if (nStart - targetEnd < g.minGapSeconds) {
                double newNextStart = targetEnd + g.minGapSeconds;
                if (newNextStart < 0) newNextStart = 0;
                rows[i + 1].start = SubtitleRow::formatTime(newNextStart);
                rows[i + 1].recalcDuration();
                m_activeFile->model->refreshRow(i + 1);
            }
        }
    }

    m_activeFile->model->refreshRow(i);
    m_activeFile->model->updateGaps();
    m_activeFile->markDirty();
    refreshFileList();
    m_waveform->update();
    updateTimeIndicator();
}

void MainWindow::extendStartToCPS()
{
    if (!m_activeFile) return;
    auto idx = m_tableView->currentIndex();
    if (!idx.isValid()) return;

    int i = idx.row();
    auto& rows = m_activeFile->model->rows();
    auto& row = rows[i];
    auto& g = globalSettings();
    double fps = g.frameRate;

    Decimal startSec, endSec;
    if (!SubtitleRow::tryParseTime(row.start, startSec) || !SubtitleRow::tryParseTime(row.end, endSec)) return;
    double charCount = CharCountHelper::countForCPS(row.target);

    if (fps > 0) {
        long long startFrame = secToFrame(startSec, fps);
        long long endFrame = secToFrame(endSec, fps);
        long long durFrames = endFrame - startFrame;
        double currentCps = durFrames > 0 ? charCount / (static_cast<double>(durFrames) / fps) : 0;

        long long targetDurFrames = durFrames;
        long long minDurFrames = ceilFrames(g.minDuration, fps);
        if (targetDurFrames < minDurFrames) targetDurFrames = minDurFrames;

        if (g.maxCPS > 0 && currentCps > g.maxCPS) {
            double cps = g.maxCPS > 0 ? g.maxCPS.toDouble() : 0.1;
            double reqSec = std::ceil(charCount / cps * 1000.0) / 1000.0;
            long long cpsFrames = ceilFrames(reqSec, fps);
            if (cpsFrames > targetDurFrames) targetDurFrames = cpsFrames;
        }
        if (g.maxLineLength > 0 && CharCountHelper::maxDisplayLineLength(row.target) > g.maxLineLength && CharCountHelper::maxDisplayLineLength(row.target) <= 24) {
            long long threeSecFrames = ceilFrames(3.0, fps);
            if (threeSecFrames > targetDurFrames) targetDurFrames = threeSecFrames;
        }

        if (targetDurFrames <= durFrames) return;
        m_activeFile->saveSnapshot();

        long long newStartFrame = endFrame - targetDurFrames;
        if (newStartFrame < 0) newStartFrame = 0;
        row.start = frameToTime(newStartFrame, fps);
        row.recalcDuration();

        // Cascade push to previous rows
        long long curNewStart = newStartFrame;
        for (int j = i - 1; j >= 0; --j) {
            auto& prev = rows[j];
            long long prevEndFrame = timeToFrame(prev.end, fps);
            if (prevEndFrame <= curNewStart) break;

            long long prevStartFrame = timeToFrame(prev.start, fps);
            double prevCharCount = CharCountHelper::countForCPS(prev.target);
            long long prevMinDur = ceilFrames(g.minDuration, fps);
            if (g.maxCPS > 0) {
                long long cpsF = ceilFrames(std::ceil(prevCharCount / g.maxCPS * 1000.0) / 1000.0, fps);
                if (cpsF > prevMinDur) prevMinDur = cpsF;
            }

            prev.end = frameToTime(curNewStart, fps);
            long long newPrevDur = curNewStart - prevStartFrame;

            if (newPrevDur >= prevMinDur) {
                prev.recalcDuration();
                m_activeFile->model->refreshRow(j);
                break;
            }

            long long prevNewStart = curNewStart - prevMinDur;
            if (prevNewStart < 0) prevNewStart = 0;
            prev.start = frameToTime(prevNewStart, fps);
            prev.recalcDuration();
            m_activeFile->model->refreshRow(j);
            curNewStart = prevNewStart;
        }
    } else {
        double durSec = endSec - startSec;
        double currentCps = durSec > 0 ? charCount / durSec : 0;
        double targetDur = durSec;

        if (targetDur < g.minDuration) targetDur = g.minDuration;
        if (g.maxCPS > 0 && currentCps > g.maxCPS) {
            double cps = g.maxCPS > 0 ? g.maxCPS.toDouble() : 0.1;
            double cpsDur = std::ceil(charCount / cps * 1000.0) / 1000.0;
            if (cpsDur > targetDur) targetDur = cpsDur;
        }
        if (g.maxLineLength > 0 && CharCountHelper::maxDisplayLineLength(row.target) > g.maxLineLength && CharCountHelper::maxDisplayLineLength(row.target) <= 24) {
            if (3.0 > targetDur) targetDur = 3.0;
        }
        if (targetDur <= durSec) return;
        m_activeFile->saveSnapshot();

        double newStartSec = endSec - targetDur;
        if (newStartSec < 0) newStartSec = 0;
        row.start = SubtitleRow::formatTime(newStartSec);
        row.recalcDuration();

        double curNewStart = newStartSec;
        for (int j = i - 1; j >= 0; --j) {
            auto& prev = rows[j];
            Decimal prevEnd;
            if (!SubtitleRow::tryParseTime(prev.end, prevEnd)) break;
            if (prevEnd <= curNewStart) break;
            Decimal prevStart;
            if (!SubtitleRow::tryParseTime(prev.start, prevStart)) break;

            double prevCharCount = CharCountHelper::countForCPS(prev.target);
            double prevMinDur = g.minDuration;
            if (g.maxCPS > 0) {
                double req = std::ceil(prevCharCount / g.maxCPS * 1000.0) / 1000.0;
                if (req > prevMinDur) prevMinDur = req;
            }

            prev.end = SubtitleRow::formatTime(curNewStart);
            if (curNewStart - prevStart >= prevMinDur) {
                prev.recalcDuration();
                m_activeFile->model->refreshRow(j);
                break;
            }

            double prevNewStart = curNewStart - prevMinDur;
            if (prevNewStart < 0) prevNewStart = 0;
            prev.start = SubtitleRow::formatTime(prevNewStart);
            prev.recalcDuration();
            m_activeFile->model->refreshRow(j);
            curNewStart = prevNewStart;
        }
    }

    m_activeFile->model->refreshRow(i);
    m_activeFile->model->updateGaps();
    m_activeFile->markDirty();
    refreshFileList();
    m_waveform->update();
    updateTimeIndicator();
}

void MainWindow::extendEndToCPS()
{
    if (!m_activeFile) return;
    auto idx = m_tableView->currentIndex();
    if (!idx.isValid()) return;

    int i = idx.row();
    auto& rows = m_activeFile->model->rows();
    auto& row = rows[i];
    auto& g = globalSettings();
    double fps = g.frameRate;

    Decimal startSec, endSec;
    if (!SubtitleRow::tryParseTime(row.start, startSec) || !SubtitleRow::tryParseTime(row.end, endSec)) return;
    double charCount = CharCountHelper::countForCPS(row.target);

    if (fps > 0) {
        long long startFrame = secToFrame(startSec, fps);
        long long endFrame = secToFrame(endSec, fps);
        long long durFrames = endFrame - startFrame;
        double currentCps = durFrames > 0 ? charCount / (static_cast<double>(durFrames) / fps) : 0;

        long long targetDurFrames = durFrames;
        long long minDurFrames = ceilFrames(g.minDuration, fps);
        if (targetDurFrames < minDurFrames) targetDurFrames = minDurFrames;

        if (g.maxCPS > 0 && currentCps > g.maxCPS) {
            double cps = g.maxCPS > 0 ? g.maxCPS.toDouble() : 0.1;
            double reqSec = std::ceil(charCount / cps * 1000.0) / 1000.0;
            long long cpsFrames = ceilFrames(reqSec, fps);
            if (cpsFrames > targetDurFrames) targetDurFrames = cpsFrames;
        }
        if (g.maxLineLength > 0 && CharCountHelper::maxDisplayLineLength(row.target) > g.maxLineLength && CharCountHelper::maxDisplayLineLength(row.target) <= 24) {
            long long threeSecFrames = ceilFrames(3.0, fps);
            if (threeSecFrames > targetDurFrames) targetDurFrames = threeSecFrames;
        }

        if (targetDurFrames <= durFrames) return;
        m_activeFile->saveSnapshot();

        long long newEndFrame = startFrame + targetDurFrames;
        row.end = frameToTime(newEndFrame, fps);
        row.recalcDuration();

        // Cascade push to next rows
        long long curNewEnd = newEndFrame;
        for (int j = i + 1; j < static_cast<int>(rows.size()); ++j) {
            auto& next = rows[j];
            long long nextStartFrame = timeToFrame(next.start, fps);
            if (nextStartFrame >= curNewEnd) break;

            long long nextEndFrame = timeToFrame(next.end, fps);
            double nextCharCount = CharCountHelper::countForCPS(next.target);
            long long nextMinDur = ceilFrames(g.minDuration, fps);
            if (g.maxCPS > 0) {
                long long cpsF = ceilFrames(std::ceil(nextCharCount / g.maxCPS * 1000.0) / 1000.0, fps);
                if (cpsF > nextMinDur) nextMinDur = cpsF;
            }

            next.start = frameToTime(curNewEnd, fps);
            long long newNextDur = nextEndFrame - curNewEnd;

            if (newNextDur >= nextMinDur) {
                next.recalcDuration();
                m_activeFile->model->refreshRow(j);
                break;
            }

            long long nextNewEnd = curNewEnd + nextMinDur;
            next.end = frameToTime(nextNewEnd, fps);
            next.recalcDuration();
            m_activeFile->model->refreshRow(j);
            curNewEnd = nextNewEnd;
        }
    } else {
        double durSec = endSec - startSec;
        double currentCps = durSec > 0 ? charCount / durSec : 0;
        double targetDur = durSec;

        if (targetDur < g.minDuration) targetDur = g.minDuration;
        if (g.maxCPS > 0 && currentCps > g.maxCPS) {
            double cps = g.maxCPS > 0 ? g.maxCPS.toDouble() : 0.1;
            double cpsDur = std::ceil(charCount / cps * 1000.0) / 1000.0;
            if (cpsDur > targetDur) targetDur = cpsDur;
        }
        if (g.maxLineLength > 0 && CharCountHelper::maxDisplayLineLength(row.target) > g.maxLineLength && CharCountHelper::maxDisplayLineLength(row.target) <= 24) {
            if (3.0 > targetDur) targetDur = 3.0;
        }
        if (targetDur <= durSec) return;
        m_activeFile->saveSnapshot();

        double newEndSec = startSec + targetDur;
        row.end = SubtitleRow::formatTime(newEndSec);
        row.recalcDuration();

        double curNewEnd = newEndSec;
        for (int j = i + 1; j < static_cast<int>(rows.size()); ++j) {
            auto& next = rows[j];
            Decimal nextStart;
            if (!SubtitleRow::tryParseTime(next.start, nextStart)) break;
            if (nextStart >= curNewEnd) break;
            Decimal nextEnd;
            if (!SubtitleRow::tryParseTime(next.end, nextEnd)) break;

            double nextCharCount = CharCountHelper::countForCPS(next.target);
            double nextMinDur = g.minDuration;
            if (g.maxCPS > 0) {
                double req = std::ceil(nextCharCount / g.maxCPS * 1000.0) / 1000.0;
                if (req > nextMinDur) nextMinDur = req;
            }

            next.start = SubtitleRow::formatTime(curNewEnd);
            if (nextEnd - curNewEnd >= nextMinDur) {
                next.recalcDuration();
                m_activeFile->model->refreshRow(j);
                break;
            }

            double nextNewEnd = curNewEnd + nextMinDur;
            next.end = SubtitleRow::formatTime(nextNewEnd);
            next.recalcDuration();
            m_activeFile->model->refreshRow(j);
            curNewEnd = nextNewEnd;
        }
    }

    m_activeFile->model->refreshRow(i);
    m_activeFile->model->updateGaps();
    m_activeFile->markDirty();
    refreshFileList();
    m_waveform->update();
    updateTimeIndicator();
}

void MainWindow::minimizeStartToCPS()
{
    if (!m_activeFile) return;
    auto idx = m_tableView->currentIndex();
    if (!idx.isValid()) return;

    int i = idx.row();
    auto& rows = m_activeFile->model->rows();
    auto& row = rows[i];
    auto& g = globalSettings();
    double fps = g.frameRate;

    Decimal startSec, endSec;
    if (!SubtitleRow::tryParseTime(row.start, startSec) || !SubtitleRow::tryParseTime(row.end, endSec)) return;
    double charCount = CharCountHelper::countForCPS(row.target);

    if (fps > 0) {
        long long startFrame = secToFrame(startSec, fps);
        long long endFrame = secToFrame(endSec, fps);
        long long durFrames = endFrame - startFrame;

        long long minDurFrames = ceilFrames(g.minDuration, fps);
        long long targetDurFrames = minDurFrames;

        if (g.maxCPS > 0) {
            double cps = g.maxCPS > 0 ? g.maxCPS.toDouble() : 0.1;
            long long cpsFrames = ceilFrames(std::ceil(charCount / cps * 1000.0) / 1000.0, fps);
            if (cpsFrames > targetDurFrames) targetDurFrames = cpsFrames;
        }
        if (g.maxLineLength > 0 && CharCountHelper::maxDisplayLineLength(row.target) > g.maxLineLength && CharCountHelper::maxDisplayLineLength(row.target) <= 24) {
            long long threeSecFrames = ceilFrames(3.0, fps);
            if (threeSecFrames > targetDurFrames) targetDurFrames = threeSecFrames;
        }

        if (durFrames <= targetDurFrames) return;
        m_activeFile->saveSnapshot();

        long long newStartFrame = endFrame - targetDurFrames;
        row.start = frameToTime(newStartFrame, fps);
    } else {
        double durSec = endSec - startSec;
        double targetDur = g.minDuration;

        if (g.maxCPS > 0) {
            double cps = g.maxCPS > 0 ? g.maxCPS.toDouble() : 0.1;
            double cpsDur = std::ceil(charCount / cps * 1000.0) / 1000.0;
            if (cpsDur > targetDur) targetDur = cpsDur;
        }
        if (g.maxLineLength > 0 && CharCountHelper::maxDisplayLineLength(row.target) > g.maxLineLength && CharCountHelper::maxDisplayLineLength(row.target) <= 24) {
            if (3.0 > targetDur) targetDur = 3.0;
        }

        if (durSec <= targetDur) return;
        m_activeFile->saveSnapshot();

        row.start = SubtitleRow::formatTime(endSec - targetDur);
    }

    row.recalcDuration();
    m_activeFile->model->refreshRow(i);
    m_activeFile->model->updateGaps();
    m_activeFile->markDirty();
    refreshFileList();
    m_waveform->update();
    updateTimeIndicator();
}

void MainWindow::minimizeEndToCPS()
{
    if (!m_activeFile) return;
    auto idx = m_tableView->currentIndex();
    if (!idx.isValid()) return;

    int i = idx.row();
    auto& rows = m_activeFile->model->rows();
    auto& row = rows[i];
    auto& g = globalSettings();
    double fps = g.frameRate;

    Decimal startSec, endSec;
    if (!SubtitleRow::tryParseTime(row.start, startSec) || !SubtitleRow::tryParseTime(row.end, endSec)) return;
    double charCount = CharCountHelper::countForCPS(row.target);

    if (fps > 0) {
        long long startFrame = secToFrame(startSec, fps);
        long long endFrame = secToFrame(endSec, fps);
        long long durFrames = endFrame - startFrame;

        long long minDurFrames = ceilFrames(g.minDuration, fps);
        long long targetDurFrames = minDurFrames;

        if (g.maxCPS > 0) {
            double cps = g.maxCPS > 0 ? g.maxCPS.toDouble() : 0.1;
            long long cpsFrames = ceilFrames(std::ceil(charCount / cps * 1000.0) / 1000.0, fps);
            if (cpsFrames > targetDurFrames) targetDurFrames = cpsFrames;
        }
        if (g.maxLineLength > 0 && CharCountHelper::maxDisplayLineLength(row.target) > g.maxLineLength && CharCountHelper::maxDisplayLineLength(row.target) <= 24) {
            long long threeSecFrames = ceilFrames(3.0, fps);
            if (threeSecFrames > targetDurFrames) targetDurFrames = threeSecFrames;
        }

        if (durFrames <= targetDurFrames) return;
        m_activeFile->saveSnapshot();

        long long newEndFrame = startFrame + targetDurFrames;
        row.end = frameToTime(newEndFrame, fps);
    } else {
        double durSec = endSec - startSec;
        double targetDur = g.minDuration;

        if (g.maxCPS > 0) {
            double cps = g.maxCPS > 0 ? g.maxCPS.toDouble() : 0.1;
            double cpsDur = std::ceil(charCount / cps * 1000.0) / 1000.0;
            if (cpsDur > targetDur) targetDur = cpsDur;
        }
        if (g.maxLineLength > 0 && CharCountHelper::maxDisplayLineLength(row.target) > g.maxLineLength && CharCountHelper::maxDisplayLineLength(row.target) <= 24) {
            if (3.0 > targetDur) targetDur = 3.0;
        }

        if (durSec <= targetDur) return;
        m_activeFile->saveSnapshot();

        row.end = SubtitleRow::formatTime(startSec + targetDur);
    }

    row.recalcDuration();
    m_activeFile->model->refreshRow(i);
    m_activeFile->model->updateGaps();
    m_activeFile->markDirty();
    refreshFileList();
    m_waveform->update();
    updateTimeIndicator();
}

void MainWindow::extendStartToCPSOverwrite()
{
    if (!m_activeFile) return;
    auto idx = m_tableView->currentIndex();
    if (!idx.isValid()) return;

    int i = idx.row();
    auto& rows = m_activeFile->model->rows();
    auto& row = rows[i];
    auto& g = globalSettings();
    double fps = g.frameRate;

    Decimal startSec, endSec;
    if (!SubtitleRow::tryParseTime(row.start, startSec) || !SubtitleRow::tryParseTime(row.end, endSec)) return;
    double charCount = CharCountHelper::countForCPS(row.target);

    if (fps > 0) {
        long long startFrame = secToFrame(startSec, fps);
        long long endFrame = secToFrame(endSec, fps);
        long long durFrames = endFrame - startFrame;
        double currentCps = durFrames > 0 ? charCount / (static_cast<double>(durFrames) / fps) : 0;

        long long targetDurFrames = durFrames;
        long long minDurFrames = ceilFrames(g.minDuration, fps);
        if (targetDurFrames < minDurFrames) targetDurFrames = minDurFrames;

        if (g.maxCPS > 0 && currentCps > g.maxCPS) {
            double cps = g.maxCPS > 0 ? g.maxCPS.toDouble() : 0.1;
            long long cpsFrames = ceilFrames(std::ceil(charCount / cps * 1000.0) / 1000.0, fps);
            if (cpsFrames > targetDurFrames) targetDurFrames = cpsFrames;
        }
        if (g.maxLineLength > 0 && CharCountHelper::maxDisplayLineLength(row.target) > g.maxLineLength && CharCountHelper::maxDisplayLineLength(row.target) <= 24) {
            long long threeSecFrames = ceilFrames(3.0, fps);
            if (threeSecFrames > targetDurFrames) targetDurFrames = threeSecFrames;
        }

        if (targetDurFrames <= durFrames) return;
        m_activeFile->saveSnapshot();

        long long newStartFrame = endFrame - targetDurFrames;
        if (newStartFrame < 0) newStartFrame = 0;
        row.start = frameToTime(newStartFrame, fps);
        row.recalcDuration();

        // Overwrite: only trim immediate previous row's end for gap
        if (i > 0) {
            long long prevEndFrame = timeToFrame(rows[i - 1].end, fps);
            long long minGapFrames = ceilFrames(g.minGapSeconds, fps);
            if (newStartFrame - prevEndFrame < minGapFrames) {
                long long maxPrevEnd = newStartFrame - minGapFrames;
                if (maxPrevEnd < 0) maxPrevEnd = 0;
                rows[i - 1].end = frameToTime(maxPrevEnd, fps);
                rows[i - 1].recalcDuration();
                m_activeFile->model->refreshRow(i - 1);
            }
        }
    } else {
        double durSec = endSec - startSec;
        double currentCps = durSec > 0 ? charCount / durSec : 0;
        double targetDur = durSec;

        if (targetDur < g.minDuration) targetDur = g.minDuration;
        if (g.maxCPS > 0 && currentCps > g.maxCPS) {
            double cps = g.maxCPS > 0 ? g.maxCPS.toDouble() : 0.1;
            double cpsDur = std::ceil(charCount / cps * 1000.0) / 1000.0;
            if (cpsDur > targetDur) targetDur = cpsDur;
        }
        if (g.maxLineLength > 0 && CharCountHelper::maxDisplayLineLength(row.target) > g.maxLineLength && CharCountHelper::maxDisplayLineLength(row.target) <= 24) {
            if (3.0 > targetDur) targetDur = 3.0;
        }
        if (targetDur <= durSec) return;
        m_activeFile->saveSnapshot();

        double newStartSec = endSec - targetDur;
        if (newStartSec < 0) newStartSec = 0;
        row.start = SubtitleRow::formatTime(newStartSec);
        row.recalcDuration();

        if (i > 0) {
            Decimal prevEnd;
            if (SubtitleRow::tryParseTime(rows[i - 1].end, prevEnd)) {
                if (newStartSec - prevEnd < g.minGapSeconds) {
                    double maxPrevEnd = newStartSec - g.minGapSeconds;
                    if (maxPrevEnd < 0) maxPrevEnd = 0;
                    rows[i - 1].end = SubtitleRow::formatTime(maxPrevEnd);
                    rows[i - 1].recalcDuration();
                    m_activeFile->model->refreshRow(i - 1);
                }
            }
        }
    }

    m_activeFile->model->refreshRow(i);
    m_activeFile->model->updateGaps();
    m_activeFile->markDirty();
    refreshFileList();
    m_waveform->update();
    updateTimeIndicator();
}

void MainWindow::extendEndToCPSOverwrite()
{
    if (!m_activeFile) return;
    auto idx = m_tableView->currentIndex();
    if (!idx.isValid()) return;

    int i = idx.row();
    auto& rows = m_activeFile->model->rows();
    auto& row = rows[i];
    auto& g = globalSettings();
    double fps = g.frameRate;

    Decimal startSec, endSec;
    if (!SubtitleRow::tryParseTime(row.start, startSec) || !SubtitleRow::tryParseTime(row.end, endSec)) return;
    double charCount = CharCountHelper::countForCPS(row.target);

    if (fps > 0) {
        long long startFrame = secToFrame(startSec, fps);
        long long endFrame = secToFrame(endSec, fps);
        long long durFrames = endFrame - startFrame;
        double currentCps = durFrames > 0 ? charCount / (static_cast<double>(durFrames) / fps) : 0;

        long long targetDurFrames = durFrames;
        long long minDurFrames = ceilFrames(g.minDuration, fps);
        if (targetDurFrames < minDurFrames) targetDurFrames = minDurFrames;

        if (g.maxCPS > 0 && currentCps > g.maxCPS) {
            double cps = g.maxCPS > 0 ? g.maxCPS.toDouble() : 0.1;
            long long cpsFrames = ceilFrames(std::ceil(charCount / cps * 1000.0) / 1000.0, fps);
            if (cpsFrames > targetDurFrames) targetDurFrames = cpsFrames;
        }
        if (g.maxLineLength > 0 && CharCountHelper::maxDisplayLineLength(row.target) > g.maxLineLength && CharCountHelper::maxDisplayLineLength(row.target) <= 24) {
            long long threeSecFrames = ceilFrames(3.0, fps);
            if (threeSecFrames > targetDurFrames) targetDurFrames = threeSecFrames;
        }

        if (targetDurFrames <= durFrames) return;
        m_activeFile->saveSnapshot();

        long long newEndFrame = startFrame + targetDurFrames;
        row.end = frameToTime(newEndFrame, fps);
        row.recalcDuration();

        // Overwrite: only push immediate next row's start for gap
        if (i < static_cast<int>(rows.size()) - 1) {
            long long nextStartFrame = timeToFrame(rows[i + 1].start, fps);
            long long minGapFrames = ceilFrames(g.minGapSeconds, fps);
            if (nextStartFrame - newEndFrame < minGapFrames) {
                long long minNextStart = newEndFrame + minGapFrames;
                rows[i + 1].start = frameToTime(minNextStart, fps);
                rows[i + 1].recalcDuration();
                m_activeFile->model->refreshRow(i + 1);
            }
        }
    } else {
        double durSec = endSec - startSec;
        double currentCps = durSec > 0 ? charCount / durSec : 0;
        double targetDur = durSec;

        if (targetDur < g.minDuration) targetDur = g.minDuration;
        if (g.maxCPS > 0 && currentCps > g.maxCPS) {
            double cps = g.maxCPS > 0 ? g.maxCPS.toDouble() : 0.1;
            double cpsDur = std::ceil(charCount / cps * 1000.0) / 1000.0;
            if (cpsDur > targetDur) targetDur = cpsDur;
        }
        if (g.maxLineLength > 0 && CharCountHelper::maxDisplayLineLength(row.target) > g.maxLineLength && CharCountHelper::maxDisplayLineLength(row.target) <= 24) {
            if (3.0 > targetDur) targetDur = 3.0;
        }
        if (targetDur <= durSec) return;
        m_activeFile->saveSnapshot();

        double newEndSec = startSec + targetDur;
        row.end = SubtitleRow::formatTime(newEndSec);
        row.recalcDuration();

        if (i < static_cast<int>(rows.size()) - 1) {
            Decimal nextStart;
            if (SubtitleRow::tryParseTime(rows[i + 1].start, nextStart)) {
                if (nextStart - newEndSec < g.minGapSeconds) {
                    double minNextStart = newEndSec + g.minGapSeconds;
                    rows[i + 1].start = SubtitleRow::formatTime(minNextStart);
                    rows[i + 1].recalcDuration();
                    m_activeFile->model->refreshRow(i + 1);
                }
            }
        }
    }

    m_activeFile->model->refreshRow(i);
    m_activeFile->model->updateGaps();
    m_activeFile->markDirty();
    refreshFileList();
    m_waveform->update();
    updateTimeIndicator();
}

// ── Dialogs ─────────────────────────────────────────────────────

void MainWindow::findReplace()
{
    if (!m_findDialog)
        m_findDialog = new FindReplaceDialog(this);
    m_findDialog->show();
    m_findDialog->raise();
    m_findDialog->activateWindow();
}

void MainWindow::ruleCheck()
{
    std::vector<RuleViolation> violations;
    for (auto& file : m_files) {
        auto& rows = file->model->rows();
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            auto& row = rows[i];
            if (row.isDurationWarning())
                violations.push_back({file.get(), i, "Duration", file->displayName() + " - Duration"});
            if (row.gapWarning)
                violations.push_back({file.get(), i, "Gap", file->displayName() + " - Gap"});
            if (row.isCPSWarning())
                violations.push_back({file.get(), i, "CPS", file->displayName() + " - CPS"});
            if (row.isMaxLineLenWarning())
                violations.push_back({file.get(), i, "MaxLen", file->displayName() + " - MaxLen"});
        }
    }

    if (violations.empty()) {
        QMessageBox::information(this, "Rule Check", "No violation found.");
        return;
    }

    if (!m_ruleCheckDialog)
        m_ruleCheckDialog = new RuleCheckDialog(this, violations);
    else
        m_ruleCheckDialog->updateViolations(violations);
    m_ruleCheckDialog->show();
    m_ruleCheckDialog->raise();
}

void MainWindow::shiftTime()
{
    if (!m_activeFile || m_activeFile->model->rows().empty()) return;

    ShiftTimeDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;

    m_activeFile->saveSnapshot();
    double shift = dlg.shiftSeconds();
    double fps = globalSettings().frameRate;

    if (fps > 0) {
        long long shiftFrames = std::llround(shift * fps);
        for (auto& row : m_activeFile->model->rows()) {
            Decimal st, en;
            if (SubtitleRow::tryParseTime(row.start, st)) {
                long long f = secToFrame(st, fps) + shiftFrames;
                if (f < 0) f = 0;
                row.start = frameToTime(f, fps);
            }
            if (SubtitleRow::tryParseTime(row.end, en)) {
                long long f = secToFrame(en, fps) + shiftFrames;
                if (f < 0) f = 0;
                row.end = frameToTime(f, fps);
            }
            row.recalcDuration();
        }
    } else {
        for (auto& row : m_activeFile->model->rows()) {
            Decimal st, en;
            if (SubtitleRow::tryParseTime(row.start, st)) {
                st += shift;
                if (st < 0) st = 0;
                row.start = SubtitleRow::formatTime(st);
            }
            if (SubtitleRow::tryParseTime(row.end, en)) {
                en += shift;
                if (en < 0) en = 0;
                row.end = SubtitleRow::formatTime(en);
            }
            row.recalcDuration();
        }
    }

    m_activeFile->model->refreshAll();
    m_activeFile->markDirty();
    refreshFileList();
    m_waveform->update();
    updateTimeIndicator();
}

void MainWindow::projectSettings()
{
    ProjectSettingsDialog::Settings current;
    current.frameRate = globalSettings().frameRate;
    current.minGap = globalSettings().minGapSeconds;
    current.minDuration = globalSettings().minDuration;
    current.maxDuration = globalSettings().maxDuration;
    current.maxLength = globalSettings().maxLineLength;
    current.maxCPS = globalSettings().maxCPS;
    current.audioOffsetMs = m_audioOffsetSeconds * 1000.0;
    current.highlightConsecutiveSpeaker = globalSettings().highlightConsecutiveSpeaker;

    ProjectSettingsDialog dlg(this, current);
    if (dlg.exec() != QDialog::Accepted) return;

    auto s = dlg.settings();
    globalSettings().frameRate = s.frameRate;
    globalSettings().minGapSeconds = s.minGap;
    globalSettings().minDuration = s.minDuration;
    globalSettings().maxDuration = s.maxDuration;
    globalSettings().maxLineLength = s.maxLength;
    globalSettings().maxCPS = s.maxCPS;
    globalSettings().highlightConsecutiveSpeaker = s.highlightConsecutiveSpeaker;
    m_audioOffsetSeconds = s.audioOffsetMs / 1000.0;
    m_waveform->setAudioOffset(m_audioOffsetSeconds);

    m_suppressDirty = true;
    for (auto& f : m_files) {
        f->model->updateGaps();
        for (auto& r : f->model->rows()) {
            r.recalcAll();
        }
        f->model->refreshAll();
    }
    m_suppressDirty = false;
    m_waveform->update();
    updateTimeIndicator();
}

void MainWindow::autoFixDuration()
{
    if (!m_activeFile || m_activeFile->model->rows().empty()) return;

    m_activeFile->saveSnapshot();
    auto& rows = m_activeFile->model->rows();
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        QString nextStart;
        if (i < static_cast<int>(rows.size()) - 1)
            nextStart = rows[i + 1].start;
        fixSingleRow(rows[i], false, nextStart);
    }
    m_activeFile->model->refreshAll();
    m_activeFile->markDirty();
    refreshFileList();
    m_waveform->update();
    updateTimeIndicator();
}

void MainWindow::fixSingleRow(SubtitleRow& row, bool /*forceMinGap*/, const QString& nextStartStr)
{
    Decimal startSec, endSec;
    if (!SubtitleRow::tryParseTime(row.start, startSec)) return;
    if (!SubtitleRow::tryParseTime(row.end, endSec)) return;

    double charCount = CharCountHelper::countForCPS(row.target);
    auto& g = globalSettings();
    double fps = g.frameRate;

    if (fps > 0) {
        long long startFrame = secToFrame(startSec, fps);
        long long endFrame = secToFrame(endSec, fps);
        long long durFrames = endFrame - startFrame;
        long long minDurFrames = ceilFrames(g.minDuration, fps);
        long long minGapFrames = ceilFrames(g.minGapSeconds, fps);
        long long maxDurFrames = g.maxDuration > 0 ? floorFrames(g.maxDuration, fps) : LLONG_MAX;

        long long targetDurFrames = durFrames;
        if (targetDurFrames < minDurFrames)
            targetDurFrames = minDurFrames;

        double currentSec = static_cast<double>(targetDurFrames) / fps;
        double newCps = currentSec > 0 ? charCount / currentSec : 0;
        if (g.maxCPS > 0 && newCps > g.maxCPS) {
            double cps = g.maxCPS > 0 ? g.maxCPS.toDouble() : 0.1;
            double reqSec = std::ceil(charCount / cps * 1000.0) / 1000.0;
            targetDurFrames = ceilFrames(reqSec, fps);
        }

        // If maxlen is violated, extend to 3s to exempt from maxlen check
        double maxLineLen = CharCountHelper::maxDisplayLineLength(row.target);
        if (g.maxLineLength > 0 && maxLineLen > g.maxLineLength && maxLineLen <= 24) {
            long long threeSecFrames = ceilFrames(3.0, fps);
            if (targetDurFrames < threeSecFrames)
                targetDurFrames = threeSecFrames;
        }

        if (targetDurFrames > maxDurFrames)
            targetDurFrames = maxDurFrames;

        long long newEndFrame = startFrame + targetDurFrames;

        if (!nextStartStr.isEmpty()) {
            Decimal nextStart;
            if (SubtitleRow::tryParseTime(nextStartStr, nextStart)) {
                long long nextStartFrame = secToFrame(nextStart, fps);
                if (nextStartFrame - newEndFrame < minGapFrames) {
                    long long maxEnd = nextStartFrame - minGapFrames;
                    if (maxEnd < startFrame) maxEnd = startFrame;
                    newEndFrame = maxEnd;
                }
            }
        }

        // Re-snap start too (in case it wasn't frame-aligned)
        QString finalStart = frameToTime(startFrame, fps);
        if (row.start != finalStart) row.start = finalStart;

        QString finalEnd = frameToTime(newEndFrame, fps);
        if (row.end != finalEnd) row.end = finalEnd;
    } else {
        double durSec = endSec - startSec;
        double targetDur = durSec;

        if (targetDur < g.minDuration)
            targetDur = g.minDuration;

        double newCps = targetDur > 0 ? charCount / targetDur : 0;
        if (g.maxCPS > 0 && newCps > g.maxCPS) {
            double cps = g.maxCPS > 0 ? g.maxCPS.toDouble() : 0.1;
            targetDur = std::ceil(charCount / cps * 1000.0) / 1000.0;
        }

        // If maxlen is violated, extend to 3s to exempt from maxlen check
        double maxLineLen = CharCountHelper::maxDisplayLineLength(row.target);
        if (g.maxLineLength > 0 && maxLineLen > g.maxLineLength && maxLineLen <= 24) {
            if (targetDur < 3.0)
                targetDur = 3.0;
        }

        if (g.maxDuration > 0 && targetDur > g.maxDuration)
            targetDur = g.maxDuration;

        double newEnd = startSec + targetDur;

        Decimal nextStart;
        if (!nextStartStr.isEmpty() && SubtitleRow::tryParseTime(nextStartStr, nextStart)) {
            if (nextStart - newEnd < g.minGapSeconds) {
                double maxEnd = nextStart - g.minGapSeconds;
                if (maxEnd < startSec) maxEnd = startSec;
                newEnd = maxEnd;
            }
        }

        QString finalEnd = SubtitleRow::formatTime(newEnd);
        if (row.end != finalEnd) row.end = finalEnd;
    }

    row.recalcDuration();
}

// ── Config ──────────────────────────────────────────────────────

void MainWindow::saveConfig()
{
    auto path = QFileDialog::getSaveFileName(this, "Save Config", m_lastConfigDir, "JSON Files (*.json)");
    if (path.isEmpty()) return;
    m_lastConfigDir = QFileInfo(path).absolutePath();
    persistDirSetting("lastConfigDir", m_lastConfigDir);

    QJsonObject obj;

    // Speaker shortcuts
    QJsonObject speakers;
    for (const auto& s : m_speakerShortcuts)
        speakers[s.key] = s.name();
    obj["Speakers"] = speakers;

    // Project settings
    auto& g = globalSettings();
    QJsonObject settings;
    settings["FrameRate"] = g.frameRate;
    settings["MinGap"] = g.minGapSeconds.toDouble();
    settings["MinDuration"] = g.minDuration.toDouble();
    settings["MaxDuration"] = g.maxDuration.toDouble();
    settings["MaxLength"] = g.maxLineLength.toDouble();
    settings["MaxCPS"] = g.maxCPS.toDouble();
    settings["AudioOffset"] = m_audioOffsetSeconds * 1000.0;
    settings["HighlightConsecutiveSpeaker"] = g.highlightConsecutiveSpeaker;
    obj["Settings"] = settings;

    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
}

void MainWindow::loadConfig()
{
    auto path = QFileDialog::getOpenFileName(this, "Load Config", m_lastConfigDir, "JSON Files (*.json)");
    if (path.isEmpty()) return;
    m_lastConfigDir = QFileInfo(path).absolutePath();
    persistDirSetting("lastConfigDir", m_lastConfigDir);

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;

    auto doc = QJsonDocument::fromJson(f.readAll());
    auto obj = doc.object();

    // Speaker shortcuts — support both new format (nested) and legacy (flat)
    QJsonObject speakers = obj.contains("Speakers") ? obj["Speakers"].toObject() : obj;
    for (auto& s : m_speakerShortcuts) {
        if (speakers.contains(s.key))
            s.nameEdit->setText(speakers[s.key].toString());
    }

    // Project settings
    QJsonObject settings = obj["Settings"].toObject();
    if (!settings.isEmpty()) {
        auto& g = globalSettings();
        if (settings.contains("FrameRate")) g.frameRate = settings["FrameRate"].toDouble();
        if (settings.contains("MinGap")) g.minGapSeconds = settings["MinGap"].toDouble();
        if (settings.contains("MinDuration")) g.minDuration = settings["MinDuration"].toDouble();
        if (settings.contains("MaxDuration")) g.maxDuration = settings["MaxDuration"].toDouble();
        if (settings.contains("MaxLength")) g.maxLineLength = settings["MaxLength"].toDouble();
        if (settings.contains("MaxCPS")) g.maxCPS = settings["MaxCPS"].toDouble();
        if (settings.contains("AudioOffset")) m_audioOffsetSeconds = settings["AudioOffset"].toDouble() / 1000.0;
        if (settings.contains("HighlightConsecutiveSpeaker"))
            g.highlightConsecutiveSpeaker = settings["HighlightConsecutiveSpeaker"].toBool();
        m_waveform->setAudioOffset(m_audioOffsetSeconds);

        m_suppressDirty = true;
        for (auto& file : m_files) {
            file->model->updateGaps();
            for (auto& r : file->model->rows())
                r.recalcAll();
            file->model->refreshAll();
        }
        m_suppressDirty = false;
        m_waveform->update();
        updateTimeIndicator();
    }
}

// ── Speaker Shortcuts ───────────────────────────────────────────

void MainWindow::applySpeakerShortcut(const QString& key)
{
    if (!m_activeFile) return;
    auto idx = m_tableView->currentIndex();
    if (!idx.isValid()) return;

    auto it = std::find_if(m_speakerShortcuts.begin(), m_speakerShortcuts.end(),
        [&](const auto& s) { return s.key == key; });
    if (it == m_speakerShortcuts.end()) return;

    int row = idx.row();
    auto& rows = m_activeFile->model->rows();
    if (row >= static_cast<int>(rows.size())) return;

    m_activeFile->saveSnapshot();
    rows[row].speaker = it->name();
    m_activeFile->model->refreshRow(row);
    m_activeFile->markDirty();
    refreshFileList();

    // Move to next row
    if (row < m_activeFile->model->rowCount() - 1)
        focusCell(row + 1, ColStart);
}

void MainWindow::switchToPreviousFile()
{
    if (!m_activeFile || m_files.size() <= 1) return;
    for (int i = 0; i < static_cast<int>(m_files.size()); ++i) {
        if (m_files[i].get() == m_activeFile && i > 0) {
            setActiveFile(m_files[i - 1].get());
            return;
        }
    }
}

void MainWindow::switchToNextFile()
{
    if (!m_activeFile || m_files.size() <= 1) return;
    for (int i = 0; i < static_cast<int>(m_files.size()); ++i) {
        if (m_files[i].get() == m_activeFile && i < static_cast<int>(m_files.size()) - 1) {
            setActiveFile(m_files[i + 1].get());
            return;
        }
    }
}

// ── Keyboard Handling ────────────────────────────────────────────

void MainWindow::keyPressEvent(QKeyEvent* e)
{
    bool ctrl = e->modifiers() & Qt::ControlModifier;
    bool shift = e->modifiers() & Qt::ShiftModifier;
    bool alt = e->modifiers() & Qt::AltModifier;

    if (!ctrl) { QMainWindow::keyPressEvent(e); return; }

    int key = e->key();

    // File operations
    if (key == Qt::Key_O && !shift) { openFile(); return; }
    if (key == Qt::Key_S && shift) { saveAsFile(); return; }
    if (key == Qt::Key_S && !shift) { saveFile(); return; }
    if (key == Qt::Key_E && shift) { exportSrtAll(); return; }
    if (key == Qt::Key_E && !shift) { exportSrtFile(); return; }
    if (key == Qt::Key_A && shift) { saveAllFiles(); return; }
    if (key == Qt::Key_W && shift) { closeAllFiles(); return; }
    if (key == Qt::Key_W && !shift) { closeFile(); return; }
    if (key == Qt::Key_F && !shift) { findReplace(); return; }
    if (key == Qt::Key_PageUp) { switchToPreviousFile(); return; }
    if (key == Qt::Key_PageDown) { switchToNextFile(); return; }

    // Row operations
    if (key == Qt::Key_Delete && !shift) { deleteCurrentRow(); return; }
    if (key == Qt::Key_Z && shift) { undoStructural(); return; }
    if (key == Qt::Key_J && shift) { mergeLineNoSpace(); return; }
    if (key == Qt::Key_J && !shift) { mergeLine(); return; }
    if (key == Qt::Key_L && !shift) { addBlankNext(); return; }
    if (key == Qt::Key_K && !shift) { splitLine(); return; }
    if (key == Qt::Key_P && !shift) { playSelection(); return; }
    if (key == Qt::Key_I && !shift) { shiftTime(); return; }
    if (key == Qt::Key_G && !shift) { toggleView(); return; }

    // Time operations
    // Note: Shift+[ = { (Key_BraceLeft) on US layout, so check both key codes
    bool isBracketLeft = (key == Qt::Key_BracketLeft || key == Qt::Key_BraceLeft);
    bool isBracketRight = (key == Qt::Key_BracketRight || key == Qt::Key_BraceRight);

    if (isBracketLeft && shift && !alt) { connectToPrevious(); return; }
    if (isBracketRight && shift && !alt) { connectToNext(); return; }
    if (key == Qt::Key_BracketLeft && !shift && !alt) { setStartTime(); return; }
    if (key == Qt::Key_BracketRight && !shift && !alt) { setEndTime(); return; }

    // Minimize to CPS / MinDur (Ctrl+Alt+Shift+Minus/Plus)
    if ((key == Qt::Key_Minus || key == Qt::Key_Underscore) && alt && shift) { minimizeStartToCPS(); return; }
    if ((key == Qt::Key_Plus || key == Qt::Key_Equal) && alt && shift) { minimizeEndToCPS(); return; }

    // Extend to CPS overwrite (Ctrl+Alt+Shift+; / ' → : / ")
    if ((key == Qt::Key_Semicolon || key == Qt::Key_Colon) && alt && shift) { extendStartToCPSOverwrite(); return; }
    if ((key == Qt::Key_Apostrophe || key == Qt::Key_QuoteDbl) && alt && shift) { extendEndToCPSOverwrite(); return; }

    // Extend Start/End to CPS push (Ctrl+Alt+Shift+[ or Left / ] or Right)
    if ((isBracketLeft || key == Qt::Key_Left) && alt && shift) { extendStartToCPS(); return; }
    if ((isBracketRight || key == Qt::Key_Right) && alt && shift) { extendEndToCPS(); return; }

    // Navigation (Ctrl+Up/Down → move to Source column, matching WPF col 7)
    if ((key == Qt::Key_Up || key == Qt::Key_Down) && !shift) {
        auto idx = m_tableView->currentIndex();
        if (idx.isValid()) {
            int newRow = (key == Qt::Key_Up) ? idx.row() - 1 : idx.row() + 1;
            if (newRow >= 0 && newRow < m_activeFile->model->rowCount())
                focusCell(newRow, ColSource);
        }
        return;
    }

    // Speaker shortcuts
    QString shortcutKey;
    if (key == Qt::Key_Plus || key == Qt::Key_Equal) {
        shortcutKey = "Plus";
    } else if (!shift) {
        switch (key) {
        case Qt::Key_1: shortcutKey = "1"; break;
        case Qt::Key_2: shortcutKey = "2"; break;
        case Qt::Key_3: shortcutKey = "3"; break;
        case Qt::Key_4: shortcutKey = "4"; break;
        case Qt::Key_5: shortcutKey = "5"; break;
        case Qt::Key_6: shortcutKey = "6"; break;
        case Qt::Key_7: shortcutKey = "7"; break;
        case Qt::Key_8: shortcutKey = "8"; break;
        case Qt::Key_9: shortcutKey = "9"; break;
        case Qt::Key_0: shortcutKey = "0"; break;
        case Qt::Key_Period: shortcutKey = "Period"; break;
        case Qt::Key_Slash: shortcutKey = "Slash"; break;
        case Qt::Key_Asterisk: shortcutKey = "Multiply"; break;
        case Qt::Key_Minus: shortcutKey = "Minus"; break;
        case Qt::Key_F1: shortcutKey = "F1"; break;
        case Qt::Key_F2: shortcutKey = "F2"; break;
        case Qt::Key_F3: shortcutKey = "F3"; break;
        case Qt::Key_F4: shortcutKey = "F4"; break;
        case Qt::Key_F5: shortcutKey = "F5"; break;
        case Qt::Key_F6: shortcutKey = "F6"; break;
        case Qt::Key_F7: shortcutKey = "F7"; break;
        case Qt::Key_F8: shortcutKey = "F8"; break;
        case Qt::Key_F9: shortcutKey = "F9"; break;
        case Qt::Key_F10: shortcutKey = "F10"; break;
        case Qt::Key_F11: shortcutKey = "F11"; break;
        case Qt::Key_F12: shortcutKey = "F12"; break;
        }
    }

    if (!shortcutKey.isEmpty()) {
        applySpeakerShortcut(shortcutKey);
        return;
    }

    QMainWindow::keyPressEvent(e);
}

void MainWindow::mousePressEvent(QMouseEvent* e)
{
    if (handleMouseAction(e->button())) return;
    QMainWindow::mousePressEvent(e);
}

bool MainWindow::handleMouseAction(Qt::MouseButton btn)
{
    if (btn == Qt::BackButton) { setEndTime(); return true; }
    if (btn == Qt::ForwardButton) { setStartTime(); return true; }
    if (btn == Qt::MiddleButton) { splitLine(); return true; }
    if (btn == Qt::RightButton) { playSelection(); return true; }
    return false;
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    // Global mouse button shortcuts (Back/Forward/Middle/Right anywhere in window)
    if (event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() != Qt::LeftButton && handleMouseAction(me->button()))
            return true;
    }

    // Tab navigation in table: skip read-only columns
    if (obj == m_tableView && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) {
            if (!m_activeFile) return false;

            bool shift = (ke->key() == Qt::Key_Backtab) || (ke->modifiers() & Qt::ShiftModifier);
            static const int editableCols[] = { ColSpeaker, ColSource, ColTarget };
            static const int numEditable = 3;

            auto idx = m_tableView->currentIndex();
            if (!idx.isValid()) return false;

            int curRow = idx.row();
            int curCol = idx.column();

            // Find current position in editable list
            int curEditIdx = -1;
            for (int e = 0; e < numEditable; ++e) {
                if (editableCols[e] == curCol) { curEditIdx = e; break; }
            }

            int nextEditIdx, nextRow;
            if (curEditIdx < 0) {
                nextEditIdx = shift ? numEditable - 1 : 0;
                nextRow = curRow;
            } else if (shift) {
                nextEditIdx = curEditIdx - 1;
                nextRow = curRow;
                if (nextEditIdx < 0) {
                    nextEditIdx = numEditable - 1;
                    nextRow = std::max(0, curRow - 1);
                }
            } else {
                nextEditIdx = curEditIdx + 1;
                nextRow = curRow;
                if (nextEditIdx >= numEditable) {
                    nextEditIdx = 0;
                    nextRow = std::min(m_activeFile->model->rowCount() - 1, curRow + 1);
                }
            }

            focusCell(nextRow, editableCols[nextEditIdx]);
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls()) {
        for (const auto& url : event->mimeData()->urls()) {
            QString path = url.toLocalFile().toLower();
            if (path.endsWith(".xlsx") || path.endsWith(".m4a") ||
                path.endsWith(".mp3") || path.endsWith(".wav")) {
                event->acceptProposedAction();
                return;
            }
        }
    }
}

void MainWindow::dropEvent(QDropEvent* event)
{
    if (!event->mimeData()->hasUrls()) return;

    QStringList xlsxFiles;
    QString audioFile;

    for (const auto& url : event->mimeData()->urls()) {
        QString path = url.toLocalFile();
        QString lower = path.toLower();
        if (lower.endsWith(".xlsx"))
            xlsxFiles.append(path);
        else if (lower.endsWith(".m4a") || lower.endsWith(".mp3") || lower.endsWith(".wav"))
            audioFile = path;
    }

    // Handle audio drop
    if (!audioFile.isEmpty()) {
        loadAudioFile(audioFile);
    }

    // Handle xlsx drop
    if (!xlsxFiles.isEmpty()) {
        m_bulkLoading = true;
        for (const auto& f : xlsxFiles)
            doOpenFile(f);
        m_bulkLoading = false;
        if (!m_files.empty())
            setActiveFile(m_files.back().get());
    }
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    m_player->stop();
    for (auto& file : m_files) {
        if (file->dirty) {
            auto result = QMessageBox::question(this, "Save Changes",
                QString("Save changes to %1?").arg(QFileInfo(file->filePath).fileName()),
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
            if (result == QMessageBox::Cancel) { e->ignore(); return; }
            if (result == QMessageBox::Yes)
                doSaveFile(file.get(), file->filePath);
        }
    }

    e->accept();
}
