#include "RuleCheckDialog.h"
#include "MainWindow.h"
#include <algorithm>

RuleCheckDialog::RuleCheckDialog(MainWindow* parent, const std::vector<RuleViolation>& violations)
    : QDialog(parent, Qt::Tool)
    , m_parent(parent)
{
    setWindowTitle("Rule Check");
    resize(600, 400);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel("<b>Rule Violations:</b>"));
    m_list = new QListWidget;
    layout->addWidget(m_list);

    connect(m_list, &QListWidget::currentRowChanged, this, &RuleCheckDialog::onSelectionChanged);

    updateViolations(violations);
}

void RuleCheckDialog::updateViolations(const std::vector<RuleViolation>& violations)
{
    m_violations = violations;
    m_list->clear();
    for (const auto& v : m_violations)
        m_list->addItem(v.displayText);
}

bool RuleCheckDialog::removeFile(FileTab* file)
{
    size_t before = m_violations.size();
    m_violations.erase(
        std::remove_if(m_violations.begin(), m_violations.end(),
                       [file](const RuleViolation& v) { return v.file == file; }),
        m_violations.end());
    if (m_violations.size() == before)
        return false;

    // Preserve the current selection if its entry still exists, else clear.
    int cur = m_list->currentRow();
    m_list->clear();
    for (const auto& v : m_violations)
        m_list->addItem(v.displayText);
    if (cur >= 0 && cur < static_cast<int>(m_violations.size()))
        m_list->setCurrentRow(cur);
    else
        m_list->setCurrentRow(-1);
    return true;
}

void RuleCheckDialog::onSelectionChanged(int row)
{
    if (row < 0 || row >= static_cast<int>(m_violations.size())) return;
    auto& v = m_violations[row];
    m_parent->focusFileAndCell(v.file, v.rowIndex, v.column);
}
