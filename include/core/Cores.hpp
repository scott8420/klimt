#pragma once
// core::on_cores -- s055 (Responsive): `n` independent items on all the cores,
// each taken as a thread frees up. The calling thread works too and is the
// only one to report progress (from + (to - from) * done / n), so a
// Progress that touches the UI's state is heard from one thread, as before.
// false: stopped -- p.stop(), or an item returned false.
#include "formats/Formats.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

namespace klimt::core {

inline bool on_cores(std::size_t n, const std::function<bool(std::size_t)>& item, const formats::Progress& p = {},
                     double from = 0, double to = 1) {
    std::atomic<std::size_t> next{0}, done{0};
    std::atomic<bool> stop{false};
    const auto main_id = std::this_thread::get_id();
    const auto work = [&] {
        for (;;) {
            const std::size_t i = next++;
            if (i >= n || stop) return;
            if (p.stop() || !item(i)) { stop = true; return; }
            const std::size_t d = ++done;
            if (std::this_thread::get_id() == main_id)
                p.at(from + (to - from) * static_cast<double>(d) / static_cast<double>(n));
        }
    };
    const unsigned nt = static_cast<unsigned>(
        std::min<std::size_t>(std::max<std::size_t>(n, 1), std::max(1u, std::thread::hardware_concurrency())));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < nt; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    return !stop;
}

}  // namespace klimt::core
