// ============================================================================
// PROJECT: TITAN_GATE (Extended Backpressure Engine)
// AUTHOR:  Rohan Kapri (Optimized Version)
// DATE:    2026-01-20
// STD:     C++23
// ============================================================================
//
// DESCRIPTION:
// A high-performance, multi-threaded task ingestion engine with strict
// backpressure, priority scheduling, and detailed telemetry.
//
// ARCHITECTURE:
// [Producers] -> [Gate (Circuit Breaker)] -> [Priority Router] -> [Worker Pool]
//                                                  |
//                                            [Telemetry DB]
//
// KEY FEATURES:
// 1. Lock-Free/Fine-Grained Locking queues for 4 priority levels.
// 2. Thread-safe Asynchronous Logger (Double Buffering).
// 3. Latency Histogram (P50, P99 calculation).
// 4. Circuit Breaker pattern for overload protection.
// 5. Zero-allocation hot paths where possible.
// ============================================================================

#include <algorithm>
#include <atomic>
#include <array>
#include <barrier>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <format>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <semaphore>
#include <source_location>
#include <stop_token>
#include <string>
#include <string_view>
#include <syncstream>
#include <thread>
#include <vector>
#include <variant>

// ============================================================================
// SECTION 1: CORE UTILITIES & TYPES
// ============================================================================

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;
using Microseconds = std::chrono::microseconds;
using Milliseconds = std::chrono::milliseconds;

// --- ID Generator ---
struct SnowflakeId {
    static uint64_t generate() {
        static std::atomic<uint64_t> seq{0};
        // Simple composition: Timestamp (44 bits) | Sequence (20 bits)
        uint64_t ts = std::chrono::duration_cast<Milliseconds>(
            Clock::now().time_since_epoch()).count();
        return (ts << 20) | (seq.fetch_add(1, std::memory_order_relaxed) & 0xFFFFF);
    }
};

// --- Time Helper ---
static inline uint64_t now_ns() {
    return std::chrono::duration_cast<Nanoseconds>(
        Clock::now().time_since_epoch()
    ).count();
}

// --- Spinlock (for ultra-low latency critical sections) ---
class SpinLock {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
public:
    void lock() noexcept {
        while (flag.test_and_set(std::memory_order_acquire)) {
            while (flag.test(std::memory_order_relaxed)) {
                std::this_thread::yield(); 
            }
        }
    }
    void unlock() noexcept {
        flag.clear(std::memory_order_release);
    }
};

// ============================================================================
// SECTION 2: ASYNCHRONOUS LOGGER
// ============================================================================
// Decouples formatting/IO from the hot processing path.

enum class LogLevel { DEBUG, INFO, WARN, ERROR, FATAL };

struct LogMessage {
    LogLevel level;
    uint64_t timestamp;
    std::string message;
    std::string thread_id;
};

class AsyncLogger {
public:
    static AsyncLogger& instance() {
        static AsyncLogger logger;
        return logger;
    }

    void log(LogLevel level, std::string_view msg) {
        if (!running_.load(std::memory_order_relaxed)) return;

        // Fast formatting on caller thread, enqueue string
        std::stringstream ss;
        ss << std::this_thread::get_id();
        
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            log_queue_.push_back({
                level, 
                now_ns(), 
                std::string(msg), 
                ss.str()
            });
        }
        cv_.notify_one();
    }

    void shutdown() {
        running_.store(false);
        cv_.notify_all();
        if (writer_thread_.joinable()) writer_thread_.join();
    }

private:
    AsyncLogger() : running_(true) {
        writer_thread_ = std::thread(&AsyncLogger::process_logs, this);
    }

    ~AsyncLogger() { if (running_) shutdown(); }

    void process_logs() {
        std::vector<LogMessage> local_buffer;
        local_buffer.reserve(100);

        while (true) {
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                cv_.wait(lock, [this] { 
                    return !log_queue_.empty() || !running_.load(); 
                });

                if (log_queue_.empty() && !running_.load()) break;

                // Swap queues to minimize lock time
                local_buffer.insert(local_buffer.end(), 
                                  log_queue_.begin(), log_queue_.end());
                log_queue_.clear();
            }

            for (const auto& log : local_buffer) {
                print_log(log);
            }
            local_buffer.clear();
        }
    }

    void print_log(const LogMessage& log) {
        char level_char = 'I';
        switch(log.level) {
            case LogLevel::DEBUG: level_char = 'D'; break;
            case LogLevel::INFO:  level_char = 'I'; break;
            case LogLevel::WARN:  level_char = 'W'; break;
            case LogLevel::ERROR: level_char = 'E'; break;
            case LogLevel::FATAL: level_char = 'F'; break;
        }

        // Using syncstream to prevent tearing if cout is used elsewhere
        std::osyncstream(std::cout) 
            << "[" << level_char << "] "
            << "[" << log.timestamp << "] "
            << "[TID:" << log.thread_id << "] "
            << log.message << "\n";
    }

    std::atomic<bool> running_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::deque<LogMessage> log_queue_;
    std::thread writer_thread_;
};

// Helper macro for logging
#define LOG_INFO(msg) AsyncLogger::instance().log(LogLevel::INFO, msg)
#define LOG_WARN(msg) AsyncLogger::instance().log(LogLevel::WARN, msg)
#define LOG_ERR(msg)  AsyncLogger::instance().log(LogLevel::ERROR, msg)

// ============================================================================
// SECTION 3: METRICS & TELEMETRY
// ============================================================================

class Histogram {
public:
    Histogram(uint64_t min_val, uint64_t max_val, size_t buckets)
        : min_val_(min_val), max_val_(max_val), bucket_count_(buckets) {
        buckets_.resize(bucket_count_, 0);
        step_ = (max_val - min_val) / bucket_count_;
        if (step_ == 0) step_ = 1;
    }

    void record(uint64_t value) {
        if (value < min_val_) value = min_val_;
        if (value > max_val_) value = max_val_;
        size_t idx = (value - min_val_) / step_;
        if (idx >= bucket_count_) idx = bucket_count_ - 1;
        
        // Loose atomicity for speed
        std::atomic_ref<uint64_t>(buckets_[idx]).fetch_add(1, std::memory_order_relaxed);
        total_count_.fetch_add(1, std::memory_order_relaxed);
        sum_.fetch_add(value, std::memory_order_relaxed);
    }

    uint64_t get_percentile(double p) const {
        uint64_t count = total_count_.load();
        if (count == 0) return 0;
        
        uint64_t target = static_cast<uint64_t>(count * p);
        uint64_t current = 0;
        
        for (size_t i = 0; i < bucket_count_; ++i) {
            current += buckets_[i];
            if (current >= target) {
                return min_val_ + (i * step_);
            }
        }
        return max_val_;
    }

    uint64_t get_mean() const {
        uint64_t count = total_count_.load();
        return count == 0 ? 0 : sum_.load() / count;
    }

private:
    uint64_t min_val_;
    uint64_t max_val_;
    size_t bucket_count_;
    uint64_t step_;
    mutable std::vector<uint64_t> buckets_;
    std::atomic<uint64_t> total_count_{0};
    std::atomic<uint64_t> sum_{0};
};

struct SystemMetrics {
    std::atomic<uint64_t> tasks_submitted{0};
    std::atomic<uint64_t> tasks_rejected_queue_full{0};
    std::atomic<uint64_t> tasks_rejected_circuit_open{0};
    std::atomic<uint64_t> tasks_processed{0};
    std::atomic<uint64_t> tasks_failed{0};
    
    // Latency histogram for end-to-end time (0 to 100ms)
    Histogram processing_latency_us{0, 100000, 100}; 
    
    // Queue depth tracking
    std::atomic<size_t> current_queue_depth{0};
};

// ============================================================================
// SECTION 4: DOMAIN OBJECTS & WORK ITEMS
// ============================================================================

enum class Priority : uint8_t {
    CRITICAL = 0,
    HIGH     = 1,
    NORMAL   = 2,
    LOW      = 3,
    COUNT    = 4
};

enum class TaskType {
    CPU_INTENSIVE,
    IO_BOUND,
    ADMINISTRATIVE
};

struct TaskPayload {
    TaskType type;
    uint32_t complexity_score; // 0-1000
    std::string metadata;
};

struct WorkItem {
    uint64_t id;
    Priority priority;
    uint64_t created_at_ns;
    TaskPayload payload;
    uint32_t producer_id;

    // Helper to calculate age
    uint64_t age_us() const {
        return (now_ns() - created_at_ns) / 1000;
    }
};

// ============================================================================
// SECTION 5: MULTI-LEVEL PRIORITY QUEUE
// ============================================================================

class PriorityRouter {
public:
    explicit PriorityRouter(size_t base_capacity) {
        // Configure capacities based on priority logic
        // Critical queue is smaller but higher priority
        queues_[0] = std::make_unique<BoundedQueue>(base_capacity / 4); // Critical
        queues_[1] = std::make_unique<BoundedQueue>(base_capacity / 2); // High
        queues_[2] = std::make_unique<BoundedQueue>(base_capacity);     // Normal
        queues_[3] = std::make_unique<BoundedQueue>(base_capacity * 2); // Low
    }

    // Returns true if enqueued, false if full
    bool try_push(WorkItem&& item) {
        int prio_idx = static_cast<int>(item.priority);
        bool success = queues_[prio_idx]->try_push(std::move(item));
        
        if (success) {
             // Signal that work is available
             total_items_.fetch_add(1, std::memory_order_release);
             return true;
        }
        return false;
    }

    // Try to pop the highest priority item available
    std::optional<WorkItem> try_pop() {
        if (total_items_.load(std::memory_order_acquire) == 0) {
            return std::nullopt;
        }

        // Strict Priority Scheduling: Check 0, then 1, then 2, then 3
        for (int i = 0; i < static_cast<int>(Priority::COUNT); ++i) {
            auto item = queues_[i]->pop();
            if (item.has_value()) {
                total_items_.fetch_sub(1, std::memory_order_release);
                return item;
            }
        }
        return std::nullopt;
    }

    size_t total_size() const {
        return total_items_.load(std::memory_order_relaxed);
    }
    
    size_t size_at_priority(Priority p) const {
        return queues_[static_cast<int>(p)]->size();
    }

private:
    // --- Internal Bounded Queue Class ---
    // (Nested to ensure it's only used by Router)
    class BoundedQueue {
    public:
        explicit BoundedQueue(size_t cap) 
            : capacity_(cap), head_(0), tail_(0), size_(0) {
            buffer_.resize(cap);
        }

        bool try_push(WorkItem&& item) {
            std::lock_guard<SpinLock> lock(lock_);
            if (size_ >= capacity_) return false;
            
            buffer_[tail_] = std::move(item);
            tail_ = (tail_ + 1) % capacity_;
            size_++;
            return true;
        }

        std::optional<WorkItem> pop() {
            // Optimistic check before lock
            if (size_ == 0) return std::nullopt; 

            std::lock_guard<SpinLock> lock(lock_);
            if (size_ == 0) return std::nullopt;

            WorkItem item = std::move(buffer_[head_]);
            head_ = (head_ + 1) % capacity_;
            size_--;
            return item;
        }

        size_t size() const { return size_; }

    private:
        size_t capacity_;
        std::vector<WorkItem> buffer_;
        size_t head_;
        size_t tail_;
        size_t size_;
        SpinLock lock_;
    };

    std::array<std::unique_ptr<BoundedQueue>, 4> queues_;
    std::atomic<size_t> total_items_{0};
};

// ============================================================================
// SECTION 6: CIRCUIT BREAKER
// ============================================================================

class CircuitBreaker {
public:
    enum class State { CLOSED, OPEN, HALF_OPEN };

    CircuitBreaker(double failure_threshold, uint64_t reset_timeout_ms)
        : failure_threshold_(failure_threshold), 
          reset_timeout_ms_(reset_timeout_ms),
          state_(State::CLOSED),
          failures_(0),
          total_(0) {}

    bool allow_request() {
        State current = state_.load(std::memory_order_acquire);
        
        if (current == State::OPEN) {
            uint64_t now = now_ns();
            if (now > next_try_timestamp_.load()) {
                // Time to try Half-Open
                if (state_.compare_exchange_strong(current, State::HALF_OPEN)) {
                    LOG_WARN("Circuit Breaker entering HALF_OPEN state");
                    return true; // Allow one probe request
                }
            }
            return false;
        }
        return true;
    }

    void record_result(bool success) {
        if (state_.load() == State::OPEN) return;

        if (state_.load() == State::HALF_OPEN) {
            if (success) {
                reset();
                LOG_INFO("Circuit Breaker CLOSED (Recovered)");
            } else {
                trip(); // Back to open
            }
            return;
        }

        // CLOSED State logic
        total_.fetch_add(1, std::memory_order_relaxed);
        if (!success) {
            failures_.fetch_add(1, std::memory_order_relaxed);
        }

        // Check threshold periodically (e.g., every 100 requests) to avoid math overhead
        if (total_.load() > 100) {
            double rate = static_cast<double>(failures_.load()) / total_.load();
            if (rate > failure_threshold_) {
                trip();
            } else {
                // Decay metrics
                failures_.store(0);
                total_.store(0);
            }
        }
    }

private:
    void trip() {
        state_.store(State::OPEN, std::memory_order_release);
        next_try_timestamp_.store(now_ns() + (reset_timeout_ms_ * 1'000'000));
        LOG_ERR("Circuit Breaker TRIPPED to OPEN state due to high failure rate");
    }

    void reset() {
        state_.store(State::CLOSED, std::memory_order_release);
        failures_.store(0);
        total_.store(0);
    }

    double failure_threshold_;
    uint64_t reset_timeout_ms_;
    std::atomic<State> state_;
    std::atomic<uint64_t> next_try_timestamp_{0};
    
    std::atomic<uint64_t> failures_;
    std::atomic<uint64_t> total_;
};

// ============================================================================
// SECTION 7: MAIN PROCESSING ENGINE
// ============================================================================

class TitanEngine {
public:
    struct Config {
        size_t queue_capacity = 1024;
        size_t num_workers = 4;
        double circuit_failure_rate = 0.5; // 50% failure trips breaker
    };

    explicit TitanEngine(Config config)
        : config_(config),
          router_(config.queue_capacity),
          circuit_breaker_(config.circuit_failure_rate, 2000), // 2s reset
          running_(true) {
        
        LOG_INFO(std::format("Initializing TitanEngine with {} workers", config.num_workers));
        start_workers();
    }

    ~TitanEngine() {
        stop();
    }

    // The main entry point for producers
    bool submit(WorkItem item) {
        if (!running_.load()) return false;

        // 1. Check Circuit Breaker
        if (!circuit_breaker_.allow_request()) {
            metrics_.tasks_rejected_circuit_open.fetch_add(1);
            return false; 
        }

        // 2. Try Enqueue
        bool accepted = router_.try_push(std::move(item));
        
        if (accepted) {
            metrics_.tasks_submitted.fetch_add(1, std::memory_order_relaxed);
            metrics_.current_queue_depth.store(router_.total_size(), std::memory_order_relaxed);
            work_available_cv_.notify_one();
        } else {
            metrics_.tasks_rejected_queue_full.fetch_add(1, std::memory_order_relaxed);
        }

        return accepted;
    }

    void stop() {
        bool expected = true;
        if (running_.compare_exchange_strong(expected, false)) {
            LOG_INFO("Stopping TitanEngine...");
            work_available_cv_.notify_all();
            for (auto& t : workers_) {
                if (t.joinable()) t.join();
            }
            LOG_INFO("TitanEngine Stopped.");
        }
    }

    const SystemMetrics& get_metrics() const { return metrics_; }

private:
    void start_workers() {
        workers_.reserve(config_.num_workers);
        for (size_t i = 0; i < config_.num_workers; ++i) {
            workers_.emplace_back(&TitanEngine::worker_loop, this, i);
        }
    }

    void worker_loop(size_t worker_id) {
        LOG_INFO(std::format("Worker {} started", worker_id));
        
        while (running_.load() || router_.total_size() > 0) {
            std::unique_lock<std::mutex> lock(cv_mutex_);
            work_available_cv_.wait(lock, [this] {
                return !running_.load() || router_.total_size() > 0;
            });

            if (!running_.load() && router_.total_size() == 0) break;

            // Unlock to allow other workers to wake up
            lock.unlock();

            // Try to fetch work
            auto item_opt = router_.try_pop();
            if (item_opt.has_value()) {
                WorkItem item = std::move(item_opt.value());
                process_item(worker_id, item);
                metrics_.current_queue_depth.store(router_.total_size(), std::memory_order_relaxed);
            }
        }
        LOG_INFO(std::format("Worker {} exiting", worker_id));
    }

    void process_item(size_t worker_id, const WorkItem& item) {
        auto start = now_ns();

        // Simulate work based on payload type
        bool success = true;
        try {
            switch (item.payload.type) {
                case TaskType::CPU_INTENSIVE:
                    // Matrix multiplication simulation
                    simulate_cpu_load(item.payload.complexity_score);
                    break;
                case TaskType::IO_BOUND:
                    // Network wait simulation
                    std::this_thread::sleep_for(std::chrono::microseconds(100 * item.payload.complexity_score));
                    break;
                case TaskType::ADMINISTRATIVE:
                    // Fast path
                    break;
            }
        } catch (...) {
            success = false;
        }

        auto end = now_ns();
        uint64_t latency_us = (end - item.created_at_ns) / 1000;

        // Update Stats
        circuit_breaker_.record_result(success);
        
        if (success) {
            metrics_.tasks_processed.fetch_add(1, std::memory_order_relaxed);
            metrics_.processing_latency_us.record(latency_us);
        } else {
            metrics_.tasks_failed.fetch_add(1, std::memory_order_relaxed);
        }

        // Trace logging for Critical items only to reduce noise
        if (item.priority == Priority::CRITICAL) {
            // Uncomment for verbose debugging
            // LOG_INFO(std::format("Worker {} finished CRITICAL item {}", worker_id, item.id));
        }
    }

    void simulate_cpu_load(uint32_t difficulty) {
        // Volatile to prevent compiler optimization
        volatile double result = 0;
        for (uint32_t i = 0; i < difficulty * 100; ++i) {
            result += std::sin(i) * std::cos(i);
        }
    }

    Config config_;
    PriorityRouter router_;
    CircuitBreaker circuit_breaker_;
    SystemMetrics metrics_;

    std::atomic<bool> running_;
    std::vector<std::jthread> workers_;
    std::mutex cv_mutex_;
    std::condition_variable work_available_cv_;
};

// ============================================================================
// SECTION 8: PRODUCER SIMULATION
// ============================================================================

class ProducerGroup {
public:
    ProducerGroup(TitanEngine& engine, size_t count, std::string name)
        : engine_(engine), count_(count), name_(std::move(name)) {}

    void start(uint64_t duration_ms) {
        for (size_t i = 0; i < count_; ++i) {
            threads_.emplace_back(&ProducerGroup::run, this, i, duration_ms);
        }
    }

    void wait() {
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
    }

private:
    void run(size_t id, uint64_t duration_ms) {
        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<uint32_t> prio_dist(0, 3);
        std::uniform_int_distribution<uint32_t> type_dist(0, 2);
        std::uniform_int_distribution<uint32_t> complexity_dist(10, 500);
        
        // Burstiness parameters
        std::exponential_distribution<double> sleep_dist(1.0 / 500.0); // Avg 500us

        auto end_time = Clock::now() + std::chrono::milliseconds(duration_ms);

        while (Clock::now() < end_time) {
            WorkItem item;
            item.id = SnowflakeId::generate();
            item.created_at_ns = now_ns();
            item.producer_id = static_cast<uint32_t>(id);
            
            // Assign priority (weighted: fewer criticals)
            int p_roll = prio_dist(rng);
            if (p_roll == 0) item.priority = Priority::CRITICAL; // 25% chance
            else if (p_roll == 1) item.priority = Priority::HIGH;
            else item.priority = Priority::NORMAL; // Skew towards Normal

            // Payload
            item.payload.type = static_cast<TaskType>(type_dist(rng));
            item.payload.complexity_score = complexity_dist(rng);
            item.payload.metadata = "Simulated Request";

            // Submit to engine
            if (!engine_.submit(std::move(item))) {
                // If rejected, maybe backoff slightly?
                std::this_thread::yield();
            }

            // Sleep random amount
            uint64_t sleep_us = static_cast<uint64_t>(sleep_dist(rng));
            std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
        }
    }

    TitanEngine& engine_;
    size_t count_;
    std::string name_;
    std::vector<std::jthread> threads_;
};

// ============================================================================
// SECTION 9: REPORTING & MAIN
// ============================================================================

void print_final_report(const TitanEngine& engine, double duration_s) {
    const auto& m = engine.get_metrics();
    
    uint64_t total = m.tasks_submitted.load();
    uint64_t processed = m.tasks_processed.load();
    uint64_t q_rej = m.tasks_rejected_queue_full.load();
    uint64_t c_rej = m.tasks_rejected_circuit_open.load();
    
    std::cout << "\n";
    std::cout << "========================================================\n";
    std::cout << "              TITAN ENGINE FINAL REPORT                 \n";
    std::cout << "========================================================\n";
    std::cout << std::fixed << std::setprecision(2);
    
    std::cout << "Runtime:            " << duration_s << " s\n";
    std::cout << "Throughput:         " << (processed / duration_s) << " ops/sec\n";
    std::cout << "\n--- Volume ---\n";
    std::cout << "Total Submitted:    " << total << "\n";
    std::cout << "Processed Success:  " << processed << "\n";
    std::cout << "Failures (Internal):" << m.tasks_failed.load() << "\n";
    
    std::cout << "\n--- Rejection (Backpressure) ---\n";
    std::cout << "Queue Full Rejects: " << q_rej << " (" 
              << (total ? (100.0 * q_rej / total) : 0.0) << "%)\n";
    std::cout << "Circuit Breaks:     " << c_rej << "\n";
    
    std::cout << "\n--- Latency (us) ---\n";
    std::cout << "Mean Latency:       " << m.processing_latency_us.get_mean() << " us\n";
    std::cout << "P50  Latency:       " << m.processing_latency_us.get_percentile(0.50) << " us\n";
    std::cout << "P90  Latency:       " << m.processing_latency_us.get_percentile(0.90) << " us\n";
    std::cout << "P99  Latency:       " << m.processing_latency_us.get_percentile(0.99) << " us\n";
    std::cout << "========================================================\n";
}

int main() {
    // Configure System
    TitanEngine::Config config;
    config.queue_capacity = 2000;
    config.num_workers = std::thread::hardware_concurrency(); 
    config.circuit_failure_rate = 0.2; // Strict breaker

    std::cout << "Starting TITAN GATE System...\n";
    std::cout << "Workers: " << config.num_workers << "\n";
    std::cout << "Buffer:  " << config.queue_capacity << "\n";

    TitanEngine engine(config);

    // Create Producers
    // Group 1: High Frequency web requests
    ProducerGroup web_producers(engine, 4, "WebFrontend");
    
    // Group 2: Heavy Batch jobs (lower frequency, high cost)
    ProducerGroup batch_producers(engine, 2, "BatchBackend");

    auto start_time = Clock::now();

    // Start Simulation
    LOG_INFO("Starting Producers...");
    web_producers.start(5000);   // Run for 5 seconds
    batch_producers.start(5000); // Run for 5 seconds

    // Monitor Loop (runs on main thread)
    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto depth = engine.get_metrics().current_queue_depth.load();
        auto processed = engine.get_metrics().tasks_processed.load();
        std::cout << "[Monitor] Queue Depth: " << depth 
                  << " | Processed: " << processed << "\n";
    }

    // Join Producers
    web_producers.wait();
    batch_producers.wait();
    
    auto end_time = Clock::now();
    std::chrono::duration<double> diff = end_time - start_time;

    // Graceful Shutdown
    engine.stop();
    
    // Wait for logger to flush
    AsyncLogger::instance().shutdown();

    print_final_report(engine, diff.count());

    return 0;
}