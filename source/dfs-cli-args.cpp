#include "dfs-cli-args.h"

#include "dfs-diagnostic.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

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

bool validate_solo_bonuses(DfsCommonArgs const& args) {
  if (args.solo_words.empty()) return true;
  if (args.word_bonus < 0.0) {
    fputs("error: --word-bonus must be non-negative with --solo-words\n",
          stderr);
    return false;
  }
  if (args.pair_bonus < 0.0) {
    fputs("error: --pair-bonus must be non-negative with --solo-words\n",
          stderr);
    return false;
  }
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

bool load_pair_file(
    char const* path, char const* what, DfsPairSet* pairs, bool quiet,
    bool reject_hyphens, bool allow_single_words) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    fprintf(stderr, "error: can't open %s \"%s\"\n", what, path);
    return false;
  }

  std::vector<std::pair<std::string, std::string> > loaded;
  std::string line;
  std::string left;
  std::string right;
  size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (line.find('-') != std::string::npos) {
      if (!reject_hyphens) continue;
      fprintf(stderr,
          "error: %s \"%s\" line %zu: '-' would silently skip this entry\n",
          what, path, line_number);
      return false;
    }

    size_t const comma = line.find(',');
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
          what, path, line_number,
          allow_single_words
              ? "expected one word or two comma-separated words"
              : "expected two comma-separated words");
      return false;
    }
    loaded.push_back(std::make_pair(left, right));
  }

  if (!input.eof()) {
    fprintf(stderr, "error: can't read %s \"%s\"\n", what, path);
    return false;
  }

  pairs->reserve(2 * loaded.size());
  for (size_t i = 0; i < loaded.size(); ++i) {
    if (loaded[i].second.empty()) {
      pairs->insert(loaded[i].first);
    } else {
      pairs->insert(loaded[i].first + " " + loaded[i].second);
      pairs->insert(loaded[i].second + " " + loaded[i].first);
    }
  }
  if (!quiet)
    dfs_diagnostic("%s: %zu pairs, %zu keys\n",
                   what, loaded.size(), pairs->size());
  return true;
}

static bool load_exclude_pair_file(char const* path, DfsPairSet* pairs) {
  struct stat status;
  if (stat(path, &status) != 0 || !S_ISDIR(status.st_mode))
    return load_pair_file(path, "exclude list", pairs, false, true);

  std::string const metadata = std::string(path) + "/.wf";
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
  std::string const resolved = metadata + "/classified/no/no.pairs";
  return load_pair_file(resolved.c_str(), "exclude list", pairs, false, true);
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
      if (out->search_threads < 1) {
        fputs("error: --search-threads must be at least 1\n", stderr);
        return DFS_OPTION_ERROR;
      }
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
