#pragma once
#include <QDialog>
#include <QListWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <vector>

class MainWindow;
class FileTab;

struct RuleViolation {
    FileTab* file = nullptr;
    int rowIndex = -1;
    QString column;
    QString displayText;
};

class RuleCheckDialog : public QDialog {
    Q_OBJECT
public:
    explicit RuleCheckDialog(MainWindow* parent, const std::vector<RuleViolation>& violations);
    void updateViolations(const std::vector<RuleViolation>& violations);

protected:
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Escape) close();
        else QDialog::keyPressEvent(e);
    }

private slots:
    void onSelectionChanged(int row);

private:
    MainWindow* m_parent;
    QListWidget* m_list;
    std::vector<RuleViolation> m_violations;
};
