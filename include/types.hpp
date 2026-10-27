#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <vector>
#include <string>
#include <stdexcept>
#include <system_error>
#include <type_traits>

namespace vcdb {

// ============================================================================
// ERROR HANDLING HIERARCHY
// ============================================================================

enum class ErrorCode : int {
    Success = 0,
    InvalidArgument,
    OutOfBounds,
    AllocationFailure,
    TypeMismatch,
    ImmutableViolation,
    SerializationError,
    ConcurrencyError,
    InternalError
};

class EngineErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override {
        return "vector-db-engine";
    }

    std::string message(int ev) const override {
        switch (static_cast<ErrorCode>(ev)) {
            case ErrorCode::Success:             return "Success";
            case ErrorCode::InvalidArgument:     return "Invalid argument supplied";
            case ErrorCode::OutOfBounds:         return "Index out of bounds";
            case ErrorCode::AllocationFailure:   return "Memory allocation failed";
            case ErrorCode::TypeMismatch:        return "Data type mismatch";
            case ErrorCode::ImmutableViolation:  return "Attempted modification of immutable storage";
            case ErrorCode::SerializationError:  return "Serialization or deserialization failure";
            case ErrorCode::ConcurrencyError:    return "Concurrency or lock acquisition failure";
            case ErrorCode::InternalError:       return "Internal engine invariant violation";
            default:                             return "Unknown engine error";
        }
    }
};

inline const std::error_category& get_engine_error_category() noexcept {
    static EngineErrorCategory instance;
    return instance;
}

inline std::error_code make_error_code(ErrorCode e) noexcept {
    return {static_cast<int>(e), get_engine_error_category()};
}

class EngineException : public std::runtime_error {
public:
    explicit EngineException(ErrorCode code, const std::string& message)
        : std::runtime_error(message), code_(code) {}

    explicit EngineException(ErrorCode code, const char* message)
        : std::runtime_error(message), code_(code) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] std::error_code error_code() const noexcept { return make_error_code(code_); }

private:
    ErrorCode code_;
};

// ============================================================================
// SYSTEM DATA TYPES & METADATA
// ============================================================================

enum class DataType : uint8_t {
    Int32     = 0,
    Int64     = 1,
    Float64   = 2,
    Timestamp = 3,
    String    = 4,
    Blob      = 5
};

template <DataType T> struct TypeMapper;
template <> struct TypeMapper<DataType::Int32>     { using type = int32_t; };
template <> struct TypeMapper<DataType::Int64>     { using type = int64_t; };
template <> struct TypeMapper<DataType::Float64>   { using type = double; };
template <> struct TypeMapper<DataType::Timestamp> { using type = uint64_t; };

// ============================================================================
// 64-BYTE ALIGNED CUSTOM ALLOCATOR
// ============================================================================

template <typename T>
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
        using other = AlignedAllocator<U>;
    };

    AlignedAllocator() noexcept = default;
    
    template <typename U>
    AlignedAllocator(const AlignedAllocator<U>&) noexcept {}

    [[nodiscard]] pointer allocate(size_type n) {
        if (n == 0) {
            return nullptr;
        }
        if (n > static_cast<size_type>(-1) / sizeof(T)) {
            throw EngineException(ErrorCode::AllocationFailure, "Allocation size overflow");
        }

        size_type bytes = n * sizeof(T);
        constexpr size_type alignment = 64;
        
        // Ensure alignment is at least alignof(T) and a multiple of void*
        size_type actual_alignment = alignment;
        if (actual_alignment < alignof(T)) {
            actual_alignment = alignof(T);
        }

        void* ptr = nullptr;
#if defined(_MSC_VER) || defined(__MINGW32__)
        ptr = _aligned_malloc(bytes, actual_alignment);
#else
        if (posix_memalign(&ptr, actual_alignment, bytes) != 0) {
            ptr = nullptr;
        }
#endif

        if (ptr == nullptr) {
            throw EngineException(ErrorCode::AllocationFailure, "Failed to allocate 64-byte aligned memory");
        }

        return static_cast<pointer>(ptr);
    }

    void deallocate(pointer p, size_type) noexcept {
        if (p == nullptr) {
            return;
        }
#if defined(_MSC_VER) || defined(__MINGW32__)
        _aligned_free(p);
#else
        free(p);
#endif
    }

    template <typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }

    template <typename U>
    void destroy(U* p) noexcept {
        p->~U();
    }

    bool operator==(const AlignedAllocator&) const noexcept { return true; }
    bool operator!=(const AlignedAllocator&) const noexcept { return false; }
};

// ============================================================================
// ALIGNED VECTOR DEFINITION
// ============================================================================

template <typename T>
using AlignedVector = std::vector<T, AlignedAllocator<T>>;

// ============================================================================
// SELECTION VECTOR INTERFACE
// ============================================================================

class SelectionVector {
public:
    SelectionVector() = default;
    
    explicit SelectionVector(size_t capacity) {
        indices_.reserve(capacity);
    }

    [[nodiscard]] const uint32_t* data() const noexcept {
        return indices_.data();
    }

    [[nodiscard]] uint32_t* data() noexcept {
        return indices_.data();
    }

    [[nodiscard]] size_t size() const noexcept {
        return indices_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return indices_.empty();
    }

    void push_back(uint32_t index) {
        indices_.push_back(index);
    }

    void resize(size_t new_size) {
        indices_.resize(new_size);
    }

    void clear() noexcept {
        indices_.clear();
    }

    void reserve(size_t capacity) {
        indices_.reserve(capacity);
    }

    [[nodiscard]] uint32_t operator[](size_t idx) const {
        if (idx >= indices_.size()) {
            throw EngineException(ErrorCode::OutOfBounds, "SelectionVector index out of bounds");
        }
        return indices_[idx];
    }

    [[nodiscard]] uint32_t& operator[](size_t idx) {
        if (idx >= indices_.size()) {
            throw EngineException(ErrorCode::OutOfBounds, "SelectionVector index out of bounds");
        }
        return indices_[idx];
    }

    void initialize_identity(size_t n) {
        indices_.resize(n);
        for (size_t i = 0; i < n; ++i) {
            indices_[i] = static_cast<uint32_t>(i);
        }
    }

private:
    AlignedVector<uint32_t> indices_;
};

} // namespace vcdb