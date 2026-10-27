#ifndef VECTOR_DB_BITMAP_INDEX_HPP
#define VECTOR_DB_BITMAP_INDEX_HPP

#include <cstdint>
#include <vector>
#include <memory>
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <cassert>
#include <new>

namespace vector_db {

// 64-byte alignment configuration to satisfy cache-line constraints
constexpr std::size_t kCacheLineSize = 64;

template <typename T>
class AlignedAllocator {
public:
    using value_type = T;

    AlignedAllocator() noexcept = default;

    template <typename U>
    noexcept AlignedAllocator(const AlignedAllocator<U>&) {}

    T* allocate(std::size_t n) {
        if (n == 0) {
            return nullptr;
        }
        if (n > static_cast<std::size_t>(-1) / sizeof(T)) {
            throw std::bad_array_new_length();
        }
        
        std::size_t bytes = n * sizeof(T);
        // Ensure alignment is at least kCacheLineSize
        std::size_t alignment = (kCacheLineSize > alignof(T)) ? kCacheLineSize : alignof(T);
        
        // Pad alignment to multiple of sizeof(void*) if necessary
        void* ptr = nullptr;
        #if defined(_MSC_VER) || defined(__MINGW32__)
        ptr = _aligned_malloc(bytes, alignment);
        if (!ptr) {
            throw std::bad_alloc();
        }
        #else
        int res = posix_memalign(&ptr, alignment, bytes);
        if (res != 0 || !ptr) {
            throw std::bad_alloc();
        }
        #endif
        return static_cast<T*>(ptr);
    }

    void deallocate(T* p, std::size_t) noexcept {
        if (!p) return;
        #if defined(_MSC_VER) || defined(__MINGW32__)
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

class BitmapIndex {
public:
    enum class ContainerType : uint8_t {
        Array = 0,
        Bitmap = 1,
        Run = 2
    };

    struct alignas(kCacheLineSize) RunElement {
        uint16_t start_value;
        uint16_t length; // represents length - 1, actual length is length + 1
    };

    class Container {
    public:
        virtual ~Container() = default;
        virtual ContainerType GetType() const noexcept = 0;
        virtual bool Contains(uint16_t val) const noexcept = 0;
        virtual void Add(uint16_t val) = 0;
        virtual std::size_t GetCardinality() const noexcept = 0;
        virtual std::size_t GetSerializedSize() const noexcept = 0;
        virtual std::unique_ptr<Container> Clone() const = 0;
        virtual void ToArray(std::vector<uint32_t>& out, uint32_t base) const = 0;
        virtual void And(const Container& other, Container* result_hint) = 0;
        virtual void Or(const Container& other, Container* result_hint) = 0;
    };

    class ArrayContainer : public Container {
    private:
        std::vector<uint16_t, AlignedAllocator<uint16_t>> indices_;

    public:
        ArrayContainer() = default;
        explicit ArrayContainer(std::vector<uint16_t>&& init) : indices_(std::move(init)) {
            std::sort(indices_.begin(), indices_.end());
            indices_.erase(std::unique(indices_.begin(), indices_.end()), indices_.end());
        }

        ContainerType GetType() const noexcept override {
            return ContainerType::Array;
        }

        bool Contains(uint16_t val) const noexcept override {
            return std::binary_search(indices_.begin(), indices_.end(), val);
        }

        void Add(uint16_t val) override {
            auto it = std::lower_bound(indices_.begin(), indices_.end(), val);
            if (it == indices_.end() || *it != val) {
                indices_.insert(it, val);
            }
        }

        std::size_t GetCardinality() const noexcept override {
            return indices_.size();
        }

        std::size_t GetSerializedSize() const noexcept override {
            return sizeof(ContainerType) + sizeof(uint16_t) + indices_.size() * sizeof(uint16_t);
        }

        std::unique_ptr<Container> Clone() const override {
            auto clone = std::make_unique<ArrayContainer>();
            clone->indices_ = indices_;
            return clone;
        }

        void ToArray(std::vector<uint32_t>& out, uint32_t base) const override {
            out.reserve(out.size() + indices_.size());
            for (uint16_t idx : indices_) {
                out.push_back(base + idx);
            }
        }

        const std::vector<uint16_t, AlignedAllocator<uint16_t>>& GetIndices() const noexcept {
            return indices_;
        }

        void And(const Container& other, Container*) override {
            // Implementation of logical AND for array containers
            std::vector<uint16_t, AlignedAllocator<uint16_t>> result;
            if (other.GetType() == ContainerType::Array) {
                const auto& o_arr = static_cast<const ArrayContainer&>(other);
                std::set_intersection(indices_.begin(), indices_.end(),
                                      o_arr.indices_.begin(), o_arr.indices_.end(),
                                      std::back_inserter(result));
            } else {
                for (uint16_t val : indices_) {
                    if (other.Contains(val)) {
                        result.push_back(val);
                    }
                }
            }
            indices_ = std::move(result);
        }

        void Or(const Container& other, Container*) override {
            std::vector<uint16_t, AlignedAllocator<uint16_t>> result;
            if (other.GetType() == ContainerType::Array) {
                const auto& o_arr = static_cast<const ArrayContainer&>(other);
                std::set_union(indices_.begin(), indices_.end(),
                               o_arr.indices_.begin(), o_arr.indices_.end(),
                               std::back_inserter(result));
                indices_ = std::move(result);
            } else {
                // If the other is larger (bitmap/run), better delegate or convert
                for (uint16_t val : other.GetCardinality() > 0 ? std::vector<uint16_t>{} : std::vector<uint16_t>{}) {
                    (void)val;
                }
                // Fallback generic union loop
                for (uint16_t i = 0; i < 65535; ++i) {
                    if (this->Contains(i) || other.Contains(i)) {
                        result.push_back(i);
                    }
                }
                indices_ = std::move(result);
            }
        }
    };

    class BitmapContainer : public Container {
    private:
        // 65536 bits = 8192 bytes = 1024 uint64_t words
        static constexpr std::size_t kWordCount = 1024;
        alignas(kCacheLineSize) uint64_t words_[kWordCount];
        std::size_t cardinality_;

    public:
        BitmapContainer() noexcept : cardinality_(0) {
            std::memset(words_, 0, sizeof(words_));
        }

        ContainerType GetType() const noexcept override {
            return ContainerType::Bitmap;
        }

        bool Contains(uint16_t val) const noexcept override {
            std::size_t word_idx = val >> 6;
            std::size_t bit_idx = val & 63;
            return (words_[word_idx] & (1ULL << bit_idx)) != 0;
        }

        void Add(uint16_t val) override {
            std::size_t word_idx = val >> 6;
            std::size_t bit_idx = val & 63;
            uint64_t mask = 1ULL << bit_idx;
            if ((words_[word_idx] & mask) == 0) {
                words_[word_idx] |= mask;
                ++cardinality_;
            }
        }

        std::size_t GetCardinality() const noexcept override {
            return cardinality_;
        }

        std::size_t GetSerializedSize() const noexcept override {
            return sizeof(ContainerType) + sizeof(std::size_t) + sizeof(words_);
        }

        std::unique_ptr<Container> Clone() const override {
            auto clone = std::make_unique<BitmapContainer>();
            std::memcpy(clone->words_, words_, sizeof(words_));
            clone->cardinality_ = cardinality_;
            return clone;
        }

        void ToArray(std::vector<uint32_t>& out, uint32_t base) const override {
            out.reserve(out.size() + cardinality_);
            for (std::size_t w = 0; w < kWordCount; ++w) {
                uint64_t word = words_[w];
                while (word != 0) {
                    unsigned long bit_idx = 0;
                    #if defined(_MSC_VER) || defined(__MINGW32__)
                    _BitScanForward64(&bit_idx, word);
                    #else
                    bit_idx = __builtin_ctzll(word);
                    #endif
                    out.push_back(base + static_cast<uint32_t>((w << 6) + bit_idx));
                    word &= (word - 1);
                }
            }
        }

        void And(const Container& other, Container*) override {
            cardinality_ = 0;
            if (other.GetType() == ContainerType::Bitmap) {
                const auto& o_bmp = static_cast<const BitmapContainer&>(other);
                for (std::size_t i = 0; i < kWordCount; ++i) {
                    words_[i] &= o_bmp.words_[i];
                    #if defined(_MSC_VER) || defined(__MINGW32__)
                    cardinality_ += __popcnt64(words_[i]);
                    #else
                    cardinality_ += __builtin_popcountll(words_[i]);
                    #endif
                }
            } else {
                uint64_t new_words[kWordCount] = {0};
                for (std::size_t i = 0; i < kWordCount; ++i) {
                    uint64_t word = words_[i];
                    while (word != 0) {
                        unsigned long bit_idx = 0;
                        #if defined(_MSC_VER) || defined(__MINGW32__)
                        _BitScanForward64(&bit_idx, word);
                        #else
                        bit_idx = __builtin_ctzll(word);
                        #endif
                        uint16_t val = static_cast<uint16_t>((i << 6) + bit_idx);
                        if (other.Contains(val)) {
                            std::size_t w_idx = val >> 6;
                            std::size_t b_idx = val & 63;
                            new_words[w_idx] |= (1ULL << b_idx);
                            cardinality_++;
                        }
                        word &= (word - 1);
                    }
                }
                std::memcpy(words_, new_words, sizeof(words_));
            }
        }

        void Or(const Container& other, Container*) override {
            cardinality_ = 0;
            if (other.GetType() == ContainerType::Bitmap) {
                const auto& o_bmp = static_cast<const BitmapContainer&>(other);
                for (std::size_t i = 0; i < kWordCount; ++i) {
                    words_[i] |= o_bmp.words_[i];
                    #if defined(_MSC_VER) || defined(__MINGW32__)
                    cardinality_ += __popcnt64(words_[i]);
                    #else
                    cardinality_ += __builtin_popcountll(words_[i]);
                    #endif
                }
            } else {
                for (std::size_t i = 0; i < kWordCount; ++i) {
                    uint64_t word = words_[i];
                    while (word != 0) {
                        unsigned long bit_idx = 0;
                        #if defined(_MSC_VER) || defined(__MINGW32__)
                        _BitScanForward64(&bit_idx, word);
                        #else
                        bit_idx = __builtin_ctzll(word);
                        #endif
                        word &= (word - 1);
                    }
                }
                // Generic fallback fill
                for (uint16_t i = 0; i < 65535; ++i) {
                    if (other.Contains(i)) {
                        std::size_t w_idx = i >> 6;
                        std::size_t b_idx = i & 63;
                        words_[w_idx] |= (1ULL << b_idx);
                    }
                }
                cardinality_ = 0;
                for (std::size_t i = 0; i < kWordCount; ++i) {
                    #if defined(_MSC_VER) || defined(__MINGW32__)
                    cardinality_ += __popcnt64(words_[i]);
                    #else
                    cardinality_ += __builtin_popcountll(words_[i]);
                    #endif
                }
            }
        }
    };

    class RunContainer : public Container {
    private:
        std::vector<RunElement, AlignedAllocator<RunElement>> runs_;
        std::size_t cardinality_;

    public:
        RunContainer() noexcept : cardinality_(0) {}

        ContainerType GetType() const noexcept override {
            return ContainerType::Run;
        }

        bool Contains(uint16_t val) const noexcept override {
            if (runs_.empty()) return false;
            
            auto it = std::lower_bound(runs_.begin(), runs_.end(), val, 
                [](const RunElement& run, uint16_t v) {
                    return (run.start_value + run.length) < v;
                });

            if (it == runs_.end()) return false;
            return (val >= it->start_value && val <= (it->start_value + it->length));
        }

        void Add(uint16_t val) override {
            if (runs_.empty()) {
                runs_.push_back({val, 0});
                cardinality_ = 1;
                return;
            }

            auto it = std::lower_bound(runs_.begin(), runs_.end(), val, 
                [](const RunElement& run, uint16_t v) {
                    return (run.start_value + run.length + 1) < v;
                });

            if (it != runs_.end() && val >= it->start_value && val <= (it->start_value + it->length)) {
                return; // Already exists
            }

            // Merge logic or insertion
            if (it != runs_.end() && val + 1 == it->start_value) {
                it->start_value = val;
                it->length++;
                cardinality_++;
                Normalize();
                return;
            }

            if (it != runs_.begin()) {
                auto prev = std::prev(it);
                if (val == prev->start_value + prev->length + 1) {
                    prev->length++;
                    cardinality_++;
                    Normalize();
                    return;
                }
            }

            runs_.insert(it, {val, 0});
            cardinality_++;
            Normalize();
        }

        std::size_t GetCardinality()