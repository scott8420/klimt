#pragma once
// core::Jobs -- THE UI NEVER WAITS (RULES, Scott s001), as a mechanism.
//
// A long job (a filter, an auto-levels, a trace, a model run) is split in two:
//
//   work(Token&)  runs on a WORKER thread. It reads what it was handed (copies,
//                 or immutable tiles), computes, reports progress, checks
//                 cancelled() often -- and NEVER touches the document. It
//                 returns the LANDING: a closure holding the result.
//   landing()     runs on the UI thread, from pump(). It is the only part that
//                 writes, and it writes through the journal -- so a job lands
//                 as ONE undo step, and only when it has finished.
//
// A cancelled job's landing is dropped: a cancel leaves nothing behind. A job
// that throws lands nothing either; its error is kept for the UI to show.
//
// GTK-free. The app wires set_wake() to a Glib::Dispatcher (emit() is
// thread-safe) whose handler calls pump(); the selftest calls pump() itself.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace klimt::core {

class Jobs {
public:
    using Id      = std::uint64_t;
    using Landing = std::function<void()>;

    enum class State { Queued, Running, Done, Cancelled, Failed, Landed };

    // What a worker sees of its own job.
    class Token {
    public:
        bool cancelled() const { return m_cancel.load(std::memory_order_relaxed); }
        void progress(double fraction);   // 0..1, clamped; the UI polls it
    private:
        friend class Jobs;
        std::atomic<bool>   m_cancel{false};
        std::atomic<double> m_progress{0.0};
    };
    using Work = std::function<Landing(Token&)>;

    // threads == 0: one fewer than the cores (at least one) -- the UI keeps one.
    explicit Jobs(unsigned threads = 0);
    ~Jobs();   // cancels everything still queued or running, joins the workers
    Jobs(const Jobs&) = delete;
    Jobs& operator=(const Jobs&) = delete;

    Id   submit(const std::string& label, Work work);
    void cancel(Id id);          // queued: never runs; running: its landing is dropped
    void cancel_all();

    // UI thread: land every finished, un-cancelled job, in the order they
    // finished. Returns how many landed.
    std::size_t pump();
    // Called (from a worker) when a job finishes; the app emits a dispatcher.
    void set_wake(std::function<void()> f);

    State       state(Id id) const;
    double      progress(Id id) const;
    std::string label(Id id) const;
    std::string error(Id id) const;
    std::size_t active() const;  // queued + running
    unsigned    threads() const { return static_cast<unsigned>(m_workers.size()); }
    // Tests only: block until nothing is queued or running.
    void wait_idle();

private:
    struct Job {
        Id          id = 0;
        std::string label;
        Work        work;
        Landing     landing;
        std::string error;
        State       state = State::Queued;
        Token       token;
    };
    void loop(std::stop_token st);
    std::shared_ptr<Job> find(Id id) const;

    mutable std::mutex                    m_mu;
    std::condition_variable_any           m_cv;
    std::condition_variable               m_idle;
    std::deque<std::shared_ptr<Job>>     m_queue;
    std::map<Id, std::shared_ptr<Job>>   m_jobs;
    std::vector<Id>                       m_finished;   // waiting for pump()
    std::size_t                           m_running = 0;
    Id                                    m_next = 1;
    std::function<void()>                 m_wake;
    std::vector<std::jthread>             m_workers;     // last: joined first
};

}  // namespace klimt::core
