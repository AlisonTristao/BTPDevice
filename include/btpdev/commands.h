#ifndef BTPDEV_COMMANDS_H
#define BTPDEV_COMMANDS_H

// Commands: what can be called from TraceView's terminal (and by a
// COMMAND_REQUEST). Declared once, in the application's btp_commands.cpp:
//
//   uint8_t relay_set(uint8_t on) {
//       gpio_set_level(RELAY_PIN, on);
//       btpdev::printf("relay %s\n", on ? "on" : "off");
//       return RESULT_OK;                      // 0 = success
//   }
//
//   void app_commands(btpdev::Commands& c) {
//       c.module("relay", "Pump relay")
//        .add("set", relay_set, "relay -set <0|1>");
//   }
//
// Typed in the terminal as `relay -set 1`. A function may take up to 8
// arguments of the usual types (integers, float, double, bool, const char*,
// std::string_view) and returns uint8_t: RESULT_OK (0) or an error code.
// Commands run one at a time on the library's shell task, never on the
// communication task, so a slow command does not stall telemetry.
//
// The module "btp" is the library's own (btp -status, -errors, -topics,
// -reboot); "help" is TinyShell's. Mistakes (a duplicate name, a full table)
// are reported by begin() as BTPDEV_ERR_BAD_COMMAND.

// Keep TinyShell's 1.x `using namespace std` out of the application.
#ifndef TINYSHELL_NO_USING_NAMESPACE_STD
#define TINYSHELL_NO_USING_NAMESPACE_STD
#endif
#include <TinyShell.h>

#include <cstdint>
#include <cstdio>
#include <utility>

namespace btpdev {

class Commands {
public:
    class Module {
    public:
        /// Adds `function` as `<module> -<name>`. `help` is shown by `help`.
        template <typename F>
        Module& add(const char* name, F&& function, const char* help = "") {
            owner_->check(owner_->shell_->add(std::forward<F>(function), name, help, module_), module_, name);
            return *this;
        }

    private:
        friend class Commands;
        Module(Commands* owner, const char* module) noexcept : owner_(owner), module_(module) {}
        Commands* owner_;
        const char* module_;
    };

    explicit Commands(TinyShell& shell) noexcept : shell_(&shell) {}

    /// Creates a module (or reopens one this function already created).
    Module module(const char* name, const char* description = "") {
        const std::uint8_t code = shell_->create_module(name, description);
        // MODULE_NOT_FOUND from create_module() means "already exists": fine
        // for a module declared here earlier, a mistake for "btp" / "help".
        if (code != RESULT_OK && !(code == MODULE_NOT_FOUND && !reserved(name))) check(code, name, nullptr);
        return Module(this, name);
    }

    /// First mistake, for begin(): 0 when there was none.
    /// The TinyShell itself, for code that already registers on one
    /// (shell.create_module() / shell.add()). Its mistakes are caught too:
    /// begin() fails if a registration did not fit the tables.
    TinyShell& shell() noexcept { return *shell_; }

    std::uint8_t error_code() const noexcept { return error_code_; }
    const char* error_message() const noexcept { return error_message_; }

private:
    static bool reserved(const char* name) noexcept {
        return name != nullptr && ((name[0] == 'b' && name[1] == 't' && name[2] == 'p' && name[3] == '\0') ||
                                   (name[0] == 'h' && name[1] == 'e' && name[2] == 'l' && name[3] == 'p' &&
                                    name[4] == '\0'));
    }
    void check(std::uint8_t code, const char* module, const char* name) noexcept {
        if (code == RESULT_OK || error_code_ != 0U) return;
        error_code_ = code;
        const char* what = code == TABLE_FULL ? "the command table is full (TINYSHELL_MAX_* in menuconfig)"
                           : code == MODULE_NOT_FOUND
                               ? (name == nullptr ? "the module name is reserved (btp, help)" : "unknown module")
                               : "the name is already used";
        std::snprintf(error_message_, sizeof(error_message_), "command \"%s%s%s\": %s",
                      module != nullptr ? module : "?", name != nullptr ? " -" : "",
                      name != nullptr ? name : "", what);
    }

    TinyShell* shell_;
    std::uint8_t error_code_ = 0U;
    char error_message_[128] = {};
};

/// Writes to TraceView's terminal. Inside a command, the text is part of that
/// command's output; anywhere else (any task), it appears above the prompt of
/// every open terminal. Never blocks; dropped when nobody is listening.
void printf(const char* format, ...) noexcept
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

}  // namespace btpdev

#endif  // BTPDEV_COMMANDS_H
