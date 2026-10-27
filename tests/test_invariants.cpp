#include <iostream>
#include <vector>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <memory>
#include <shared_mutex>
#include <mutex>
#include <thread>
#include <atomic>
#include <random>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cassert>
#include <cstring>
#include <chrono>
#include <filesystem>

// ============================================================================
// VECTOR DATABASE ENGINE IMPLEMENTATION
// ============================================================================

namespace VectorDB {

using id_t = uint64_t;
using dimension_t = size_t;
using vector_t = std::vector<float>;

enum class MetricType {
    EUCLIDEAN,
    COSINE,
    DOT_PRODUCT
};

struct VectorRecord {
    id_t id;
    vector_t data;
    std::string metadata;
};

struct QueryResult {
    id_t id;
    float distance;
    std::string metadata;

    bool operator>(const QueryResult& other) const {
        return distance > other.distance;
    }
};

class DistanceMetric {
public:
    static float compute(const vector_t& a, const vector_t& b, MetricType metric) {
        assert(a.size() == b.size());
        switch (metric) {
            case MetricType::EUCLIDEAN: {
                float sum = 0.0f;
                for (size_t i = 0; i < a.size(); ++i) {
                    float diff = a[i] - b[i];
                    sum += diff * diff;
                }
                return std::sqrt(sum);
            }
            case MetricType::COSINE: {
                float dot = 0.0f, normA = 0.0f, normB = 0.0f;
                for (size_t i = 0; i < a.size(); ++i) {
                    dot += a[i] * b[i];
                    normA += a[i] * a[i];
                    normB += b[i] * b[i];
                }
                if (normA == 0.0f || normB == 0.0f) return 1.0f; // Max distance for orthogonal/zero
                float sim = dot / (std::sqrt(normA) * std::sqrt(normB));
                return 1.0f - sim; // Convert similarity to distance
            }
            case MetricType::DOT_PRODUCT: {
                float dot = 0.0f;
                for (size_t i = 0; i < a.size(); ++i) {
                    dot += a[i] * b[i];
                }
                return -dot; // Maximize dot product by minimizing negative dot product
            }
        }
        return 0.0f;
    }
};

class Index {
public:
    virtual ~Index() = default;
    virtual void insert(id_t id, const vector_t& vec) = 0;
    virtual void remove(id_t id) = 0;
    virtual std::vector<QueryResult> search(const vector_t& query, size_t k) const = 0;
    virtual size_t size() const = 0;
    virtual void clear() = 0;
};

class FlatIndex : public Index {
private:
    dimension_t dim_;
    MetricType metric_;
    std::unordered_map<id_t, vector_t> vectors_;
    mutable std::shared_mutex mutex_;

public:
    FlatIndex(dimension_t dim, MetricType metric) : dim_(dim), metric_(metric) {}

    void insert(id_t id, const vector_t& vec) override {
        assert(vec.size() == dim_);
        std::unique_lock lock(mutex_);
        vectors_[id] = vec;
    }

    void remove(id_t id) override {
        std::unique_lock lock(mutex_);
        vectors_.erase(id);
    }

    std::vector<QueryResult> search(const vector_t& query, size_t k) const override {
        assert(query.size() == dim_);
        std::shared_lock lock(mutex_);
        
        std::priority_queue<QueryResult, std::vector<QueryResult>, std::less<QueryResult>> pq;

        for (const auto& [id, vec] : vectors_) {
            float dist = DistanceMetric::compute(query, vec, metric_);
            if (pq.size() < k) {
                pq.push({id, dist, ""});
            } else if (dist < pq.top().distance) {
                pq.pop();
                pq.push({id, dist, ""});
            }
        }

        std::vector<QueryResult> results;
        results.reserve(pq.size());
        while (!pq.empty()) {
            results.push_back(pq.top());
            pq.pop();
        }
        std::reverse(results.begin(), results.end());
        return results;
    }

    size_t size() const override {
        std::shared_lock lock(mutex_);
        return vectors_.size();
    }

    void clear() override {
        std::unique_lock lock(mutex_);
        vectors_.clear();
    }
};

class Collection {
private:
    std::string name_;
    dimension_t dim_;
    MetricType metric_;
    std::unique_ptr<Index> index_;
    std::unordered_map<id_t, std::string> metadata_store_;
    mutable std::shared_mutex mutex_;

public:
    Collection(std::string name, dimension_t dim, MetricType metric)
        : name_(std::move(name)), dim_(dim), metric_(metric),
          index_(std::make_unique<FlatIndex>(dim, metric)) {}

    const std::string& name() const { return name_; }
    dimension_t dimension() const { return dim_; }
    MetricType metric() const { return metric_; }

    void upsert(id_t id, const vector_t& vec, const std::string& metadata = "") {
        assert(vec.size() == dim_);
        {
            std::unique_lock lock(mutex_);
            metadata_store_[id] = metadata;
        }
        index_->insert(id, vec);
    }

    void remove(id_t id) {
        {
            std::unique_lock lock(mutex_);
            metadata_store_.erase(id);
        }
        index_->remove(id);
    }

    std::vector<QueryResult> query(const vector_t& vec, size_t k) const {
        assert(vec.size() == dim_);
        auto raw_results = index_->search(vec, k);
        std::shared_lock lock(mutex_);
        for (auto& res : raw_results) {
            auto it = metadata_store_.find(res.id);
            if (it != metadata_store_.end()) {
                res.metadata = it->second;
            }
        }
        return raw_results;
    }

    size_t size() const {
        return index_->size();
    }

    void serialize(std::ostream& out) const {
        std::shared_lock lock(mutex_);
        // Simple binary format: name, dim, metric, size, then records
        size_t name_len = name_.size();
        out.write(reinterpret_cast<const char*>(&name_len), sizeof(name_len));
        out.write(name_.data(), name_len);
        out.write(reinterpret_cast<const char*>(&dim_), sizeof(dim_));
        int metric_int = static_cast<int>(metric_);
        out.write(reinterpret_cast<const char*>(&metric_int), sizeof(metric_int));
        
        size_t meta_size = metadata_store_.size();
        out.write(reinterpret_cast<const char*>(&meta_size), sizeof(meta_size));
        for (const auto& [id, meta] : metadata_store_) {
            out.write(reinterpret_cast<const char*>(&id), sizeof(id));
            size_t meta_len = meta.size();
            out.write(reinterpret_cast<const char*>(&meta_len), sizeof(meta_len));
            out.write(meta.data(), meta_len);
        }
    }

    static std::unique_ptr<Collection> deserialize(std::istream& in) {
        size_t name_len;
        if (!in.read(reinterpret_cast<char*>(&name_len), sizeof(name_len))) return nullptr;
        std::string name(name_len, '\0');
        in.read(&name[0], name_len);
        
        dimension_t dim;
        in.read(reinterpret_cast<char*>(&dim), sizeof(dim));
        
        int metric_int;
        in.read(reinterpret_cast<char*>(&metric_int), sizeof(metric_int));
        MetricType metric = static_cast<MetricType>(metric_int);
        
        auto col = std::make_unique<Collection>(name, dim, metric);
        
        size_t meta_size;
        in.read(reinterpret_cast<char*>(&meta_size), sizeof(meta_size));
        for (size_t i = 0; i < meta_size; ++i) {
            id_t id;
            in.read(reinterpret_cast<char*>(&id), sizeof(id));
            size_t meta_len;
            in.read(reinterpret_cast<char*>(&meta_len), sizeof(meta_len));
            std::string meta(meta_len, '\0');
            in.read(&meta[0], meta_len);
            
            std::unique_lock lock(col->mutex_);
            col->metadata_store_[id] = meta;
        }
        return col;
    }
};

class Database {
private:
    std::unordered_map<std::string, std::shared_ptr<Collection>> collections_;
    mutable std::shared_mutex mutex_;

public:
    Database() = default;

    void create_collection(const std::string& name, dimension_t dim, MetricType metric) {
        std::unique_lock lock(mutex_);
        if (collections_.find(name) != collections_.end()) {
            throw std::runtime_error("Collection already exists");
        }
        collections_[name] = std::make_shared<Collection>(name, dim, metric);
    }

    std::shared_ptr<Collection> get_collection(const std::string& name) const {
        std::shared_lock lock(mutex_);
        auto it = collections_.find(name);
        if (it == collections_.end()) {
            return nullptr;
        }
        return it->second;
    }

    void drop_collection(const std::string& name) {
        std::unique_lock lock(mutex_);
        collections_.erase(name);
    }

    void save_to_disk(const std::string& filepath) const {
        std::shared_lock lock(mutex_);
        std::ofstream out(filepath, std::ios::binary);
        if (!out) throw std::runtime_error("Failed to open file for writing");

        size_t num_cols = collections_.size();
        out.write(reinterpret_cast<const char*>(&num_cols), sizeof(num_cols));
        for (const auto& [name, col] : collections_) {
            col->serialize(out);
        }
    }

    void load_from_disk(const std::string& filepath) {
        std::unique_lock lock(mutex_);
        std::ifstream in(filepath, std::ios::binary);
        if (!in) throw std::runtime_error("Failed to open file for reading");

        collections_.clear();
        size_t num_cols;
        if (!in.read(reinterpret_cast<char*>(&num_cols), sizeof(num_cols))) return;
        
        for (size_t i = 0; i < num_cols; ++i) {
            auto col = Collection::deserialize(in);
            if (col) {
                collections_[col->name()] = std::move(col);
            }
        }
    }
};

} // namespace VectorDB

// ============================================================================
// EXHAUSTIVE TEST SUITE
// ============================================================================

#define ASSERT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "[ASSERT FAILED