#pragma once
#include <QDialog>
#include <QLineEdit>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include "helpers/Decimal.h"

class ProjectSettingsDialog : public QDialog {
    Q_OBJECT
public:
    struct Settings {
        double frameRate = 0.0;
        Decimal minGap = 0.0;
        Decimal minDuration = 0.8;
        Decimal maxDuration = 0.0;
        Decimal maxLength = 12.0;
        Decimal maxCPS = 6.9;
        double audioOffsetMs = 0.0;
        bool highlightConsecutiveSpeaker = false;
    };

    explicit ProjectSettingsDialog(QWidget* parent, const Settings& current)
        : QDialog(parent), m_settings(current)
    {
        setWindowTitle("Project Settings");
        setMinimumWidth(320);

        auto* grid = new QGridLayout(this);
        int row = 0;

        auto addField = [&](const QString& label, QLineEdit*& edit, const QString& val) {
            grid->addWidget(new QLabel(label), row, 0, 1, 2);
            ++row;
            edit = new QLineEdit(val);
            grid->addWidget(edit, row, 0, 1, 2);
            ++row;
        };

        addField("Framerate (0 to disable snapping):", m_frameRateEdit,
                 QString::number(current.frameRate, 'f', 3));
        addField("Min Gap (s) (0 to never red):", m_minGapEdit,
                 current.minGap.toString(3));
        addField("Min Duration (s):", m_minDurationEdit,
                 current.minDuration.toString(3));
        addField("Max Duration (s):", m_maxDurationEdit,
                 current.maxDuration.toString(3));
        addField("Max Length (chars):", m_maxLengthEdit,
                 current.maxLength.toString(0));
        addField("Max CPS:", m_maxCPSEdit,
                 current.maxCPS.toString(1));
        addField("Audio Offset (ms):", m_audioOffsetEdit,
                 QString::number(current.audioOffsetMs, 'f', 0));

        m_highlightCheck = new QCheckBox("Highlight consecutive same speaker");
        m_highlightCheck->setChecked(current.highlightConsecutiveSpeaker);
        grid->addWidget(m_highlightCheck, row, 0, 1, 2);
        ++row;

        auto* btnRow = new QHBoxLayout;
        btnRow->addStretch();
        auto* okBtn = new QPushButton("OK");
        auto* cancelBtn = new QPushButton("Cancel");
        btnRow->addWidget(okBtn);
        btnRow->addWidget(cancelBtn);
        grid->addLayout(btnRow, row, 0, 1, 2);

        connect(okBtn, &QPushButton::clicked, this, &ProjectSettingsDialog::tryAccept);
        connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);

        m_frameRateEdit->setFocus();
        m_frameRateEdit->selectAll();
    }

    Settings settings() const { return m_settings; }

private:
    bool validate(Settings& out) {
        bool ok = true;
        out.frameRate = m_frameRateEdit->text().toDouble(&ok); if (!ok) goto fail;
        out.minGap = m_minGapEdit->text().toDouble(&ok); if (!ok) goto fail;
        out.minDuration = m_minDurationEdit->text().toDouble(&ok); if (!ok) goto fail;
        out.maxDuration = m_maxDurationEdit->text().toDouble(&ok); if (!ok) goto fail;
        out.maxLength = m_maxLengthEdit->text().toDouble(&ok); if (!ok) goto fail;
        out.maxCPS = m_maxCPSEdit->text().toDouble(&ok); if (!ok) goto fail;
        out.audioOffsetMs = m_audioOffsetEdit->text().toDouble(&ok); if (!ok) goto fail;
        if (out.frameRate < 0) out.frameRate = 0;
        if (out.minGap < 0) out.minGap = 0;
        if (out.minDuration < 0) out.minDuration = 0;
        if (out.maxDuration < 0) out.maxDuration = 0;
        if (out.maxLength < 0) out.maxLength = 0;
        if (out.maxCPS < 0) out.maxCPS = 0;
        out.highlightConsecutiveSpeaker = m_highlightCheck->isChecked();
        return true;
    fail:
        QMessageBox::warning(this, "Error", "Invalid input format.");
        return false;
    }

private slots:
    void tryAccept() {
        Settings s;
        if (validate(s)) {
            m_settings = s;
            accept();
        }
    }

private:
    Settings m_settings;
    QLineEdit* m_frameRateEdit;
    QLineEdit* m_minGapEdit;
    QLineEdit* m_minDurationEdit;
    QLineEdit* m_maxDurationEdit;
    QLineEdit* m_maxLengthEdit;
    QLineEdit* m_maxCPSEdit;
    QLineEdit* m_audioOffsetEdit;
    QCheckBox* m_highlightCheck;
};
