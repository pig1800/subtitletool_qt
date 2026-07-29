#pragma once
#include <QAbstractTableModel>
#include <QColor>
#include <vector>
#include "SubtitleRow.h"

// Column indices
enum Column {
    ColIndex = 0,
    ColStart,
    ColEnd,
    ColDuration,
    ColGap,
    ColCPS,
    ColSpeaker,
    ColSource,
    ColTarget,
    ColMaxLen,
    ColCount
};

class SubtitleModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit SubtitleModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

    void setRows(std::vector<SubtitleRow> rows);
    std::vector<SubtitleRow>& rows() { return m_rows; }
    const std::vector<SubtitleRow>& rows() const { return m_rows; }

    void insertRow(int position, const SubtitleRow& row);
    void removeRow(int position);
    void updateGaps();
    void refreshRow(int row);
    void refreshAll();
    void setHighlightRow(int row);
    int highlightRow() const { return m_highlightRow; }

private:
    std::vector<SubtitleRow> m_rows;
    int m_highlightRow = -1;
};
