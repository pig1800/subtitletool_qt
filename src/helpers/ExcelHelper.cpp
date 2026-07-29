#include "ExcelHelper.h"
#include <xlnt/xlnt.hpp>

namespace ExcelHelper {

std::vector<SubtitleRow> readXlsx(const QString& filePath)
{
    std::vector<SubtitleRow> rows;

    xlnt::workbook wb;
    wb.load(filePath.toStdString());
    auto ws = wb.active_sheet();

    // Detect last column/row
    auto dim = ws.calculate_dimension();
    int lastCol = static_cast<int>(dim.width());
    int lastRow = static_cast<int>(dim.height());
    if (lastCol == 0 || lastRow == 0) return rows;

    // Read header row (row 1) to find column indices
    int colStart = -1, colEnd = -1, colSpeaker = -1, colSource = -1, colTarget = -1;

    for (int c = 1; c <= lastCol; ++c) {
        auto ref = xlnt::cell_reference(static_cast<xlnt::column_t::index_t>(c), 1);
        if (!ws.has_cell(ref)) continue;
        std::string header = ws.cell(ref).to_string();

        // toLower + trim
        std::string lower;
        lower.reserve(header.size());
        for (char ch : header) lower += static_cast<char>(std::tolower(ch));
        while (!lower.empty() && (lower.front() == ' ' || lower.front() == '\t')) lower.erase(lower.begin());
        while (!lower.empty() && (lower.back() == ' ' || lower.back() == '\t')) lower.pop_back();

        if (lower == "start") colStart = c;
        else if (lower == "end") colEnd = c;
        else if (lower == "speaker") colSpeaker = c;
        else if (lower == "source") colSource = c;
        else if (lower == "target") colTarget = c;
    }

    // Read data rows (row 2 onwards)
    auto getStr = [&](int r, int col) -> QString {
        if (col < 0) return {};
        auto ref = xlnt::cell_reference(static_cast<xlnt::column_t::index_t>(col), static_cast<xlnt::row_t>(r));
        if (!ws.has_cell(ref)) return {};
        return QString::fromStdString(ws.cell(ref).to_string());
    };

    for (int r = 2; r <= lastRow; ++r) {
        SubtitleRow sr;
        sr.start   = getStr(r, colStart);
        sr.end     = getStr(r, colEnd);
        sr.speaker = getStr(r, colSpeaker);
        sr.source  = getStr(r, colSource);
        sr.target  = getStr(r, colTarget);
        sr.recalcAll();
        rows.push_back(std::move(sr));
    }

    return rows;
}

void writeXlsx(const QString& filePath, const std::vector<SubtitleRow>& rows)
{
    xlnt::workbook wb;
    auto ws = wb.active_sheet();
    ws.title("Subtitles");

    ws.cell("A1").value("Start");
    ws.cell("B1").value("End");
    ws.cell("C1").value("Speaker");
    ws.cell("D1").value("Source");
    ws.cell("E1").value("Target");

    for (size_t i = 0; i < rows.size(); ++i) {
        auto r = static_cast<xlnt::row_t>(i + 2);
        const auto& row = rows[i];
        ws.cell(xlnt::cell_reference(1, r)).value(row.start.toStdString());
        ws.cell(xlnt::cell_reference(2, r)).value(row.end.toStdString());
        ws.cell(xlnt::cell_reference(3, r)).value(row.speaker.toStdString());
        ws.cell(xlnt::cell_reference(4, r)).value(row.source.toStdString());
        ws.cell(xlnt::cell_reference(5, r)).value(row.target.toStdString());
    }

    wb.save(filePath.toStdString());
}

} // namespace ExcelHelper
