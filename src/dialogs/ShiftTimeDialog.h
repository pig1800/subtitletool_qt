#pragma once
#include <QDialog>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>

class ShiftTimeDialog : public QDialog {
    Q_OBJECT
public:
    explicit ShiftTimeDialog(QWidget* parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle("Shift Time");
        setFixedSize(300, 150);

        auto* layout = new QVBoxLayout(this);
        layout->addWidget(new QLabel("Enter time to shift in seconds (e.g. 1.500 or -0.5):"));
        m_edit = new QLineEdit;
        layout->addWidget(m_edit);
        layout->addStretch();

        auto* btnLayout = new QHBoxLayout;
        btnLayout->addStretch();
        auto* okBtn = new QPushButton("OK");
        auto* cancelBtn = new QPushButton("Cancel");
        btnLayout->addWidget(okBtn);
        btnLayout->addWidget(cancelBtn);
        layout->addLayout(btnLayout);

        connect(okBtn, &QPushButton::clicked, this, &ShiftTimeDialog::tryAccept);
        connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
        connect(m_edit, &QLineEdit::returnPressed, this, &ShiftTimeDialog::tryAccept);

        m_edit->setFocus();
    }

    double shiftSeconds() const { return m_shiftSeconds; }

private slots:
    void tryAccept() {
        bool ok = false;
        double val = m_edit->text().toDouble(&ok);
        if (!ok) {
            QMessageBox::warning(this, "Invalid Input", "Please enter a valid number (e.g. 1.5 or -2).");
            return;
        }
        m_shiftSeconds = val;
        accept();
    }

private:
    QLineEdit* m_edit;
    double m_shiftSeconds = 0.0;
};
