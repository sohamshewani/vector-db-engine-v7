#ifndef VECTOR_DB_ENGINE_ALLOCATOR_HPP
#define VECTOR_DB_ENGINE_ALLOCATOR_HPP

#include <cstdlib>
#include <new>
#include <limits>
#include <cstdint>
#include <cstddef>
#include <atomic>
#include <vector>
#include <memory>
#include <utility>
#include <stdexcept>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace vdb {
namespace memory {

constexpr std::size_t CACHE_LINE_SIZE = 64;

inline void* aligned_alloc_bytes(std::size_t alignment, std::size_t size) {
    if (alignment < sizeof(void*)) {
        alignment = sizeof(void*);
    }
    // Ensure size is a multiple of alignment
    std::size_t remainder = size % alignment;
    if (remainder != 0) {
        size += (alignment - remainder);
    }

    void* ptr = nullptr;
#if defined(_MSC_VER)
    ptr = _aligned_malloc(size, alignment);
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    ptr = std::aligned_alloc(alignment, size);
#else
    if (posix_memalign(&ptr, alignment, size) != 0) {
        ptr = nullptr;
    }
#endif
    if (!ptr && size > 0) {
        throw std::bad_alloc();
    }
    return ptr;
}

inline void aligned_free_bytes(void* ptr) noexcept {
    if (!ptr) {
        return;
    }
#if defined(_MSC_VER)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

template <typename T, std::size_t Alignment = CACHE_LINE_SIZE>
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

    constexpr AlignedAllocator() noexcept = default;

    template <typename U>
    constexpr AlignedAllocator(const AlignedAllocator<U, Alignment>&) noexcept {}

    ~AlignedAllocator() = default;

    pointer allocate(size_type n, const void* /*hint*/ = nullptr) {
        if (n == 0) {
            return nullptr;
        }
        if (n > std::numeric_limits<size_type>::max() / sizeof(T)) {
            throw std::bad_array_new_length();
        }
        std::size_t bytes = n * sizeof(T);
        void* raw = aligned_alloc_bytes(Alignment, bytes);
        return static_cast<pointer>(raw);
    }

    void deallocate(pointer p, size_type /*n*/) noexcept {
        aligned_free_bytes(p);
    }

    template <typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }

    template <typename U>
    void destroy(U* p) noexcept {
        p->~U();
    }

    constexpr size_type max_size() const noexcept {
        return std::numeric_limits<size_type>::max() / sizeof(T);
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

class ArenaAllocator {
public:
    explicit ArenaAllocator(std::size_t page_size = 65536)
        : page_size_(page_size < CACHE_LINE_SIZE ? CACHE_LINE_SIZE : page_size),
          current_offset_(0),
          total_allocated_bytes_(0) {
        if (page_size_ % CACHE_LINE_SIZE != 0) {
            page_size_ = ((page_size_ + CACHE_LINE_SIZE - 1) / CACHE_LINE_SIZE) * CACHE_LINE_SIZE;
        }
        allocate_new_page(page_size_);
    }

    ~ArenaAllocator() {
        release_all();
    }

    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    ArenaAllocator(ArenaAllocator&& other) noexcept
        : pages_(std::move(other.pages_)),
          page_size_(other.page_size_),
          current_page_index_(other.current_page_index_),
          current_offset_(other.current_offset_),
          total_allocated_bytes_(other.total_allocated_bytes_) {
        other.page_size_ = 0;
        other.current_page_index_ = 0;
        other.current_offset_ = 0;
        other.total_allocated_bytes_ = 0;
    }

    ArenaAllocator& operator=(ArenaAllocator&& other) noexcept {
        if (this != &other) {
            release_all();
            pages_ = std::move(other.pages_);
            page_size_ = other.page_size_;
            current_page_index_ = other.current_page_index_;
            current_offset_ = other.current_offset_;
            total_allocated_bytes_ = other.total_allocated_bytes_;

            other.page_size_ = 0;
            other.current_page_index_ = 0;
            other.current_offset_ = 0;
            other.total_allocated_bytes_ = 0;
        }
        return *this;
    }

    void* allocate(std::size_t size, std::size_t alignment = CACHE_LINE_SIZE) {
        if (size == 0) {
            return nullptr;
        }
        if (alignment < CACHE_LINE_SIZE) {
            alignment = CACHE_LINE_SIZE;
        }

        std::size_t current_address = reinterpret_cast<std::size_t>(pages_[current_page_index_].data) + current_offset_;
        std::size_t aligned_address = (current_address + alignment - 1) & ~(alignment - 1);
        std::size_t padding = aligned_address - current_address;
        std::size_t required_space = size + padding;

        if (current_offset_ + required_space > page_size_) {
            // Need a new page. If the requested size is larger than default page size, create a custom-sized page.
            std::size_t new_page_sz = (size + alignment > page_size_) ? (size + alignment + CACHE_LINE_SIZE) : page_size_;
            allocate_new_page(new_page_sz);

            current_address = reinterpret_cast<std::size_t>(pages_[current_page_index_].data);
            aligned_address = (current_address + alignment - 1) & ~(alignment - 1);
            padding = aligned_address - current_address;
            required_space = size + padding;
        }

        void* ptr = reinterpret_cast<void*>(aligned_address);
        current_offset_ += required_space;
        return ptr;
    }

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        void* mem = allocate(sizeof(T), alignof(T));
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    void reset() noexcept {
        for (std::size_t i = 1; i < pages_.size(); ++i) {
            aligned_free_bytes(pages_[i].data);
        }
        if (!pages_.empty()) {
            pages_.resize(1);
            pages_[0].size = page_size_;
            current_offset_ = 0;
            current_page_index_ = 0;
            total_allocated_bytes_ = page_size_;
        } else {
            total_allocated_bytes_ = 0;
            current_offset_ = 0;
            current_page_index_ = 0;
        }
    }

    std::size_t total_memory_usage() const noexcept {
        return total_allocated_bytes_;
    }

private:
    struct Page {
        void* data;
        std::size_t size;
    };

    std::vector<Page> pages_;
    std::size_t page_size_;
    std::size_t current_page_index_ = 0;
    std::size_t current_offset_ = 0;
    std::size_t total_allocated_bytes_ = 0;

    void allocate_new_page(std::size_t size) {
        void* raw = aligned_alloc_bytes(CACHE_LINE_SIZE, size);
        pages_.push_back(Page{raw, size});
        current_page_index_ = pages_.size() - 1;
        current_offset_ = 0;
        total_allocated_bytes_ += size;
    }

    void release_all() noexcept {
        for (auto& page : pages_) {
            aligned_free_bytes(page.data);
        }
        pages_.clear();
        total_allocated_bytes_ = 0;
        current_offset_ = 0;
        current_page_index_ = 0;
    }
};

template <typename T, std::size_t Alignment = CACHE_LINE_SIZE>
using AlignedVector = std::vector<T, AlignedAllocator<T, Alignment>>;

} // namespace memory
} // namespace vdb

#endif // VECTOR_DB_ENGINE_ALLOCATOR_HPP