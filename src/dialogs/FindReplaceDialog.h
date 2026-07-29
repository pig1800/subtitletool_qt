#pragma once
#include <QDialog>
#include <QLineEdit>
#include <QCheckBox>
#include <QListWidget>
#include <QPushButton>
#include <vector>

class MainWindow;
struct SubtitleRow;
class FileTab;

struct SearchResult {
    FileTab* file = nullptr;
    int rowIndex = -1;
    QString column;
    QString matchText;
    QString displayText;
};

class FindReplaceDialog : public QDialog {
    Q_OBJECT
public:
    explicit FindReplaceDialog(MainWindow* parent);

private slots:
    void findNext();
    void findAll();
    void replace();
    void replaceAll();
    void onResultSelected(int row);

private:
    MainWindow* m_parent;
    QLineEdit* m_findBox;
    QLineEdit* m_replaceBox;
    QCheckBox* m_regexCheck;
    QCheckBox* m_targetOnlyCheck;
    QListWidget* m_resultsList;

    std::vector<SearchResult> m_results;

    int m_currentFileIndex = 0;
    int m_currentRowIndex = 0;
    int m_currentColIndex = 0;
    bool m_hasStarted = false;

    void advanceSearchPosition();
};
