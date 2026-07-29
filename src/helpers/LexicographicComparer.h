#pragma once
#include <QString>

// Natural-order string comparison (numbers treated numerically)
inline int lexicographicCompare(const QString& x, const QString& y)
{
    if (x == y) return 0;
    if (x.isEmpty() && !y.isEmpty()) return -1;
    if (!x.isEmpty() && y.isEmpty()) return 1;
    if (x.isEmpty() && y.isEmpty()) return 0;

    int i = 0, j = 0;
    while (i < x.size() && j < y.size()) {
        QChar cx = x[i], cy = y[j];

        if (cx.isDigit() && cy.isDigit()) {
            // Compare numeric spans
            int ni = i, nj = j;
            while (ni < x.size() && x[ni].isDigit()) ++ni;
            while (nj < y.size() && y[nj].isDigit()) ++nj;

            // Count leading zeros
            int lzx = 0, lzy = 0;
            for (int k = i; k < ni && x[k] == '0'; ++k) ++lzx;
            for (int k = j; k < nj && y[k] == '0'; ++k) ++lzy;

            double vx = x.mid(i, ni - i).toDouble();
            double vy = y.mid(j, nj - j).toDouble();

            if (vx != vy) return vx < vy ? -1 : 1;
            if (lzx != lzy) return lzx > lzy ? -1 : 1;

            i = ni;
            j = nj;
        } else {
            int cmp = cx.toUpper().unicode() - cy.toUpper().unicode();
            if (cmp != 0) return cmp;
            ++i;
            ++j;
        }
    }

    if (x.size() < y.size()) return -1;
    if (x.size() > y.size()) return 1;
    return 0;
}
