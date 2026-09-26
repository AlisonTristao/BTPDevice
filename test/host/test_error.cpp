#include <btpdev/error.h>

#include <cstring>
#include <string>

#include "check.h"

namespace {

void fail_records_code_module_and_message() {
    btpdev::ErrorInfo error;
    CHECK(error.ok());

    const esp_err_t returned = btpdev::fail(error, BTPDEV_ERR_NO_KEY, "btpdev.key",
                                            "no key for link %s", "TCP");
    CHECK(returned == BTPDEV_ERR_NO_KEY);
    CHECK(!error.ok());
    CHECK(error.code == BTPDEV_ERR_NO_KEY);
    CHECK_STR_EQ(error.module, "btpdev.key");
    CHECK_STR_EQ(error.message, "no key for link TCP");
}

void a_long_message_is_truncated_not_overflowed() {
    btpdev::ErrorInfo error;
    const std::string long_text(500, 'x');
    btpdev::fail(error, ESP_FAIL, "btpdev", "%s", long_text.c_str());
    CHECK(std::strlen(error.message) == btpdev::ErrorInfo::kMessageSize - 1U);
}

void null_module_and_format_are_tolerated() {
    btpdev::ErrorInfo error;
    btpdev::fail(error, ESP_FAIL, nullptr, nullptr);
    CHECK_STR_EQ(error.module, "");
    CHECK_STR_EQ(error.message, "");
}

void clear_resets_to_ok() {
    btpdev::ErrorInfo error;
    btpdev::fail(error, ESP_FAIL, "btpdev", "boom");
    btpdev::clear(error);
    CHECK(error.ok());
    CHECK_STR_EQ(error.module, "");
    CHECK_STR_EQ(error.message, "");
}

void every_btpdev_code_has_its_own_name() {
    const esp_err_t codes[] = {
        BTPDEV_ERR_CONFIG,        BTPDEV_ERR_LINK_UNSUPPORTED, BTPDEV_ERR_LINK_START,
        BTPDEV_ERR_MEMORY_BUDGET, BTPDEV_ERR_STORAGE,          BTPDEV_ERR_NO_KEY,
        BTPDEV_ERR_BAD_KEY,       BTPDEV_ERR_BAD_TOPIC,        BTPDEV_ERR_BAD_COMMAND,
        BTPDEV_ERR_ALREADY_STARTED, BTPDEV_ERR_NOT_IMPLEMENTED,
    };
    for (esp_err_t code : codes) {
        const char* name = btpdev::err_name(code);
        CHECK(std::strncmp(name, "BTPDEV_ERR_", 11) == 0);
    }
    CHECK_STR_EQ(btpdev::err_name(BTPDEV_ERR_NO_KEY), "BTPDEV_ERR_NO_KEY");
}

void other_codes_are_named_and_never_null() {
    CHECK_STR_EQ(btpdev::err_name(ESP_OK), "ESP_OK");
    CHECK_STR_EQ(btpdev::err_name(ESP_ERR_NO_MEM), "ESP_ERR_NO_MEM");
    CHECK(btpdev::err_name(0x12345) != nullptr);
}

void codes_stay_in_their_own_range() {
    // ESP-IDF 6.0 uses 0x7000 (HTTP), 0x9000 (HTTPS OTA) and 0xB000 (HTTPD);
    // the BTPDevice range must not reach into the next one.
    CHECK(BTPDEV_ERR_BASE == 0xA000);
    CHECK(BTPDEV_ERR_NOT_IMPLEMENTED < 0xB000);
}

}  // namespace

int main() {
    fail_records_code_module_and_message();
    a_long_message_is_truncated_not_overflowed();
    null_module_and_format_are_tolerated();
    clear_resets_to_ok();
    every_btpdev_code_has_its_own_name();
    other_codes_are_named_and_never_null();
    codes_stay_in_their_own_range();
    return test_result("test_error");
}
