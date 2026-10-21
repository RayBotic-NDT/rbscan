#include "compression.hpp"

#include <libdeflate.h>

#include <cstring>
#include <memory>

namespace raybotic::scan::detail {

namespace {

struct CompressorDeleter {
    void operator()(libdeflate_compressor* c) const { libdeflate_free_compressor(c); }
};

/// One compressor per thread and level; they are not thread-safe and cost a
/// few hundred KB each, so they are kept.
libdeflate_compressor* compressor(int level) {
    thread_local std::unique_ptr<libdeflate_compressor, CompressorDeleter> cache[13];
    level = level < 0 ? 1 : (level > 12 ? 12 : level);
    auto& slot = cache[level];
    if (!slot) {
        slot.reset(libdeflate_alloc_compressor(level));
        if (!slot) throw Error("libdeflate_alloc_compressor failed");
    }
    return slot.get();
}

/// HDF5's shuffle: byte k of every element goes into plane k.
void shuffle(const std::byte* in, std::byte* out, std::size_t size, std::size_t element_size) {
    const std::size_t n = size / element_size;
    for (std::size_t k = 0; k < element_size; ++k) {
        std::byte* plane = out + k * n;
        const std::byte* src = in + k;
        for (std::size_t i = 0; i < n; ++i) plane[i] = src[i * element_size];
    }
    // Trailing bytes that do not form a whole element stay where they are.
    const std::size_t rest = size - n * element_size;
    if (rest > 0) std::memcpy(out + n * element_size, in + n * element_size, rest);
}

struct DecompressorDeleter {
    void operator()(libdeflate_decompressor* d) const { libdeflate_free_decompressor(d); }
};

libdeflate_decompressor* decompressor() {
    thread_local std::unique_ptr<libdeflate_decompressor, DecompressorDeleter> d(
        libdeflate_alloc_decompressor());
    if (!d) throw Error("libdeflate_alloc_decompressor failed");
    return d.get();
}

/// Inverse of shuffle().
void unshuffle(const std::byte* in, std::byte* out, std::size_t size, std::size_t element_size) {
    const std::size_t n = size / element_size;
    for (std::size_t k = 0; k < element_size; ++k) {
        const std::byte* plane = in + k * n;
        std::byte* dst = out + k;
        for (std::size_t i = 0; i < n; ++i) dst[i * element_size] = plane[i];
    }
    const std::size_t rest = size - n * element_size;
    if (rest > 0) std::memcpy(out + n * element_size, in + n * element_size, rest);
}

}  // namespace

void decode_chunk(const std::byte* data, std::size_t size, std::byte* out, std::size_t out_size,
                  std::size_t element_size, bool deflated, bool shuffled) {
    shuffled = shuffled && element_size > 1;
    std::vector<std::byte> inflated;
    const std::byte* plain = data;
    std::size_t plain_size = size;
    if (deflated) {
        // Straight into `out` unless a shuffle still has to follow.
        std::byte* target = out;
        if (shuffled) {
            inflated.resize(out_size);
            target = inflated.data();
        }
        std::size_t written = 0;
        const auto result = libdeflate_zlib_decompress(decompressor(), data, size, target,
                                                       out_size, &written);
        if (result != LIBDEFLATE_SUCCESS || written != out_size) {
            throw Error("corrupt or truncated deflate chunk");
        }
        if (!shuffled) return;
        plain = inflated.data();
        plain_size = written;
    }
    if (plain_size != out_size) throw Error("chunk has an unexpected size");
    if (shuffled) {
        unshuffle(plain, out, out_size, element_size);
    } else {
        std::memcpy(out, plain, out_size);
    }
}

std::vector<std::byte> encode_chunk(const std::byte* data, std::size_t size,
                                    std::size_t element_size,
                                    const CompressionOptions& options) {
    if (options.method == Compression::None) return {data, data + size};

    std::vector<std::byte> shuffled;
    const std::byte* input = data;
    if (options.method == Compression::ShuffleDeflate && element_size > 1) {
        shuffled.resize(size);
        shuffle(data, shuffled.data(), size, element_size);
        input = shuffled.data();
    }

    auto* c = compressor(options.level);
    std::vector<std::byte> out(libdeflate_zlib_compress_bound(c, size));
    const std::size_t written =
        libdeflate_zlib_compress(c, input, size, out.data(), out.size());
    if (written == 0) throw Error("libdeflate_zlib_compress failed");
    out.resize(written);
    return out;
}

void set_filters(hid_t dcpl, const CompressionOptions& options) {
    switch (options.method) {
        case Compression::None: return;
        case Compression::ShuffleDeflate:
            h5::check(H5Pset_shuffle(dcpl), "H5Pset_shuffle");
            [[fallthrough]];
        case Compression::Deflate: {
            const int level = options.level < 0 ? 1 : (options.level > 9 ? 9 : options.level);
            h5::check(H5Pset_deflate(dcpl, static_cast<unsigned>(level)), "H5Pset_deflate");
            return;
        }
    }
}

}  // namespace raybotic::scan::detail
