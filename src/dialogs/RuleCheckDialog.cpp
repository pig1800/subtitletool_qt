#include "RuleCheckDialog.h"
#include "MainWindow.h"

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

void RuleCheckDialog::onSelectionChanged(int row)
{
    if (row < 0 || row >= static_cast<int>(m_violations.size())) return;
    auto& v = m_violations[row];
    m_parent->focusFileAndCell(v.file, v.rowIndex, v.column);
}
