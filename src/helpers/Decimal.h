#pragma once
#include <cstdint>
#include <cmath>
#include <QString>

// Fixed-point decimal with millisecond precision (3 decimal places).
// Stores value * 1000 as int64_t, so 0.8 → 800, 6.9 → 6900.
// Comparisons are exact integer ops — no floating-point drift.
// Implicit conversion to/from double; friend operators resolve ambiguity.

class Decimal {
    int64_t m_val = 0;
public:
    constexpr Decimal() = default;

    // Implicit from double — rounds to nearest thousandth
    Decimal(double v) : m_val(std::llround(v * 1000.0)) {}

    // Construct from raw thousandths (e.g., 800 = 0.800)
    static constexpr Decimal fromRaw(int64_t raw) { Decimal d; d.m_val = raw; return d; }

    // Accessors
    constexpr int64_t raw() const { return m_val; }
    double toDouble() const { return m_val / 1000.0; }
    operator double() const { return toDouble(); }

    // Decimal ↔ Decimal arithmetic
    Decimal operator+(Decimal o) const { return fromRaw(m_val + o.m_val); }
    Decimal operator-(Decimal o) const { return fromRaw(m_val - o.m_val); }
    Decimal operator-() const { return fromRaw(-m_val); }
    Decimal& operator+=(Decimal o) { m_val += o.m_val; return *this; }
    Decimal& operator-=(Decimal o) { m_val -= o.m_val; return *this; }

    // Decimal ↔ Decimal comparison (exact integer)
    bool operator==(Decimal o) const { return m_val == o.m_val; }
    bool operator!=(Decimal o) const { return m_val != o.m_val; }
    bool operator< (Decimal o) const { return m_val <  o.m_val; }
    bool operator> (Decimal o) const { return m_val >  o.m_val; }
    bool operator<=(Decimal o) const { return m_val <= o.m_val; }
    bool operator>=(Decimal o) const { return m_val >= o.m_val; }

    // Mixed comparison: Decimal vs double (resolves ambiguity with operator double)
    friend bool operator==(Decimal a, double b) { return a.m_val == Decimal(b).m_val; }
    friend bool operator!=(Decimal a, double b) { return a.m_val != Decimal(b).m_val; }
    friend bool operator< (Decimal a, double b) { return a.m_val <  Decimal(b).m_val; }
    friend bool operator> (Decimal a, double b) { return a.m_val >  Decimal(b).m_val; }
    friend bool operator<=(Decimal a, double b) { return a.m_val <= Decimal(b).m_val; }
    friend bool operator>=(Decimal a, double b) { return a.m_val >= Decimal(b).m_val; }

    // Mixed comparison: double vs Decimal
    friend bool operator==(double a, Decimal b) { return Decimal(a).m_val == b.m_val; }
    friend bool operator!=(double a, Decimal b) { return Decimal(a).m_val != b.m_val; }
    friend bool operator< (double a, Decimal b) { return Decimal(a).m_val <  b.m_val; }
    friend bool operator> (double a, Decimal b) { return Decimal(a).m_val >  b.m_val; }
    friend bool operator<=(double a, Decimal b) { return Decimal(a).m_val <= b.m_val; }
    friend bool operator>=(double a, Decimal b) { return Decimal(a).m_val >= b.m_val; }

    // Mixed comparison: Decimal vs int (resolves ambiguity with built-in op(double,int))
    bool operator==(int o) const { return m_val == Decimal(static_cast<double>(o)).m_val; }
    bool operator!=(int o) const { return m_val != Decimal(static_cast<double>(o)).m_val; }
    bool operator< (int o) const { return m_val <  Decimal(static_cast<double>(o)).m_val; }
    bool operator> (int o) const { return m_val >  Decimal(static_cast<double>(o)).m_val; }
    bool operator<=(int o) const { return m_val <= Decimal(static_cast<double>(o)).m_val; }
    bool operator>=(int o) const { return m_val >= Decimal(static_cast<double>(o)).m_val; }

    friend bool operator==(int a, Decimal b) { return Decimal(static_cast<double>(a)).m_val == b.m_val; }
    friend bool operator!=(int a, Decimal b) { return Decimal(static_cast<double>(a)).m_val != b.m_val; }
    friend bool operator< (int a, Decimal b) { return Decimal(static_cast<double>(a)).m_val <  b.m_val; }
    friend bool operator> (int a, Decimal b) { return Decimal(static_cast<double>(a)).m_val >  b.m_val; }
    friend bool operator<=(int a, Decimal b) { return Decimal(static_cast<double>(a)).m_val <= b.m_val; }
    friend bool operator>=(int a, Decimal b) { return Decimal(static_cast<double>(a)).m_val >= b.m_val; }

    // Mixed arithmetic: Decimal ↔ double (returns double for frame math precision)
    friend double operator+(Decimal a, double b) { return a.toDouble() + b; }
    friend double operator+(double a, Decimal b) { return a + b.toDouble(); }
    friend double operator-(Decimal a, double b) { return a.toDouble() - b; }
    friend double operator-(double a, Decimal b) { return a - b.toDouble(); }
    friend double operator*(Decimal a, double b) { return a.toDouble() * b; }
    friend double operator*(double a, Decimal b) { return a * b.toDouble(); }
    friend double operator/(Decimal a, double b) { return a.toDouble() / b; }
    friend double operator/(double a, Decimal b) { return a / b.toDouble(); }

    // Display
    QString toString(int decimals = 3) const {
        return QString::number(toDouble(), 'f', decimals);
    }
};
