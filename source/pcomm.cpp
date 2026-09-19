#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

struct PairState {
  bool matched = false;
};

struct PairHash {
  using is_transparent = void;

  size_t operator()(std::string_view value) const {
    return std::hash<std::string_view>{}(value);
  }
};

struct PairEqual {
  using is_transparent = void;

  bool operator()(std::string_view left, std::string_view right) const {
    return left == right;
  }
};

using PairMap = std::unordered_map<std::string, PairState, PairHash, PairEqual>;

struct PairSet {
  PairMap index;
  // Node addresses remain stable when the hash table grows.
  std::vector<PairMap::value_type*> order;
};

void usage(FILE* out, char const* program) {
  fprintf(out,
      "usage: %s [-123] FILE1 FILE2\n"
      "  compare word,word lines with either word order considered equal\n"
      "  -1  suppress pairs only in FILE1\n"
      "  -2  suppress pairs only in FILE2\n"
      "  -3  suppress pairs in both files\n"
      "  columns use comm's tab prefixes; shared pairs use FILE1's spelling\n"
      "  scanned-file rows precede unmatched in-memory rows\n"
      "  repeated scanned rows print repeatedly; the set side is deduplicated\n"
      "  each input line must have exactly two nonempty comma-separated words\n",
      program);
}

bool reverse_pair(std::string_view line, char const* path, size_t number,
                  std::string* reverse) {
  size_t const comma = line.find(',');
  if (comma == 0 || comma == std::string_view::npos ||
      comma + 1 == line.size() ||
      line.find(',', comma + 1) != std::string_view::npos) {
    fprintf(stderr, "pcomm: %s:%zu: expected word,word\n", path, number);
    return false;
  }
  reverse->assign(line.substr(comma + 1));
  reverse->push_back(',');
  reverse->append(line.substr(0, comma));
  return true;
}

// Map regular files. For files that cannot be mapped, read one line at a time.
template <typename Visit>
bool for_each_line(char const* path, Visit visit) {
  int const fd = open(path, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "pcomm: %s: %s\n", path, strerror(errno));
    return false;
  }

  struct stat status;
  if (fstat(fd, &status) != 0) {
    fprintf(stderr, "pcomm: %s: %s\n", path, strerror(errno));
    close(fd);
    return false;
  }

  if (S_ISREG(status.st_mode) && status.st_size == 0) {
    close(fd);
    return true;
  }
  if (S_ISREG(status.st_mode) && status.st_size > 0 &&
      static_cast<uintmax_t>(status.st_size) <= SIZE_MAX) {
    size_t const size = static_cast<size_t>(status.st_size);
    void* const mapped = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped != MAP_FAILED) {
      close(fd);
      char const* const data = static_cast<char const*>(mapped);
      size_t start = 0;
      size_t number = 0;
      bool ok = true;
      while (start < size) {
        char const* const newline = static_cast<char const*>(
            memchr(data + start, '\n', size - start));
        size_t const end = newline == NULL ? size : size_t(newline - data);
        std::string_view line(data + start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!visit(line, ++number)) {
          ok = false;
          break;
        }
        start = end == size ? size : end + 1;
      }
      munmap(mapped, size);
      return ok;
    }
  }

  FILE* const input = fdopen(fd, "rb");
  if (input == NULL) {
    fprintf(stderr, "pcomm: %s: %s\n", path, strerror(errno));
    close(fd);
    return false;
  }
  char* buffer = NULL;
  size_t capacity = 0;
  size_t number = 0;
  bool ok = true;
  ssize_t length;
  while ((length = getline(&buffer, &capacity, input)) >= 0) {
    std::string_view line(buffer, size_t(length));
    if (!line.empty() && line.back() == '\n') line.remove_suffix(1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!visit(line, ++number)) {
      ok = false;
      break;
    }
  }
  if (ferror(input)) {
    fprintf(stderr, "pcomm: %s: %s\n", path, strerror(errno));
    ok = false;
  }
  free(buffer);
  if (fclose(input) != 0) {
    fprintf(stderr, "pcomm: %s: %s\n", path, strerror(errno));
    ok = false;
  }
  return ok;
}

PairMap::iterator find_pair(PairSet* set, std::string_view line,
                            std::string const& reverse) {
  auto found = set->index.find(line);
  if (found == set->index.end()) found = set->index.find(reverse);
  return found;
}

bool load_set(char const* path, PairSet* set) {
  return for_each_line(path, [&](std::string_view line, size_t number) {
    std::string reverse;
    if (!reverse_pair(line, path, number, &reverse)) return false;
    if (find_pair(set, line, reverse) != set->index.end()) return true;
    auto const inserted = set->index.emplace(std::string(line), PairState{});
    set->order.push_back(&*inserted.first);
    return true;
  });
}

bool print_pair(int column, std::string_view spelling, bool const show[3]) {
  if (!show[column]) return true;
  for (int i = 0; i < column; ++i)
    if (show[i] && putchar('\t') == EOF) return false;
  return fwrite(spelling.data(), 1, spelling.size(), stdout) ==
      spelling.size() && putchar('\n') != EOF;
}

// The streamed side's rows are emitted as they are read. The set side's
// unmatched rows follow in their original file order.
bool compare(char const* stream_path, char const* set_path,
             int stream_column, bool const show[3]) {
  PairSet set;
  if (!load_set(set_path, &set)) return false;
  bool const stream_is_first = stream_column == 0;
  if (!for_each_line(stream_path, [&](std::string_view line, size_t number) {
        std::string reverse;
        if (!reverse_pair(line, stream_path, number, &reverse)) return false;
        auto const found = find_pair(&set, line, reverse);
        if (found == set.index.end())
          return print_pair(stream_column, line, show);
        found->second.matched = true;
        std::string_view const spelling = stream_is_first
            ? line : std::string_view(found->first);
        return print_pair(2, spelling, show);
      })) return false;

  int const set_column = 1 - stream_column;
  for (PairMap::value_type const* pair : set.order)
    if (!pair->second.matched && !print_pair(set_column, pair->first, show))
      return false;
  return fflush(stdout) == 0;
}

bool smaller_first(char const* first, char const* second) {
  struct stat a, b;
  if (stat(first, &a) == 0 && stat(second, &b) == 0 &&
      S_ISREG(a.st_mode) && S_ISREG(b.st_mode))
    return a.st_size <= b.st_size;
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  bool show[3] = {true, true, true};
  char const* paths[2] = {NULL, NULL};
  int count = 0;
  bool options_done = false;
  for (int i = 1; i < argc; ++i) {
    std::string_view const arg(argv[i]);
    if (!options_done && (arg == "-h" || arg == "--help")) {
      usage(stdout, argv[0]);
      return 0;
    }
    if (!options_done && arg == "--") {
      options_done = true;
      continue;
    }
    if (!options_done && arg.size() > 1 && arg[0] == '-') {
      for (size_t j = 1; j < arg.size(); ++j) {
        if (arg[j] < '1' || arg[j] > '3') {
          fprintf(stderr, "pcomm: unknown option %s\n", argv[i]);
          usage(stderr, argv[0]);
          return 2;
        }
        show[arg[j] - '1'] = false;
      }
      continue;
    }
    if (count == 2) {
      fprintf(stderr, "pcomm: expected two PAIRS files\n");
      usage(stderr, argv[0]);
      return 2;
    }
    paths[count++] = argv[i];
  }
  if (count != 2) {
    fprintf(stderr, "pcomm: expected two PAIRS files\n");
    usage(stderr, argv[0]);
    return 2;
  }

  // A single requested exclusive column determines the streamed file. When
  // either both or neither exclusive columns are requested, map the larger
  // file and hold the smaller one in the set.
  bool stream_first;
  if (show[0] != show[1])
    stream_first = show[0];
  else
    stream_first = !smaller_first(paths[0], paths[1]);
  return compare(paths[stream_first ? 0 : 1],
                 paths[stream_first ? 1 : 0],
                 stream_first ? 0 : 1, show) ? 0 : 1;
}
