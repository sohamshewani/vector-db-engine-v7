#ifndef VECTORIZED_EXECUTOR_HPP
#define VECTORIZED_EXECUTOR_HPP

#include <cstdint>
#include <vector>
#include <memory>
#include <string>
#include <stdexcept>
#include <atomic>
#include <shared_mutex>
#include <cstring>
#include <cstdlib>
#include <new>
#include <algorithm>

namespace vector_db_engine {

template <typename T, std::size_t Alignment = 64>
class AlignedAllocator {
public:
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    template <typename U>
    struct rebind {
        using other = AlignedAllocator<U, Alignment>;
    };

    AlignedAllocator() noexcept = default;
    template <typename U>
    AlignedAllocator(const AlignedAllocator<U, Alignment>&) noexcept {}

    pointer allocate(size_type n) {
        if (n == 0) {
            return nullptr;
        }
        if (n > max_size()) {
            throw std::bad_array_new_length();
        }
        std::size_t bytes = n * sizeof(T);
        std::size_t remainder = bytes % Alignment;
        if (remainder != 0) {
            bytes += (Alignment - remainder);
        }
        void* ptr = nullptr;
#if defined(_MSC_VER)
        ptr = _aligned_malloc(bytes, Alignment);
        if (!ptr) {
            throw std::bad_alloc();
        }
#else
        int result = posix_memalign(&ptr, Alignment, bytes);
        if (result != 0 || !ptr) {
            throw std::bad_alloc();
        }
#endif
        return static_cast<pointer>(ptr);
    }

    void deallocate(pointer p, size_type) noexcept {
        if (!p) {
            return;
        }
#if defined(_MSC_VER)
        _aligned_free(p);
#else
        free(p);
#endif
    }

    size_type max_size() const noexcept {
        return static_cast<size_type>(-1) / sizeof(T);
    }

    template <typename U>
    bool operator==(const AlignedAllocator<U, Alignment>&) const noexcept {
        return true;
    }

    template <typename U>
    bool operator!=(const AlignedAllocator<U, Alignment>&) const noexcept {
        return false;
    }
};

template <typename T>
using AlignedVector = std::vector<T, AlignedAllocator<T, 64>>;

enum class DataType : uint8_t {
    Int32 = 0,
    Int64 = 1,
    Float64 = 2,
    Timestamp = 3
};

struct SelectionVector {
    AlignedVector<uint32_t> indices;
    uint32_t size;

    explicit SelectionVector(uint32_t capacity = 0) 
        : indices(capacity, 0, AlignedAllocator<uint32_t, 64>()), size(0) {}

    void resize(uint32_t new_size) {
        if (new_size > indices.size()) {
            indices.resize(new_size);
        }
        size = new_size;
    }

    void clear() noexcept {
        size = 0;
    }

    uint32_t* data() noexcept {
        return indices.data();
    }

    const uint32_t* data() const noexcept {
        return indices.data();
    }
};

class ArenaAllocator {
private:
    struct Page {
        uint8_t* memory;
        size_t capacity;
        size_t offset;
        Page* next;

        Page(size_t cap) : capacity(cap), offset(0), next(nullptr) {
            int res = posix_memalign(reinterpret_cast<void**>(&memory), 64, capacity);
            if (res != 0 || !memory) {
                throw std::bad_alloc();
            }
        }

        ~Page() {
            free(memory);
        }
    };

    Page* head_;
    size_t page_size_;
    std::size_t total_allocated_;

public:
    explicit ArenaAllocator(size_t page_size = 65536) 
        : page_size_(page_size), total_allocated_(0) {
        head_ = new Page(page_size_);
        total_allocated_ += page_size_;
    }

    ~ArenaAllocator() {
        Page* current = head_;
        while (current != nullptr) {
            Page* next = current->next;
            delete current;
            current = next;
        }
    }

    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    void* allocate(size_t size, size_t alignment = 64) {
        if (size > page_size_) {
            Page* new_page = new Page(size + alignment);
            new_page->next = head_->next;
            head_->next = new_page;
            total_allocated_ += (size + alignment);
            uint8_t* ptr = new_page->memory;
            uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
            size_t remainder = addr % alignment;
            if (remainder != 0) {
                addr += (alignment - remainder);
            }
            return reinterpret_cast<void*>(addr);
        }

        uintptr_t current_addr = reinterpret_cast<uintptr_t>(head_->memory + head_->offset);
        size_t remainder = current_addr % alignment;
        size_t adjustment = (remainder == 0) ? 0 : (alignment - remainder);

        if (head_->offset + adjustment + size > head_->capacity) {
            Page* new_page = new Page(page_size_);
            new_page->next = head_;
            head_ = new_page;
            total_allocated_ += page_size_;
            current_addr = reinterpret_cast<uintptr_t>(head_->memory);
            remainder = current_addr % alignment;
            adjustment = (remainder == 0) ? 0 : (alignment - remainder);
        }

        uint8_t* ptr = head_->memory + head_->offset + adjustment;
        head_->offset += (adjustment + size);
        return static_cast<void*>(ptr);
    }

    void reset() noexcept {
        Page* current = head_->next;
        while (current != nullptr) {
            Page* next = current->next;
            delete current;
            current = next;
        }
        head_->next = nullptr;
        head_->offset = 0;
    }
};

class ColumnChunk {
private:
    DataType type_;
    uint32_t row_count_;
    AlignedVector<uint8_t> data_;

public:
    ColumnChunk(DataType type, uint32_t row_count) 
        : type_(type), row_count_(row_count) {
        size_t element_size = 0;
        switch (type_) {
            case DataType::Int32: element_size = sizeof(int32_t); break;
            case DataType::Int64: element_size = sizeof(int64_t); break;
            case DataType::Float64: element_size = sizeof(double); break;
            case DataType::Timestamp: element_size = sizeof(int64_t); break;
        }
        data_.resize(row_count_ * element_size);
    }

    DataType get_type() const noexcept { return type_; }
    uint32_t get_row_count() const noexcept { return row_count_; }

    template <typename T>
    const T* get_data_as() const noexcept {
        return reinterpret_cast<const T*>(data_.data());
    }

    template <typename T>
    T* get_mutable_data_as() noexcept {
        return reinterpret_cast<T*>(data_.data());
    }
};

class VectorizedExecutor {
public:
    enum class OpType {
        Equals,
        NotEquals,
        LessThan,
        LessThanOrEqual,
        GreaterThan,
        GreaterThanOrEqual
    };

    template <typename T>
    static void execute_filter_scalar(
        const T* restrict data,
        const uint32_t* restrict input_sel,
        uint32_t input_size,
        T threshold,
        OpType op,
        SelectionVector& output_sel
    ) {
        output_sel.resize(input_size);
        uint32_t match_count = 0;

        for (uint32_t i = 0; i < input_size; ++i) {
            uint32_t idx = input_sel[i];
            T val = data[idx];
            bool pass = false;
            switch (op) {
                case OpType::Equals: pass = (val == threshold); break;
                case OpType::NotEquals: pass = (val != threshold); break;
                case OpType::LessThan: pass = (val < threshold); break;
                case OpType::LessThanOrEqual: pass = (val <= threshold); break;
                case OpType::GreaterThan: pass = (val > threshold); break;
                case OpType::GreaterThanOrEqual: pass = (val >= threshold); break;
            }
            if (pass) {
                output_sel.indices[match_count++] = idx;
            }
        }
        output_sel.size = match_count;
    }

    template <typename T>
    static void execute_filter_vectorized_4(
        const T* restrict data,
        const uint32_t* restrict input_sel,
        uint32_t input_size,
        T threshold,
        OpType op,
        SelectionVector& output_sel
    ) {
        output_sel.resize(input_size);
        uint32_t match_count = 0;
        uint32_t i = 0;

        uint32_t vec_limit = (input_size / 4) * 4;

        for (; i < vec_limit; i += 4) {
            uint32_t idx0 = input_sel[i];
            uint32_t idx1 = input_sel[i + 1];
            uint32_t idx2 = input_sel[i + 2];
            uint32_t idx3 = input_sel[i + 3];

            T val0 = data[idx0];
            T val1 = data[idx1];
            T val2 = data[idx2];
            T val3 = data[idx3];

            bool p0 = false, p1 = false, p2 = false, p3 = false;

            switch (op) {
                case OpType::Equals:
                    p0 = (val0 == threshold); p1 = (val1 == threshold);
                    p2 = (val2 == threshold); p3 = (val3 == threshold);
                    break;
                case OpType::NotEquals:
                    p0 = (val0 != threshold); p1 = (val1 != threshold);
                    p2 = (val2 != threshold); p3 = (val3 != threshold);
                    break;
                case OpType::LessThan:
                    p0 = (val0 < threshold); p1 = (val1 < threshold);
                    p2 = (val2 < threshold); p3 = (val3 < threshold);
                    break;
                case OpType::LessThanOrEqual:
                    p0 = (val0 <= threshold); p1 = (val1 <= threshold);
                    p2 = (val2 <= threshold); p3 = (val3 <= threshold);
                    break;
                case OpType::GreaterThan:
                    p0 = (val0 > threshold); p1 = (val1 > threshold);
                    p2 = (val2 > threshold); p3 = (val3 > threshold);
                    break;
                case OpType::GreaterThanOrEqual:
                    p0 = (val0 >= threshold); p1 = (val1 >= threshold);
                    p2 = (val2 >= threshold); p3 = (val3 >= threshold);
                    break;
            }

            if (p0) output_sel.indices[match_count++] = idx0;
            if (p1) output_sel.indices[match_count++] = idx1;
            if (p2) output_sel.indices[match_count++] = idx2;
            if (p3) output_sel.indices[match_count++] = idx3;
        }

        for (; i < input_size; ++i) {
            uint32_t idx = input_sel[i];
            T val = data[idx];
            bool pass = false;
            switch (op) {
                case OpType::Equals: pass = (val == threshold); break;
                case OpType::NotEquals: pass = (val != threshold); break;
                case OpType::LessThan: pass = (val < threshold); break;
                case OpType::LessThanOrEqual: pass = (val <= threshold); break;
                case OpType::GreaterThan: pass = (val > threshold); break;
                case OpType::GreaterThanOrEqual: pass = (val >= threshold); break;
            }
            if (pass) {
                output_sel.indices[match_count++] = idx;
            }
        }

        output_sel.size = match_count;
    }

    template <typename T>
    static void execute_filter(
        const ColumnChunk& chunk,
        const SelectionVector& input_sel,
        T threshold,
        OpType op,
        SelectionVector& output_sel
    ) {
        if (chunk.get_row_count() == 0 || input_sel.size == 0) {
            output_sel.clear();
            return;
        }

        const T* data = chunk.get_data_as<T>();
        execute_filter_vectorized_4(data, input_sel.data(), input_sel.size, threshold, op, output_sel);
    }
};

class PartitionState {
private:
    std::shared_mutex mutex_;
    std::vector<std::shared_ptr<ColumnChunk>> columns_;
    uint32_t row_count_;

public:
    PartitionState(uint32_t row_count, std::vector<std::shared_ptr<ColumnChunk>> cols)
        : columns_(std::move(cols)), row_count_(row_count) {}

    std::shared_ptr<ColumnChunk> get_column(size_t index) const {
        std::shared_lock<std::shared_mutex> lock(const_cast<std::shared_mutex&>(mutex_));
        if (index >= columns_.size()) {
            throw std::out_of_range("Column index out of range");
        }
        return columns_[index];
    }

    uint32_t get_row_count() const noexcept {
        return row_count_;
    }
};

class TablePartitionManager {
private:
    std::atomic<PartitionState*> current_state_;

public:
    explicit TablePartitionManager(std::shared_ptr<PartitionState> initial_state) {
        current_state_.store(initial_state.get());
        initial_state_ref_ = initial_state;
    }

    ~TablePartitionManager() = default;

    std::shared_ptr<PartitionState> get_current_state() const noexcept {
        return initial_state_ref_;
    }

    void update_state(std::shared_ptr<PartitionState> new_state) {
        initial_state_ref_ = new_state;
        current_state_.store(new_state.get(), std::memory_order_release);
    }

private:
    std::shared_ptr<PartitionState> initial_state_ref_;
};

} // namespace vector_db_engine

#endif // VECTORIZED_EXECUTOR_HPP