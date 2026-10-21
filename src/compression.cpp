#include "compression.hpp"

#include <libdeflate.h>
#include <lz4.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define RBSCAN_SSE2 1
#endif

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
void shuffle(const std::byte* in, std::byte* out, std::size_t size,
             std::size_t element_size) {
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
void unshuffle(const std::byte* in, std::byte* out, std::size_t size,
               std::size_t element_size) {
    const std::size_t n = size / element_size;
    for (std::size_t k = 0; k < element_size; ++k) {
        const std::byte* plane = in + k * n;
        std::byte* dst = out + k;
        for (std::size_t i = 0; i < n; ++i) dst[i * element_size] = plane[i];
    }
    const std::size_t rest = size - n * element_size;
    if (rest > 0) std::memcpy(out + n * element_size, in + n * element_size, rest);
}

// --- Bitshuffle ------------------------------------------------------------------
//
// Compatible with https://github.com/kiyo-masui/bitshuffle (HDF5 filter 32008,
// LZ4 variant). A chunk is
//   uint64 BE  uncompressed bytes
//   uint32 BE  block size in bytes
//   per block: uint32 BE compressed size, LZ4 block of the bit-shuffled block
//   the last size % 8 elements, raw
// Blocks hold `block` elements (a multiple of 8); the last one may be smaller
// (rounded down to a multiple of 8). Bit-shuffled: bit row r = 8 * byte + bit
// holds that bit of every element, element i at bit i % 8 of byte i / 8.

constexpr std::size_t bshuf_target_block_bytes = 8192;
constexpr std::size_t bshuf_min_block = 128;
constexpr std::size_t bshuf_header = 12;

/// The reference implementation's default block size; must never change.
std::size_t bshuf_default_block(std::size_t element_size) {
    const std::size_t block = bshuf_target_block_bytes / element_size / 8 * 8;
    return std::max(block, bshuf_min_block);
}

std::uint64_t read_be(const std::byte* p, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = (v << 8) | std::to_integer<std::uint64_t>(p[i]);
    return v;
}

void write_be(std::byte* p, std::uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) {
        p[i] = static_cast<std::byte>(v & 0xff);
        v >>= 8;
    }
}

/// Transposes the 8x8 bit matrix held in x (byte m = row m).
inline std::uint64_t transpose_8x8(std::uint64_t x) {
    std::uint64_t t = (x ^ (x >> 7)) & 0x00AA00AA00AA00AAull;
    x = x ^ t ^ (t << 7);
    t = (x ^ (x >> 14)) & 0x0000CCCC0000CCCCull;
    x = x ^ t ^ (t << 14);
    t = (x ^ (x >> 28)) & 0x00000000F0F0F0F0ull;
    return x ^ t ^ (t << 28);
}

/// Bit-shuffles n elements (n % 8 == 0).
void bitshuffle(const std::byte* in, std::byte* out, std::size_t n,
                std::size_t element_size) {
    const auto* src = reinterpret_cast<const std::uint8_t*>(in);
    auto* dst = reinterpret_cast<std::uint8_t*>(out);
    const std::size_t row = n / 8;
    std::size_t g = 0;
#ifdef RBSCAN_SSE2
    if (element_size == 2) {
        const __m128i mask = _mm_set1_epi16(0x00ff);
        for (; g + 1 < row; g += 2) {
            const std::uint8_t* e = src + g * 16;
            const __m128i v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(e));
            const __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(e + 16));
            __m128i p0 =
                _mm_packus_epi16(_mm_and_si128(v0, mask), _mm_and_si128(v1, mask));
            __m128i p1 = _mm_packus_epi16(_mm_srli_epi16(v0, 8), _mm_srli_epi16(v1, 8));
            std::uint8_t* d0 = dst + g;
            std::uint8_t* d1 = dst + 8 * row + g;
            for (int k = 7; k >= 0; --k) {
                const auto m0 = static_cast<std::uint16_t>(_mm_movemask_epi8(p0));
                const auto m1 = static_cast<std::uint16_t>(_mm_movemask_epi8(p1));
                std::memcpy(d0 + static_cast<std::size_t>(k) * row, &m0, 2);
                std::memcpy(d1 + static_cast<std::size_t>(k) * row, &m1, 2);
                p0 = _mm_add_epi8(p0, p0);
                p1 = _mm_add_epi8(p1, p1);
            }
        }
    } else if (element_size == 4) {
        const __m128i mask = _mm_set1_epi16(0x00ff);
        for (; g + 1 < row; g += 2) {
            const std::uint8_t* e = src + g * 32;
            const __m128i v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(e));
            const __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(e + 16));
            const __m128i v2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(e + 32));
            const __m128i v3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(e + 48));
            const __m128i lo0 =
                _mm_packus_epi16(_mm_and_si128(v0, mask), _mm_and_si128(v1, mask));
            const __m128i lo1 =
                _mm_packus_epi16(_mm_and_si128(v2, mask), _mm_and_si128(v3, mask));
            const __m128i hi0 =
                _mm_packus_epi16(_mm_srli_epi16(v0, 8), _mm_srli_epi16(v1, 8));
            const __m128i hi1 =
                _mm_packus_epi16(_mm_srli_epi16(v2, 8), _mm_srli_epi16(v3, 8));
            __m128i p0 =
                _mm_packus_epi16(_mm_and_si128(lo0, mask), _mm_and_si128(lo1, mask));
            __m128i p1 =
                _mm_packus_epi16(_mm_and_si128(hi0, mask), _mm_and_si128(hi1, mask));
            __m128i p2 = _mm_packus_epi16(_mm_srli_epi16(lo0, 8), _mm_srli_epi16(lo1, 8));
            __m128i p3 = _mm_packus_epi16(_mm_srli_epi16(hi0, 8), _mm_srli_epi16(hi1, 8));
            std::uint8_t* d0 = dst + g;
            std::uint8_t* d1 = dst + 8 * row + g;
            std::uint8_t* d2 = dst + 16 * row + g;
            std::uint8_t* d3 = dst + 24 * row + g;
            for (int k = 7; k >= 0; --k) {
                const auto m0 = static_cast<std::uint16_t>(_mm_movemask_epi8(p0));
                const auto m1 = static_cast<std::uint16_t>(_mm_movemask_epi8(p1));
                const auto m2 = static_cast<std::uint16_t>(_mm_movemask_epi8(p2));
                const auto m3 = static_cast<std::uint16_t>(_mm_movemask_epi8(p3));
                const std::size_t off = static_cast<std::size_t>(k) * row;
                std::memcpy(d0 + off, &m0, 2);
                std::memcpy(d1 + off, &m1, 2);
                std::memcpy(d2 + off, &m2, 2);
                std::memcpy(d3 + off, &m3, 2);
                p0 = _mm_add_epi8(p0, p0);
                p1 = _mm_add_epi8(p1, p1);
                p2 = _mm_add_epi8(p2, p2);
                p3 = _mm_add_epi8(p3, p3);
            }
        }
    }
#endif
    for (; g < row; ++g) {
        const std::uint8_t* e = src + g * 8 * element_size;
        for (std::size_t j = 0; j < element_size; ++j) {
            std::uint64_t x = 0;
            for (std::size_t m = 0; m < 8; ++m) {
                x |= std::uint64_t{e[m * element_size + j]} << (8 * m);
            }
            x = transpose_8x8(x);
            std::uint8_t* d = dst + 8 * j * row + g;
            for (std::size_t k = 0; k < 8; ++k) {
                d[k * row] = static_cast<std::uint8_t>(x >> (8 * k));
            }
        }
    }
}

/// Inverse of bitshuffle().
void bitunshuffle(const std::byte* in, std::byte* out, std::size_t n,
                  std::size_t element_size) {
    const auto* src = reinterpret_cast<const std::uint8_t*>(in);
    auto* dst = reinterpret_cast<std::uint8_t*>(out);
    const std::size_t row = n / 8;
    std::size_t g = 0;
#ifdef RBSCAN_SSE2
    auto load_rows = [row](const std::uint8_t* s) {
        std::uint16_t r[8];
        for (std::size_t k = 0; k < 8; ++k) std::memcpy(&r[k], s + k * row, 2);
        return _mm_loadu_si128(reinterpret_cast<const __m128i*>(r));
    };
    const __m128i mask = _mm_set1_epi16(0x00ff);
    if (element_size == 2) {
        for (; g + 1 < row; g += 2) {
            const __m128i w0 = load_rows(src + g);
            const __m128i w1 = load_rows(src + 8 * row + g);
            __m128i vg0 =
                _mm_packus_epi16(_mm_and_si128(w0, mask), _mm_and_si128(w1, mask));
            __m128i vg1 = _mm_packus_epi16(_mm_srli_epi16(w0, 8), _mm_srli_epi16(w1, 8));
            std::uint8_t* e = dst + g * 16;
            for (int m = 7; m >= 0; --m) {
                const auto u0 = static_cast<std::uint16_t>(_mm_movemask_epi8(vg0));
                const auto u1 = static_cast<std::uint16_t>(_mm_movemask_epi8(vg1));
                std::memcpy(e + m * 2, &u0, 2);
                std::memcpy(e + (8 + m) * 2, &u1, 2);
                vg0 = _mm_add_epi8(vg0, vg0);
                vg1 = _mm_add_epi8(vg1, vg1);
            }
        }
    } else if (element_size == 4) {
        for (; g + 1 < row; g += 2) {
            const __m128i w0 = load_rows(src + g);
            const __m128i w1 = load_rows(src + 8 * row + g);
            const __m128i w2 = load_rows(src + 16 * row + g);
            const __m128i w3 = load_rows(src + 24 * row + g);
            __m128i lo0 =
                _mm_packus_epi16(_mm_and_si128(w0, mask), _mm_and_si128(w1, mask));
            __m128i lo1 = _mm_packus_epi16(_mm_srli_epi16(w0, 8), _mm_srli_epi16(w1, 8));
            __m128i hi0 =
                _mm_packus_epi16(_mm_and_si128(w2, mask), _mm_and_si128(w3, mask));
            __m128i hi1 = _mm_packus_epi16(_mm_srli_epi16(w2, 8), _mm_srli_epi16(w3, 8));
            std::uint8_t* e = dst + g * 32;
            for (int m = 7; m >= 0; --m) {
                const std::uint32_t u0 =
                    static_cast<std::uint16_t>(_mm_movemask_epi8(lo0)) |
                    (static_cast<std::uint32_t>(
                         static_cast<std::uint16_t>(_mm_movemask_epi8(hi0)))
                     << 16);
                const std::uint32_t u1 =
                    static_cast<std::uint16_t>(_mm_movemask_epi8(lo1)) |
                    (static_cast<std::uint32_t>(
                         static_cast<std::uint16_t>(_mm_movemask_epi8(hi1)))
                     << 16);
                std::memcpy(e + m * 4, &u0, 4);
                std::memcpy(e + (8 + m) * 4, &u1, 4);
                lo0 = _mm_add_epi8(lo0, lo0);
                lo1 = _mm_add_epi8(lo1, lo1);
                hi0 = _mm_add_epi8(hi0, hi0);
                hi1 = _mm_add_epi8(hi1, hi1);
            }
        }
    }
#endif
    for (; g < row; ++g) {
        std::uint8_t* e = dst + g * 8 * element_size;
        for (std::size_t j = 0; j < element_size; ++j) {
            const std::uint8_t* s = src + 8 * j * row + g;
            std::uint64_t x = 0;
            for (std::size_t k = 0; k < 8; ++k) x |= std::uint64_t{s[k * row]} << (8 * k);
            x = transpose_8x8(x);
            for (std::size_t m = 0; m < 8; ++m) {
                e[m * element_size + j] = static_cast<std::uint8_t>(x >> (8 * m));
            }
        }
    }
}

/// Calls fn(first_element, count) for every block and returns the index of the
/// first leftover element (those are stored raw).
template <class Fn>
std::size_t for_each_block(std::size_t n, std::size_t block, Fn&& fn) {
    std::size_t i = 0;
    for (; i + block <= n; i += block) fn(i, block);
    const std::size_t last = (n - i) / 8 * 8;
    if (last > 0) {
        fn(i, last);
        i += last;
    }
    return i;
}

std::vector<std::byte> encode_bitshuffle_lz4(const std::byte* data, std::size_t size,
                                             std::size_t element_size,
                                             std::size_t block) {
    if (element_size == 0 || size % element_size != 0) {
        throw Error("bitshuffle: chunk is not a whole number of elements");
    }
    if (block == 0) block = bshuf_default_block(element_size);
    if (block % 8 != 0) throw Error("bitshuffle: block size must be a multiple of 8");
    const std::size_t n = size / element_size;
    const std::size_t block_bytes = block * element_size;
    if (block_bytes > LZ4_MAX_INPUT_SIZE) throw Error("bitshuffle: block too large");

    const auto bound =
        static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(block_bytes)));
    std::vector<std::byte> out(bshuf_header + size +
                               (n / block + 1) * (4 + bound - block_bytes));
    write_be(out.data(), size, 8);
    write_be(out.data() + 8, block_bytes, 4);
    std::vector<std::byte> shuffled(block_bytes);
    std::size_t pos = bshuf_header;
    const std::size_t rest =
        for_each_block(n, block, [&](std::size_t first, std::size_t count) {
            const std::size_t bytes = count * element_size;
            bitshuffle(data + first * element_size, shuffled.data(), count, element_size);
            const int written = LZ4_compress_default(
                reinterpret_cast<const char*>(shuffled.data()),
                reinterpret_cast<char*>(out.data() + pos + 4), static_cast<int>(bytes),
                static_cast<int>(out.size() - pos - 4));
            if (written <= 0) throw Error("LZ4_compress_default failed");
            write_be(out.data() + pos, static_cast<std::uint64_t>(written), 4);
            pos += 4 + static_cast<std::size_t>(written);
        });
    const std::size_t left = size - rest * element_size;
    std::memcpy(out.data() + pos, data + rest * element_size, left);
    out.resize(pos + left);
    return out;
}

// --- HDF5 filter 32008 ----------------------------------------------------------
//
// cd_values: [0] major, [1] minor version of the writer, [2] element size,
// [3] block size in elements (0 = default), [4] compression (2 = LZ4).

constexpr unsigned bshuf_version_major = 0;
constexpr unsigned bshuf_version_minor = 5;

herr_t bitshuffle_set_local(hid_t dcpl, hid_t type, hid_t /*space*/) {
    unsigned flags = 0;
    std::size_t n = 8;
    unsigned values[8] = {};
    if (H5Pget_filter_by_id2(dcpl, bitshuffle_filter_id, &flags, &n, values, 0, nullptr,
                             nullptr) < 0) {
        return -1;
    }
    // Like the reference plugin: user values (block size, compression) given
    // without the three reserved slots move behind them.
    if (n < 3) {
        for (std::size_t i = n; i-- > 0;) values[i + 3] = values[i];
        n += 3;
    }
    const std::size_t element_size = H5Tget_size(type);
    if (element_size == 0) return -1;
    values[0] = bshuf_version_major;
    values[1] = bshuf_version_minor;
    values[2] = static_cast<unsigned>(element_size);
    return H5Pmodify_filter(dcpl, bitshuffle_filter_id, flags, n, values);
}

std::size_t bitshuffle_filter(unsigned flags, std::size_t cd_nelmts,
                              const unsigned cd_values[], std::size_t nbytes,
                              std::size_t* buf_size, void** buf) {
    // Only the LZ4 variant (plain bitshuffle and zstd are not implemented).
    if (cd_nelmts < 5 || cd_values[4] != bitshuffle_lz4 || cd_values[2] == 0) return 0;
    void* result = nullptr;
    try {
        const std::size_t element_size = cd_values[2];
        const auto* in = static_cast<const std::byte*>(*buf);
        std::size_t result_size = 0;
        if ((flags & H5Z_FLAG_REVERSE) != 0) {
            if (nbytes < bshuf_header) return 0;
            result_size = static_cast<std::size_t>(read_be(in, 8));
            result = H5allocate_memory(result_size, false);
            if (result == nullptr) return 0;
            decode_bitshuffle_lz4(in, nbytes, static_cast<std::byte*>(result),
                                  result_size, element_size);
        } else {
            const auto encoded =
                encode_bitshuffle_lz4(in, nbytes, element_size, cd_values[3]);
            result_size = encoded.size();
            result = H5allocate_memory(result_size, false);
            if (result == nullptr) return 0;
            std::memcpy(result, encoded.data(), result_size);
        }
        H5free_memory(*buf);
        *buf = result;
        *buf_size = result_size;
        return result_size;
    } catch (...) {
        if (result != nullptr) H5free_memory(result);
        return 0;
    }
}

}  // namespace

void decode_bitshuffle_lz4(const std::byte* data, std::size_t size, std::byte* out,
                           std::size_t out_size, std::size_t element_size) {
    if (size < bshuf_header) throw Error("bitshuffle chunk too short");
    if (read_be(data, 8) != out_size)
        throw Error("bitshuffle chunk has an unexpected size");
    const auto block_bytes = static_cast<std::size_t>(read_be(data + 8, 4));
    if (element_size == 0 || out_size % element_size != 0 || block_bytes == 0 ||
        block_bytes % element_size != 0 || (block_bytes / element_size) % 8 != 0) {
        throw Error("corrupt bitshuffle chunk header");
    }
    const std::size_t n = out_size / element_size;
    thread_local std::vector<std::byte> shuffled;
    shuffled.resize(block_bytes);
    std::size_t pos = bshuf_header;
    const std::size_t rest = for_each_block(
        n, block_bytes / element_size, [&](std::size_t first, std::size_t count) {
            const std::size_t bytes = count * element_size;
            if (pos + 4 > size) throw Error("truncated bitshuffle chunk");
            const auto packed = static_cast<std::size_t>(read_be(data + pos, 4));
            pos += 4;
            if (packed > size - pos) throw Error("truncated bitshuffle chunk");
            const int got =
                LZ4_decompress_safe(reinterpret_cast<const char*>(data + pos),
                                    reinterpret_cast<char*>(shuffled.data()),
                                    static_cast<int>(packed), static_cast<int>(bytes));
            if (got < 0 || static_cast<std::size_t>(got) != bytes) {
                throw Error("corrupt LZ4 block in bitshuffle chunk");
            }
            bitunshuffle(shuffled.data(), out + first * element_size, count,
                         element_size);
            pos += packed;
        });
    const std::size_t left = out_size - rest * element_size;
    if (left > size - pos) throw Error("truncated bitshuffle chunk");
    std::memcpy(out + rest * element_size, data + pos, left);
}

void register_filters() {
    if (H5Zfilter_avail(bitshuffle_filter_id) > 0) return;
    static const H5Z_class2_t bitshuffle_class = {
        H5Z_CLASS_T_VERS,
        bitshuffle_filter_id,
        1,
        1,
        "bitshuffle (rbscan); see https://github.com/kiyo-masui/bitshuffle",
        nullptr,
        bitshuffle_set_local,
        bitshuffle_filter,
    };
    h5::check(H5Zregister(&bitshuffle_class), "H5Zregister bitshuffle");
}

void decode_chunk(const std::byte* data, std::size_t size, std::byte* out,
                  std::size_t out_size, std::size_t element_size, bool deflated,
                  bool shuffled) {
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
    if (options.method == Compression::BitshuffleLZ4) {
        return encode_bitshuffle_lz4(data, size, element_size, 0);
    }

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
        case Compression::None:
            return;
        case Compression::BitshuffleLZ4: {
            // set_local fills in the version and the element size.
            const unsigned values[5] = {0, 0, 0, 0, bitshuffle_lz4};
            h5::check(
                H5Pset_filter(dcpl, bitshuffle_filter_id, H5Z_FLAG_MANDATORY, 5, values),
                "H5Pset_filter bitshuffle");
            return;
        }
        case Compression::ShuffleDeflate:
            h5::check(H5Pset_shuffle(dcpl), "H5Pset_shuffle");
            [[fallthrough]];
        case Compression::Deflate: {
            const int level =
                options.level < 0 ? 1 : (options.level > 9 ? 9 : options.level);
            h5::check(H5Pset_deflate(dcpl, static_cast<unsigned>(level)),
                      "H5Pset_deflate");
            return;
        }
    }
}

}  // namespace raybotic::scan::detail
