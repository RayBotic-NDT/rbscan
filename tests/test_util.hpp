#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
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

}  // namespace rbscan_test
