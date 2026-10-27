#ifndef VECTOR_DB_ENGINE_COMPRESSION_HPP
#define VECTOR_DB_ENGINE_COMPRESSION_HPP

#include <vector>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <memory>
#include <algorithm>
#include <unordered_map>
#include <limits>

namespace VectorDB {

enum class CompressionType : uint8_t {
    Uncompressed = 0,
    RunLengthEncoding = 1,
    Dictionary = 2
};

struct CompressedBlockHeader {
    CompressionType type;
    uint32_t element_count;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
};

class CompressionException : public std::runtime_error {
public:
    explicit CompressionException(const std::string& message) 
        : std::runtime_error("CompressionException: " + message) {}
};

class Compressor {
public:
    virtual ~Compressor() = default;
    virtual std::vector<uint8_t> compress(const uint8_t* data, size_t size, size_t element_count) = 0;
    virtual std::vector<uint8_t> decompress(const uint8_t* compressed_data, size_t compressed_size, size_t expected_element_count) = 0;
};

template <typename T>
class RLECompressor : public Compressor {
public:
    std::vector<uint8_t> compress(const uint8_t* data, size_t size, size_t element_count) override {
        if (element_count == 0) {
            CompressedBlockHeader header{CompressionType::RunLengthEncoding, 0, 0, 0};
            std::vector<uint8_t> result(sizeof(CompressedBlockHeader));
            std::memcpy(result.data(), &header, sizeof(CompressedBlockHeader));
            return result;
        }

        const T* typed_data = reinterpret_cast<const T*>(data);
        std::vector<std::pair<T, uint32_t>> runs;
        
        if (element_count > 0) {
            T current_val = typed_data[0];
            uint32_t current_run = 1;

            for (size_t i = 1; i < element_count; ++i) {
                if (typed_data[i] == current_val && current_run < std::numeric_limits<uint32_t>::max()) {
                    current_run++;
                } else {
                    runs.emplace_back(current_val, current_run);
                    current_val = typed_data[i];
                    current_run = 1;
                }
            }
            runs.emplace_back(current_val, current_run);
        }

        size_t payload_size = runs.size() * (sizeof(T) + sizeof(uint32_t));
        std::vector<uint8_t> result(sizeof(CompressedBlockHeader) + payload_size);

        CompressedBlockHeader header;
        header.type = CompressionType::RunLengthEncoding;
        header.element_count = static_cast<uint32_t>(element_count);
        header.compressed_size = static_cast<uint32_t>(payload_size);
        header.uncompressed_size = static_cast<uint32_t>(size);

        std::memcpy(result.data(), &header, sizeof(CompressedBlockHeader));

        uint8_t* dest = result.data() + sizeof(CompressedBlockHeader);
        for (const auto& run : runs) {
            std::memcpy(dest, &run.first, sizeof(T));
            std::memcpy(dest + sizeof(T), &run.second, sizeof(uint32_t));
            dest += sizeof(T) + sizeof(uint32_t);
        }

        return result;
    }

    std::vector<uint8_t> decompress(const uint8_t* compressed_data, size_t compressed_size, size_t expected_element_count) override {
        if (compressed_size < sizeof(CompressedBlockHeader)) {
            throw CompressionException("Compressed data size smaller than header size.");
        }

        CompressedBlockHeader header;
        std::memcpy(&header, compressed_data, sizeof(CompressedBlockHeader));

        if (header.type != CompressionType::RunLengthEncoding) {
            throw CompressionException("Invalid compression type for RLE decompressor.");
        }

        if (header.element_count != expected_element_count) {
            throw CompressionException("Element count mismatch during RLE decompression.");
        }

        std::vector<uint8_t> result(header.uncompressed_size);
        T* dest = reinterpret_cast<T*>(result.data());

        const uint8_t* src = compressed_data + sizeof(CompressedBlockHeader);
        const uint8_t* src_end = compressed_data + compressed_size;

        size_t decoded_elements = 0;
        while (src < src_end) {
            if (src + sizeof(T) + sizeof(uint32_t) > src_end) {
                throw CompressionException("Corrupted RLE stream: unexpected end of buffer.");
            }

            T val;
            uint32_t run_length;
            std::memcpy(&val, src, sizeof(T));
            std::memcpy(&run_length, src + sizeof(T), sizeof(uint32_t));
            src += sizeof(T) + sizeof(uint32_t);

            if (decoded_elements + run_length > expected_element_count) {
                throw CompressionException("Corrupted RLE stream: run length exceeds expected element count.");
            }

            std::fill_n(dest + decoded_elements, run_length, val);
            decoded_elements += run_length;
        }

        if (decoded_elements != expected_element_count) {
            throw CompressionException("Decoded element count does not match expected element count.");
        }

        return result;
    }
};

template <typename T>
class DictionaryCompressor : public Compressor {
public:
    std::vector<uint8_t> compress(const uint8_t* data, size_t size, size_t element_count) override {
        if (element_count == 0) {
            CompressedBlockHeader header{CompressionType::Dictionary, 0, 0, 0};
            std::vector<uint8_t> result(sizeof(CompressedBlockHeader));
            std::memcpy(result.data(), &header, sizeof(CompressedBlockHeader));
            return result;
        }

        const T* typed_data = reinterpret_cast<const T*>(data);
        std::unordered_map<T, uint32_t> val_to_id;
        std::vector<T> dictionary;
        std::vector<uint32_t> indices(element_count);

        for (size_t i = 0; i < element_count; ++i) {
            T val = typed_data[i];
            auto it = val_to_id.find(val);
            if (it == val_to_id.end()) {
                uint32_t new_id = static_cast<uint32_t>(dictionary.size());
                val_to_id[val] = new_id;
                dictionary.push_back(val);
                indices[i] = new_id;
            } else {
                indices[i] = it->second;
            }
        }

        uint32_t dict_size = static_cast<uint32_t>(dictionary.size());
        size_t payload_size = sizeof(uint32_t) + (dict_size * sizeof(T)) + (element_count * sizeof(uint32_t));

        std::vector<uint8_t> result(sizeof(CompressedBlockHeader) + payload_size);

        CompressedBlockHeader header;
        header.type = CompressionType::Dictionary;
        header.element_count = static_cast<uint32_t>(element_count);
        header.compressed_size = static_cast<uint32_t>(payload_size);
        header.uncompressed_size = static_cast<uint32_t>(size);

        std::memcpy(result.data(), &header, sizeof(CompressedBlockHeader));

        uint8_t* dest = result.data() + sizeof(CompressedBlockHeader);
        
        std::memcpy(dest, &dict_size, sizeof(uint32_t));
        dest += sizeof(uint32_t);

        std::memcpy(dest, dictionary.data(), dict_size * sizeof(T));
        dest += dict_size * sizeof(T);

        std::memcpy(dest, indices.data(), element_count * sizeof(uint32_t));

        return result;
    }

    std::vector<uint8_t> decompress(const uint8_t* compressed_data, size_t compressed_size, size_t expected_element_count) override {
        if (compressed_size < sizeof(CompressedBlockHeader)) {
            throw CompressionException("Compressed data size smaller than header size.");
        }

        CompressedBlockHeader header;
        std::memcpy(&header, compressed_data, sizeof(CompressedBlockHeader));

        if (header.type != CompressionType::Dictionary) {
            throw CompressionException("Invalid compression type for Dictionary decompressor.");
        }

        if (header.element_count != expected_element_count) {
            throw CompressionException("Element count mismatch during Dictionary decompression.");
        }

        if (expected_element_count == 0) {
            return std::vector<uint8_t>();
        }

        const uint8_t* src = compressed_data + sizeof(CompressedBlockHeader);
        const uint8_t* src_end = compressed_data + compressed_size;

        if (src + sizeof(uint32_t) > src_end) {
            throw CompressionException("Corrupted Dictionary stream: missing dictionary size.");
        }

        uint32_t dict_size;
        std::memcpy(&dict_size, src, sizeof(uint32_t));
        src += sizeof(uint32_t);

        size_t dict_bytes = dict_size * sizeof(T);
        size_t indices_bytes = expected_element_count * sizeof(uint32_t);

        if (src + dict_bytes + indices_bytes > src_end) {
            throw CompressionException("Corrupted Dictionary stream: buffer overflow on read.");
        }

        const T* dictionary = reinterpret_cast<const T*>(src);
        src += dict_bytes;

        const uint32_t* indices = reinterpret_cast<const uint32_t*>(src);

        std::vector<uint8_t> result(header.uncompressed_size);
        T* dest = reinterpret_cast<T*>(result.data());

        for (size_t i = 0; i < expected_element_count; ++i) {
            uint32_t id = indices[i];
            if (id >= dict_size) {
                throw CompressionException("Corrupted Dictionary stream: index out of bounds.");
            }
            dest[i] = dictionary[id];
        }

        return result;
    }
};

class CompressionManager {
public:
    template <typename T>
    static std::vector<uint8_t> compress(CompressionType type, const std::vector<T>& data) {
        const uint8_t* raw_data = reinterpret_cast<const uint8_t*>(data.data());
        size_t size = data.size() * sizeof(T);
        size_t element_count = data.size();

        if (type == CompressionType::RunLengthEncoding) {
            RLECompressor<T> compressor;
            return compressor.compress(raw_data, size, element_count);
        } else if (type == CompressionType::Dictionary) {
            DictionaryCompressor<T> compressor;
            return compressor.compress(raw_data, size, element_count);
        } else {
            CompressedBlockHeader header{CompressionType::Uncompressed, static_cast<uint32_t>(element_count), static_cast<uint32_t>(size), static_cast<uint32_t>(size)};
            std::vector<uint8_t> result(sizeof(CompressedBlockHeader) + size);
            std::memcpy(result.data(), &header, sizeof(CompressedBlockHeader));
            std::memcpy(result.data() + sizeof(CompressedBlockHeader), raw_data, size);
            return result;
        }
    }

    template <typename T>
    static std::vector<T> decompress(const std::vector<uint8_t>& compressed_block, size_t expected_element_count) {
        if (compressed_block.size() < sizeof(CompressedBlockHeader)) {
            throw CompressionException("Block size too small to contain a valid header.");
        }

        CompressedBlockHeader header;
        std::memcpy(&header, compressed_block.data(), sizeof(CompressedBlockHeader));

        std::vector<uint8_t> raw_bytes;

        if (header.type == CompressionType::RunLengthEncoding) {
            RLECompressor<T> compressor;
            raw_bytes = compressor.decompress(compressed_block.data(), compressed_block.size(), expected_element_count);
        } else if (header.type == CompressionType::Dictionary) {
            DictionaryCompressor<T> compressor;
            raw_bytes = compressor.decompress(compressed_block.data(), compressed_block.size(), expected_element_count);
        } else if (header.type == CompressionType::Uncompressed) {
            if (compressed_block.size() < sizeof(CompressedBlockHeader) + header.uncompressed_size) {
                throw CompressionException("Uncompressed block size mismatch.");
            }
            raw_bytes.resize(header.uncompressed_size);
            std::memcpy(raw_bytes.data(), compressed_block.data() + sizeof(CompressedBlockHeader), header.uncompressed_size);
        } else {
            throw CompressionException("Unknown compression type encountered.");
        }

        std::vector<T> result(expected_element_count);
        std::memcpy(result.data(), raw_bytes.data(), raw_bytes.size());
        return result;
    }
};

} // namespace VectorDB

#endif // VECTOR_DB_ENGINE_COMPRESSION_HPP