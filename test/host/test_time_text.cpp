// Time as text (src/core/TimeText): what `btp -time_set` accepts and how
// `btp -time` prints UTC. Epochs checked against Python's datetime.

#include <cstring>

#include "check.h"
#include "core/TimeText.h"

namespace {

using btpdev::ParsedTime;
using btpdev::parse_time_text;

void offsets_and_utc() {
    ParsedTime t;
    // datetime(2026,11,19,14,0,0,tzinfo=timezone(timedelta(hours=-3))).timestamp() == 1795107600
    CHECK(parse_time_text("2026-11-19T14:00:00-03:00", &t));
    CHECK(t.has_offset && !t.is_epoch && t.epoch == 1795107600);
    CHECK(parse_time_text("2026-11-19T17:00:00Z", &t) && t.epoch == 1795107600);
    CHECK(parse_time_text("2026-11-19 17:00Z", &t) && t.epoch == 1795107600);
    CHECK(parse_time_text("2026-11-19T19:30:00+0230", &t) && t.epoch == 1795107600);
    CHECK(parse_time_text("2026-11-19T14:00-03", &t) && t.epoch == 1795107600);
    CHECK(parse_time_text("  1795107600 ", &t) && t.is_epoch && t.epoch == 1795107600);
    // leap day, end of year
    CHECK(parse_time_text("2028-02-29T00:00:00Z", &t) && t.epoch == 1835395200);
    CHECK(parse_time_text("2025-12-31T23:59:59Z", &t) && t.epoch == 1767225599);
}

void local_time_is_left_to_the_device() {
    ParsedTime t;
    CHECK(parse_time_text("2026-11-19T14:05:09", &t));
    CHECK(!t.has_offset && t.year == 2026 && t.month == 11 && t.day == 19 && t.hour == 14 && t.minute == 5 &&
          t.second == 9);
}

void rejects() {
    ParsedTime t;
    CHECK(!parse_time_text("", &t));
    CHECK(!parse_time_text("2026-02-30T10:00:00Z", &t));   // no such day
    CHECK(!parse_time_text("2027-02-29T10:00:00Z", &t));   // not a leap year
    CHECK(!parse_time_text("2026-11-19T24:00:00Z", &t));
    CHECK(!parse_time_text("2026-11-19T10:60Z", &t));
    CHECK(!parse_time_text("2026-11-19", &t));             // no time
    CHECK(!parse_time_text("2026-11-19T10:00:00+15:00", &t));
    CHECK(!parse_time_text("2026-11-19T10:00:00 junk", &t));
    CHECK(!parse_time_text("19/11/2026 10:00", &t));
    CHECK(!parse_time_text("12345678901234", &t));          // a number too long to be seconds
    CHECK(!parse_time_text(nullptr, &t));
}

void utc_text() {
    char text[32];
    btpdev::format_utc(1795107600, text, sizeof(text));
    CHECK_STR_EQ(text, "2026-11-19T17:00:00Z");
    btpdev::format_utc(0, text, sizeof(text));
    CHECK_STR_EQ(text, "1970-01-01T00:00:00Z");
    btpdev::format_utc(1835395200 - 1, text, sizeof(text));
    CHECK_STR_EQ(text, "2028-02-28T23:59:59Z");
    CHECK(btpdev::days_from_civil(1970, 1, 1) == 0);
    CHECK(btpdev::days_from_civil(2000, 3, 1) == 11017);
}

}  // namespace

int main() {
    offsets_and_utc();
    local_time_is_left_to_the_device();
    rejects();
    utc_text();
    return test_result("test_time_text");
}
