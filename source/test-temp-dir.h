#ifndef TEST_TEMP_DIR_H
#define TEST_TEMP_DIR_H

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

// Fixture paths for tests.  See findings/test-fixture-paths.md.

inline std::string test_temp_root() {
  char const* env = getenv("TMPDIR");
  if (env == NULL || env[0] == '\0') return "/tmp/";
  std::string root(env);
  if (root[root.size() - 1] != '/') root.push_back('/');
  return root;
}

inline std::vector<std::string>& test_temp_dir_registry() {
  static std::vector<std::string> registry;
  return registry;
}

inline int test_temp_dir_remove_entry(char const* path, struct stat const*,
                                      int, struct FTW*) {
  return remove(path);
}

inline void test_temp_dir_remove_tree(std::string const& path) {
  nftw(path.c_str(), test_temp_dir_remove_entry, 8, FTW_DEPTH | FTW_PHYS);
}

inline void test_temp_dir_cleanup() {
  std::vector<std::string>& registry = test_temp_dir_registry();
  for (size_t i = 0; i < registry.size(); ++i)
    test_temp_dir_remove_tree(registry[i]);
  registry.clear();
}

class TestTempDir {
 public:
  explicit TestTempDir(char const* name) {
    std::string const pattern =
        test_temp_root() + "nutrimatic-" + name + ".XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char const* const made = mkdtemp(&buffer[0]);
    if (made == NULL) {
      fprintf(stderr, "FAIL: could not create fixture directory under %s\n",
              test_temp_root().c_str());
      exit(1);
    }
    path_ = made;
    std::vector<std::string>& registry = test_temp_dir_registry();
    static bool registered = false;
    if (!registered) {
      atexit(test_temp_dir_cleanup);
      registered = true;
    }
    registry.push_back(path_);
  }

  ~TestTempDir() {
    std::vector<std::string>& registry = test_temp_dir_registry();
    for (size_t i = 0; i < registry.size(); ++i) {
      if (registry[i] == path_) {
        registry.erase(registry.begin() + i);
        break;
      }
    }
    test_temp_dir_remove_tree(path_);
  }

  TestTempDir(TestTempDir const&) = delete;
  TestTempDir& operator=(TestTempDir const&) = delete;

  std::string const& path() const { return path_; }
  std::string file(char const* name) const { return path_ + "/" + name; }

 private:
  std::string path_;
};

#endif  // TEST_TEMP_DIR_H
