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
#include <utility>
#include <vector>

namespace {

std::pair<std::string_view, std::string_view> split_pair(
    std::string_view line) {
  size_t const comma = line.find(',');
  return {line.substr(0, comma), line.substr(comma + 1)};
}

// Either word order hashes and compares equal.
struct PairHash {
  size_t operator()(std::string_view line) const {
    auto [first, second] = split_pair(line);
    if (second < first) std::swap(first, second);
    std::hash<std::string_view> const hash;
    size_t const seed = hash(first);
    return seed ^ (hash(second) + 0x9e3779b97f4a7c15ULL + (seed << 6) +
                   (seed >> 2));
  }
};

struct PairEqual {
  bool operator()(std::string_view left, std::string_view right) const {
    if (left.size() != right.size()) return false;
    if (left == right) return true;
    auto const [left_first, left_second] = split_pair(left);
    auto const [right_first, right_second] = split_pair(right);
    return left_first == right_second && left_second == right_first;
  }
};

// Keys are the first spelling of each pair; the value is set once the pair
// is found in the other file.
using PairMap = std::unordered_map<std::string_view, bool, PairHash, PairEqual>;

// Both spellings of a pair have the same length, so a row keeps only where
// its line starts.
struct Row {
  char const* start;
  PairMap::value_type* pair;

  std::string_view line() const { return {start, pair->first.size()}; }
};

struct PairSet {
  PairMap index;
  // Node addresses remain stable when the hash table grows.
  std::vector<Row> rows;
};

struct Text {
  Text() = default;
  Text(Text const&) = delete;
  Text& operator=(Text const&) = delete;
  ~Text() {
    if (mapped != MAP_FAILED) munmap(mapped, mapped_size);
  }

  void* mapped = MAP_FAILED;
  size_t mapped_size = 0;
  std::string owned;
  std::string_view view;
};

void usage(FILE* out, char const* program) {
  fprintf(out,
      "usage: %s [--tabs] [-123] FILE1 FILE2\n"
      "  compare word,word lines with either word order considered equal\n"
      "  --tabs  prefix columns with tabs as in comm\n"
      "  -1  suppress pairs only in FILE1\n"
      "  -2  suppress pairs only in FILE2\n"
      "  -3  suppress pairs in both files\n"
      "  output is left justified by default\n"
      "  FILE1's rows come first in FILE1 order, then FILE2-only rows in\n"
      "  FILE2 order\n"
      "  a shared pair is printed once, with its first spelling in FILE1;\n"
      "  other lines are printed as given, repeats included\n"
      "  use - for standard input in either position, but not both\n"
      "  each input line must have exactly two nonempty comma-separated words\n",
      program);
}

bool valid_pair(std::string_view line, char const* path, size_t number) {
  size_t const comma = line.find(',');
  if (comma == 0 || comma == std::string_view::npos ||
      comma + 1 == line.size() ||
      line.find(',', comma + 1) != std::string_view::npos) {
    fprintf(stderr, "pcomm: %s:%zu: expected word,word: ", path, number);
    fwrite(line.data(), 1, line.size(), stderr);
    fputc('\n', stderr);
    return false;
  }
  return true;
}

bool open_input(char const* path, int* fd, struct stat* status) {
  bool const from_stdin = strcmp(path, "-") == 0;
  *fd = from_stdin ? STDIN_FILENO : open(path, O_RDONLY);
  if (*fd < 0) {
    fprintf(stderr, "pcomm: %s: %s\n", path, strerror(errno));
    return false;
  }
  if (fstat(*fd, status) != 0) {
    fprintf(stderr, "pcomm: %s: %s\n", path, strerror(errno));
    if (!from_stdin) close(*fd);
    return false;
  }
  return true;
}

bool map_text(int fd, struct stat const& status, Text* text) {
  if (status.st_size == 0) return true;
  if (static_cast<uintmax_t>(status.st_size) > SIZE_MAX) return false;
  size_t const size = static_cast<size_t>(status.st_size);
  void* const mapped = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (mapped == MAP_FAILED) return false;
  text->mapped = mapped;
  text->mapped_size = size;
  text->view = std::string_view(static_cast<char const*>(mapped), size);
  return true;
}

template <typename Visit>
bool visit_lines(std::string_view text, Visit visit) {
  size_t start = 0;
  size_t number = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!visit(line, ++number)) return false;
    start = end + 1;
  }
  return true;
}

// Map regular files. For files that cannot be mapped, read one line at a time.
template <typename Visit>
bool for_each_line(char const* path, Visit visit) {
  int fd;
  struct stat status;
  if (!open_input(path, &fd, &status)) return false;
  bool const from_stdin = strcmp(path, "-") == 0;

  if (!from_stdin && S_ISREG(status.st_mode)) {
    Text text;
    if (map_text(fd, status, &text)) {
      close(fd);
      return visit_lines(text.view, visit);
    }
  }

  FILE* const input = from_stdin ? stdin : fdopen(fd, "rb");
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
  if (!from_stdin && fclose(input) != 0) {
    fprintf(stderr, "pcomm: %s: %s\n", path, strerror(errno));
    ok = false;
  }
  return ok;
}

// Map regular files. Read anything else into memory whole.
bool load_text(char const* path, Text* text) {
  int fd;
  struct stat status;
  if (!open_input(path, &fd, &status)) return false;
  bool const from_stdin = strcmp(path, "-") == 0;
  bool ok = true;
  if (from_stdin || !S_ISREG(status.st_mode) ||
      !map_text(fd, status, text)) {
    char chunk[1 << 16];
    ssize_t length;
    while ((length = read(fd, chunk, sizeof chunk)) != 0) {
      if (length < 0) {
        if (errno == EINTR) continue;
        fprintf(stderr, "pcomm: %s: %s\n", path, strerror(errno));
        ok = false;
        break;
      }
      text->owned.append(chunk, size_t(length));
    }
    text->view = text->owned;
  }
  if (!from_stdin) close(fd);
  return ok;
}

bool load_set(char const* path, Text* text, PairSet* set) {
  if (!load_text(path, text)) return false;
  return visit_lines(text->view, [&](std::string_view line, size_t number) {
    if (!valid_pair(line, path, number)) return false;
    auto const inserted = set->index.try_emplace(line, false);
    set->rows.push_back(Row{line.data(), &*inserted.first});
    return true;
  });
}

bool print_pair(int column, std::string_view spelling, bool const show[3],
                bool tabs) {
  if (!show[column]) return true;
  if (tabs)
    for (int i = 0; i < column; ++i)
      if (show[i] && putchar('\t') == EOF) return false;
  return fwrite(spelling.data(), 1, spelling.size(), stdout) ==
      spelling.size() && putchar('\n') != EOF;
}

bool compare_streaming_first(char const* stream_path, PairSet* set,
                             bool const show[3], bool tabs) {
  if (!for_each_line(stream_path, [&](std::string_view line, size_t number) {
        if (!valid_pair(line, stream_path, number)) return false;
        auto const found = set->index.find(line);
        if (found == set->index.end()) return print_pair(0, line, show, tabs);
        if (found->second) return true;
        found->second = true;
        return print_pair(2, line, show, tabs);
      })) return false;
  for (Row const& row : set->rows)
    if (!row.pair->second && !print_pair(1, row.line(), show, tabs))
      return false;
  return true;
}

// FILE2-only rows follow FILE1's rows, so when both are shown they are
// printed by a second pass over FILE2, or kept in memory when FILE2 cannot
// be read twice. The first pass marks which lines the second pass prints.
bool compare_streaming_second(char const* stream_path, bool rereadable,
                              PairSet* set, bool const show[3], bool tabs) {
  bool const defer = show[1] && (show[0] || show[2]);
  bool const reread = defer && rereadable;
  std::string pending;
  std::vector<bool> exclusive;
  if (!for_each_line(stream_path, [&](std::string_view line, size_t number) {
        if (!valid_pair(line, stream_path, number)) return false;
        auto const found = set->index.find(line);
        bool const shared = found != set->index.end();
        if (reread) exclusive.push_back(!shared);
        if (shared) {
          found->second = true;
          return true;
        }
        if (!defer) return print_pair(1, line, show, tabs);
        if (!reread) {
          pending.append(line);
          pending.push_back('\n');
        }
        return true;
      })) return false;
  for (Row const& row : set->rows) {
    if (!row.pair->second) {
      if (!print_pair(0, row.line(), show, tabs)) return false;
    } else if (row.start == row.pair->first.data()) {
      if (!print_pair(2, row.line(), show, tabs)) return false;
    }
  }
  if (!defer) return true;
  if (!reread)
    return visit_lines(pending, [&](std::string_view line, size_t) {
      return print_pair(1, line, show, tabs);
    });
  return for_each_line(stream_path, [&](std::string_view line, size_t number) {
    if (number > exclusive.size()) {
      fprintf(stderr, "pcomm: %s: changed while reading\n", stream_path);
      return false;
    }
    return !exclusive[number - 1] || print_pair(1, line, show, tabs);
  });
}

bool compare(char const* const paths[2], bool stream_first, bool rereadable,
             bool const show[3], bool tabs) {
  Text text;
  PairSet set;
  if (!load_set(paths[stream_first ? 1 : 0], &text, &set)) return false;
  bool const ok = stream_first
      ? compare_streaming_first(paths[0], &set, show, tabs)
      : compare_streaming_second(paths[1], rereadable, &set, show, tabs);
  return ok && fflush(stdout) == 0;
}

bool get_status(char const* path, struct stat* status) {
  if (stat(path, status) == 0) return true;
  fprintf(stderr, "pcomm: %s: %s\n", path, strerror(errno));
  return false;
}

bool smaller_first(struct stat const& first, struct stat const& second) {
  if (S_ISREG(first.st_mode) && S_ISREG(second.st_mode))
    return first.st_size <= second.st_size;
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  bool show[3] = {true, true, true};
  bool tabs = false;
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
    if (!options_done && arg == "--tabs") {
      tabs = true;
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
  bool const stdin_first = strcmp(paths[0], "-") == 0;
  bool const stdin_second = strcmp(paths[1], "-") == 0;
  if (stdin_first && stdin_second) {
    fputs("pcomm: standard input cannot be used for both files\n", stderr);
    usage(stderr, argv[0]);
    return 2;
  }
  struct stat statuses[2];
  if ((!stdin_first && !get_status(paths[0], &statuses[0])) ||
      (!stdin_second && !get_status(paths[1], &statuses[1])))
    return 1;

  // Standard input must be streamed. Otherwise, map the larger file and hold
  // the smaller one in the set.
  bool const stream_first = stdin_first ||
      (!stdin_second && !smaller_first(statuses[0], statuses[1]));
  bool const rereadable = !stdin_second && S_ISREG(statuses[1].st_mode);
  return compare(paths, stream_first, rereadable, show, tabs) ? 0 : 1;
}
