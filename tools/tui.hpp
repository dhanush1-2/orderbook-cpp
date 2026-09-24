// tools/tui.hpp
#pragma once

// Minimal ANSI terminal depth ladder.
//
// Hand-rolled rather than pulled from a widget library: the engine, harness and
// tools depend on the standard library alone, and this keeps that true. The risk
// that justifies care here is not layout, it is LEAVING THE TERMINAL BROKEN. The
// alternate screen and the hidden cursor must be undone on every exit path,
// including SIGINT, so Terminal is RAII and the signal handler only sets a flag.
//
// render() returns a std::string and takes explicit dimensions, precisely so it can
// be tested without a terminal attached.

#include <sys/ioctl.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <ob/l2_snapshot.hpp>
#include <string>

namespace ob::tui {

// Set by the SIGINT handler. Handlers may not allocate or do I/O, so it does
// nothing else; the render loop polls this and returns.
inline std::atomic<bool> g_interrupted{false};

inline void on_sigint(int) {
    g_interrupted.store(true, std::memory_order_relaxed);
}

// Enters the alternate screen and hides the cursor; restores both on destruction,
// including when the stack unwinds. Also installs the SIGINT handler.
class Terminal {
public:
    Terminal() {
        std::signal(SIGINT, on_sigint);
        std::fputs("\033[?1049h", stdout);  // alternate screen
        std::fputs("\033[?25l", stdout);    // hide cursor
        std::fflush(stdout);
        active_ = true;
    }

    ~Terminal() { restore(); }

    Terminal(const Terminal&)            = delete;
    Terminal& operator=(const Terminal&) = delete;

    void restore() noexcept {
        if (!active_) {
            return;
        }
        std::fputs("\033[?25h", stdout);    // show cursor
        std::fputs("\033[?1049l", stdout);  // leave alternate screen
        std::fflush(stdout);
        active_ = false;
    }

    // Queried per frame, so a resize needs no signal handling at all.
    static void size(int& rows, int& cols) noexcept {
        winsize w{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0 && w.ws_col > 0) {
            rows = w.ws_row;
            cols = w.ws_col;
        } else {
            rows = 24;
            cols = 80;
        }
    }

    static void draw(const std::string& frame) {
        std::fputs("\033[H", stdout);  // cursor home; no clear, to avoid flicker
        std::fwrite(frame.data(), 1, frame.size(), stdout);
        std::fflush(stdout);
    }

private:
    bool active_ = false;
};

namespace detail {

inline std::string bar(QtySum qty, QtySum max_qty, int width) {
    if (width <= 0 || max_qty == 0) {
        return {};
    }
    const std::size_t n = static_cast<std::size_t>(
        (static_cast<double>(qty) / static_cast<double>(max_qty)) * static_cast<double>(width));
    return std::string(n, '#');
}

inline QtySum max_level_qty(const L2Snapshot& s) {
    QtySum m = 0;
    for (std::uint32_t i = 0; i < s.bid_levels; ++i) {
        m = s.bids[i].qty > m ? s.bids[i].qty : m;
    }
    for (std::uint32_t i = 0; i < s.ask_levels; ++i) {
        m = s.asks[i].qty > m ? s.asks[i].qty : m;
    }
    return m;
}

}  // namespace detail

// Renders a depth ladder: asks worst-to-best going down, the spread, then bids
// best-to-worst. Clamps to the given dimensions and never writes past them.
inline std::string render(const L2Snapshot& s, int rows, int cols, std::uint64_t commands,
                          std::uint64_t trades, double ops_per_sec) {
    if (rows < 3 || cols < 20) {
        return "terminal too small\n";
    }

    // Reserve: title, spread line, footer, and a blank.
    const int chrome   = 4;
    const int usable   = rows - chrome;
    const int per_side = usable / 2 > 0 ? usable / 2 : 1;

    const std::uint32_t nb =
        std::min<std::uint32_t>(s.bid_levels, static_cast<std::uint32_t>(per_side));
    const std::uint32_t na =
        std::min<std::uint32_t>(s.ask_levels, static_cast<std::uint32_t>(per_side));

    const QtySum maxq      = detail::max_level_qty(s);
    const int    bar_width = cols > 46 ? cols - 46 : 0;

    std::string out;
    out.reserve(static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols) + 64);
    char line[512];

    std::snprintf(line, sizeof(line), "\033[K  ORDER BOOK   seq %-12llu\n",
                  static_cast<unsigned long long>(s.seq));
    out += line;

    // Asks descending so the best ask sits just above the spread.
    for (std::uint32_t i = na; i-- > 0;) {
        const L2Level& lv = s.asks[i];
        std::snprintf(line, sizeof(line), "\033[K  \033[31m%8d  %10llu  %4u\033[0m  %s\n", lv.price,
                      static_cast<unsigned long long>(lv.qty), lv.orders,
                      detail::bar(lv.qty, maxq, bar_width).c_str());
        out += line;
    }

    if (s.spread() == kNoPrice) {
        std::snprintf(line, sizeof(line), "\033[K  %s\n", "------- one side empty -------");
    } else {
        std::snprintf(line, sizeof(line), "\033[K  ------- spread %d -------\n", s.spread());
    }
    out += line;

    for (std::uint32_t i = 0; i < nb; ++i) {
        const L2Level& lv = s.bids[i];
        std::snprintf(line, sizeof(line), "\033[K  \033[32m%8d  %10llu  %4u\033[0m  %s\n", lv.price,
                      static_cast<unsigned long long>(lv.qty), lv.orders,
                      detail::bar(lv.qty, maxq, bar_width).c_str());
        out += line;
    }

    // Adaptive units. A fixed "M ops/s" reads 0.00 at any rate a human can watch,
    // which is exactly the rate the --rate flag exists to produce.
    char rate[32];
    if (ops_per_sec >= 1e6) {
        std::snprintf(rate, sizeof(rate), "%.2f M ops/s", ops_per_sec / 1e6);
    } else if (ops_per_sec >= 1e3) {
        std::snprintf(rate, sizeof(rate), "%.1f K ops/s", ops_per_sec / 1e3);
    } else {
        std::snprintf(rate, sizeof(rate), "%.0f ops/s", ops_per_sec);
    }
    std::snprintf(line, sizeof(line),
                  "\033[K\n\033[K  commands %-12llu trades %-12llu  %-14s "
                  "ctrl-c to quit\033[J\n",
                  static_cast<unsigned long long>(commands),
                  static_cast<unsigned long long>(trades), rate);
    out += line;
    return out;
}

}  // namespace ob::tui
