#include "protocol/SettingsRegistry.h"

#include <cctype>
#include <cerrno>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/LogTags.h"

namespace tracekit {
namespace {

constexpr const char* kTag = tag::kSettings;
constexpr std::size_t kBlobMax = 1U + Settings::kMaxText;

double type_min(SettingType t) noexcept {
    switch (t) {
        case SettingType::I32: return -2147483648.0;
        case SettingType::F32: return -FLT_MAX;
        default: return 0.0;
    }
}

double type_max(SettingType t) noexcept {
    switch (t) {
        case SettingType::Bool: return 1.0;
        case SettingType::U8: return 255.0;
        case SettingType::U16: return 65535.0;
        case SettingType::U32: return 4294967295.0;
        case SettingType::I32: return 2147483647.0;
        case SettingType::F32: return FLT_MAX;
        default: return 0.0;
    }
}

std::size_t value_size(SettingType t) noexcept {
    switch (t) {
        case SettingType::Bool:
        case SettingType::U8: return 1U;
        case SettingType::U16: return 2U;
        default: return 4U;
    }
}

bool is_number(SettingType t) noexcept { return t != SettingType::Text; }

const char* type_name(SettingType t) noexcept {
    switch (t) {
        case SettingType::Bool: return "bool";
        case SettingType::U8: return "u8";
        case SettingType::U16: return "u16";
        case SettingType::U32: return "u32";
        case SettingType::I32: return "i32";
        case SettingType::F32: return "f32";
        case SettingType::Text: return "text";
    }
    return "?";
}

bool equals_ignore_case(const char* a, const char* b) noexcept {
    for (; *a != '\0' && *b != '\0'; ++a, ++b) {
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    }
    return *a == *b;
}

// Leading and trailing blanks off, into `out`.
void trim(const char* in, char* out, std::size_t size) noexcept {
    while (*in == ' ' || *in == '\t') ++in;
    std::size_t n = std::strlen(in);
    while (n > 0U && (in[n - 1U] == ' ' || in[n - 1U] == '\t' || in[n - 1U] == '\r' || in[n - 1U] == '\n')) --n;
    if (n >= size) n = size - 1U;
    std::memcpy(out, in, n);
    out[n] = '\0';
}

}  // namespace

// ---------------------------------------------------------------------------
// Variables
// ---------------------------------------------------------------------------

namespace {

double read_number(const SettingDecl& d, SettingType type, const void* variable) noexcept {
    (void)d;
    switch (type) {
        case SettingType::Bool: return *static_cast<const bool*>(variable) ? 1.0 : 0.0;
        case SettingType::U8: return *static_cast<const std::uint8_t*>(variable);
        case SettingType::U16: return *static_cast<const std::uint16_t*>(variable);
        case SettingType::U32: return *static_cast<const std::uint32_t*>(variable);
        case SettingType::I32: return *static_cast<const std::int32_t*>(variable);
        case SettingType::F32: return *static_cast<const float*>(variable);
        default: return 0.0;
    }
}

// One store of the variable's own width: another task reads old or new.
void write_number(SettingType type, void* variable, double v) noexcept {
    switch (type) {
        case SettingType::Bool: *static_cast<bool*>(variable) = v != 0.0; break;
        case SettingType::U8: *static_cast<std::uint8_t*>(variable) = static_cast<std::uint8_t>(v); break;
        case SettingType::U16: *static_cast<std::uint16_t*>(variable) = static_cast<std::uint16_t>(v); break;
        case SettingType::U32: *static_cast<std::uint32_t*>(variable) = static_cast<std::uint32_t>(v); break;
        case SettingType::I32: *static_cast<std::int32_t*>(variable) = static_cast<std::int32_t>(v); break;
        case SettingType::F32: *static_cast<float*>(variable) = static_cast<float>(v); break;
        default: break;
    }
}

}  // namespace

// The accessors below need the private members: they are friends through
// SettingsRegistry, so they live as members.

const SettingDecl* SettingsRegistry::find(const char* name) const noexcept {
    if (name == nullptr) return nullptr;
    for (const SettingDecl& d : list_) {
        if (std::strcmp(d.name_, name) == 0) return &d;
    }
    return nullptr;
}

SettingDecl* SettingsRegistry::find(const char* name) noexcept {
    return const_cast<SettingDecl*>(static_cast<const SettingsRegistry*>(this)->find(name));
}

void SettingsRegistry::apply_default(SettingDecl& d) noexcept {
    if (d.type_ == SettingType::Text) {
        char* buffer = static_cast<char*>(d.variable_);
        std::size_t n = std::strlen(d.default_text_);
        if (n >= d.text_size_) n = d.text_size_ - 1U;
        std::memcpy(buffer, d.default_text_, n);
        buffer[n] = '\0';
    } else {
        write_number(d.type_, d.variable_, d.default_number_);
    }
}

bool SettingsRegistry::encode(const SettingDecl& d, std::uint8_t* out, std::size_t* size) const noexcept {
    out[0] = static_cast<std::uint8_t>(d.type_);
    if (d.type_ == SettingType::Text) {
        const std::size_t n = std::strlen(static_cast<const char*>(d.variable_));
        std::memcpy(out + 1, d.variable_, n);
        *size = 1U + n;
        return true;
    }
    // Little-endian, the ESP32's own order: copy the variable as it is.
    const std::size_t n = value_size(d.type_);
    if (d.type_ == SettingType::Bool) {
        out[1] = *static_cast<const bool*>(d.variable_) ? 1U : 0U;
    } else {
        std::memcpy(out + 1, d.variable_, n);
    }
    *size = 1U + n;
    return true;
}

bool SettingsRegistry::decode(const SettingDecl& d, const std::uint8_t* data, std::size_t size) noexcept {
    if (size < 1U || data[0] != static_cast<std::uint8_t>(d.type_)) return false;
    if (d.type_ == SettingType::Text) {
        const std::size_t n = size - 1U;
        if (n >= d.text_size_ || n > Settings::kMaxText) return false;
        char* buffer = static_cast<char*>(d.variable_);
        std::memcpy(buffer, data + 1, n);
        buffer[n] = '\0';
        return true;
    }
    const std::size_t n = value_size(d.type_);
    if (size != 1U + n) return false;
    std::uint8_t raw[4] = {};
    std::memcpy(raw, data + 1, n);
    double v = 0.0;
    switch (d.type_) {
        case SettingType::Bool:
            if (raw[0] > 1U) return false;
            v = raw[0];
            break;
        case SettingType::U8: v = raw[0]; break;
        case SettingType::U16: {
            std::uint16_t x;
            std::memcpy(&x, raw, 2);
            v = x;
            break;
        }
        case SettingType::U32: {
            std::uint32_t x;
            std::memcpy(&x, raw, 4);
            v = x;
            break;
        }
        case SettingType::I32: {
            std::int32_t x;
            std::memcpy(&x, raw, 4);
            v = x;
            break;
        }
        case SettingType::F32: {
            float x;
            std::memcpy(&x, raw, 4);
            if (!std::isfinite(x)) return false;
            v = x;
            break;
        }
        default: return false;
    }
    // Saved under an older, wider range: not acceptable any more.
    const double lo = std::isnan(d.min_) ? type_min(d.type_) : d.min_;
    const double hi = std::isnan(d.max_) ? type_max(d.type_) : d.max_;
    if (v < lo || v > hi) return false;
    write_number(d.type_, d.variable_, v);
    return true;
}

bool SettingsRegistry::persist(const SettingDecl& d) noexcept {
    if (store_ == nullptr) return true;
    std::uint8_t blob[kBlobMax];
    std::size_t size = 0U;
    return encode(d, blob, &size) && store_->save(d.name_, blob, size);
}

// ---------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------

esp_err_t SettingsRegistry::install(Settings& settings, SettingsStore* store, ErrorInfo& error,
                                    const std::function<void(const char*)>& warn) {
    list_ = std::move(settings.list_);
    settings.list_.clear();
    store_ = store;
    if (list_.size() > Settings::kMaxSettings) {
        return fail(error, TRACEKIT_ERR_CONFIG, kTag, "%u settings declared, at most %u",
                    static_cast<unsigned>(list_.size()), static_cast<unsigned>(Settings::kMaxSettings));
    }
    for (std::size_t i = 0U; i < list_.size(); ++i) {
        const SettingDecl& d = list_[i];
        const std::size_t name_length = d.name_ != nullptr ? std::strlen(d.name_) : 0U;
        if (name_length == 0U || name_length > Settings::kMaxName) {
            return fail(error, TRACEKIT_ERR_CONFIG, kTag, "setting #%u: the name must have 1 to %u characters",
                        static_cast<unsigned>(i + 1U), static_cast<unsigned>(Settings::kMaxName));
        }
        for (std::size_t c = 0U; c < name_length; ++c) {
            const unsigned char ch = static_cast<unsigned char>(d.name_[c]);
            if (ch <= 0x20U || ch >= 0x7FU || ch == ',') {
                return fail(error, TRACEKIT_ERR_CONFIG, kTag,
                            "setting \"%s\": the name takes printable ASCII without spaces or commas", d.name_);
            }
        }
        for (std::size_t j = 0U; j < i; ++j) {
            if (std::strcmp(list_[j].name_, d.name_) == 0) {
                return fail(error, TRACEKIT_ERR_CONFIG, kTag, "setting \"%s\" is declared twice", d.name_);
            }
        }
        if (equals_ignore_case(d.name_, "all")) {
            return fail(error, TRACEKIT_ERR_CONFIG, kTag, "\"all\" is reserved (cfg -reset all)");
        }
        if (d.variable_ == nullptr) {
            return fail(error, TRACEKIT_ERR_CONFIG, kTag, "setting \"%s\" has no variable", d.name_);
        }
        if (d.type_ == SettingType::Text) {
            if (d.text_size_ < 2U || d.text_size_ - 1U > Settings::kMaxText) {
                return fail(error, TRACEKIT_ERR_CONFIG, kTag, "setting \"%s\": the text buffer must hold 1 to %u characters",
                            d.name_, static_cast<unsigned>(Settings::kMaxText));
            }
            if (std::strlen(d.default_text_) >= d.text_size_) {
                return fail(error, TRACEKIT_ERR_CONFIG, kTag, "setting \"%s\": the default does not fit the buffer",
                            d.name_);
            }
            continue;
        }
        const double lo = std::isnan(d.min_) ? type_min(d.type_) : d.min_;
        const double hi = std::isnan(d.max_) ? type_max(d.type_) : d.max_;
        if (lo > hi || lo < type_min(d.type_) || hi > type_max(d.type_)) {
            return fail(error, TRACEKIT_ERR_CONFIG, kTag, "setting \"%s\": the range does not fit a %s", d.name_,
                        type_name(d.type_));
        }
        if (d.default_number_ < lo || d.default_number_ > hi) {
            return fail(error, TRACEKIT_ERR_CONFIG, kTag, "setting \"%s\": the default is outside its range", d.name_);
        }
    }

    for (SettingDecl& d : list_) {
        std::uint8_t blob[kBlobMax];
        std::size_t size = sizeof(blob);
        if (store_ != nullptr && store_->load(d.name_, blob, &size)) {
            if (decode(d, blob, size)) continue;
            if (warn) {
                char line[96];
                std::snprintf(line, sizeof(line), "setting \"%s\": the saved value no longer fits (%s), using the default",
                              d.name_, type_name(d.type_));
                warn(line);
            }
            store_->erase(d.name_);
        }
        apply_default(d);
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// get / set / reset / save
// ---------------------------------------------------------------------------

void SettingsRegistry::format(const SettingDecl& d, char* out, std::size_t size, bool fallback) const noexcept {
    if (size == 0U) return;
    if (d.type_ == SettingType::Text) {
        std::snprintf(out, size, "%s", fallback ? d.default_text_ : static_cast<const char*>(d.variable_));
        return;
    }
    const double v = fallback ? d.default_number_ : read_number(d, d.type_, d.variable_);
    switch (d.type_) {
        case SettingType::Bool: std::snprintf(out, size, "%s", v != 0.0 ? "true" : "false"); break;
        case SettingType::I32: std::snprintf(out, size, "%ld", static_cast<long>(v)); break;
        case SettingType::F32: std::snprintf(out, size, "%g", v); break;
        default: std::snprintf(out, size, "%lu", static_cast<unsigned long>(v)); break;
    }
}

bool SettingsRegistry::get(const char* name, char* out, std::size_t size) const noexcept {
    const SettingDecl* d = find(name);
    if (d == nullptr) return false;
    format(*d, out, size, false);
    return true;
}

SettingsRegistry::Result SettingsRegistry::set(const char* name, const char* text) {
    char key[Settings::kMaxName + 2U];
    trim(name != nullptr ? name : "", key, sizeof(key));
    SettingDecl* d = find(key);
    if (d == nullptr) return Result::UnknownName;
    char value[Settings::kMaxText + 2U];
    trim(text != nullptr ? text : "", value, sizeof(value));

    if (d->type_ == SettingType::Text) {
        const std::size_t n = std::strlen(text != nullptr ? text : "");
        if (n > Settings::kMaxText || std::strlen(value) >= d->text_size_) return Result::TooLong;
        char* buffer = static_cast<char*>(d->variable_);
        std::memcpy(buffer, value, std::strlen(value) + 1U);
    } else {
        double v = 0.0;
        if (d->type_ == SettingType::Bool) {
            if (equals_ignore_case(value, "true") || equals_ignore_case(value, "on") ||
                equals_ignore_case(value, "yes") || std::strcmp(value, "1") == 0) {
                v = 1.0;
            } else if (equals_ignore_case(value, "false") || equals_ignore_case(value, "off") ||
                       equals_ignore_case(value, "no") || std::strcmp(value, "0") == 0) {
                v = 0.0;
            } else {
                return Result::BadValue;
            }
        } else {
            if (value[0] == '\0') return Result::BadValue;
            char* end = nullptr;
            errno = 0;
            if (d->type_ == SettingType::F32) {
                v = std::strtod(value, &end);
                if (!std::isfinite(v)) return Result::BadValue;
            } else if (d->type_ == SettingType::I32) {
                v = static_cast<double>(std::strtoll(value, &end, 10));
            } else {
                if (value[0] == '-') return Result::OutOfRange;
                v = static_cast<double>(std::strtoull(value, &end, 10));
            }
            if (end == value || *end != '\0') return Result::BadValue;
            if (errno == ERANGE) return Result::OutOfRange;
        }
        const double lo = std::isnan(d->min_) ? type_min(d->type_) : d->min_;
        const double hi = std::isnan(d->max_) ? type_max(d->type_) : d->max_;
        if (v < lo || v > hi) return Result::OutOfRange;
        write_number(d->type_, d->variable_, v);
    }
    const bool saved = persist(*d);
    if (d->on_change_) d->on_change_();
    if (!saved) return Result::NotSaved;
    return d->reboot_ ? Result::OkReboot : Result::Ok;
}

SettingsRegistry::Result SettingsRegistry::reset(const char* name) {
    char key[Settings::kMaxName + 2U];
    trim(name != nullptr ? name : "", key, sizeof(key));
    const bool all = equals_ignore_case(key, "all");
    bool any = false;
    bool reboot = false;
    for (SettingDecl& d : list_) {
        if (!all && std::strcmp(d.name_, key) != 0) continue;
        any = true;
        reboot |= d.reboot_;
        apply_default(d);
        if (store_ != nullptr) store_->erase(d.name_);
        if (d.on_change_) d.on_change_();
    }
    if (!any) return Result::UnknownName;
    return reboot ? Result::OkReboot : Result::Ok;
}

SettingsRegistry::Result SettingsRegistry::save(const char* name) {
    const SettingDecl* d = find(name);
    if (d == nullptr) return Result::UnknownName;
    return persist(*d) ? Result::Ok : Result::NotSaved;
}

// ---------------------------------------------------------------------------
// Text for the terminal
// ---------------------------------------------------------------------------

void SettingsRegistry::describe(std::size_t index, char* out, std::size_t size) const noexcept {
    if (index >= list_.size() || size == 0U) {
        if (size != 0U) out[0] = '\0';
        return;
    }
    const SettingDecl& d = list_[index];
    char value[Settings::kMaxText + 1U];
    char fallback[Settings::kMaxText + 1U];
    format(d, value, sizeof(value), false);
    format(d, fallback, sizeof(fallback), true);
    char range[48] = "";
    if (is_number(d.type_) && d.type_ != SettingType::Bool && (!std::isnan(d.min_) || !std::isnan(d.max_))) {
        std::snprintf(range, sizeof(range), ", %g..%g", std::isnan(d.min_) ? type_min(d.type_) : d.min_,
                      std::isnan(d.max_) ? type_max(d.type_) : d.max_);
    }
    const bool quoted = d.type_ == SettingType::Text;
    std::snprintf(out, size, "%-15s %s%s%s%s%s  (%s, default %s%s%s%s)%s%s%s", d.name_, quoted ? "\"" : "", value,
                  quoted ? "\"" : "", d.unit_[0] != '\0' ? " " : "", d.unit_, type_name(d.type_), quoted ? "\"" : "",
                  fallback, quoted ? "\"" : "", range, d.reboot_ ? " reboot" : "", d.help_[0] != '\0' ? "  " : "",
                  d.help_);
}

void SettingsRegistry::explain(Result result, const char* name, char* out, std::size_t size) const noexcept {
    const SettingDecl* d = find(name);
    switch (result) {
        case Result::Ok: std::snprintf(out, size, "%s saved", name); break;
        case Result::OkReboot: std::snprintf(out, size, "%s saved: takes effect after btp -reboot", name); break;
        case Result::UnknownName: std::snprintf(out, size, "no setting \"%s\" (cfg -list)", name); break;
        case Result::BadValue:
            std::snprintf(out, size, "not a %s value", d != nullptr ? type_name(d->type_) : "valid");
            break;
        case Result::OutOfRange:
            if (d != nullptr) {
                std::snprintf(out, size, "out of range: %s takes %g..%g", name,
                              std::isnan(d->min_) ? type_min(d->type_) : d->min_,
                              std::isnan(d->max_) ? type_max(d->type_) : d->max_);
            } else {
                std::snprintf(out, size, "out of range");
            }
            break;
        case Result::TooLong:
            std::snprintf(out, size, "too long: %s takes up to %u characters", name,
                          d != nullptr ? static_cast<unsigned>(d->text_size_ - 1U) : 0U);
            break;
        case Result::NotSaved: std::snprintf(out, size, "%s changed but could not be saved (NVS)", name); break;
    }
}

}  // namespace tracekit
