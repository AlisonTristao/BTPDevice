#include <tracekit/error.h>

#include <cstring>
#include <string>

#include "check.h"

namespace {

void fail_records_code_module_and_message() {
    tracekit::ErrorInfo error;
    CHECK(error.ok());

    const esp_err_t returned = tracekit::fail(error, TRACEKIT_ERR_NO_KEY, "tracekit.key",
                                            "no key for link %s", "TCP");
    CHECK(returned == TRACEKIT_ERR_NO_KEY);
    CHECK(!error.ok());
    CHECK(error.code == TRACEKIT_ERR_NO_KEY);
    CHECK_STR_EQ(error.module, "tracekit.key");
    CHECK_STR_EQ(error.message, "no key for link TCP");
}

void a_long_message_is_truncated_not_overflowed() {
    tracekit::ErrorInfo error;
    const std::string long_text(500, 'x');
    tracekit::fail(error, ESP_FAIL, "tracekit", "%s", long_text.c_str());
    CHECK(std::strlen(error.message) == tracekit::ErrorInfo::kMessageSize - 1U);
}

void null_module_and_format_are_tolerated() {
    tracekit::ErrorInfo error;
    tracekit::fail(error, ESP_FAIL, nullptr, nullptr);
    CHECK_STR_EQ(error.module, "");
    CHECK_STR_EQ(error.message, "");
}

void clear_resets_to_ok() {
    tracekit::ErrorInfo error;
    tracekit::fail(error, ESP_FAIL, "tracekit", "boom");
    tracekit::clear(error);
    CHECK(error.ok());
    CHECK_STR_EQ(error.module, "");
    CHECK_STR_EQ(error.message, "");
}

void every_tracekit_code_has_its_own_name() {
    const esp_err_t codes[] = {
        TRACEKIT_ERR_CONFIG,        TRACEKIT_ERR_LINK_UNSUPPORTED, TRACEKIT_ERR_LINK_START,
        TRACEKIT_ERR_MEMORY_BUDGET, TRACEKIT_ERR_STORAGE,          TRACEKIT_ERR_NO_KEY,
        TRACEKIT_ERR_BAD_KEY,       TRACEKIT_ERR_BAD_TOPIC,        TRACEKIT_ERR_BAD_COMMAND,
        TRACEKIT_ERR_ALREADY_STARTED, TRACEKIT_ERR_NOT_IMPLEMENTED,
    };
    for (esp_err_t code : codes) {
        const char* name = tracekit::err_name(code);
        CHECK(std::strncmp(name, "TRACEKIT_ERR_", 11) == 0);
    }
    CHECK_STR_EQ(tracekit::err_name(TRACEKIT_ERR_NO_KEY), "TRACEKIT_ERR_NO_KEY");
}

void other_codes_are_named_and_never_null() {
    CHECK_STR_EQ(tracekit::err_name(ESP_OK), "ESP_OK");
    CHECK_STR_EQ(tracekit::err_name(ESP_ERR_NO_MEM), "ESP_ERR_NO_MEM");
    CHECK(tracekit::err_name(0x12345) != nullptr);
}

void codes_stay_in_their_own_range() {
    // ESP-IDF 6.0 uses 0x7000 (HTTP), 0x9000 (HTTPS OTA) and 0xB000 (HTTPD);
    // the TraceKit range must not reach into the next one.
    CHECK(TRACEKIT_ERR_BASE == 0xA000);
    CHECK(TRACEKIT_ERR_NOT_IMPLEMENTED < 0xB000);
}

}  // namespace

int main() {
    fail_records_code_module_and_message();
    a_long_message_is_truncated_not_overflowed();
    null_module_and_format_are_tolerated();
    clear_resets_to_ok();
    every_tracekit_code_has_its_own_name();
    other_codes_are_named_and_never_null();
    codes_stay_in_their_own_range();
    return test_result("test_error");
}
