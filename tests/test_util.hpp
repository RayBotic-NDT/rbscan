#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <raybotic/scan/types.hpp>
#include <string>

namespace rbscan_test {

/// Fresh directory per test, removed afterwards.
class TempDir {
  public:
    TempDir() {
        static std::atomic<int> counter{0};
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / "rbscan_tests" /
                (std::string(info ? info->name() : "test") + "_" + std::to_string(stamp) +
                 "_" + std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

/// `sample` repeated `n` times as one column [n, sample shape...].
inline raybotic::scan::Array stack(const raybotic::scan::Array& sample, std::size_t n) {
    raybotic::scan::Shape shape{n};
    shape.insert(shape.end(), sample.shape().begin(), sample.shape().end());
    raybotic::scan::Array out(sample.dtype(), shape);
    for (std::size_t i = 0; i < n; ++i) {
        std::memcpy(static_cast<std::byte*>(out.data()) + i * sample.byte_size(),
                    sample.data(), sample.byte_size());
    }
    return out;
}

/// Same dtype, shape and bytes.
inline bool same(const raybotic::scan::Array& a, const raybotic::scan::Array& b) {
    return a.dtype() == b.dtype() && a.shape() == b.shape() &&
           std::memcmp(a.data(), b.data(), a.byte_size()) == 0;
}

}  // namespace rbscan_test
