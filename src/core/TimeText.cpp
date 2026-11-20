#include "core/TimeText.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace tracekit {
namespace {

// Exactly `digits` decimal digits at *p; advances past them.
bool take_number(const char** p, int digits, int* out) noexcept {
    int value = 0;
    for (int i = 0; i < digits; ++i) {
        const char c = (*p)[i];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    *p += digits;
    *out = value;
    return true;
}

bool leap(int year) noexcept { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

int days_in_month(int year, int month) noexcept {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && leap(year) ? 29 : kDays[month - 1];
}

}  // namespace

std::int64_t days_from_civil(int year, int month, int day) noexcept {
    // Howard Hinnant's algorithm.
    const int y = year - (month <= 2 ? 1 : 0);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153U * static_cast<unsigned>(month + (month > 2 ? -3 : 9)) + 2U) / 5U +
                         static_cast<unsigned>(day) - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

bool parse_time_text(const char* text, ParsedTime* out) noexcept {
    if (text == nullptr || out == nullptr) return false;
    *out = ParsedTime{};
    while (*text == ' ') ++text;
    char buffer[40];
    std::size_t n = std::strlen(text);
    while (n > 0U && (text[n - 1U] == ' ' || text[n - 1U] == '\r' || text[n - 1U] == '\n')) --n;
    if (n == 0U || n >= sizeof(buffer)) return false;
    std::memcpy(buffer, text, n);
    buffer[n] = '\0';

    bool all_digits = true;
    for (std::size_t i = 0U; i < n; ++i) all_digits &= buffer[i] >= '0' && buffer[i] <= '9';
    if (all_digits) {
        if (n > 12U) return false;
        long long v = 0;
        for (std::size_t i = 0U; i < n; ++i) v = v * 10 + (buffer[i] - '0');
        out->is_epoch = true;
        out->has_offset = true;
        out->epoch = v;
        return true;
    }

    const char* p = buffer;
    ParsedTime t;
    if (!take_number(&p, 4, &t.year) || *p++ != '-' || !take_number(&p, 2, &t.month) || *p++ != '-' ||
        !take_number(&p, 2, &t.day)) {
        return false;
    }
    if (*p != 'T' && *p != 't' && *p != ' ') return false;
    ++p;
    if (!take_number(&p, 2, &t.hour) || *p++ != ':' || !take_number(&p, 2, &t.minute)) return false;
    if (*p == ':') {
        ++p;
        if (!take_number(&p, 2, &t.second)) return false;
    }
    if (t.year < 1970 || t.month < 1 || t.month > 12 || t.day < 1 || t.day > days_in_month(t.year, t.month) ||
        t.hour > 23 || t.minute > 59 || t.second > 59) {
        return false;
    }
    int offset_s = 0;
    if (*p == 'Z' || *p == 'z') {
        t.has_offset = true;
        ++p;
    } else if (*p == '+' || *p == '-') {
        const int sign = *p == '-' ? -1 : 1;
        ++p;
        int oh = 0;
        int om = 0;
        if (!take_number(&p, 2, &oh)) return false;
        if (*p == ':') ++p;
        if (*p != '\0' && !take_number(&p, 2, &om)) return false;
        if (oh > 14 || om > 59) return false;
        offset_s = sign * (oh * 3600 + om * 60);
        t.has_offset = true;
    }
    if (*p != '\0') return false;
    if (t.has_offset) {
        t.epoch = days_from_civil(t.year, t.month, t.day) * 86400 + t.hour * 3600 + t.minute * 60 + t.second -
                  offset_s;
    }
    *out = t;
    return true;
}

void format_utc(std::int64_t epoch, char* out, std::size_t size) noexcept {
    std::int64_t days = epoch / 86400;
    std::int64_t secs = epoch % 86400;
    if (secs < 0) {
        secs += 86400;
        --days;
    }
    // civil_from_days (Hinnant).
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
    const unsigned doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
    const unsigned mp = (5U * doy + 2U) / 153U;
    const unsigned d = doy - (153U * mp + 2U) / 5U + 1U;
    const unsigned m = mp < 10U ? mp + 3U : mp - 9U;
    const long long y = static_cast<long long>(yoe) + era * 400 + (m <= 2U ? 1 : 0);
    std::snprintf(out, size, "%04lld-%02u-%02uT%02u:%02u:%02uZ", y, m, d, static_cast<unsigned>(secs / 3600),
                  static_cast<unsigned>(secs / 60 % 60), static_cast<unsigned>(secs % 60));
}

}  // namespace tracekit
