#include "core/Jobs.hpp"

#include <algorithm>
#include <exception>

namespace klimt::core {

void Jobs::Token::progress(double fraction) {
    m_progress.store(std::clamp(fraction, 0.0, 1.0), std::memory_order_relaxed);
}

Jobs::Jobs(unsigned threads) {
    if (threads == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        threads = hw > 1 ? hw - 1 : 1;
    }
    for (unsigned i = 0; i < threads; ++i)
        m_workers.emplace_back([this](std::stop_token st) { loop(st); });
}

Jobs::~Jobs() {
    cancel_all();
    for (auto& w : m_workers) w.request_stop();
    m_cv.notify_all();
    m_workers.clear();   // joins
}

Jobs::Id Jobs::submit(const std::string& label, Work work) {
    auto job = std::make_shared<Job>();
    {
        std::lock_guard lk(m_mu);
        job->id    = m_next++;
        job->label = label;
        job->work  = std::move(work);
        m_jobs[job->id] = job;
        m_queue.push_back(job);
    }
    m_cv.notify_one();
    return job->id;
}

void Jobs::cancel(Id id) {
    std::lock_guard lk(m_mu);
    const auto it = m_jobs.find(id);
    if (it == m_jobs.end()) return;
    Job& j = *it->second;
    j.token.m_cancel.store(true);
    if (j.state == State::Queued) {
        j.state = State::Cancelled;
        m_queue.erase(std::remove(m_queue.begin(), m_queue.end(), it->second), m_queue.end());
        if (m_queue.empty() && m_running == 0) m_idle.notify_all();
    } else if (j.state == State::Done) {
        j.state = State::Cancelled;   // finished but not landed: drop the landing
        j.landing = nullptr;
    }
}

void Jobs::cancel_all() {
    std::vector<Id> ids;
    {
        std::lock_guard lk(m_mu);
        for (const auto& [id, j] : m_jobs) ids.push_back(id);
    }
    for (const auto id : ids) cancel(id);
}

void Jobs::loop(std::stop_token st) {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lk(m_mu);
            m_cv.wait(lk, st, [this] { return !m_queue.empty(); });
            if (st.stop_requested() && m_queue.empty()) return;
            if (m_queue.empty()) continue;
            job = m_queue.front();
            m_queue.pop_front();
            job->state = State::Running;
            ++m_running;
        }
        Landing landing;
        std::string err;
        try {
            landing = job->work(job->token);
        } catch (const std::exception& e) {
            err = e.what();
            if (err.empty()) err = "failed";
        } catch (...) {
            err = "failed";
        }
        std::function<void()> wake;
        {
            std::lock_guard lk(m_mu);
            --m_running;
            job->work = nullptr;
            if (job->token.cancelled())  job->state = State::Cancelled;
            else if (!err.empty())       { job->state = State::Failed; job->error = err; }
            else {
                job->state   = State::Done;
                job->landing = std::move(landing);
                m_finished.push_back(job->id);
            }
            wake = m_wake;
            if (m_queue.empty() && m_running == 0) m_idle.notify_all();
        }
        if (wake) wake();
    }
}

std::size_t Jobs::pump() {
    std::vector<std::shared_ptr<Job>> ready;
    {
        std::lock_guard lk(m_mu);
        for (const auto id : m_finished) {
            const auto it = m_jobs.find(id);
            if (it != m_jobs.end() && it->second->state == State::Done) ready.push_back(it->second);
        }
        m_finished.clear();
    }
    std::size_t landed = 0;
    for (auto& job : ready) {
        Landing f;
        {
            std::lock_guard lk(m_mu);
            if (job->state != State::Done) continue;   // cancelled since
            f = std::move(job->landing);
            job->landing = nullptr;
            job->state = State::Landed;
        }
        if (f) f();   // outside the lock: the landing writes through the journal
        ++landed;
    }
    return landed;
}

void Jobs::set_wake(std::function<void()> f) {
    std::lock_guard lk(m_mu);
    m_wake = std::move(f);
}

std::shared_ptr<Jobs::Job> Jobs::find(Id id) const {
    const auto it = m_jobs.find(id);
    return it == m_jobs.end() ? nullptr : it->second;
}

Jobs::State Jobs::state(Id id) const {
    std::lock_guard lk(m_mu);
    const auto j = find(id);
    return j ? j->state : State::Cancelled;
}

double Jobs::progress(Id id) const {
    std::lock_guard lk(m_mu);
    const auto j = find(id);
    if (!j) return 0.0;
    if (j->state == State::Done || j->state == State::Landed) return 1.0;
    return j->token.m_progress.load(std::memory_order_relaxed);
}

std::string Jobs::label(Id id) const {
    std::lock_guard lk(m_mu);
    const auto j = find(id);
    return j ? j->label : std::string{};
}

std::string Jobs::error(Id id) const {
    std::lock_guard lk(m_mu);
    const auto j = find(id);
    return j ? j->error : std::string{};
}

std::size_t Jobs::active() const {
    std::lock_guard lk(m_mu);
    return m_queue.size() + m_running;
}

void Jobs::wait_idle() {
    std::unique_lock lk(m_mu);
    m_idle.wait(lk, [this] { return m_queue.empty() && m_running == 0; });
}

}  // namespace klimt::core
