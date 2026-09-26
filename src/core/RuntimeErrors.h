#ifndef BTPDEV_RUNTIME_ERRORS_H
#define BTPDEV_RUNTIME_ERRORS_H

// Errors that happen while running, possibly many times per second, in
// contexts that must not log (driver callbacks). Producers only bump an atomic
// counter; the communication task calls report() and gets, per kind:
//
//   * the FIRST occurrence right away, so the problem shows up immediately;
//   * after that, at most one summary per `period_ms` with how many happened
//     ("send queue full: 37 in the last second"), so a 100 Hz failure does
//     not bury the console.
//
// Lock-free for the producers, single consumer. Portable, covered by
// test/host/test_runtime_errors.cpp.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace btpdev {

enum class RuntimeError : std::uint8_t {
    RxOverflow,       ///< receive buffer full: link bytes dropped
    TxRefused,        ///< a frame the link refused (no peer, queue full)
    StreamCorrupt,    ///< COBS / CRC error on a stream link
    ShellBusy,        ///< a command arrived while the shell was busy
    TerminalDropped,  ///< terminal input or output dropped (too long / too fast)
    SessionTimeout,   ///< the peer went silent: session closed by the watchdog
    Count
};

const char* runtime_error_name(RuntimeError kind) noexcept;

class RuntimeErrors {
public:
    static constexpr std::size_t kKinds = static_cast<std::size_t>(RuntimeError::Count);

    /// Any task / any context. Never blocks, never logs.
    void note(RuntimeError kind, std::uint32_t count = 1U) noexcept {
        const auto i = static_cast<std::size_t>(kind);
        if (i < kKinds) counters_[i].count.fetch_add(count, std::memory_order_relaxed);
    }

    /// Total so far (for STATUS / `btp -errors`).
    std::uint32_t total(RuntimeError kind) const noexcept {
        const auto i = static_cast<std::size_t>(kind);
        return i < kKinds ? counters_[i].count.load(std::memory_order_relaxed) : 0U;
    }

    /// Consumer side. Calls emit(kind, how_many, first) for each kind that has
    /// something to say at `now_ms`: `first` is true for the very first
    /// occurrence(s), reported at once; later ones are grouped per period.
    template <typename Emit>
    void report(std::uint32_t now_ms, std::uint32_t period_ms, Emit&& emit) noexcept {
        for (std::size_t i = 0U; i < kKinds; ++i) {
            Counter& c = counters_[i];
            const std::uint32_t count = c.count.load(std::memory_order_relaxed);
            if (count == c.reported) continue;
            const bool first = !c.ever_reported;
            if (!first && now_ms - c.last_report_ms < period_ms) continue;
            emit(static_cast<RuntimeError>(i), count - c.reported, first);
            c.reported = count;
            c.last_report_ms = now_ms;
            c.ever_reported = true;
        }
    }

private:
    struct Counter {
        std::atomic<std::uint32_t> count{0U};
        std::uint32_t reported = 0U;         // consumer only
        std::uint32_t last_report_ms = 0U;   // consumer only
        bool ever_reported = false;          // consumer only
    };
    Counter counters_[kKinds];
};

}  // namespace btpdev

#endif  // BTPDEV_RUNTIME_ERRORS_H
