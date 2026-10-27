#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <random>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <thread>

// Mocking the 'vector-db-engine' core interfaces for standalone compilation and execution.
// In a real integration, include the actual headers from vector-db-engine.
namespace vector_db {
    using VectorId = uint64_t;
    using Dimension = size_t;

    struct Vector {
        Dimension dim;
        std::vector<float> data;
    };

    enum class MetricType {
        COSINE,
        L2,
        IP
    };

    struct IndexConfig {
        Dimension dimension;
        MetricType metric;
        size_t max_elements;
    };

    class VectorIndex {
    public:
        explicit VectorIndex(const IndexConfig& config) : config_(config) {}

        void insert(VectorId id, const Vector& vec) {
            // Simulated insert workload
            std::this_thread::yield();
            storage_[id] = vec;
        }

        std::vector<std::pair<VectorId, float>> search(const Vector& query, size_t k) {
            // Simulated approximate/exact nearest neighbor search workload
            std::vector<std::pair<VectorId, float>> results;
            size_t count = 0;
            for (const auto& pair : storage_) {
                if (count++ >= k) break;
                results.emplace_back(pair.first, 0.95f);
            }
            return results;
        }

    private:
        IndexConfig config_;
        std::vector<std::pair<VectorId, Vector>> storage_;
    };
}

class LatencyTimer {
    std::chrono::high_resolution_clock::time_point start_;
public:
    LatencyTimer() : start_(std::chrono::high_resolution_clock::now()) {}

    double elapsed_nanoseconds() const {
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::nano>(end - start_).count();
    }
};

void run_benchmark() {
    constexpr size_t VECTOR_DIM = 128;
    constexpr size_t WARMUP_ITERATIONS = 1000;
    constexpr size_t BENCHMARK_ITERATIONS = 50000;
    constexpr size_t TOP_K = 10;

    vector_db::IndexConfig config{
        VECTOR_DIM,
        vector_db::MetricType::COSINE,
        BENCHMARK_ITERATIONS * 2
    };

    vector_db::VectorIndex index(config);

    // Seed and generate deterministic random vectors
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    auto generate_vector = [&]() {
        vector_db::Vector v;
        v.dim = VECTOR_DIM;
        v.data.resize(VECTOR_DIM);
        for (size_t i = 0; i < VECTOR_DIM; ++i) {
            v.data[i] = dist(rng);
        }
        return v;
    };

    std::cout << "[*] Initializing 'vector-db-engine' microbenchmark...\n";
    std::cout << "[*] Vector Dimension: " << VECTOR_DIM << "\n";
    std::cout << "[*] Warmup Iterations: " << WARMUP_ITERATIONS << "\n";
    std::cout << "[*] Benchmark Iterations: " << BENCHMARK_ITERATIONS << "\n\n";

    // Pre-populate index for search phase
    for (size_t i = 0; i < WARMUP_ITERATIONS; ++i) {
        index.insert(i, generate_vector());
    }

    // --- WARMUP PHASE ---
    for (size_t i = 0; i < WARMUP_ITERATIONS; ++i) {
        auto q = generate_vector();
        benchmark::DoNotOptimize(index.search(q, TOP_K));
    }

    // --- BENCHMARK PHASE: SEARCH LATENCY & THROUGHPUT ---
    std::vector<double> latencies_ns;
    latencies_ns.reserve(BENCHMARK_ITERATIONS);

    auto total_start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < BENCHMARK_ITERATIONS; ++i) {
        auto query = generate_vector();
        
        LatencyTimer timer;
        auto results = index.search(query, TOP_K);
        double elapsed = timer.elapsed_nanoseconds();

        // Prevent dead-code elimination
        if (results.empty() && VECTOR_DIM == 0) {
            std::cerr << "Unreachable\n";
        }

        latencies_ns.push_back(elapsed);
    }

    auto total_end = std::chrono::high_resolution_clock::now();
    double total_duration_sec = std::chrono::duration<double>(total_end - total_start).count();

    // --- METRICS COMPUTATION ---
    std::sort(latencies_ns.begin(), latencies_ns.end());

    double ops_sec = static_cast<double>(BENCHMARK_ITERATIONS) / total_duration_sec;
    double p50 = latencies_ns[BENCHMARK_ITERATIONS * 0.50] / 1000.0; // convert to microseconds
    double p90 = latencies_ns[BENCHMARK_ITERATIONS * 0.90] / 1000.0;
    double p99 = latencies_ns[BENCHMARK_ITERATIONS * 0.99] / 1000.0;
    double p999 = latencies_ns[static_cast<size_t>(BENCHMARK_ITERATIONS * 0.999)] / 1000.0;
    double min_lat = latencies_ns.front() / 1000.0;
    double max_lat = latencies_ns.back() / 1000.0;

    // --- FORMATTED TABULAR OUTPUT ---
    const char separator = '-';
    const int width = 75;

    std::cout << std::string(width, separator) << "\n";
    std::cout << std::left << std::setw(30) << "Workload Metric" 
              << std::right << std::setw(20) << "Value" 
              << std::setw(25) << "Unit" << "\n";
    std::cout << std::string(width, separator) << "\n";

    std::cout << std::left << std::setw(30) << "Total Operations" 
              << std::right << std::setw(20) << BENCHMARK_ITERATIONS 
              << std::setw(25) << "ops" << "\n";

    std::cout << std::left << std::setw(30) << "Total Duration" 
              << std::right << std::setw(20) << std::fixed << std::setprecision(4) << total_duration_sec 
              << std::setw(25) << "seconds" << "\n";

    std::cout << std::left << std::setw(30) << "Throughput" 
              << std::right << std::setw(20) << std::fixed << std::setprecision(2) << ops_sec 
              << std::setw(25) << "ops/sec" << "\n";

    std::cout << std::left << std::setw(30) << "Latency Min" 
              << std::right << std::setw(20) << std::fixed << std::setprecision(3) << min_lat 
              << std::setw(25) << "microseconds" << "\n";

    std::cout << std::left << std::setw(30) << "Latency P50 (Median)" 
              << std::right << std::setw(20) << std::fixed << std::setprecision(3) << p50 
              << std::setw(25) << "microseconds" << "\n";

    std::cout << std::left << std::setw(30) << "Latency P90" 
              << std::right << std::setw(20) << std::fixed << std::setprecision(3) << p90 
              << std::setw(25) << "microseconds" << "\n";

    std::cout << std::left << std::setw(30) << "Latency P99" 
              << std::right << std::setw(20) << std::fixed << std::setprecision(3) << p99 
              << std::setw(25) << "microseconds" << "\n";

    std::cout << std::left << std::setw(30) << "Latency P99.9" 
              << std::right << std::setw(20) << std::fixed << std::setprecision(3) << p999 
              << std::setw(25) << "microseconds" << "\n";

    std::cout << std::left << std::setw(30) << "Latency Max" 
              << std::right << std::setw(20) << std::fixed << std::setprecision(3) << max_lat 
              << std::setw(25) << "microseconds" << "\n";

    std::cout << std::string(width, separator) << "\n";
}

int main() {
    try {
        run_benchmark();
    } catch (const std::exception& e) {
        std::cerr << "Benchmark failed with exception: " << e.what() << "\n";
        return 1;
    }
    return 0;
}