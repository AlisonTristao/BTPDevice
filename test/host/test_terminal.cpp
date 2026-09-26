#include "protocol/Terminal.h"

#include <string>
#include <vector>

#include "check.h"

using btpdev::Terminal;

namespace {

struct Harness {
    Terminal terminal;
    std::vector<std::string> submitted;
    std::string out;               // everything emitted, concatenated
    std::size_t emits = 0U;
    bool accept = true;

    static bool submit(void* context, std::uint32_t, std::uint32_t, const char* line) {
        auto* self = static_cast<Harness*>(context);
        if (!self->accept) return false;
        self->submitted.emplace_back(line);
        return true;
    }
    static bool emit(void* context, const std::uint8_t* data, std::size_t size) {
        auto* self = static_cast<Harness*>(context);
        self->out.append(reinterpret_cast<const char*>(data), size);
        ++self->emits;
        return true;
    }

    explicit Harness(std::size_t max_chunk = 200U) {
        terminal.configure(nullptr, &Harness::submit, &Harness::emit, this, "dev> ", max_chunk);
    }
    void type(const char* text, std::uint32_t source = 1U, std::uint32_t now = 0U) {
        terminal.on_input(source, 1U, reinterpret_cast<const std::uint8_t*>(text),
                          std::char_traits<char>::length(text), now);
        terminal.pump(now);
    }
};

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

void first_input_paints_the_prompt_and_echoes() {
    Harness h;
    h.type("he");
    CHECK(contains(h.out, "dev> "));
    CHECK(contains(h.out, "h"));
    CHECK(h.submitted.empty());
}

void enter_submits_the_line_and_output_comes_back() {
    Harness h;
    h.type("help\r");
    CHECK(h.submitted.size() == 1U);
    CHECK_STR_EQ(h.submitted[0].c_str(), "help");

    h.out.clear();
    h.terminal.deliver(1U, 1U, "modules:\n  relay\n", 0U);
    h.terminal.pump(10U);
    CHECK(contains(h.out, "! modules:\r\n"));
    CHECK(contains(h.out, "!   relay\r\n"));
    CHECK(contains(h.out, "dev> "));  // fresh prompt after the output
    CHECK(!contains(h.out, "[status="));
}

void a_failed_command_shows_its_status() {
    const std::string text = btpdev::render_command_output("", 255U);
    CHECK_STR_EQ(text.c_str(), "! [status=255]\r\n");
}

void long_output_is_truncated_with_a_notice() {
    const std::string big(5000, 'x');
    const std::string text = btpdev::render_command_output(big.c_str(), 0U);
    CHECK(text.size() < 5000U);
    CHECK(contains(text, "[output truncated]"));
}

void a_line_typed_during_a_command_waits_for_it() {
    Harness h;
    h.type("slow\r");
    h.type("next\r");
    CHECK(h.submitted.size() == 1U);  // "next" is held
    h.type("third\r");                // a second extra line is dropped
    CHECK(contains(h.out, "busy, command dropped"));
    h.terminal.deliver(1U, 1U, "done", 0U);
    h.terminal.pump(5U);
    CHECK(h.submitted.size() == 2U);
    CHECK_STR_EQ(h.submitted[1].c_str(), "next");
}

void a_busy_shell_refuses_with_a_notice() {
    Harness h;
    h.accept = false;
    h.type("help\r");
    CHECK(h.submitted.empty());
    CHECK(contains(h.out, "busy, command dropped"));
    CHECK(h.terminal.stats().lines_dropped_busy == 1U);
}

void origins_do_not_share_their_line() {
    Harness h;
    h.type("rel", 1U);
    h.type("xyz\r", 2U);   // another TraceView window
    h.type("ay\r", 1U);
    CHECK(h.submitted.size() == 2U);
    CHECK_STR_EQ(h.submitted[0].c_str(), "xyz");
    CHECK_STR_EQ(h.submitted[1].c_str(), "relay");
}

void output_is_cut_into_link_sized_chunks() {
    Harness h(16U);
    h.type("x\r");
    h.out.clear();
    h.emits = 0U;
    h.terminal.deliver(1U, 1U, std::string(100, 'a').c_str(), 0U);
    h.terminal.pump(1U);
    CHECK(h.emits >= 7U);  // > 100 bytes in 16-byte TERMINAL_OUT payloads
    CHECK(contains(h.out, std::string(50, 'a').c_str()));
}

void reset_forgets_every_origin() {
    Harness h;
    h.type("abc", 1U);
    h.terminal.reset();
    h.out.clear();
    h.type("\r", 1U);            // a fresh origin: the old "abc" is gone
    CHECK(h.submitted.empty());  // an empty line is not submitted
    CHECK(contains(h.out, "dev> "));
}

void async_lines_print_above_the_prompt() {
    Harness h;
    h.type("a");
    h.out.clear();
    h.terminal.async_line("sensor offline\n");
    h.terminal.pump(100U);
    CHECK(contains(h.out, "! sensor offline\r\n"));
    h.out.clear();
    h.terminal.pump(100U + Terminal::kPromptRestoreMs);
    CHECK(contains(h.out, "dev> "));  // prompt and the typed "a" come back
}

}  // namespace

int main() {
    first_input_paints_the_prompt_and_echoes();
    enter_submits_the_line_and_output_comes_back();
    a_failed_command_shows_its_status();
    long_output_is_truncated_with_a_notice();
    a_line_typed_during_a_command_waits_for_it();
    a_busy_shell_refuses_with_a_notice();
    origins_do_not_share_their_line();
    output_is_cut_into_link_sized_chunks();
    reset_forgets_every_origin();
    async_lines_print_above_the_prompt();
    return test_result("test_terminal");
}
