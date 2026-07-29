#include "SubtitleModel.h"
#include "helpers/Decimal.h"

SubtitleModel::SubtitleModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int SubtitleModel::rowCount(const QModelIndex&) const
{
    return static_cast<int>(m_rows.size());
}

int SubtitleModel::columnCount(const QModelIndex&) const
{
    return ColCount;
}

QVariant SubtitleModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= static_cast<int>(m_rows.size()))
        return {};

    const auto& row = m_rows[index.row()];

    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
        case ColIndex:    return index.row() + 1;
        case ColStart:    return row.start;
        case ColEnd:      return row.end;
        case ColDuration: return row.durationStr();
        case ColGap:      return row.gapStr();
        case ColCPS:      return row.cpsStr();
        case ColSpeaker:  return row.speaker;
        case ColSource:   return row.source;
        case ColTarget:   return row.target;
        case ColMaxLen:   return row.maxLineLenStr();
        }
    }

    if (role == Qt::ForegroundRole) {
        switch (index.column()) {
        case ColIndex:    return QColor(Qt::gray);
        case ColDuration: return row.isDurationWarning() ? QColor(Qt::red) : QVariant{};
        case ColGap:      return row.gapWarning ? QColor(Qt::red) : QVariant{};
        case ColCPS:      return row.isCPSWarning() ? QColor(Qt::red) : QVariant{};
        case ColMaxLen:   return row.isMaxLineLenWarning() ? QColor(Qt::red) : QVariant{};
        case ColSpeaker:
            if (globalSettings().highlightConsecutiveSpeaker &&
                !row.speaker.isEmpty() && index.row() > 0 &&
                m_rows[index.row() - 1].speaker == row.speaker &&
                m_rows[index.row() - 1].end == row.start)
                return QColor(Qt::blue);
            return {};
        }
    }

    if (role == Qt::BackgroundRole) {
        if (index.row() == m_highlightRow)
            return QColor(220, 235, 255);
    }

    if (role == Qt::TextAlignmentRole) {
        switch (index.column()) {
        case ColIndex:
        case ColDuration:
        case ColGap:
        case ColCPS:
        case ColMaxLen:
            return QVariant(Qt::AlignRight | Qt::AlignVCenter);
        }
    }

    return {};
}

QVariant SubtitleModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};

    switch (section) {
    case ColIndex:    return QStringLiteral("#");
    case ColStart:    return QStringLiteral("Start");
    case ColEnd:      return QStringLiteral("End");
    case ColDuration: return QStringLiteral("Duration");
    case ColGap:      return QStringLiteral("Gap");
    case ColCPS:      return QStringLiteral("CPS");
    case ColSpeaker:  return QStringLiteral("Speaker");
    case ColSource:   return QStringLiteral("Source");
    case ColTarget:   return QStringLiteral("Target");
    case ColMaxLen:   return QStringLiteral("MaxLen");
    }
    return {};
}

Qt::ItemFlags SubtitleModel::flags(const QModelIndex& index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;

    auto base = QAbstractTableModel::flags(index);

    switch (index.column()) {
    case ColSpeaker:
    case ColSource:
    case ColTarget:
        return base | Qt::ItemIsEditable;
    default:
        return base;
    }
}

bool SubtitleModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (!index.isValid() || role != Qt::EditRole)
        return false;

    auto& row = m_rows[index.row()];
    QString val = value.toString();

    switch (index.column()) {
    case ColSpeaker:
        if (row.speaker == val) return false;
        row.speaker = val;
        break;
    case ColSource:
        if (row.source == val) return false;
        row.source = val;
        break;
    case ColTarget:
        if (row.target == val) return false;
        row.target = val;
        row.recalcTargetMetrics();
        break;
    default:
        return false;
    }

    // Emit change for the entire row (computed columns may have changed)
    emit dataChanged(this->index(index.row(), 0),
                     this->index(index.row(), ColCount - 1));
    return true;
}

void SubtitleModel::setRows(std::vector<SubtitleRow> rows)
{
    beginResetModel();
    m_rows = std::move(rows);
    for (auto& r : m_rows)
        r.recalcAll();
    updateGaps();
    endResetModel();
}

void SubtitleModel::insertRow(int position, const SubtitleRow& row)
{
    beginInsertRows({}, position, position);
    m_rows.insert(m_rows.begin() + position, row);
    endInsertRows();
    updateGaps();
}

void SubtitleModel::removeRow(int position)
{
    if (position < 0 || position >= static_cast<int>(m_rows.size()))
        return;
    beginRemoveRows({}, position, position);
    m_rows.erase(m_rows.begin() + position);
    endRemoveRows();
    updateGaps();
}

void SubtitleModel::updateGaps()
{
    for (size_t i = 0; i < m_rows.size(); ++i) {
        auto& cur = m_rows[i];
        if (i == m_rows.size() - 1) {
            cur.gapSeconds = 0.0;
            cur.gapWarning = false;
            continue;
        }
        auto& next = m_rows[i + 1];
        Decimal curEnd, nextStart;
        if (SubtitleRow::tryParseTime(cur.end, curEnd) && SubtitleRow::tryParseTime(next.start, nextStart)) {
            cur.gapSeconds = nextStart - curEnd;
            cur.gapWarning = cur.gapSeconds < globalSettings().minGapSeconds;
        } else {
            cur.gapSeconds = 0.0;
            cur.gapWarning = false;
        }
    }
}

void SubtitleModel::refreshRow(int row)
{
    if (row < 0 || row >= static_cast<int>(m_rows.size()))
        return;
    emit dataChanged(index(row, 0), index(row, ColCount - 1));
}

void SubtitleModel::setHighlightRow(int row)
{
    int old = m_highlightRow;
    m_highlightRow = row;
    if (old >= 0 && old < static_cast<int>(m_rows.size()))
        emit dataChanged(index(old, 0), index(old, ColCount - 1), {Qt::BackgroundRole});
    if (row >= 0 && row < static_cast<int>(m_rows.size()))
        emit dataChanged(index(row, 0), index(row, ColCount - 1), {Qt::BackgroundRole});
}

void SubtitleModel::refreshAll()
{
    if (m_rows.empty()) return;
    updateGaps();
    emit dataChanged(index(0, 0), index(static_cast<int>(m_rows.size()) - 1, ColCount - 1));
}
