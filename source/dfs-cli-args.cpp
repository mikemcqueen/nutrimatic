#include "dfs-cli-args.h"

#include "dfs-diagnostic.h"
#include "workflow-paths.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

// One field's worth of load_dictionary()'s cleanup: lowercased, with every
// character outside a-z/0-9 dropped.
void clean_word(char const* begin, char const* end, std::string* out) {
  out->clear();
  out->reserve(size_t(end - begin));
  for (char const* p = begin; p != end; ++p) {
    unsigned char const ch = (unsigned char) *p;
    if (ch >= 'A' && ch <= 'Z')
      out->push_back(char(ch - 'A' + 'a'));
    else if ((ch >= 'a' && ch <= 'z') ||
             (ch >= '0' && ch <= '9'))
      out->push_back(char(ch));
  }
}

bool is_counted_component(std::string const& value, char prefix) {
  if (value.size() < 2 || value[0] != prefix ||
      value[1] < '1' || value[1] > '9')
    return false;
  for (size_t i = 2; i < value.size(); ++i)
    if (value[i] < '0' || value[i] > '9') return false;
  return true;
}

bool parse_workflow_target(
    std::string const& input, char const* program,
    std::vector<std::string>* parts) {
  if (input.empty()) return true;
  size_t start = 0;
  while (true) {
    size_t const slash = input.find('/', start);
    size_t const end = slash == std::string::npos ? input.size() : slash;
    if (end == start || parts->size() == 4) {
      fprintf(stderr, "%s: invalid --target \"%s\"\n",
              program, input.c_str());
      return false;
    }
    parts->push_back(input.substr(start, end - start));
    if (slash == std::string::npos) break;
    start = slash + 1;
  }

  if ((*parts)[0][0] == 'S') (*parts)[0][0] = 's';
  bool valid = is_counted_component((*parts)[0], 's');
  if (parts->size() >= 2) {
    std::string const& letters = (*parts)[1];
    valid = valid && letters.size() > 2 &&
        (letters[0] == 'o' || letters[0] == 'u') && letters[1] == '-';
    for (size_t i = 2; valid && i < letters.size(); ++i)
      valid = letters[i] >= 'a' && letters[i] <= 'z';
  }
  if (parts->size() >= 3)
    valid = valid && is_counted_component((*parts)[2], 'm');
  if (parts->size() >= 4)
    valid = valid && is_counted_component((*parts)[3], 'g');
  if (!valid) {
    fprintf(stderr,
        "%s: invalid --target \"%s\"; expected a prefix of "
        "sN/[ou]-letters/mN/gN\n",
        program, input.c_str());
    return false;
  }
  return true;
}

std::string join_path(std::vector<std::string> const& parts) {
  std::string result;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) result.push_back('/');
    result += parts[i];
  }
  return result;
}

bool seed_name_matches(std::string const& name, std::string const& universe) {
  std::string const prefix = universe.empty()
      ? "seed.m" : "seed." + universe;
  if (name.compare(0, prefix.size(), prefix) != 0) return false;
  size_t position = prefix.size();
  if (universe.empty()) {
    if (position == name.size() || name[position] < '1' ||
        name[position] > '9')
      return false;
    while (position < name.size() && name[position] >= '0' &&
           name[position] <= '9')
      ++position;
  }
  if (name.compare(position, 6, ".pairs") == 0 &&
      position + 6 == name.size())
    return true;
  return position < name.size() && name[position] == '.' &&
      name.size() > position + 7 &&
      name.compare(name.size() - 6, 6, ".pairs") == 0;
}

bool resolve_sentence_seed(
    fs::path const& sentence_dir, std::string const& universe,
    char const* program, std::string* out) {
  std::error_code error;
  if (!fs::is_directory(sentence_dir, error)) {
    fprintf(stderr, "%s: workflow sentence \"%s\" is not a directory\n",
            program, sentence_dir.c_str());
    return false;
  }
  std::vector<fs::path> matches;
  fs::directory_iterator it(sentence_dir, error);
  fs::directory_iterator const end;
  for (; !error && it != end; it.increment(error)) {
    std::string const name = it->path().filename().string();
    if (seed_name_matches(name, universe) &&
        fs::is_regular_file(it->path(), error))
      matches.push_back(it->path());
  }
  if (error) {
    fprintf(stderr, "%s: can't inspect workflow sentence \"%s\": %s\n",
            program, sentence_dir.c_str(), error.message().c_str());
    return false;
  }
  std::sort(matches.begin(), matches.end());
  if (matches.size() != 1) {
    std::string const pattern = universe.empty()
        ? "seed.m<N>[.*].pairs" : "seed." + universe + "[.*].pairs";
    fprintf(stderr, "%s: expected exactly one %s in \"%s\", found %zu\n",
            program, pattern.c_str(), sentence_dir.c_str(), matches.size());
    return false;
  }
  *out = matches[0].string();
  return true;
}

bool require_regular_file(
    fs::path const& path, char const* program, char const* what) {
  std::error_code error;
  if (fs::is_regular_file(path, error)) return true;
  fprintf(stderr, "%s: %s \"%s\" is not a regular file\n",
          program, what, path.c_str());
  return false;
}

// Appends an optional workflow pair file. A path that cannot be inspected,
// or that is present as something other than a regular file, is an error.
bool push_optional_pair_file(
    fs::path const& path, char const* program, char const* what,
    std::vector<std::string>* paths) {
  std::error_code error;
  fs::file_status const status = fs::symlink_status(path, error);
  if (status.type() == fs::file_type::not_found) return true;
  if (error) {
    fprintf(stderr, "%s: can't inspect %s \"%s\": %s\n",
            program, what, path.c_str(), error.message().c_str());
    return false;
  }
  if (!require_regular_file(path, program, what)) return false;
  paths->push_back(path.string());
  return true;
}

bool canonical_target_below(
    fs::path const& best, fs::path const& selected,
    char const* program) {
  std::error_code error;
  fs::path const canonical_best = fs::canonical(best, error);
  if (error) {
    fprintf(stderr, "%s: can't resolve workflow best directory \"%s\": %s\n",
            program, best.c_str(), error.message().c_str());
    return false;
  }
  fs::path const canonical_selected = fs::canonical(selected, error);
  if (error) {
    fprintf(stderr, "%s: can't resolve workflow target \"%s\": %s\n",
            program, selected.c_str(), error.message().c_str());
    return false;
  }
  fs::path::const_iterator base = canonical_best.begin();
  fs::path::const_iterator target = canonical_selected.begin();
  for (; base != canonical_best.end() && target != canonical_selected.end();
       ++base, ++target)
    if (*base != *target) break;
  if (base == canonical_best.end() && target != canonical_selected.end())
    return true;
  fprintf(stderr,
      "%s: workflow target \"%s\" resolves outside \"%s\"\n",
      program, selected.c_str(), canonical_best.c_str());
  return false;
}

void merge_pair_tier(
    DfsPairSet const& source, DfsPairBonusKind kind,
    DfsPairBonusMap* destination) {
  destination->reserve(destination->size() + source.size());
  for (DfsPairSet::const_iterator it = source.begin();
       it != source.end(); ++it) {
    DfsPairBonusKind& saved = (*destination)[*it];
    if (kind > saved) saved = kind;
  }
}

}  // namespace

bool clean_letters(char const* in, char const* what, std::string* out) {
  for (; *in != '\0'; ++in) {
    if (*in == ' ') continue;
    if ((*in < 'a' || *in > 'z') && (*in < '0' || *in > '9')) {
      fprintf(stderr, "error: bad character '%c' in %s\n", *in, what);
      return false;
    }
    out->push_back(*in);
  }
  return true;
}

bool subtract_letters(std::string const& bag, std::string const& used,
                      std::string* out) {
  int have[UCHAR_MAX + 1] = { 0 };
  for (size_t i = 0; i < bag.size(); ++i)
    ++have[(unsigned char) bag[i]];

  for (size_t i = 0; i < used.size(); ++i) {
    unsigned char const ch = (unsigned char) used[i];
    if (have[ch] == 0) {
      fprintf(stderr, "error: no '%c' left in \"%s\" to use\n",
              ch, bag.c_str());
      return false;
    }
    --have[ch];
  }

  out->clear();
  for (int ch = 0; ch <= UCHAR_MAX; ++ch)
    out->append(size_t(have[ch]), char(ch));

  if (out->empty()) {
    fputs("error: no letters left after removing used letters\n", stderr);
    return false;
  }
  return true;
}

bool check_bag_length(std::string const& bag) {
  if (bag.size() <= DFS_MAX_BAG_LETTERS) return true;
  fprintf(stderr,
      "error: %zu letters after removing used letters, %zu maximum\n",
      bag.size(), DFS_MAX_BAG_LETTERS);
  return false;
}

bool parse_count(char const* in, char const* what, int* out) {
  char* end;
  long const value = strtol(in, &end, 10);
  if (*in == '\0' || *end != '\0' || value < 0 || value > INT_MAX) {
    fprintf(stderr, "error: %s needs a count, not \"%s\"\n", what, in);
    return false;
  }
  *out = int(value);
  return true;
}

bool parse_count64(char const* in, char const* what, int64_t* out) {
  errno = 0;
  char* end;
  long long const value = strtoll(in, &end, 10);
  if (*in == '\0' || *end != '\0' || errno == ERANGE || value < 0) {
    fprintf(stderr, "error: %s needs a count, not \"%s\"\n", what, in);
    return false;
  }
  *out = int64_t(value);
  return true;
}

bool parse_mib(char const* in, char const* what, size_t* out) {
  if (*in == '\0' || *in == '-') {
    fprintf(stderr, "error: %s needs a count, not \"%s\"\n", what, in);
    return false;
  }
  errno = 0;
  char* end;
  unsigned long long const value = strtoull(in, &end, 10);
  if (*end != '\0' || errno == ERANGE ||
      value > static_cast<unsigned long long>(SIZE_MAX / DFS_MIB)) {
    fprintf(stderr, "error: %s needs a count, not \"%s\"\n", what, in);
    return false;
  }
  *out = size_t(value) * DFS_MIB;
  return true;
}

bool parse_double(char const* in, char const* what, double* out) {
  char* end;
  double const value = strtod(in, &end);
  if (*in == '\0' || *end != '\0' || !isfinite(value)) {
    fprintf(stderr, "error: %s needs a number, not \"%s\"\n", what, in);
    return false;
  }
  *out = value;
  return true;
}

bool parse_segment_penalty(char const* in, double* out) {
  if (!parse_double(in, "--segment-penalty", out)) return false;
  if (*out < 1.0) {
    fputs("error: --segment-penalty must be at least 1\n", stderr);
    return false;
  }
  return true;
}

bool parse_solo_words(
    char const* in, std::vector<std::string>* solo_words) {
  std::string const value(in);
  size_t start = 0;
  for (;;) {
    size_t const comma = value.find(',', start);
    size_t const end = comma == std::string::npos ? value.size() : comma;
    if (end == start) {
      fputs("error: --solo-words contains an empty word\n", stderr);
      return false;
    }
    std::string const word = value.substr(start, end - start);
    for (size_t i = 0; i < word.size(); ++i) {
      char const ch = word[i];
      if ((ch < 'a' || ch > 'z') && (ch < '0' || ch > '9')) {
        fprintf(stderr,
                "error: --solo-words word \"%s\" must contain only"
                " lowercase a-z0-9\n",
                word.c_str());
        return false;
      }
    }
    if (std::find(solo_words->begin(), solo_words->end(), word) !=
        solo_words->end()) {
      fprintf(stderr, "error: duplicate --solo-words word \"%s\"\n",
              word.c_str());
      return false;
    }
    if (solo_words->size() == 16) {
      fputs("error: --solo-words accepts at most 16 words\n", stderr);
      return false;
    }
    solo_words->push_back(word);
    if (comma == std::string::npos) return true;
    start = comma + 1;
  }
}

// Checks raw solo-word bonus constraints, then normalizes the effective pair
// bonus. Solo assignment is an optional reward, so negative score bonuses are
// legal only when no solo words were supplied; the raw values are validated
// before a missing --pairs file clears the pair bonus.
static bool finalize_dfs_bonuses(DfsCommonArgs* args) {
  if (!args->solo_words.empty()) {
    if (args->word_bonus < 0.0) {
      fputs("error: --word-bonus must be non-negative with --solo-words\n",
            stderr);
      return false;
    }
    if (args->pair_bonus < 0.0) {
      fputs("error: --pair-bonus must be non-negative with --solo-words\n",
            stderr);
      return false;
    }
  }
  if (args->pair_file == NULL) args->pair_bonus = 0.0;
  return true;
}

bool finalize_min_word_length(
    std::string const& letters, bool explicitly_given, int* min_word_len) {
  if (!explicitly_given && *min_word_len > int(letters.size()))
    *min_word_len = int(letters.size());

  if (*min_word_len > int(letters.size())) {
    fprintf(stderr,
        "error: no word of %d letters fits in the %zu left in \"%s\"\n",
        *min_word_len, letters.size(), letters.c_str());
    return false;
  }
  return true;
}

size_t resolve_preprocess_threads(int requested, size_t letter_count) {
  if (requested != 0) return size_t(requested);
  if (letter_count < 26) return 1;
  unsigned int const available = std::thread::hardware_concurrency();
  if (available <= 1) return 1;
  return size_t(std::min(
      available, DFS_DEFAULT_MAX_PREPROCESS_THREADS));
}

size_t resolve_search_threads(int requested) {
  if (requested != 0) return size_t(requested);
  unsigned int const available = std::thread::hardware_concurrency();
  return available == 0 ? 1 : size_t(available);
}

bool load_dictionary(char const* path, DfsDictionary* dictionary) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    fprintf(stderr, "error: can't open dictionary \"%s\"\n", path);
    return false;
  }

  std::string line;
  while (std::getline(input, line)) {
    if (line.find('-') != std::string::npos) continue;

    std::string word;
    clean_word(line.data(), line.data() + line.size(), &word);
    if (!word.empty()) dictionary->insert(word);
  }

  if (!input.eof()) {
    fprintf(stderr, "error: can't read dictionary \"%s\"\n", path);
    return false;
  }
  return true;
}

namespace {

bool load_pair_rows_stream(
    std::istream& input, char const* what, char const* source,
    bool reject_hyphens, bool allow_single_words, bool ignore_single_words,
    size_t* ignored_single_word_count,
    std::vector<DfsPairRow>* loaded) {
  std::string line;
  std::string left;
  std::string right;
  size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    size_t const comma = line.find(',');
    if (ignore_single_words && comma == std::string::npos && !line.empty()) {
      ++*ignored_single_word_count;
      continue;
    }
    if (line.find('-') != std::string::npos) {
      if (!reject_hyphens) continue;
      fprintf(stderr,
          "error: %s \"%s\" line %zu: '-' would silently skip this entry\n",
          what, source, line_number);
      return false;
    }

    bool const one_field =
        allow_single_words && comma == std::string::npos;
    bool const two_fields =
        comma != std::string::npos &&
        line.find(',', comma + 1) == std::string::npos;
    if (one_field) {
      clean_word(line.data(), line.data() + line.size(), &left);
      right.clear();
    } else if (two_fields) {
      clean_word(line.data(), line.data() + comma, &left);
      clean_word(line.data() + comma + 1, line.data() + line.size(), &right);
    }
    if ((!one_field && !two_fields) || left.empty() ||
        (two_fields && right.empty())) {
      fprintf(stderr,
          "error: %s \"%s\" line %zu: %s\n",
          what, source, line_number,
          allow_single_words
              ? "expected one word or two comma-separated words"
              : "expected two comma-separated words");
      return false;
    }
    DfsPairRow const row = { left, right, line_number };
    loaded->push_back(row);
  }

  if (!input.eof()) {
    fprintf(stderr, "error: can't read %s \"%s\"\n", what, source);
    return false;
  }

  return true;
}

bool load_pair_rows(
    char const* path, char const* what, bool reject_hyphens,
    bool allow_single_words, bool ignore_single_words,
    size_t* ignored_single_word_count,
    std::vector<DfsPairRow>* loaded) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    fprintf(stderr, "error: can't open %s \"%s\"\n", what, path);
    return false;
  }
  return load_pair_rows_stream(
      input, what, path, reject_hyphens, allow_single_words,
      ignore_single_words, ignored_single_word_count, loaded);
}

void insert_pair_keys(
    std::vector<DfsPairRow> const& loaded, DfsPairSet* pairs) {
  pairs->reserve(pairs->size() + 2 * loaded.size());
  for (size_t i = 0; i < loaded.size(); ++i) {
    if (loaded[i].right.empty()) {
      pairs->insert(loaded[i].entry());
    } else {
      pairs->insert(loaded[i].entry());
      pairs->insert(loaded[i].entry(/*reverse=*/true));
    }
  }
}

bool build_extraction_pairs(
    std::vector<DfsPairRow> const& loaded, char const* what,
    char const* source, int min_word_len, DfsPairSet* pairs,
    DfsPairSet* exception_prefixes, bool quiet,
    std::vector<DfsPairRow>* rows) {
  size_t const minimum = size_t(std::max(min_word_len, 0));
  for (size_t i = 0; i < loaded.size(); ++i) {
    size_t const normalized_length =
        loaded[i].left.size() + loaded[i].right.size();
    if (normalized_length < minimum) {
      fprintf(stderr,
          "error: %s \"%s\" line %zu: normalized entry has %zu non-space"
          " characters, fewer than -m %d\n",
          what, source, loaded[i].line_number, normalized_length,
          min_word_len);
      return false;
    }
  }

  pairs->reserve(pairs->size() + 2 * loaded.size());
  exception_prefixes->reserve(exception_prefixes->size() + loaded.size());
  for (size_t i = 0; i < loaded.size(); ++i) {
    DfsPairRow const& row = loaded[i];
    if (row.right.empty()) {
      pairs->insert(row.left);
      continue;
    }

    pairs->insert(row.left + " " + row.right);
    bool const needs_exception =
        row.left.size() < minimum || row.right.size() < minimum;
    if (needs_exception) {
      exception_prefixes->insert(row.left);
    } else {
      pairs->insert(row.right + " " + row.left);
    }
  }
  if (rows != NULL) rows->insert(rows->end(), loaded.begin(), loaded.end());
  if (!quiet)
    dfs_diagnostic("%s: %zu pairs, %zu keys\n",
                   what, loaded.size(), pairs->size());
  return true;
}

bool load_pair_value(
    char const* value, char const* what, DfsPairSet* pairs, bool quiet,
    bool reject_hyphens, bool allow_single_words) {
  std::istringstream input{std::string(value)};
  std::vector<DfsPairRow> loaded;
  if (!load_pair_rows_stream(
          input, what, value, reject_hyphens, allow_single_words,
          /*ignore_single_words=*/false, NULL, &loaded))
    return false;

  insert_pair_keys(loaded, pairs);
  if (!quiet)
    dfs_diagnostic("%s: %zu pairs, %zu keys\n",
                   what, loaded.size(), pairs->size());
  return true;
}

bool load_extraction_pair_value(
    char const* value, char const* what, int min_word_len,
    DfsPairSet* pairs, DfsPairSet* exception_prefixes, bool quiet,
    bool reject_hyphens, std::vector<DfsPairRow>* rows) {
  std::istringstream input{std::string(value)};
  std::vector<DfsPairRow> loaded;
  if (!load_pair_rows_stream(
          input, what, value, reject_hyphens, /*allow_single_words=*/true,
          /*ignore_single_words=*/false, NULL, &loaded))
    return false;
  return build_extraction_pairs(
      loaded, what, value, min_word_len, pairs, exception_prefixes, quiet,
      rows);
}

}  // namespace

bool load_pair_file(
    char const* path, char const* what, DfsPairSet* pairs, bool quiet,
    bool reject_hyphens, bool allow_single_words,
    char const* diagnostic_source, std::vector<DfsPairRow>* rows) {
  std::vector<DfsPairRow> loaded;
  if (!load_pair_rows(
          path, what, reject_hyphens, allow_single_words,
          /*ignore_single_words=*/false, NULL, &loaded))
    return false;

  insert_pair_keys(loaded, pairs);
  if (rows != NULL) *rows = loaded;
  if (!quiet) {
    if (diagnostic_source == NULL)
      dfs_diagnostic("%s: %zu pairs, %zu keys\n",
                     what, loaded.size(), pairs->size());
    else
      dfs_diagnostic("%s: %zu pairs, %zu keys from %s\n",
                     what, loaded.size(), pairs->size(), diagnostic_source);
  }
  return true;
}

bool load_pair_file_ignoring_single_words(
    char const* path, char const* what, DfsPairSet* pairs,
    size_t* ignored_single_words) {
  std::vector<DfsPairRow> loaded;
  if (!load_pair_rows(
          path, what, /*reject_hyphens=*/true,
          /*allow_single_words=*/true, /*ignore_single_words=*/true,
          ignored_single_words, &loaded))
    return false;

  pairs->reserve(pairs->size() + 2 * loaded.size());
  for (size_t i = 0; i < loaded.size(); ++i) {
    pairs->insert(loaded[i].left + " " + loaded[i].right);
    pairs->insert(loaded[i].right + " " + loaded[i].left);
  }
  return true;
}

bool load_extraction_pair_file(
    char const* path, char const* what, int min_word_len,
    DfsPairSet* pairs, DfsPairSet* exception_prefixes, bool quiet,
    bool reject_hyphens, std::vector<DfsPairRow>* rows) {
  std::vector<DfsPairRow> loaded;
  if (!load_pair_rows(
          path, what, reject_hyphens, /*allow_single_words=*/true,
          /*ignore_single_words=*/false, NULL, &loaded))
    return false;
  return build_extraction_pairs(
      loaded, what, path, min_word_len, pairs, exception_prefixes, quiet,
      rows);
}

bool load_weighted_pair_files(
    DfsCommonArgs const& args, bool score_mode, int min_word_len,
    DfsPairBonusMap* pairs, DfsPairSet* exception_prefixes,
    std::vector<DfsPairRow>* rows, std::vector<DfsPairRow>* best_rows) {
  struct Source {
    std::vector<std::string> const* paths;
    char const* description;
    DfsPairBonusKind kind;
    std::vector<DfsPairRow>* rows;
  };
  Source const sources[] = {
    { &args.seed_pair_files, "seed pair list", DFS_PAIR_BONUS_SEED, NULL },
    { &args.yes_pair_files, "YES pair list", DFS_PAIR_BONUS_YES, rows },
    { &args.best_pair_files, "BEST pair list", DFS_PAIR_BONUS_BEST,
      best_rows },
  };
  for (size_t source = 0;
       source < sizeof(sources) / sizeof(sources[0]); ++source) {
    for (size_t i = 0; i < sources[source].paths->size(); ++i) {
      DfsPairSet loaded;
      DfsPairSet prefixes;
      char const* const path = (*sources[source].paths)[i].c_str();
      bool const success = score_mode
          ? load_pair_file(path, sources[source].description, &loaded,
                           false, false, true)
          : load_extraction_pair_file(
                path, sources[source].description, min_word_len,
                &loaded, &prefixes, false, false, sources[source].rows);
      if (!success) return false;
      merge_pair_tier(loaded, sources[source].kind, pairs);
      exception_prefixes->insert(prefixes.begin(), prefixes.end());
    }
  }

  if (!args.one_best_pair.empty()) {
    DfsPairSet loaded;
    DfsPairSet prefixes;
    char const* const value = args.one_best_pair.c_str();
    bool const success = score_mode
        ? load_pair_value(value, "BEST pair list", &loaded, false, false,
                          true)
        : load_extraction_pair_value(
              value, "BEST pair list", min_word_len, &loaded, &prefixes,
              false, false, best_rows);
    if (!success) return false;
    merge_pair_tier(loaded, DFS_PAIR_BONUS_BEST, pairs);
    exception_prefixes->insert(prefixes.begin(), prefixes.end());
  }
  return true;
}

static std::string exclude_diagnostic_source(char const* path) {
  std::string const source(path);
  size_t const wf = source.find(".wf/");
  if (wf != std::string::npos &&
      (wf == 0 || source[wf - 1] == '/'))
    return source.substr(wf + 4);
  return source;
}

static bool load_exclude_pair_file(char const* path, DfsPairSet* pairs) {
  struct stat status;
  if (stat(path, &status) != 0 || !S_ISDIR(status.st_mode)) {
    std::string const source = exclude_diagnostic_source(path);
    return load_pair_file(
        path, "exclude list", pairs, false, true, false, source.c_str());
  }

  fs::path const root(path);
  std::string const metadata = (root / WORKFLOW_DIR_PATH).string();
  struct stat metadata_status;
  if (stat(metadata.c_str(), &metadata_status) != 0) {
    // Anything but ENOENT means .wf may well be there and simply unreachable,
    // so "create it" would be the wrong thing to tell the caller.
    if (errno != ENOENT) {
      fprintf(stderr,
          "error: --exclude-pairs can't read workflow metadata \"%s\": %s\n",
          metadata.c_str(), strerror(errno));
      return false;
    }
    metadata_status.st_mode = 0;
  }
  if (!S_ISDIR(metadata_status.st_mode)) {
    fprintf(stderr,
        "error: --exclude-pairs directory \"%s\" has no workflow metadata"
        " \"%s\"\n",
        path, metadata.c_str());
    return false;
  }
  std::string const resolved = (root / WORKFLOW_NO_PAIRS_PATH).string();
  std::string const source = exclude_diagnostic_source(resolved.c_str());
  return load_pair_file(
      resolved.c_str(), "exclude list", pairs, false, true, false,
      source.c_str());
}

bool load_exclude_pair_files(
    std::vector<std::string> const& paths, DfsPairSet* pairs) {
  size_t directories = 0;
  for (size_t i = 0; i < paths.size(); ++i) {
    struct stat status;
    if (stat(paths[i].c_str(), &status) == 0 && S_ISDIR(status.st_mode) &&
        ++directories > 1) {
      fputs("error: only one --exclude-pairs argument may be a directory\n",
            stderr);
      return false;
    }
  }

  for (size_t i = 0; i < paths.size(); ++i) {
    if (!load_exclude_pair_file(paths[i].c_str(), pairs)) return false;
  }
  return true;
}

DfsOptionResult dfs_parse_common_option(
    int option, struct optparse* options, DfsCommonArgs* out,
    DfsCommonOption* which) {
  DfsCommonOption info;
  info.name = NULL;
  info.score_incompatible = true;

  switch (option) {
    case 'u':
      out->used_letters += options->optarg;
      info.name = "--used-letters";
      break;
    case DFS_OPT_DICT:
      out->dictionary_file = options->optarg;
      info.name = "--dict";
      break;
    case 'm':
      if (!parse_count(options->optarg, "--min-word-length",
                       &out->min_word_len))
        return DFS_OPTION_ERROR;
      out->min_word_len_given = true;
      info.name = "--min-word-length";
      break;
    case 'x':
      if (!parse_count(options->optarg, "--max-extract-words",
                       &out->max_extract_words))
        return DFS_OPTION_ERROR;
      out->max_extract_words_given = true;
      info.name = "--max-extract-words";
      break;
    case DFS_OPT_PAIRS:
      out->pair_file = options->optarg;
      // A pair list is a scoring input, not an extraction filter.
      info.name = "--pairs";
      info.score_incompatible = false;
      break;
    case 'n':
      if (!parse_count(options->optarg, "--top", &out->top))
        return DFS_OPTION_ERROR;
      info.name = "--top";
      break;
    case 'S':
      if (!parse_count(options->optarg, "--search-threads",
                       &out->search_threads))
        return DFS_OPTION_ERROR;
      info.name = "--search-threads";
      break;
    case 'P':
      if (!parse_segment_penalty(options->optarg, &out->segment_penalty))
        return DFS_OPTION_ERROR;
      // The penalty scales a score, which --score still computes.
      info.name = "--segment-penalty";
      info.score_incompatible = false;
      break;
    case DFS_OPT_WORD_BONUS:
      if (!parse_double(options->optarg, "--word-bonus", &out->word_bonus))
        return DFS_OPTION_ERROR;
      // Like the penalty, it is a term of the score --score computes.
      info.name = "--word-bonus";
      info.score_incompatible = false;
      break;
    case DFS_OPT_PAIR_BONUS:
      if (!parse_double(options->optarg, "--pair-bonus", &out->pair_bonus))
        return DFS_OPTION_ERROR;
      info.name = "--pair-bonus";
      info.score_incompatible = false;
      break;
    case DFS_OPT_SEED_PAIRS:
      out->seed_pair_files.push_back(options->optarg);
      info.name = "--seed-pairs";
      info.score_incompatible = false;
      break;
    case DFS_OPT_YES_PAIRS:
      out->yes_pair_files.push_back(options->optarg);
      info.name = "--yes-pairs";
      info.score_incompatible = false;
      break;
    case DFS_OPT_BEST_PAIRS:
      if (out->best_pairs_given) {
        fputs("error: --best-pairs may be specified only once\n", stderr);
        return DFS_OPTION_ERROR;
      }
      out->best_pairs_given = true;
      out->best_pair_files.push_back(options->optarg);
      info.name = "--best-pairs";
      info.score_incompatible = false;
      break;
    case DFS_OPT_MORE_BEST_PAIRS:
      out->best_pair_files.push_back(options->optarg);
      info.name = "--more-best-pairs";
      info.score_incompatible = false;
      break;
    case DFS_OPT_WF:
      if (out->workflow || !out->workflow_root.empty()) {
        fputs("error: --wf and --wfroot may be specified only once\n",
              stderr);
        return DFS_OPTION_ERROR;
      }
      out->workflow = true;
      info.name = "--wf";
      info.score_incompatible = false;
      break;
    case DFS_OPT_WFROOT:
      if (out->workflow || !out->workflow_root.empty()) {
        fputs("error: --wf and --wfroot may be specified only once\n",
              stderr);
        return DFS_OPTION_ERROR;
      }
      if (options->optarg[0] == '\0') {
        fputs("error: --wfroot requires a nonempty directory\n", stderr);
        return DFS_OPTION_ERROR;
      }
      out->workflow_root = options->optarg;
      info.name = "--wfroot";
      info.score_incompatible = false;
      break;
    case 't':
      if (!out->target.empty()) {
        fputs("error: --target may be specified only once\n", stderr);
        return DFS_OPTION_ERROR;
      }
      if (options->optarg[0] == '\0') {
        fputs("error: --target requires a nonempty target\n", stderr);
        return DFS_OPTION_ERROR;
      }
      out->target = options->optarg;
      info.name = "--target";
      info.score_incompatible = false;
      break;
    case DFS_OPT_SOLO_WORDS:
      if (!parse_solo_words(options->optarg, &out->solo_words))
        return DFS_OPTION_ERROR;
      info.name = "--solo-words";
      info.score_incompatible = false;
      break;
    case DFS_OPT_HIDE_SOLO_WORDS:
      out->hide_solo_words = true;
      info.name = "--hide-solo-words";
      // --score never annotates its input sequence, so hiding annotations is
      // harmless there and keeps the shared presentation option composable.
      info.score_incompatible = false;
      break;
    default:
      return DFS_OPTION_OTHER;
  }

  if (which != NULL) *which = info;
  return DFS_OPTION_HANDLED;
}

// Resolves --wf/--wfroot defaults and pair sources after option parsing.
// Workflow mode supplies a default index when `*index_file` is NULL, plus a
// default dictionary and classified YES source. An explicit index is retained.
// Workflow mode also requires either an explicit seed input or a target whose
// sentence can identify exactly one sentence seed; a complete target
// additionally supplies its optional best.pairs unless --best-pairs replaced it.
static bool finalize_dfs_workflow_args(
    DfsCommonArgs* args, char const* program, char const** index_file) {
  bool const weighted = !args->seed_pair_files.empty() ||
      !args->yes_pair_files.empty() || !args->best_pair_files.empty();
  if (args->pair_file != NULL &&
      (weighted || args->workflow || !args->workflow_root.empty())) {
    fputs("error: --pairs cannot be combined with --seed-pairs, "
          "--yes-pairs, --best-pairs, or --more-best-pairs\n", stderr);
    return false;
  }

  if (args->workflow) {
    char const* const root = getenv("WFROOT");
    if (root == NULL || root[0] == '\0') {
      fprintf(stderr, "%s: --wf requires WFROOT to be set and nonempty\n",
              program);
      return false;
    }
    args->workflow_root = root;
  }
  if (args->workflow_root.empty()) {
    if (!args->target.empty()) {
      fprintf(stderr, "%s: --target requires --wf or --wfroot\n", program);
      return false;
    }
    return true;
  }

  fs::path const root(args->workflow_root);
  std::error_code error;
  if (!fs::is_directory(root / WORKFLOW_DIR_PATH, error)) {
    fprintf(stderr, "%s: workflow root \"%s\" has no .wf directory\n",
            program, root.c_str());
    return false;
  }

  std::vector<std::string> target_parts;
  if (!parse_workflow_target(args->target, program, &target_parts))
    return false;
  args->target_complete = target_parts.size() == 4;
  if (!target_parts.empty()) {
    args->target = join_path(target_parts);
    fs::path const selected = root / WORKFLOW_BEST_PATH / args->target;
    if (!fs::is_directory(selected, error)) {
      fprintf(stderr, "%s: workflow target \"%s\" is not a directory\n",
              program, selected.c_str());
      return false;
    }
    if (!canonical_target_below(
            root / WORKFLOW_BEST_PATH, selected, program))
      return false;
  }

  if (*index_file == NULL) {
    args->workflow_index_file = (root / WORKFLOW_INDEX_PATH).string();
    *index_file = args->workflow_index_file.c_str();
  }
  if (args->dictionary_file == NULL) {
    args->workflow_dictionary_file =
        (root / WORKFLOW_DICT_PATH).string();
    args->dictionary_file = args->workflow_dictionary_file.c_str();
  }

  fs::path const yes = root / WORKFLOW_YES_PAIRS_PATH;
  if (!require_regular_file(yes, program, "classified YES pair file"))
    return false;
  args->yes_pair_files.push_back(yes.string());

  if (args->seed_pair_files.empty()) {
    if (target_parts.empty()) {
      fprintf(stderr,
          "%s: workflow mode requires --target beginning with sN or an "
          "explicit --seed-pairs\n",
          program);
      return false;
    }
    std::string seed;
    std::string const universe = target_parts.size() >= 3
        ? target_parts[2] : std::string();
    if (!resolve_sentence_seed(
            root / WORKFLOW_BEST_PATH / target_parts[0], universe,
            program, &seed))
      return false;
    args->seed_pair_files.push_back(seed);
  }

  if (target_parts.size() == 4 && !args->best_pairs_given) {
    fs::path const best = root / WORKFLOW_BEST_PATH / args->target /
        WORKFLOW_TARGET_BEST_PAIRS_NAME;
    if (!push_optional_pair_file(
            best, program, "target BEST pair file", &args->best_pair_files))
      return false;
  }
  return true;
}

bool load_dfs_workflow_target_settings(
    DfsCommonArgs const& args, char const* program,
    DfsWorkflowTargetSettings* out) {
  if (args.workflow_root.empty() || !args.target_complete) {
    fprintf(stderr,
        "%s: requires a fully scoped sN/[ou]-letters/mN/gN target\n",
        program);
    return false;
  }

  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= args.target.size()) {
    size_t const slash = args.target.find('/', start);
    parts.push_back(args.target.substr(
        start, slash == std::string::npos
            ? std::string::npos : slash - start));
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  assert(parts.size() == 4);

  fs::path const letters_path =
      fs::path(args.workflow_root) / WORKFLOW_BEST_PATH / parts[0] / "letters";
  if (!require_regular_file(
          letters_path, program, "workflow sentence letters file"))
    return false;
  std::ifstream input(letters_path, std::ios::binary);
  if (!input.is_open()) {
    fprintf(stderr, "%s: can't open workflow sentence letters \"%s\"\n",
            program, letters_path.c_str());
    return false;
  }
  std::string sentence(
      (std::istreambuf_iterator<char>(input)),
      std::istreambuf_iterator<char>());
  if (input.bad()) {
    fprintf(stderr, "%s: can't read workflow sentence letters \"%s\"\n",
            program, letters_path.c_str());
    return false;
  }
  while (!sentence.empty() &&
         (sentence.back() == '\n' || sentence.back() == '\r'))
    sentence.pop_back();

  std::string sentence_bag;
  std::string named_bag;
  if (!clean_letters(
          sentence.c_str(), "workflow sentence letters", &sentence_bag) ||
      !clean_letters(
          parts[1].c_str() + 2, "workflow target letters", &named_bag))
    return false;
  if (parts[1][0] == 'o') {
    std::string unused;
    if (!subtract_letters(sentence_bag, named_bag, &unused)) return false;
    out->letters = named_bag;
  } else if (!subtract_letters(sentence_bag, named_bag, &out->letters)) {
    return false;
  }
  if (!check_bag_length(out->letters)) return false;
  return parse_count(
             parts[2].c_str() + 1, "target minimum word length",
             &out->min_word_len) &&
      parse_count(
             parts[3].c_str() + 1, "target segment count",
             &out->num_segments);
}

// Appends the workflow root's classified NO pairs, and a complete selected
// target's own no.pairs, to `paths` for tools that exclude pairs. Either file
// is skipped when it is absent, since a workflow need not have classified
// anything NO yet. Abbreviated targets do not name a target-local no.pairs.
// Without a workflow root nothing is appended. Call after
// finalize_dfs_workflow_args(), which resolves the root and the target.
static bool collect_workflow_exclude_pair_files(
    DfsCommonArgs const& args, char const* program,
    std::vector<std::string>* paths) {
  if (args.workflow_root.empty()) return true;

  fs::path const root(args.workflow_root);
  if (!push_optional_pair_file(
          root / WORKFLOW_NO_PAIRS_PATH, program,
          "classified NO pair file", paths))
    return false;
  if (!args.target_complete) return true;
  return push_optional_pair_file(
      root / WORKFLOW_BEST_PATH / args.target /
          WORKFLOW_TARGET_NO_PAIRS_NAME, program,
      "target NO pair file", paths);
}

bool finalize_dfs_common_args(
    DfsCommonArgs* args, char const* program, char const** index_file,
    std::vector<std::string>* exclude_pair_files) {
  if (!finalize_dfs_bonuses(args)) return false;
  if (!finalize_dfs_workflow_args(args, program, index_file)) return false;
  if (exclude_pair_files == NULL) return true;
  return collect_workflow_exclude_pair_files(
      *args, program, exclude_pair_files);
}
