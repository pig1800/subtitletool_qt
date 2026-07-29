#include "FindReplaceDialog.h"
#include "MainWindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QRegularExpression>

FindReplaceDialog::FindReplaceDialog(MainWindow* parent)
    : QDialog(parent, Qt::Tool | Qt::WindowStaysOnTopHint)
    , m_parent(parent)
{
    setWindowTitle("Find and Replace");
    resize(550, 450);

    auto* grid = new QGridLayout;

    grid->addWidget(new QLabel("Find:"), 0, 0);
    m_findBox = new QLineEdit;
    grid->addWidget(m_findBox, 0, 1);

    grid->addWidget(new QLabel("Replace:"), 1, 0);
    m_replaceBox = new QLineEdit;
    grid->addWidget(m_replaceBox, 1, 1);

    auto* checkLayout = new QHBoxLayout;
    m_regexCheck = new QCheckBox("Use Regular Expressions");
    m_targetOnlyCheck = new QCheckBox("Target Only");
    m_targetOnlyCheck->setChecked(true);
    checkLayout->addWidget(m_regexCheck);
    checkLayout->addWidget(m_targetOnlyCheck);
    grid->addLayout(checkLayout, 2, 1);

    auto* btnLayout = new QHBoxLayout;
    btnLayout->addStretch();
    auto* findBtn = new QPushButton("Find");
    auto* findAllBtn = new QPushButton("Find All");
    auto* replaceBtn = new QPushButton("Replace");
    auto* replaceAllBtn = new QPushButton("Replace All");
    findBtn->setDefault(true);
    btnLayout->addWidget(findBtn);
    btnLayout->addWidget(findAllBtn);
    btnLayout->addWidget(replaceBtn);
    btnLayout->addWidget(replaceAllBtn);
    grid->addLayout(btnLayout, 3, 1);

    m_resultsList = new QListWidget;
    grid->addWidget(m_resultsList, 4, 0, 1, 2);

    setLayout(grid);

    connect(findBtn, &QPushButton::clicked, this, &FindReplaceDialog::findNext);
    connect(findAllBtn, &QPushButton::clicked, this, &FindReplaceDialog::findAll);
    connect(replaceBtn, &QPushButton::clicked, this, &FindReplaceDialog::replace);
    connect(replaceAllBtn, &QPushButton::clicked, this, &FindReplaceDialog::replaceAll);
    connect(m_resultsList, &QListWidget::currentRowChanged, this, &FindReplaceDialog::onResultSelected);
}

void FindReplaceDialog::findNext()
{
    QString pattern = m_findBox->text();
    if (pattern.isEmpty()) return;

    if (!m_regexCheck->isChecked())
        pattern = QRegularExpression::escape(pattern);

    QRegularExpression re(pattern, QRegularExpression::CaseInsensitiveOption);
    if (!re.isValid()) {
        QMessageBox::warning(this, "Regex Error", re.errorString());
        return;
    }

    auto& files = m_parent->files();
    if (files.empty()) return;

    if (!m_hasStarted) {
        auto* active = m_parent->activeFile();
        if (active) {
            for (int i = 0; i < static_cast<int>(files.size()); ++i) {
                if (files[i].get() == active) {
                    m_currentFileIndex = i;
                    break;
                }
            }
        }
        m_currentRowIndex = 0;
        m_currentColIndex = 0;
    }

    if (m_hasStarted)
        advanceSearchPosition();
    m_hasStarted = true;

    int startF = m_currentFileIndex;
    int startR = m_currentRowIndex;
    int startC = m_currentColIndex;
    bool looped = false;

    while (!looped) {
        auto& file = files[m_currentFileIndex];
        auto& rows = file->model->rows();
        if (m_currentRowIndex < static_cast<int>(rows.size())) {
            auto& row = rows[m_currentRowIndex];
            bool skipSource = m_targetOnlyCheck->isChecked();
            if (!skipSource || m_currentColIndex == 1) {
                const QString& text = (m_currentColIndex == 0) ? row.source : row.target;
                if (re.match(text).hasMatch()) {
                    QString col = (m_currentColIndex == 0) ? "Source" : "Target";
                    m_parent->focusFileAndCell(file.get(), m_currentRowIndex, col);
                    return;
                }
            }
        }

        advanceSearchPosition();
        if (m_currentFileIndex == startF && m_currentRowIndex == startR && m_currentColIndex == startC)
            looped = true;
    }

    QMessageBox::information(this, "Find", "Not found.");
}

void FindReplaceDialog::findAll()
{
    QString pattern = m_findBox->text();
    if (pattern.isEmpty()) return;
    if (!m_regexCheck->isChecked())
        pattern = QRegularExpression::escape(pattern);

    QRegularExpression re(pattern, QRegularExpression::CaseInsensitiveOption);
    if (!re.isValid()) return;

    m_results.clear();
    m_resultsList->clear();
    bool targetOnly = m_targetOnlyCheck->isChecked();

    for (auto& file : m_parent->files()) {
        auto& rows = file->model->rows();
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            auto& row = rows[i];
            if (!targetOnly && re.match(row.source).hasMatch()) {
                SearchResult sr{file.get(), i, "Source", row.source,
                    QString("[%1] Row %2 (Source): %3").arg(file->displayName()).arg(i + 1).arg(row.source)};
                m_results.push_back(sr);
                m_resultsList->addItem(sr.displayText);
            }
            if (re.match(row.target).hasMatch()) {
                SearchResult sr{file.get(), i, "Target", row.target,
                    QString("[%1] Row %2 (Target): %3").arg(file->displayName()).arg(i + 1).arg(row.target)};
                m_results.push_back(sr);
                m_resultsList->addItem(sr.displayText);
            }
        }
    }

    if (m_results.empty())
        QMessageBox::information(this, "Find All", "Not found.");
}

void FindReplaceDialog::replace()
{
    // Replace current, then find next
    QString pattern = m_findBox->text();
    if (pattern.isEmpty()) return;
    if (!m_regexCheck->isChecked())
        pattern = QRegularExpression::escape(pattern);

    QRegularExpression re(pattern, QRegularExpression::CaseInsensitiveOption);
    if (!re.isValid()) return;

    auto& files = m_parent->files();
    if (files.empty()) return;

    auto& file = files[m_currentFileIndex];
    auto& rows = file->model->rows();
    if (m_currentRowIndex < static_cast<int>(rows.size())) {
        auto& row = rows[m_currentRowIndex];
        QString replacement = m_replaceBox->text();
        bool targetOnly = m_targetOnlyCheck->isChecked();

        if (m_currentColIndex == 1 && re.match(row.target).hasMatch()) {
            row.target = row.target.replace(re, replacement);
            row.recalcTargetMetrics();
            file->markDirty();
            file->model->refreshRow(m_currentRowIndex);
        } else if (!targetOnly && m_currentColIndex == 0 && re.match(row.source).hasMatch()) {
            row.source = row.source.replace(re, replacement);
            file->markDirty();
            file->model->refreshRow(m_currentRowIndex);
        }
    }

    findNext();
}

void FindReplaceDialog::replaceAll()
{
    QString pattern = m_findBox->text();
    if (pattern.isEmpty()) return;
    if (!m_regexCheck->isChecked())
        pattern = QRegularExpression::escape(pattern);

    QRegularExpression re(pattern, QRegularExpression::CaseInsensitiveOption);
    if (!re.isValid()) return;

    int count = 0;
    bool targetOnly = m_targetOnlyCheck->isChecked();
    QString replacement = m_replaceBox->text();

    for (auto& file : m_parent->files()) {
        auto& rows = file->model->rows();
        for (auto& row : rows) {
            if (!targetOnly && re.match(row.source).hasMatch()) {
                row.source = row.source.replace(re, replacement);
                ++count;
            }
            if (re.match(row.target).hasMatch()) {
                row.target = row.target.replace(re, replacement);
                row.recalcTargetMetrics();
                ++count;
            }
        }
        if (count > 0) {
            file->markDirty();
            file->model->refreshAll();
        }
    }

    QMessageBox::information(this, "Replace All", QString("Replaced %1 occurrences.").arg(count));
    if (!m_results.empty())
        findAll();
}

void FindReplaceDialog::onResultSelected(int row)
{
    if (row < 0 || row >= static_cast<int>(m_results.size())) return;
    auto& sr = m_results[row];
    m_parent->focusFileAndCell(sr.file, sr.rowIndex, sr.column);
}

void FindReplaceDialog::advanceSearchPosition()
{
    auto& files = m_parent->files();
    m_currentColIndex++;
    if (m_currentColIndex > 1) {
        m_currentColIndex = 0;
        m_currentRowIndex++;
        auto& file = files[m_currentFileIndex];
        if (m_currentRowIndex >= static_cast<int>(file->model->rows().size())) {
            m_currentRowIndex = 0;
            m_currentFileIndex++;
            if (m_currentFileIndex >= static_cast<int>(files.size()))
                m_currentFileIndex = 0;
        }
    }
    if (m_targetOnlyCheck->isChecked() && m_currentColIndex == 0)
        m_currentColIndex = 1;
}
