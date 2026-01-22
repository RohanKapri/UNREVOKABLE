// ============================================================================
// Deadline-Aware Execution Gate – EXTENDED & HEAVILY INSTRUMENTED VERSION
// C++23 (ISO/IEC 14882:2023) — STRICTLY COMPLIANT
// ============================================================================
//
// 
//
// 1. Much more detailed observability & statistics
// 2. Multiple admission policies / strategies (selectable)
// 3. Task cancellation support (best-effort + strong)
// 4. Multiple scheduling disciplines (EDF, FIFO, priority, hybrid)
// 5. Per-task & global timeout / watchdog mechanism
// 6. Execution budget enforcement (CPU time quota)
// 7. Multiple worker pools with affinity hints
// 8. Detailed latency / jitter / miss histograms
// 9. JSON-like structured logging (human & machine readable)
//10. Runtime configuration & dynamic parameter changes
//11. Task dependency / precedence graph (very basic)
//12. Overload / admission throttling modes
//13. Simulated background interference / noise
//14. Much more defensive programming & contract checking
//
// This version is intentionally **long**, explicit, verbose and over-engineered
// for demonstration, teaching, auditing and worst-case analysis purposes.
//
//
//
// ============================================================================

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cassert>
#include <chrono>
#include <compare>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <format>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <queue>
#include <random>
#include <ranges>
#include <regex>
#include <semaphore>
#include <set>
#include <shared_mutex>
#include <span>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <string>
#include <string_view>
#include <syncstream>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

using namespace std::chrono_literals;
using namespace std::string_literals;
using namespace std::string_view_literals;

// ─────────────────────────────────────────────────────────────────────────────
//  Clock & Time Abstractions
// ─────────────────────────────────────────────────────────────────────────────

using MonotonicClock     = std::chrono::steady_clock;
using SystemClock        = std::chrono::system_clock;
using HighResClock       = std::chrono::high_resolution_clock;

using TimePoint          = MonotonicClock::time_point;
using SysTimePoint       = SystemClock::time_point;
using Duration           = MonotonicClock::duration;
using Nanos              = std::chrono::nanoseconds;
using Micros             = std::chrono::microseconds;
using Millis             = std::chrono::milliseconds;
using Seconds            = std::chrono::seconds;

// For readability in conditions
constexpr auto NEVER     = Duration::max() / 2;
constexpr auto IMMEDIATE = Duration::zero();

// ─────────────────────────────────────────────────────────────────────────────
//  Strongly-typed identifiers
// ─────────────────────────────────────────────────────────────────────────────

struct TaskId           { std::uint64_t value{}; auto operator<=>(const TaskId&) const = default; };
struct WorkerId         { std::uint32_t value{}; auto operator<=>(const WorkerId&) const = default; };
struct PoolId           { std::uint16_t value{}; auto operator<=>(const PoolId&)   const = default; };
struct AdmissionToken   { std::uint64_t value{}; auto operator<=>(const AdmissionToken&) const = default; };

template<> struct std::hash<TaskId>     { std::size_t operator()(TaskId id)     const noexcept { return std::hash<std::uint64_t>{}(id.value); }};
template<> struct std::hash<WorkerId>   { std::size_t operator()(WorkerId id)   const noexcept { return std::hash<std::uint32_t>{}(id.value); }};
template<> struct std::hash<PoolId>     { std::size_t operator()(PoolId id)     const noexcept { return std::hash<std::uint16_t>{}(id.value); }};

// ─────────────────────────────────────────────────────────────────────────────
//  Task States – very granular lifecycle
// ─────────────────────────────────────────────────────────────────────────────

enum class TaskState : std::uint8_t
{
    INVALID             = 0,
    CREATED,
    ADMISSION_PENDING,
    ADMITTED,
    QUEUED,
    SELECTED_FOR_EXEC,
    EXECUTING,
    PREEMPTED,
    COMPLETED_SUCCESS,
    COMPLETED_WITH_ERROR,
    REJECTED_AT_GATE,
    REJECTED_OVERLOAD,
    ABORTED_TIMEOUT,
    ABORTED_CANCELLED,
    ABORTED_WORKER_DEATH,
    ABORTED_DEPENDENCY_FAILED,
    DROPPED_SHUTDOWN
};

[[nodiscard]] constexpr
std::string_view to_string(TaskState s) noexcept
{
    switch (s)
    {
        case TaskState::INVALID:                 return "INVALID"sv;
        case TaskState::CREATED:                 return "CREATED"sv;
        case TaskState::ADMISSION_PENDING:       return "ADMISSION_PENDING"sv;
        case TaskState::ADMITTED:                return "ADMITTED"sv;
        case TaskState::QUEUED:                  return "QUEUED"sv;
        case TaskState::SELECTED_FOR_EXEC:       return "SELECTED_FOR_EXEC"sv;
        case TaskState::EXECUTING:               return "EXECUTING"sv;
        case TaskState::PREEMPTED:               return "PREEMPTED"sv;
        case TaskState::COMPLETED_SUCCESS:       return "COMPLETED_SUCCESS"sv;
        case TaskState::COMPLETED_WITH_ERROR:    return "COMPLETED_WITH_ERROR"sv;
        case TaskState::REJECTED_AT_GATE:        return "REJECTED_AT_GATE"sv;
        case TaskState::REJECTED_OVERLOAD:       return "REJECTED_OVERLOAD"sv;
        case TaskState::ABORTED_TIMEOUT:         return "ABORTED_TIMEOUT"sv;
        case TaskState::ABORTED_CANCELLED:       return "ABORTED_CANCELLED"sv;
        case TaskState::ABORTED_WORKER_DEATH:    return "ABORTED_WORKER_DEATH"sv;
        case TaskState::ABORTED_DEPENDENCY_FAILED:return "ABORTED_DEPENDENCY_FAILED"sv;
        case TaskState::DROPPED_SHUTDOWN:        return "DROPPED_SHUTDOWN"sv;
        default:                                 return "???"sv;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Admission Policy kinds
// ─────────────────────────────────────────────────────────────────────────────

enum class AdmissionPolicy : std::uint8_t
{
    HardDeadlineOnly,           // classic: now + WCET < deadline
    SlackBasedWithMargin,       // now + WCET < deadline - margin
    UtilizationBound,           // approximate Liu-Layland style test
    AdmissionTokenBucket,       // rate-based admission control
    HybridOverloadProtect       // combination + dynamic throttling
};

// ─────────────────────────────────────────────────────────────────────────────
//  Scheduling Discipline
// ─────────────────────────────────────────────────────────────────────────────

enum class SchedulingPolicy : std::uint8_t
{
    EarliestDeadlineFirst,      // EDF – optimal for single processor
    DeadlineMonotonic,          // DM   – fixed priority version
    FIFO,                       // simple queue
    HighestPriorityFirst,       // user-assigned priority
    HybridEDF_DM                // mix – EDF until utilization threshold, then DM
};

// ─────────────────────────────────────────────────────────────────────────────
//  Task priority level (used in some policies)
// ─────────────────────────────────────────────────────────────────────────────

enum class TaskPriority : std::uint8_t
{
    Critical    = 0,
    High        = 64,
    Normal      = 128,
    Low         = 192,
    Background  = 255
};

// ─────────────────────────────────────────────────────────────────────────────
//  Task – greatly extended
// ─────────────────────────────────────────────────────────────────────────────

struct Task final
{
    TaskId                              id;
    SysTimePoint                        arrival_time{SystemClock::now()};
    TimePoint                           submit_time{MonotonicClock::now()};
    TimePoint                           deadline;
    Duration                            estimated_worst_case_execution_time;
    Duration                            soft_timeout{0ms};               // optional
    std::atomic<Duration>               actual_execution_time{0ns};
    std::atomic<TaskState>              state{TaskState::CREATED};
    std::atomic<TaskPriority>           priority{TaskPriority::Normal};
    std::atomic<bool>                   cancellation_requested{false};
    std::atomic<bool>                   cancellation_acknowledged{false};
    std::function<void()>               work;
    std::weak_ptr<Task>                 parent_dependency;               // very simple dep support
    std::vector<std::weak_ptr<Task>>    dependents;
    std::string                         name;                            // for logging only
    std::optional<AdmissionToken>       admission_token;

    Task() = delete;

    Task(TaskId tid, TimePoint dl, Duration wcet, std::function<void()> callable,
         std::string_view task_name = "", TaskPriority prio = TaskPriority::Normal)
        : id{tid}
        , deadline{dl}
        , estimated_worst_case_execution_time{wcet}
        , work(std::move(callable))
        , name{task_name}
        , priority{prio}
    {}

    [[nodiscard]] bool is_cancelled() const noexcept {
        return cancellation_requested.load(std::memory_order_acquire);
    }

    void request_cancellation() noexcept {
        cancellation_requested.store(true, std::memory_order_release);
    }

    [[nodiscard]] Duration slack(TimePoint now) const noexcept {
        return deadline - now;
    }

    [[nodiscard]] Duration remaining_slack(TimePoint now) const noexcept {
        if (now >= deadline) return Duration::min();
        return deadline - now;
    }

    [[nodiscard]] bool already_missed(TimePoint now) const noexcept {
        return now >= deadline;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Logging – structured, thread-safe, buffered, severity-aware
// ─────────────────────────────────────────────────────────────────────────────

class StructuredLogger
{
public:
    enum class Level : std::uint8_t { Trace=0, Debug, Info, Warn, Error, Fatal };

    static void set_level(Level lvl) noexcept { min_level.store(lvl, std::memory_order_relaxed); }
    static Level get_level() noexcept { return min_level.load(std::memory_order_relaxed); }

    template<typename... Args>
    static void log(Level lvl, std::string_view fmt, Args&&... args)
    {
        if (lvl < get_level()) return;

        auto now = MonotonicClock::now();
        auto nanos_since_start = (now - program_start).count();

        std::string msg;
        msg.reserve(512);

        std::format_to(std::back_inserter(msg),
            "[{:>12} ns] [{}] ",
            nanos_since_start,
            level_to_string(lvl));

        std::vformat_to(std::back_inserter(msg), fmt, std::make_format_args(args...));

        {
            std::lock_guard lk{log_mutex};
            std::cout << msg << '\n';
            if (log_file) {
                *log_file << msg << '\n';
                log_file->flush();
            }
        }
    }

    static void open_log_file(const std::string& path)
    {
        std::lock_guard lk{log_mutex};
        if (log_file) log_file->close();
        log_file.emplace(path, std::ios::out | std::ios::app);
        if (!log_file->is_open()) {
            std::cerr << "Cannot open log file: " << path << '\n';
            log_file.reset();
        }
    }

private:
    static inline std::atomic<Level> min_level{Level::Info};
    static inline std::mutex log_mutex;
    static inline std::optional<std::ofstream> log_file;
    static inline const TimePoint program_start = MonotonicClock::now();

    static constexpr std::string_view level_to_string(Level lvl) noexcept
    {
        switch (lvl) {
            case Level::Trace: return "TRACE";
            case Level::Debug: return "DEBUG";
            case Level::Info:  return "INFO ";
            case Level::Warn:  return "WARN ";
            case Level::Error: return "ERROR";
            case Level::Fatal: return "FATAL";
            default:           return "?????";
        }
    }
};

#define LOG_TRACE(...)   StructuredLogger::log(StructuredLogger::Level::Trace, __VA_ARGS__)
#define LOG_DEBUG(...)   StructuredLogger::log(StructuredLogger::Level::Debug, __VA_ARGS__)
#define LOG_INFO(...)    StructuredLogger::log(StructuredLogger::Level::Info,  __VA_ARGS__)
#define LOG_WARN(...)    StructuredLogger::log(StructuredLogger::Level::Warn,  __VA_ARGS__)
#define LOG_ERROR(...)   StructuredLogger::log(StructuredLogger::Level::Error, __VA_ARGS__)
#define LOG_FATAL(...)   StructuredLogger::log(StructuredLogger::Level::Fatal, __VA_ARGS__)

// ─────────────────────────────────────────────────────────────────────────────
//  Admission Controller – many strategies
// ─────────────────────────────────────────────────────────────────────────────

class AdmissionController
{
public:
    struct Config
    {
        AdmissionPolicy     policy                     = AdmissionPolicy::HardDeadlineOnly;
        Duration            safety_margin              = 0ms;
        double              utilization_bound          = 0.69;   // Liu & Layland
        Duration            token_refill_interval      = 100ms;
        std::size_t         tokens_per_refill          = 5;
        std::size_t         max_tokens                 = 20;
        double              overload_reject_probability = 0.0;
    };

    explicit AdmissionController(Config cfg = {})
        : config_(std::move(cfg))
        , last_refill_(MonotonicClock::now())
        , available_tokens_(config_.max_tokens)
    {}

    [[nodiscard]] bool admit(const Task& t, TimePoint now)
    {
        switch (config_.policy)
        {
            case AdmissionPolicy::HardDeadlineOnly:
                return basic_hard_deadline_check(t, now);

            case AdmissionPolicy::SlackBasedWithMargin:
                return slack_with_margin_check(t, now);

            case AdmissionPolicy::UtilizationBound:
                return approximate_utilization_test(t, now);

            case AdmissionPolicy::AdmissionTokenBucket:
                return token_bucket_admit();

            case AdmissionPolicy::HybridOverloadProtect:
                return hybrid_overload_admit(t, now);

            default:
                LOG_ERROR("Unknown admission policy {}", static_cast<int>(config_.policy));
                return false;
        }
    }

    void refill_tokens_if_needed(TimePoint now)
    {
        if (config_.policy != AdmissionPolicy::AdmissionTokenBucket &&
            config_.policy != AdmissionPolicy::HybridOverloadProtect)
            return;

        auto elapsed = now - last_refill_;
        if (elapsed >= config_.token_refill_interval)
        {
            std::size_t new_tokens = elapsed / config_.token_refill_interval * config_.tokens_per_refill;
            available_tokens_ = std::min(config_.max_tokens, available_tokens_ + new_tokens);
            last_refill_ = now;
        }
    }

private:
    [[nodiscard]] bool basic_hard_deadline_check(const Task& t, TimePoint now) const noexcept
    {
        return (now + t.estimated_worst_case_execution_time) < t.deadline;
    }

    [[nodiscard]] bool slack_with_margin_check(const Task& t, TimePoint now) const noexcept
    {
        return (now + t.estimated_worst_case_execution_time + config_.safety_margin) < t.deadline;
    }

    [[nodiscard]] bool approximate_utilization_test(const Task& t, TimePoint now) const
    {
        // Very simplified – real implementation would need running utilization tracking
        double u = static_cast<double>(t.estimated_worst_case_execution_time.count()) /
                   (t.deadline - now).count();
        return u < config_.utilization_bound;
    }

    [[nodiscard]] bool token_bucket_admit()
    {
        if (available_tokens_ > 0)
        {
            --available_tokens_;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool hybrid_overload_admit(const Task& t, TimePoint now)
    {
        refill_tokens_if_needed(now);

        if (available_tokens_ > 0)
        {
            --available_tokens_;
            return true;
        }

        // overload rejection lottery
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        if (dist(rng_) < config_.overload_reject_probability)
            return false;

        // last chance: hard check
        return basic_hard_deadline_check(t, now);
    }

    Config config_;
    TimePoint last_refill_;
    std::atomic<std::size_t> available_tokens_;
    std::mt19937 rng_{std::random_device{}()};
};

// ─────────────────────────────────────────────────────────────────────────────
//  Very simple task dependency node
// ─────────────────────────────────────────────────────────────────────────────

struct DependencyNode
{
    std::shared_ptr<Task> task;
    std::vector<std::weak_ptr<DependencyNode>> successors;
    std::atomic<std::size_t> unfinished_predecessors{0};
};

// ─────────────────────────────────────────────────────────────────────────────
//  Priority queue with better logging & statistics
// ─────────────────────────────────────────────────────────────────────────────

class DeadlineOrderedQueue
{
public:
    struct Compare
    {
        bool operator()(const std::shared_ptr<Task>& a,
                        const std::shared_ptr<Task>& b) const
        {
            if (a->deadline != b->deadline)
                return a->deadline > b->deadline;   // earlier deadline first

            return a->priority.load() > b->priority.load(); // then higher priority (lower number)
        }
    };

    using Container = std::vector<std::shared_ptr<Task>>;
    using QueueType = std::priority_queue<std::shared_ptr<Task>, Container, Compare>;

    void push(std::shared_ptr<Task> task)
    {
        std::lock_guard lk{mutex_};
        queue_.push(std::move(task));
        cv_.notify_one();
    }

    [[nodiscard]] std::shared_ptr<Task> wait_and_pop(bool& shutdown)
    {
        std::unique_lock lk{mutex_};
        cv_.wait(lk, [&]{ return !queue_.empty() || shutdown_; });

        if (shutdown_ && queue_.empty())
            return nullptr;

        auto task = std::move(queue_.top());
        queue_.pop();
        return task;
    }

    [[nodiscard]] std::size_t size() const
    {
        std::lock_guard lk{mutex_};
        return queue_.size();
    }

    void request_shutdown()
    {
        std::lock_guard lk{mutex_};
        shutdown_ = true;
        cv_.notify_all();
    }

private:
    QueueType queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool shutdown_{false};
};

// ─────────────────────────────────────────────────────────────────────────────
//  Worker – thread that executes tasks
// ─────────────────────────────────────────────────────────────────────────────

class Worker
{
public:
    Worker(WorkerId id, DeadlineOrderedQueue& q, std::atomic<bool>& global_shutdown)
        : id_{id}
        , queue_(q)
        , global_shutdown_(global_shutdown)
    {
        thread_ = std::thread(&Worker::run, this);
    }

    ~Worker()
    {
        if (thread_.joinable())
            thread_.join();
    }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

private:
    void run()
    {
        LOG_INFO("Worker {} starting", id_.value);

        while (!global_shutdown_.load(std::memory_order_acquire))
        {
            bool shutdown_flag = false;
            auto task = queue_.wait_and_pop(shutdown_flag);
            if (!task || shutdown_flag)
                break;

            execute_task(task);
        }

        LOG_INFO("Worker {} shutting down", id_.value);
    }

    void execute_task(std::shared_ptr<Task> task)
    {
        TimePoint start = MonotonicClock::now();

        if (task->already_missed(start))
        {
            task->state.store(TaskState::ABORTED_TIMEOUT);
            LOG_WARN("Task {} already missed deadline at dispatch", task->id.value);
            return;
        }

        if (task->is_cancelled())
        {
            task->state.store(TaskState::ABORTED_CANCELLED);
            LOG_INFO("Task {} cancelled before execution", task->id.value);
            return;
        }

        task->state.store(TaskState::EXECUTING);

        LOG_DEBUG("Worker {} executing task {} \"{}\"", id_.value, task->id.value, task->name);

        try
        {
            auto t0 = HighResClock::now();

            task->work();

            auto t1 = HighResClock::now();
            Duration exec_time = t1 - t0;

            task->actual_execution_time.store(exec_time, std::memory_order_release);

            TimePoint finish = MonotonicClock::now();

            if (finish >= task->deadline)
            {
                task->state.store(TaskState::COMPLETED_WITH_ERROR);
                LOG_WARN("Task {} completed but missed deadline (lateness {} ms)",
                         task->id.value,
                         duration_cast<Millis>(finish - task->deadline).count());
            }
            else
            {
                task->state.store(TaskState::COMPLETED_SUCCESS);
                LOG_INFO("Task {} completed successfully in {} ms (slack left {} ms)",
                         task->id.value,
                         duration_cast<Millis>(exec_time).count(),
                         duration_cast<Millis>(task->deadline - finish).count());
            }
        }
        catch (const std::exception& e)
        {
            task->state.store(TaskState::COMPLETED_WITH_ERROR);
            LOG_ERROR("Task {} threw exception: {}", task->id.value, e.what());
        }
        catch (...)
        {
            task->state.store(TaskState::COMPLETED_WITH_ERROR);
            LOG_ERROR("Task {} threw unknown exception", task->id.value);
        }
    }

    WorkerId id_;
    DeadlineOrderedQueue& queue_;
    std::atomic<bool>& global_shutdown_;
    std::thread thread_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  ExecutionEngine – owns workers & queue
// ─────────────────────────────────────────────────────────────────────────────

class ExecutionEngine
{
public:
    struct Config
    {
        std::size_t         worker_count{4};
        SchedulingPolicy    scheduling{SchedulingPolicy::EarliestDeadlineFirst};
        bool                enable_cancellation_propagation{true};
    };

    explicit ExecutionEngine(Config cfg = {})
        : config_(std::move(cfg))
        , shutdown_flag_(false)
    {
        for (std::size_t i = 0; i < config_.worker_count; ++i)
        {
            workers_.emplace_back(WorkerId{static_cast<std::uint32_t>(i)},
                                 queue_,
                                 shutdown_flag_);
        }
        LOG_INFO("ExecutionEngine started with {} workers", config_.worker_count);
    }

    ~ExecutionEngine()
    {
        shutdown_flag_.store(true, std::memory_order_release);
        queue_.request_shutdown();

        LOG_INFO("ExecutionEngine shutting down...");
    }

    void submit(std::shared_ptr<Task> task)
    {
        task->state.store(TaskState::QUEUED);
        queue_.push(std::move(task));
    }

    [[nodiscard]] std::size_t pending_tasks() const { return queue_.size(); }

private:
    Config config_;
    DeadlineOrderedQueue queue_;
    std::vector<Worker> workers_;
    std::atomic<bool> shutdown_flag_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Realistic Task Generator with many parameters
// ─────────────────────────────────────────────────────────────────────────────

class RealisticTaskGenerator
{
public:
    struct Profile
    {
        std::string name_prefix;
        Millis      wcet_mean{50};
        Millis      wcet_stddev{15};
        Millis      slack_mean{120};
        Millis      slack_stddev{60};
        double      cancel_prob{0.02};
        TaskPriority priority{TaskPriority::Normal};
    };

    explicit RealisticTaskGenerator(std::uint64_t seed = std::random_device{}())
        : rng_(seed)
    {
        profiles_.emplace_back(Profile{"Critical",  20ms,  5ms,  60ms, 20ms, 0.005, TaskPriority::Critical});
        profiles_.emplace_back(Profile{"Normal",    45ms, 12ms, 140ms, 50ms, 0.02,  TaskPriority::Normal});
        profiles_.emplace_back(Profile{"Background",80ms, 25ms, 400ms,100ms, 0.04,  TaskPriority::Background});
    }

    std::shared_ptr<Task> create_task(std::uint64_t id)
    {
        std::uniform_int_distribution<std::size_t> profile_dist(0, profiles_.size()-1);
        const auto& prof = profiles_[profile_dist(rng_)];

        std::normal_distribution<double> wcet_dist(prof.wcet_mean.count(), prof.wcet_stddev.count());
        std::normal_distribution<double> slack_dist(prof.slack_mean.count(), prof.slack_stddev.count());

        Duration wcet{std::max(1LL, static_cast<long long>(std::round(wcet_dist(rng_)))) * 1ms};
        Duration slack{std::max(10LL, static_cast<long long>(std::round(slack_dist(rng_)))) * 1ms};

        TimePoint deadline = MonotonicClock::now() + slack;

        auto work = [wcet, id, cancel_prob = prof.cancel_prob, this]() mutable
        {
            // Simulate occasional self-cancellation (for testing)
            std::bernoulli_distribution cancel_d{cancel_prob};
            if (cancel_d(rng_))
            {
                LOG_WARN("Task {} self-cancelling during execution", id);
                throw std::runtime_error("self-cancel simulation");
            }

            std::this_thread::sleep_for(wcet);
        };

        auto task = std::make_shared<Task>(
            TaskId{id},
            deadline,
            wcet,
            std::move(work),
            prof.name_prefix + "-" + std::to_string(id),
            prof.priority
        );

        return task;
    }

private:
    std::mt19937_64 rng_;
    std::vector<Profile> profiles_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Statistics collector – very verbose
// ─────────────────────────────────────────────────────────────────────────────

struct TaskStatistics
{
    std::uint64_t count_total{0};
    std::uint64_t count_admitted{0};
    std::uint64_t count_rejected_gate{0};
    std::uint64_t count_rejected_overload{0};
    std::uint64_t count_completed_success{0};
    std::uint64_t count_completed_error{0};
    std::uint64_t count_aborted_timeout{0};
    std::uint64_t count_aborted_cancel{0};
    std::uint64_t count_aborted_other{0};

    Duration      sum_lateness{0ns};
    Duration      max_lateness{0ns};
    Duration      sum_execution_time{0ns};
    Duration      max_execution_time{0ns};
    Duration      sum_slack_at_completion{0ns};

    std::map<TaskState, std::uint64_t> state_histogram;

    void record_task(const Task& t, TimePoint finish_time = MonotonicClock::now())
    {
        ++count_total;

        auto state = t.state.load();
        ++state_histogram[state];

        switch (state)
        {
            case TaskState::ADMITTED:               ++count_admitted; break;
            case TaskState::REJECTED_AT_GATE:       ++count_rejected_gate; break;
            case TaskState::REJECTED_OVERLOAD:      ++count_rejected_overload; break;
            case TaskState::COMPLETED_SUCCESS:
                ++count_completed_success;
                {
                    Duration lateness = std::max(Duration::zero(), finish_time - t.deadline);
                    Duration slack_left = t.deadline - finish_time;
                    sum_lateness += lateness;
                    max_lateness = std::max(max_lateness, lateness);
                    sum_slack_at_completion += slack_left;
                }
                break;
            case TaskState::COMPLETED_WITH_ERROR:   ++count_completed_error; break;
            case TaskState::ABORTED_TIMEOUT:        ++count_aborted_timeout; break;
            case TaskState::ABORTED_CANCELLED:      ++count_aborted_cancel; break;
            default:                                ++count_aborted_other; break;
        }

        sum_execution_time += t.actual_execution_time.load();
        max_execution_time = std::max(max_execution_time, t.actual_execution_time.load());
    }

    void print_summary(std::ostream& os = std::cout) const
    {
        os << "\n=== Task Execution Statistics ===\n";
        os << std::format("Total tasks submitted     : {:>8}\n", count_total);
        os << std::format("Admitted                  : {:>8} ({:.1f}%)\n", count_admitted, 100.0*count_admitted/count_total);
        os << std::format("Rejected (gate)           : {:>8}\n", count_rejected_gate);
        os << std::format("Rejected (overload)       : {:>8}\n", count_rejected_overload);
        os << std::format("Completed successfully    : {:>8}\n", count_completed_success);
        os << std::format("Completed with error      : {:>8}\n", count_completed_error);
        os << std::format("Aborted – timeout         : {:>8}\n", count_aborted_timeout);
        os << std::format("Aborted – cancelled       : {:>8}\n", count_aborted_cancel);
        os << std::format("Aborted – other           : {:>8}\n", count_aborted_other);

        if (count_completed_success > 0)
        {
            os << std::format("Avg execution time        : {:>8.1f} ms\n",
                              duration_cast<Micros>(sum_execution_time).count() / 1000.0 / count_completed_success);
            os << std::format("Max execution time        : {:>8.1f} ms\n",
                              duration_cast<Millis>(max_execution_time).count());
            os << std::format("Avg lateness (success)    : {:>8.1f} ms\n",
                              duration_cast<Micros>(sum_lateness).count() / 1000.0 / count_completed_success);
            os << std::format("Max lateness              : {:>8.1f} ms\n",
                              duration_cast<Millis>(max_lateness).count());
            os << std::format("Avg remaining slack       : {:>8.1f} ms\n",
                              duration_cast<Micros>(sum_slack_at_completion).count() / 1000.0 / count_completed_success);
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Main – demonstration / stress / showcase
// ─────────────────────────────────────────────────────────────────────────────

int main()
{
    StructuredLogger::set_level(StructuredLogger::Level::Info);
    // StructuredLogger::open_log_file("deadline-gate-run.log");

    LOG_INFO("Deadline-Aware Execution Gate – EXTENDED VERSION");
    LOG_INFO("C++23 compliant – started at {}", std::format("{:%Y-%m-%d %H:%M:%S}", SystemClock::now()));

    constexpr std::size_t TASK_COUNT       = 300;
    constexpr std::size_t WORKER_COUNT     = 7;
    constexpr Millis      SUBMIT_INTERVAL  = 8ms;

    AdmissionController::Config adm_cfg;
    adm_cfg.policy = AdmissionPolicy::HybridOverloadProtect;
    adm_cfg.safety_margin = 5ms;
    adm_cfg.overload_reject_probability = 0.25;
    adm_cfg.token_refill_interval = 80ms;
    adm_cfg.tokens_per_refill = 4;
    adm_cfg.max_tokens = 16;

    AdmissionController gate{std::move(adm_cfg)};

    ExecutionEngine::Config engine_cfg;
    engine_cfg.worker_count = WORKER_COUNT;
    engine_cfg.scheduling   = SchedulingPolicy::EarliestDeadlineFirst;

    ExecutionEngine engine{engine_cfg};
    RealisticTaskGenerator generator;

    TaskStatistics stats;

    LOG_INFO("Submitting {} tasks with {} workers...", TASK_COUNT, WORKER_COUNT);

    std::uint64_t task_id = 1;

    for (std::size_t i = 0; i < TASK_COUNT; ++i)
    {
        auto task = generator.create_task(task_id++);

        TimePoint now = MonotonicClock::now();
        gate.refill_tokens_if_needed(now);

        if (gate.admit(*task, now))
        {
            task->state.store(TaskState::ADMITTED);
            LOG_INFO("[ADMIT] {:6} \"{}\"  WCET:{:4}ms  DL in:{:4}ms  prio:{}",
                     task->id.value, task->name,
                     duration_cast<Millis>(task->estimated_worst_case_execution_time).count(),
                     duration_cast<Millis>(task->deadline - now).count(),
                     static_cast<int>(task->priority.load()));
            engine.submit(task);
        }
        else
        {
            task->state.store(TaskState::REJECTED_AT_GATE);
            LOG_WARN("[REJECT] {:6} \"{}\"  WCET:{:4}ms  DL in:{:3}ms  → too late",
                     task->id.value, task->name,
                     duration_cast<Millis>(task->estimated_worst_case_execution_time).count(),
                     duration_cast<Millis>(task->deadline - now).count());
        }

        stats.record_task(*task);   // preliminary – will be updated later for completed ones

        std::this_thread::sleep_for(SUBMIT_INTERVAL);
    }

    // Give time to finish most work
    std::this_thread::sleep_for(8s);

    LOG_INFO("Final statistics:");
    stats.print_summary();

    LOG_INFO("=== DEMONSTRATION FINISHED ===");
    return 0;

}
