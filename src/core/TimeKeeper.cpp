#include "core/TimeKeeper.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>

#include <esp_attr.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <nvs.h>

#include "core/LogTags.h"
#include "core/TimeText.h"

namespace tracekit {

const char* time_source_name(TimeSource source) noexcept {
    switch (source) {
        case TimeSource::Sntp: return "SNTP";
        case TimeSource::Manual: return "btp -time_set";
        case TimeSource::Application: return "the application";
        case TimeSource::None: break;
    }
    return "never set";
}

namespace {

constexpr const char* kTag = tag::kCore;
constexpr const char* kNvsNamespace = "btpdev";  // the old name, kept: devices in the field have data there
constexpr const char* kNvsZone = "tz";
constexpr std::uint32_t kMagic = 0xB7D07153U;

// Next to the RTC clock: kept by a reboot and deep sleep, garbage after a
// power-up (and then the magic does not match).
RTC_NOINIT_ATTR std::uint32_t s_magic;
RTC_NOINIT_ATTR std::uint8_t s_source;
RTC_NOINIT_ATTR std::int64_t s_set_at;  // epoch of the last set

void remember(TimeSource source, std::int64_t epoch) noexcept {
    s_source = static_cast<std::uint8_t>(source);
    s_set_at = epoch;
    s_magic = kMagic;
}

void on_sntp_sync(struct timeval* tv) {
    const bool first = s_magic != kMagic || s_source == static_cast<std::uint8_t>(TimeSource::None);
    remember(TimeSource::Sntp, tv->tv_sec);
    char text[32];
    format_utc(tv->tv_sec, text, sizeof(text));
    if (first) ESP_LOGI(kTag, "time set by SNTP: %s", text);
}

// A POSIX TZ string as setenv() takes it: printable, no spaces, starts with
// a name ("BRT", "<-03>") and has an offset digit somewhere.
bool plausible_zone(const char* tz) noexcept {
    if (tz == nullptr || tz[0] == '\0' || std::strlen(tz) >= 48U) return false;
    bool digit = false;
    for (const char* p = tz; *p != '\0'; ++p) {
        if (*p <= ' ' || *p >= 0x7F) return false;
        digit |= *p >= '0' && *p <= '9';
    }
    return digit && (tz[0] == '<' || (tz[0] >= 'A' && tz[0] <= 'Z') || (tz[0] >= 'a' && tz[0] <= 'z'));
}

}  // namespace

void TimeKeeper::begin(const TimeConfig& config) noexcept {
    config_ = config;
    std::snprintf(tz_, sizeof(tz_), "%s", plausible_zone(config.tz) ? config.tz : "UTC0");
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) == ESP_OK) {
        char saved[48] = "";
        std::size_t size = sizeof(saved);
        if (nvs_get_str(handle, kNvsZone, saved, &size) == ESP_OK && plausible_zone(saved)) {
            std::snprintf(tz_, sizeof(tz_), "%s", saved);
        }
        nvs_close(handle);
    }
    setenv("TZ", tz_, 1);
    tzset();
    if (s_magic != kMagic) remember(TimeSource::None, 0);
}

void TimeKeeper::start_sntp() noexcept {
    if (!config_.sntp || sntp_started_ || config_.server == nullptr || config_.server[0] == '\0') return;
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(config_.server);
    sntp.sync_cb = &on_sntp_sync;
    sntp.wait_for_sync = false;
    if (esp_netif_sntp_init(&sntp) != ESP_OK) {
        ESP_LOGW(kTag, "SNTP did not start: set the time with btp -time_set");
        return;
    }
    const std::uint32_t hours = config_.sync_hours != 0U ? config_.sync_hours : 6U;
    esp_sntp_set_sync_interval(hours * 3600U * 1000U);
    sntp_started_ = true;
}

bool TimeKeeper::valid() const noexcept {
    return s_magic == kMagic && s_source != static_cast<std::uint8_t>(TimeSource::None) &&
           static_cast<std::int64_t>(std::time(nullptr)) >= kMinValidEpoch;
}

TimeSource TimeKeeper::source() const noexcept {
    return valid() ? static_cast<TimeSource>(s_source) : TimeSource::None;
}

std::time_t TimeKeeper::now() const noexcept { return std::time(nullptr); }

std::int64_t TimeKeeper::now_us() const noexcept {
    timeval tv{};
    gettimeofday(&tv, nullptr);
    return static_cast<std::int64_t>(tv.tv_sec) * 1000000LL + tv.tv_usec;
}

bool TimeKeeper::set(std::int64_t epoch, TimeSource source) noexcept {
    if (epoch < kMinValidEpoch) return false;
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(epoch);
    if (settimeofday(&tv, nullptr) != 0) return false;
    remember(source, epoch);
    return true;
}

bool TimeKeeper::set_text(const char* text, char* error, std::size_t size) noexcept {
    ParsedTime parsed;
    if (!parse_time_text(text, &parsed)) {
        std::snprintf(error, size,
                      "not a time: use 2026-11-19T14:00:00-03:00, 2026-11-19 14:00 (local) or seconds since 1970");
        return false;
    }
    std::int64_t epoch = parsed.epoch;
    if (!parsed.has_offset) {
        // Local time, in the device's zone.
        std::tm local{};
        local.tm_year = parsed.year - 1900;
        local.tm_mon = parsed.month - 1;
        local.tm_mday = parsed.day;
        local.tm_hour = parsed.hour;
        local.tm_min = parsed.minute;
        local.tm_sec = parsed.second;
        local.tm_isdst = -1;
        epoch = static_cast<std::int64_t>(std::mktime(&local));
    }
    if (!set(epoch, TimeSource::Manual)) {
        std::snprintf(error, size, "that is before 2025: not accepted");
        return false;
    }
    return true;
}

bool TimeKeeper::set_zone(const char* tz) noexcept {
    if (!plausible_zone(tz)) return false;
    std::snprintf(tz_, sizeof(tz_), "%s", tz);
    setenv("TZ", tz_, 1);
    tzset();
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(handle, kNvsZone, tz_);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

void TimeKeeper::describe(char* out, std::size_t size) const noexcept {
    const std::time_t t = now();
    char utc[32];
    format_utc(static_cast<std::int64_t>(t), utc, sizeof(utc));
    std::tm local{};
    localtime_r(&t, &local);
    char local_text[40];
    std::strftime(local_text, sizeof(local_text), "%Y-%m-%d %H:%M:%S %Z", &local);
    if (!valid()) {
        std::snprintf(out, size,
                      "time      not set since power-up (%s)\n"
                      "zone      %s\n"
                      "sntp      %s\n"
                      "set it    btp -time_set 2026-11-19 14:00  (local) or ...T14:00:00-03:00\n",
                      utc, tz_, sntp_started_ ? "waiting for an answer" : (config_.sntp ? "needs the TCP link" : "off"));
        return;
    }
    const long long age = static_cast<long long>(t) - static_cast<long long>(s_set_at);
    std::snprintf(out, size,
                  "local     %s\n"
                  "utc       %s\n"
                  "zone      %s\n"
                  "source    %s, %lld min ago\n"
                  "sntp      %s\n",
                  local_text, utc, tz_, time_source_name(source()), age / 60,
                  sntp_started_ ? config_.server : (config_.sntp ? "needs the TCP link" : "off"));
}

}  // namespace tracekit
