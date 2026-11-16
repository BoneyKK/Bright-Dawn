// A directory of its own for one test, removed afterwards.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace bd_test {

class TempDir {
 public:
  TempDir() {
    static std::atomic<int> counter{0};
    path_ = std::filesystem::temp_directory_path() /
            ("bd_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "_" + std::to_string(counter++));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  const std::filesystem::path& path() const { return path_; }
  void write(const std::string& name, const std::string& content) const {
    std::ofstream out(path_ / name, std::ios::binary);
    out << content;
  }
  std::string read(const std::string& name) const {
    std::ifstream in(path_ / name, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

 private:
  std::filesystem::path path_;
};

}  // namespace bd_test
