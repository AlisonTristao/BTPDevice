#ifndef BTPDEV_TERMINAL_H
#define BTPDEV_TERMINAL_H

// The BTP terminal (TERMINAL_IN / TERMINAL_OUT, session-and-terminal.md
// section 7) served to TraceView's terminal widget: line editing runs here, in
// one ShellLineEditor per remote origin (echo, backspace, arrows, history,
// Tab, Ctrl+R), completed lines go to the shell, and a command's output comes
// back inline, "! "-prefixed, followed by a fresh prompt.
//
// Ported from bally_OS lib/TerminalResponder, simplified by the library's
// threading rule (docs/PLANO.md D8): everything here runs on the
// communication task, so there are no locks and no per-link targets -- the
// output always leaves through the one link of this boot.
//
// TERMINAL has no destination field, so origins are told apart by
// (source_id, boot_id): two TraceView windows on the same device do not mix
// their line editing. A small LRU pool holds them.
//
// Portable (std::string + TinyShell's ShellLineEditor), covered by
// test/host/test_terminal.cpp. The strings allocate a little per keystroke;
// the terminal is human-paced, not a hot path.

#include <cstddef>
#include <cstdint>
#include <string>

#include <ShellLineEditor.h>

namespace btpdev {

class Terminal {
public:
    static constexpr std::size_t kMaxOrigins = 2U;
    static constexpr std::size_t kHistory = 16U;
    static constexpr std::size_t kMaxPendingIn = 1024U;
    static constexpr std::size_t kMaxCommandOut = 2048U;
    static constexpr std::size_t kMaxAsyncOut = 1024U;
    /// Quiet time after asynchronous output before the prompt is repainted.
    static constexpr std::uint32_t kPromptRestoreMs = 250U;

    /// A completed line to run. False: the shell is busy (the line is dropped
    /// with a "! busy" notice to that origin).
    using SubmitFn = bool (*)(void* context, std::uint32_t source_id, std::uint32_t boot_id,
                              const char* line);
    /// Sends one TERMINAL_OUT payload (at most `max_chunk` bytes).
    using EmitFn = bool (*)(void* context, const std::uint8_t* data, std::size_t size);

    struct Stats {
        std::uint32_t origins_seen = 0U;
        std::uint32_t origins_evicted = 0U;
        std::uint32_t lines_submitted = 0U;
        std::uint32_t lines_dropped_busy = 0U;
        std::uint32_t bytes_in_dropped = 0U;
        std::uint32_t chunks_out = 0U;
        std::uint32_t chunks_out_failed = 0U;
    };

    Terminal() noexcept;

    /// `prompt` is copied. `max_chunk` is the largest TERMINAL_OUT payload
    /// the link carries in one logical message (BTP fragments below that).
    void configure(ShellLineEditor::CompletionProvider completion, SubmitFn submit, EmitFn emit,
                   void* context, const char* prompt, std::size_t max_chunk);

    /// One TERMINAL_IN payload from an origin.
    void on_input(std::uint32_t source_id, std::uint32_t boot_id, const std::uint8_t* data,
                  std::size_t size, std::uint32_t now_ms);

    /// A submitted command finished: its output (may be empty) and status.
    /// Ignored if that origin was evicted meanwhile.
    void deliver(std::uint32_t source_id, std::uint32_t boot_id, const char* text,
                 std::uint8_t status);

    /// One line of unsolicited output (a log, a background result) for every
    /// live origin, printed above the prompt.
    void async_line(const char* text);

    /// Runs the editors, submits lines, sends echo / output. Every pass.
    void pump(std::uint32_t now_ms);

    /// The link went away: forget every origin (a new connection does not
    /// inherit the previous one's line or history).
    void reset();

    const Stats& stats() const noexcept { return stats_; }

private:
    struct Slot {
        Slot() : editor(kHistory) {}
        bool used = false;
        std::uint32_t source_id = 0U;
        std::uint32_t boot_id = 0U;
        std::uint32_t last_used_ms = 0U;
        bool prompt_painted = false;
        bool awaiting_result = false;
        bool result_ready = false;
        bool async_dirty = false;
        std::uint32_t last_async_ms = 0U;
        std::string in;
        std::string result;
        std::string async_out;
        std::string queued_line;  // typed while a command was running
        ShellLineEditor editor;
    };

    Slot* find(std::uint32_t source_id, std::uint32_t boot_id);
    Slot* find_or_alloc(std::uint32_t source_id, std::uint32_t boot_id, std::uint32_t now_ms);
    void submit(Slot& slot, const std::string& line, std::string& out);
    void emit(const std::string& bytes);

    Slot slots_[kMaxOrigins];
    ShellLineEditor::CompletionProvider completion_;
    SubmitFn submit_ = nullptr;
    EmitFn emit_ = nullptr;
    void* context_ = nullptr;
    std::string prompt_;
    std::size_t max_chunk_ = 200U;
    Stats stats_{};
};

/// "! "-prefixed, CR+LF lines of a command's output, plus "! [status=N]" when
/// it failed; truncated past Terminal::kMaxCommandOut with a notice.
std::string render_command_output(const char* text, std::uint8_t status);

}  // namespace btpdev

#endif  // BTPDEV_TERMINAL_H
