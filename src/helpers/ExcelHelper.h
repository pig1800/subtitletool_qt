#pragma once
#include <QString>
#include <vector>
#include "models/SubtitleRow.h"

namespace ExcelHelper {

std::vector<SubtitleRow> readXlsx(const QString& filePath);
void writeXlsx(const QString& filePath, const std::vector<SubtitleRow>& rows);

} // namespace ExcelHelper
