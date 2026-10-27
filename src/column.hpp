#pragma once

#include <vector>
#include <memory>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <shared_mutex>
#include <atomic>
#include <concepts>
#include <new>

namespace vector_db_engine {

enum class DataType : uint8_t {
    Int32 = 0,
    Int64 = 1,
    Float64 = 2,
    Timestamp = 3
};

template <typename T>
concept SupportedColumnType = 
    std::is_same_v<T, int32_t> || 
    std::is_same_v<T, int64_t> || 
    std::is_same_v<T, double> || 
    std::is_same_v<T, uint64_t>;

template <typename T>
class AlignedAllocator {
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::true_type;

    constexpr AlignedAllocator() noexcept = default;

    template <typename U>
    constexpr AlignedAllocator(const AlignedAllocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        if (n == 0) {
            return nullptr;
        }
        if (n > static_cast<std::size_t>(-1) / sizeof(T)) {
            throw std::bad_array_new_length();
        }

        std::size_t alignment = 64;
        std::size_t bytes = n * sizeof(T);
        
        // Pad bytes to be a multiple of alignment
        std::size_t remainder = bytes % alignment;
        if (remainder != 0) {
            bytes += (alignment - remainder);
        }

        void* ptr = nullptr;
#if defined(_MSC_VER)
        ptr = _aligned_malloc(bytes, alignment);
#else
        if (posix_memalign(&ptr, alignment, bytes) != 0) {
            ptr = nullptr;
        }
#endif
        if (!ptr) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(ptr);
    }

    void deallocate(T* p, std::size_t) noexcept {
        if (!p) {
            return;
        }
#if defined(_MSC_VER)
        _aligned_free(p);
#else
        free(p);
#endif
    }

    template <typename U>
    bool operator==(const AlignedAllocator<U>&) const noexcept {
        return true;
    }

    template <typename U>
    bool operator!=(const AlignedAllocator<U>&) const noexcept {
        return false;
    }
};

class ColumnException : public std::runtime_error {
public:
    explicit ColumnException(const std::string& message) 
        : std::runtime_error("ColumnException: " + message) {}
};

class IColumn {
public:
    virtual ~IColumn() = default;
    virtual DataType type() const noexcept = 0;
    virtual std::size_t size() const noexcept = 0;
    virtual std::size_t capacity() const noexcept = 0;
    virtual void reserve(std::size_t capacity) = 0;
    virtual void clear() noexcept = 0;
    virtual std::shared_ptr<IColumn> clone() const = 0;
};

template <SupportedColumnType T>
class Column : public IColumn {
private:
    std::vector<T, AlignedAllocator<T>> data_;
    mutable std::shared_mutex mutex_;
    DataType data_type_;

    DataType deduce_data_type() const noexcept {
        if constexpr (std::is_same_v<T, int32_t>) return DataType::Int32;
        else if constexpr (std::is_same_v<T, int64_t>) return DataType::Int64;
        else if constexpr (std::is_same_v<T, double>) return DataType::Float64;
        else if constexpr (std::is_same_v<T, uint64_t>) return DataType::Timestamp;
        else return DataType::Int32;
    }

public:
    explicit Column(std::size_t initial_capacity = 0) 
        : data_type_(deduce_data_type()) {
        if (initial_capacity > 0) {
            data_.reserve(initial_capacity);
        }
    }

    Column(const Column& other) {
        std::shared_lock<std::shared_mutex> lock(other.mutex_);
        data_ = other.data_;
        data_type_ = other.data_type_;
    }

    Column(Column&& other) noexcept {
        std::unique_lock<std::shared_mutex> lock(other.mutex_);
        data_ = std::move(other.data_);
        data_type_ = other.data_type_;
    }

    Column& operator=(const Column& other) {
        if (this != &other) {
            std::unique_lock<std::shared_mutex> this_lock(mutex_, std::defer_lock);
            std::shared_lock<std::shared_mutex> other_lock(other.mutex_, std::defer_lock);
            std::lock(this_lock, other_lock);

            data_ = other.data_;
            data_type_ = other.data_type_;
        }
        return *this;
    }

    Column& operator=(Column&& other) noexcept {
        if (this != &other) {
            std::unique_lock<std::shared_mutex> this_lock(mutex_, std::defer_lock);
            std::unique_lock<std::shared_mutex> other_lock(other.mutex_, std::defer_lock);
            std::lock(this_lock, other_lock);

            data_ = std::move(other.data_);
            data_type_ = other.data_type_;
        }
        return *this;
    }

    ~Column() override = default;

    DataType type() const noexcept override {
        return data_type_;
    }

    std::size_t size() const noexcept override {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return data_.size();
    }

    std::size_t capacity() const noexcept override {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return data_.capacity();
    }

    void reserve(std::size_t new_capacity) override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        data_.reserve(new_capacity);
    }

    void clear() noexcept override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        data_.clear();
    }

    std::shared_ptr<IColumn> clone() const override {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return std::make_shared<Column<T>>(*this);
    }

    void append(T value) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        data_.push_back(value);
    }

    void append_batch(const T* values, std::size_t count) {
        if (!values && count > 0) {
            throw ColumnException("Null pointer passed to append_batch.");
        }
        std::unique_lock<std::shared_mutex> lock(mutex_);
        data_.insert(data_.end(), values, values + count);
    }

    T get(std::size_t index) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (index >= data_.size()) {
            throw ColumnException("Index out of bounds in Column::get.");
        }
        return data_[index];
    }

    void set(std::size_t index, T value) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (index >= data_.size()) {
            throw ColumnException("Index out of bounds in Column::set.");
        }
        data_[index] = value;
    }

    const T* raw_data() const noexcept {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return data_.data();
    }

    T* raw_data() noexcept {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        return data_.data();
    }

    void unsafe_append_unlocked(T value) {
        data_.push_back(value);
    }

    const std::vector<T, AlignedAllocator<T>>& get_vector_ref() const noexcept {
        return data_;
    }
};

class ColumnFactory {
public:
    static std::shared_ptr<IColumn> create_column(DataType type, std::size_t initial_capacity = 0) {
        switch (type) {
            case DataType::Int32:
                return std::make_shared<Column<int32_t>>(initial_capacity);
            case DataType::Int64:
                return std::make_shared<Column<int64_t>>(initial_capacity);
            case DataType::Float64:
                return std::make_shared<Column<double>>(initial_capacity);
            case DataType::Timestamp:
                return std::make_shared<Column<uint64_t>>(initial_capacity);
            default:
                throw ColumnException("Unsupported DataType supplied to ColumnFactory.");
        }
    }
};

} // namespace vector_db_engine