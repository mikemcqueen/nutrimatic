#include "coherence-measure.h"

#include "coherence-score.h"
#include "index.h"
#include "optparse.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using WordId = uint32_t;
using PairKey = uint64_t;

static double const QUIET_NAN =
    std::numeric_limits<double>::quiet_NaN();

class MemoryBudget {
 public:
  explicit MemoryBudget(size_t limit): limit_(limit), current_(0), peak_(0) {}

  bool acquire(size_t bytes, char const*) {
    if (bytes > limit_ - current_) return false;
    current_ += bytes;
    peak_ = std::max(peak_, current_);
    return true;
  }

  void release(size_t bytes) {
    assert(bytes <= current_);
    current_ -= bytes;
  }

  size_t current() const { return current_; }
  size_t peak() const { return peak_; }
  size_t limit() const { return limit_; }

 private:
  size_t limit_;
  size_t current_;
  size_t peak_;
};

static bool checked_add_size(size_t left, size_t right, size_t* result) {
  if (right > std::numeric_limits<size_t>::max() - left) return false;
  *result = left + right;
  return true;
}

static bool checked_multiply_size(
    size_t left, size_t right, size_t* result) {
  if (left != 0 && right > std::numeric_limits<size_t>::max() / left)
    return false;
  *result = left * right;
  return true;
}

static bool checked_add_u64(
    uint64_t left, uint64_t right, uint64_t* result) {
  if (right > std::numeric_limits<uint64_t>::max() - left) return false;
  *result = left + right;
  return true;
}

static bool checked_multiply_u64(
    uint64_t left, uint64_t right, uint64_t* result) {
  if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left)
    return false;
  *result = left * right;
  return true;
}

static void charge(MemoryBudget* budget, size_t bytes, char const* owner) {
  if (!budget->acquire(bytes, owner)) {
    char message[256];
    snprintf(message, sizeof(message),
             "memory phase %s requested %zu bytes with %zu charged "
             "and limit %zu",
             owner, bytes, budget->current(), budget->limit());
    throw std::runtime_error(message);
  }
}

template<typename T>
static size_t reserve_vector(
    std::vector<T>* values, size_t count,
    MemoryBudget* budget, char const* owner) {
  size_t requested;
  if (!checked_multiply_size(count, sizeof(T), &requested))
    throw std::runtime_error(std::string("size overflow in ") + owner);
  charge(budget, requested, owner);
  values->reserve(count);
  size_t actual;
  if (!checked_multiply_size(values->capacity(), sizeof(T), &actual))
    throw std::runtime_error(std::string("size overflow in ") + owner);
  if (actual > requested)
    charge(budget, actual - requested, owner);
  return actual;
}

enum InputFormat { INPUT_DFS, INPUT_TEXT };
enum OrderMode { ORDER_PRINTED, ORDER_BEST };
enum SortMode { SORT_INPUT, SORT_COHERENCE };

struct Args {
  char const* index_path;
  char const* input_path;
  InputFormat format;
  OrderMode order;
  SortMode sort;
  size_t top;
  bool pairs;
};

struct InputSummary {
  uint64_t answer_count;
  uint64_t word_occurrence_count;
  uint64_t required_pair_occurrence_count;
  uint64_t order_relaxation_count;
  size_t maximum_word_count;
  size_t total_word_text_bytes;
};

struct ParsedLine {
  std::vector<std::string> words;
  bool has_legacy_score;
  double displayed_legacy_score;
  double legacy_log_score;
};

struct WordInfo {
  std::string const* text;
  uint64_t first_input_rank;
  int64_t aggregate_count;
  IndexReader::EntryPosition position;
};

struct Candidate {
  uint64_t input_rank;
  uint32_t word_begin;
  uint8_t word_count;
  bool has_legacy_score;
  double displayed_legacy_score;
  double legacy_log_score;
};

struct PairRecord {
  PairKey key;
  int64_t observed_count;
  bool present;
  bool exceeds_history_window;
  double expected_count;
  double evidence;
};

struct MeasuredAnswer {
  size_t candidate_index;
  bool coherence_defined;
  uint32_t order_begin;
  uint8_t word_count;
  size_t present_pair_count;
  double evidence_mean;
  double evidence_minimum;
};

static PairKey pair_key(WordId left, WordId right) {
  return (uint64_t(left) << 32) | uint64_t(right);
}

static WordId left_id(PairKey key) { return WordId(key >> 32); }
static WordId right_id(PairKey key) { return WordId(key); }

static double seconds_since(
    std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(
      std::chrono::steady_clock::now() - start).count();
}

static void usage(FILE* stream, char const* program) {
  fprintf(stream,
      "Usage: %s INDEX INPUT --format dfs|text --order printed|best\n"
      "       [--sort input|coherence]\n"
      "       [--top N] [--pairs]\n",
      program);
}

static void help(char const* program) {
  usage(stdout, program);
  fputs(
      "\n"
      "Measure adjacent-word coherence for a file of candidate answers using\n"
      "aggregate word and ordered-pair counts from an existing Nutrimatic "
      "index.\n"
      "Results are written as a space-aligned table to standard output.\n"
      "\n"
      "Arguments:\n"
      "  INDEX                 Completed Nutrimatic index file.\n"
      "  INPUT                 Regular, seekable candidate file; '-' and "
      "stdin are\n"
      "                        not supported.\n"
      "\n"
      "Required options:\n"
      "  --format dfs|text     Input syntax. 'dfs' expects '<score> "
      "<answer>';\n"
      "                        'text' treats each complete line as an answer.\n"
      "  --order printed|best  Score words as printed, or find the exact "
      "best\n"
      "                        directed ordering for each answer.\n"
      "\n"
      "Optional options:\n"
      "  --sort input|coherence\n"
      "                        Preserve input order (default), or rank by "
      "coherence.\n"
      "  --top N               Write at most N answers after sorting; N must "
      "be\n"
      "                        positive (default: all answers).\n"
      "  --pairs               Write a two-row, space-aligned pair-detail "
      "table\n"
      "                        to standard error.\n"
      "  -h, -?, --help        Show this help screen and exit successfully.\n"
      "\n"
      "Input:\n"
      "  Answers must be nonempty, use exactly one ASCII space between words,\n"
      "  contain at most 63 words, and occupy at most 4096 bytes per line.\n"
      "  DFS scores must be positive and finite. Every rendered word must be\n"
      "  present as a standalone word in INDEX.\n"
      "\n"
      "Scoring:\n"
      "  Pair evidence is the signed square root of the likelihood-ratio G^2\n"
      "  statistic for the observed ordered-pair count versus independence.\n"
      "  Expected pair count is left_count * right_count / corpus_total.\n"
      "  Positive evidence means the pair occurs more often than expected;\n"
      "  negative evidence means it occurs less often. One-word answers have\n"
      "  undefined coherence.\n"
      "\n"
      "Output:\n"
      "  Answer-table columns:\n"
      "    input rank    1-based line number in INPUT.\n"
      "    legacy score Leading score from a DFS row; 'nan' for text input.\n"
      "    word count    Number of rendered words.\n"
      "    stored pairs  Number of selected adjacent ordered pairs found in\n"
      "                  INDEX.\n"
      "    evidence min  Weakest signed pair evidence; coherence sorting and\n"
      "                  best-order selection maximize this first, then mean.\n"
      "    evidence mean Mean signed pair evidence over adjacent words.\n"
      "    text          Answer in the order selected by --order.\n"
      "  Evidence summaries are 'nan' for one-word answers.\n"
      "\n"
      "  With --pairs, standard error receives a two-row, space-aligned "
      "table:\n"
      "    input_rank    Answer-table input rank.\n"
      "    boundary      1-based position of this adjacent-word boundary.\n"
      "    left, right   Words on the selected boundary.\n"
      "    left_count, right_count\n"
      "                  Standalone aggregate INDEX counts.\n"
      "    pair_count    Stored aggregate count, or zero if absent.\n"
      "    present       1 if the ordered pair was found in INDEX, else 0.\n"
      "    window_exceeded\n"
      "                  1 if the pair exceeds the 40-byte index history\n"
      "                  window.\n"
      "    expected      Pair count expected under independence.\n"
      "    evidence      Signed square-root likelihood-ratio evidence.\n"
      "  Expected and evidence use fixed notation with two digits after the\n"
      "  decimal point; aggregate word and pair counts remain exact.\n"
      "  Legacy scores use scientific notation with three fractional digits;\n"
      "  other finite values use fixed notation with three fractional digits.\n"
      "  Undefined or disabled measurements are written as 'nan'. Progress,\n"
      "  resource-use summaries, diagnostics, and optional pair rows go to\n"
      "  standard error.\n"
      "\n"
      "Example:\n"
      "  measure-coherence \"$IDX\" candidates.tsv --format dfs \\\n"
      "    --order best --sort coherence --top 100\n"
      "\n"
      "Exit status:\n"
      "  0  Success or help.\n"
      "  1  Input, index, resource, allocation, or scoring failure.\n"
      "  2  Malformed command line.\n",
      stdout);
}

static void short_usage(char const* program) {
  fprintf(stderr,
      "usage: %s INDEX INPUT --format dfs|text "
      "--order printed|best "
      "[--sort input|coherence] [--top N] [--pairs]\n",
      program);
}

static bool parse_double_strict(
    char const* text, char const* option, double* result) {
  errno = 0;
  char* end = NULL;
  double const value = strtod(text, &end);
  if (errno != 0 || end == text || *end != '\0' ||
      !std::isfinite(value)) {
    fprintf(stderr, "measure-coherence: invalid %s value \"%s\"\n",
            option, text);
    return false;
  }
  *result = value;
  return true;
}

static bool parse_positive_size(
    char const* text, char const* option, size_t* result) {
  errno = 0;
  char* end = NULL;
  unsigned long long const value = strtoull(text, &end, 10);
  if (text[0] == '-' || errno != 0 || end == text || *end != '\0' ||
      value == 0 ||
      value > std::numeric_limits<size_t>::max()) {
    fprintf(stderr, "measure-coherence: invalid %s value \"%s\"\n",
            option, text);
    return false;
  }
  *result = size_t(value);
  return true;
}

static bool parse_args(char* argv[], Args* out, bool* requested_help) {
  enum {
    OPT_FORMAT = 256,
    OPT_ORDER,
    OPT_SORT,
    OPT_TOP,
    OPT_PAIRS,
    OPT_HELP,
  };
  static struct optparse_long const long_options[] = {
    {"format", OPT_FORMAT, OPTPARSE_REQUIRED},
    {"order", OPT_ORDER, OPTPARSE_REQUIRED},
    {"sort", OPT_SORT, OPTPARSE_REQUIRED},
    {"top", OPT_TOP, OPTPARSE_REQUIRED},
    {"pairs", OPT_PAIRS, OPTPARSE_NONE},
    {"help", OPT_HELP, OPTPARSE_NONE},
    {NULL, 'h', OPTPARSE_NONE},
    {NULL, '?', OPTPARSE_NONE},
    {NULL, 0, OPTPARSE_NONE},
  };

  *requested_help = false;
  out->sort = SORT_INPUT;
  out->top = std::numeric_limits<size_t>::max();
  out->pairs = false;
  bool seen_format = false;
  bool seen_order = false;
  bool seen_sort = false;
  bool seen_top = false;
  bool seen_pairs = false;

  struct optparse options;
  optparse_init(&options, argv);
  int option;
  while ((option = optparse_long(&options, long_options, NULL)) != -1) {
    bool* singleton = NULL;
    switch (option) {
      case OPT_HELP:
      case 'h':
        *requested_help = true;
        return true;
      case '?':
        if (options.errmsg[0] != '\0') {
          fprintf(stderr, "measure-coherence: %s\n", options.errmsg);
          return false;
        }
        *requested_help = true;
        return true;
      case OPT_FORMAT: singleton = &seen_format; break;
      case OPT_ORDER: singleton = &seen_order; break;
      case OPT_SORT: singleton = &seen_sort; break;
      case OPT_TOP: singleton = &seen_top; break;
      case OPT_PAIRS: singleton = &seen_pairs; break;
      default:
        fprintf(stderr, "measure-coherence: %s\n", options.errmsg);
        return false;
    }
    if (*singleton) {
      fputs("measure-coherence: repeated option\n", stderr);
      return false;
    }
    *singleton = true;

    switch (option) {
      case OPT_FORMAT:
        if (strcmp(options.optarg, "dfs") == 0) out->format = INPUT_DFS;
        else if (strcmp(options.optarg, "text") == 0)
          out->format = INPUT_TEXT;
        else {
          fputs("measure-coherence: --format must be dfs or text\n", stderr);
          return false;
        }
        break;
      case OPT_ORDER:
        if (strcmp(options.optarg, "printed") == 0)
          out->order = ORDER_PRINTED;
        else if (strcmp(options.optarg, "best") == 0)
          out->order = ORDER_BEST;
        else {
          fputs("measure-coherence: --order must be printed or best\n",
                stderr);
          return false;
        }
        break;
      case OPT_SORT:
        if (strcmp(options.optarg, "input") == 0) out->sort = SORT_INPUT;
        else if (strcmp(options.optarg, "coherence") == 0)
          out->sort = SORT_COHERENCE;
        else {
          fputs("measure-coherence: --sort must be input or coherence\n",
                stderr);
          return false;
        }
        break;
      case OPT_TOP:
        if (!parse_positive_size(options.optarg, "--top", &out->top))
          return false;
        break;
      case OPT_PAIRS: out->pairs = true; break;
    }
  }

  out->index_path = optparse_arg(&options);
  out->input_path = optparse_arg(&options);
  if (out->index_path == NULL || out->input_path == NULL ||
      optparse_arg(&options) != NULL ||
      !seen_format || !seen_order) {
    short_usage(argv[0]);
    return false;
  }
  if (strcmp(out->input_path, "-") == 0) {
    fputs("measure-coherence: INPUT must be a regular file, not stdin\n",
          stderr);
    return false;
  }
  return true;
}

static bool parse_answer_text(
    std::string const& text, uint64_t line_number, char const* path,
    std::vector<std::string>* words) {
  if (text.empty() || text.front() == ' ' || text.back() == ' ') {
    fprintf(stderr, "measure-coherence: %s:%" PRIu64
            ": answer has leading/trailing space or is empty\n",
            path, line_number);
    return false;
  }
  for (size_t i = 0; i < text.size(); ++i) {
    unsigned char const ch = (unsigned char) text[i];
    if (ch < 0x20 || ch == 0x7f) {
      fprintf(stderr, "measure-coherence: %s:%" PRIu64
              ": answer contains an ASCII control byte\n",
              path, line_number);
      return false;
    }
    if (ch == ' ' && i + 1 < text.size() && text[i + 1] == ' ') {
      fprintf(stderr, "measure-coherence: %s:%" PRIu64
              ": answer has repeated spaces\n", path, line_number);
      return false;
    }
  }

  words->clear();
  size_t begin = 0;
  while (true) {
    size_t const end = text.find(' ', begin);
    words->push_back(text.substr(begin, end - begin));
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  if (words->size() > COHERENCE_MAX_WORDS) {
    fprintf(stderr, "measure-coherence: %s:%" PRIu64
            ": answer has %zu words; limit is %zu\n",
            path, line_number, words->size(), COHERENCE_MAX_WORDS);
    return false;
  }
  return true;
}

static bool read_line(
    FILE* fp, char const* path, uint64_t line_number,
    InputFormat format, ParsedLine* result, bool* eof) {
  char buffer[COHERENCE_MAX_LINE_BYTES + 2];
  off_t const before = ftello(fp);
  if (fgets(buffer, sizeof(buffer), fp) == NULL) {
    if (feof(fp)) {
      *eof = true;
      return true;
    }
    fprintf(stderr, "measure-coherence: can't read \"%s\"\n", path);
    return false;
  }
  *eof = false;
  off_t const after = ftello(fp);
  if (before < 0 || after < before) {
    fprintf(stderr, "measure-coherence: can't track input position \"%s\"\n",
            path);
    return false;
  }
  size_t consumed = size_t(after - before);
  if (memchr(buffer, '\0', consumed) != NULL) {
    fprintf(stderr, "measure-coherence: %s:%" PRIu64
            ": line contains NUL\n", path, line_number);
    return false;
  }

  size_t length = consumed;
  bool had_newline = length > 0 && buffer[length - 1] == '\n';
  if (had_newline) {
    --length;
  } else if (!feof(fp)) {
    int const extra = fgetc(fp);
    if (length == COHERENCE_MAX_LINE_BYTES + 1 &&
        buffer[length - 1] == '\r' && extra == '\n') {
      --length;
      had_newline = true;
    } else {
      fprintf(stderr, "measure-coherence: %s:%" PRIu64
              ": line exceeds %zu bytes\n",
              path, line_number, COHERENCE_MAX_LINE_BYTES);
      return false;
    }
  }
  if (had_newline && length > 0 && buffer[length - 1] == '\r') --length;
  if (length > COHERENCE_MAX_LINE_BYTES) {
    fprintf(stderr, "measure-coherence: %s:%" PRIu64
            ": line exceeds %zu bytes\n",
            path, line_number, COHERENCE_MAX_LINE_BYTES);
    return false;
  }

  std::string line(buffer, length);
  result->has_legacy_score = format == INPUT_DFS;
  result->displayed_legacy_score = QUIET_NAN;
  result->legacy_log_score = QUIET_NAN;
  std::string answer;
  if (format == INPUT_DFS) {
    size_t const separator = line.find(' ');
    if (separator == std::string::npos || separator == 0) {
      fprintf(stderr, "measure-coherence: %s:%" PRIu64
              ": malformed DFS row\n", path, line_number);
      return false;
    }
    std::string const token = line.substr(0, separator);
    if (!parse_double_strict(
            token.c_str(), "DFS score", &result->displayed_legacy_score) ||
        result->displayed_legacy_score <= 0.0) {
      fprintf(stderr, "measure-coherence: %s:%" PRIu64
              ": DFS score must be positive and finite\n",
              path, line_number);
      return false;
    }
    result->legacy_log_score = std::log(result->displayed_legacy_score);
    answer = line.substr(separator + 1);
  } else {
    answer = line;
  }
  return parse_answer_text(answer, line_number, path, &result->words);
}

static uint64_t ordering_relaxations(size_t words, uint64_t limit) {
  if (words < 2) return 0;
  if (words - 2 >= 64) return limit == UINT64_MAX ? UINT64_MAX : limit + 1;
  uint64_t product;
  if (!checked_multiply_u64(words, words - 1, &product) ||
      !checked_multiply_u64(product, UINT64_C(1) << (words - 2), &product))
    return limit == UINT64_MAX ? UINT64_MAX : limit + 1;
  return product;
}

static bool summarize_input(
    FILE* fp, Args const& args, CoherenceResourceLimits const& limits,
    InputSummary* summary) {
  *summary = {};
  ParsedLine row;
  uint64_t line = 0;
  while (true) {
    bool eof = false;
    if (!read_line(fp, args.input_path, line + 1, args.format, &row, &eof))
      return false;
    if (eof) break;
    ++line;
    size_t const words = row.words.size();
    if (!checked_add_u64(summary->answer_count, 1,
                         &summary->answer_count) ||
        !checked_add_u64(summary->word_occurrence_count, words,
                         &summary->word_occurrence_count))
      throw std::runtime_error("input count overflow");
    summary->maximum_word_count =
        std::max(summary->maximum_word_count, words);
    for (size_t i = 0; i < words; ++i) {
      size_t with_nul;
      if (!checked_add_size(row.words[i].size(), 1, &with_nul) ||
          !checked_add_size(summary->total_word_text_bytes, with_nul,
                            &summary->total_word_text_bytes))
        throw std::runtime_error("word text size overflow");
    }

    uint64_t opportunities = words == 0 ? 0 : words - 1;
    if (args.order == ORDER_BEST) {
      if (!checked_multiply_u64(words, words - 1, &opportunities))
        throw std::runtime_error("pair opportunity count overflow");
    }
    if (!checked_add_u64(summary->required_pair_occurrence_count,
                         opportunities,
                         &summary->required_pair_occurrence_count))
      throw std::runtime_error("pair opportunity count overflow");

    if (args.order == ORDER_BEST) {
      uint64_t const relaxations =
          ordering_relaxations(words, limits.order_relaxations);
      uint64_t total;
      if (relaxations > limits.order_relaxations ||
          !checked_add_u64(summary->order_relaxation_count,
                           relaxations, &total) ||
          total > limits.order_relaxations) {
        fprintf(stderr, "measure-coherence: ordering phase estimated more "
                "than %" PRIu64 " relaxations at %s:%" PRIu64 "\n",
                limits.order_relaxations, args.input_path, line);
        return false;
      }
      summary->order_relaxation_count = total;
    }
  }
  if (summary->answer_count == 0) {
    fprintf(stderr, "measure-coherence: %s: input is empty\n",
            args.input_path);
    return false;
  }
  if (summary->word_occurrence_count > UINT32_MAX ||
      summary->answer_count > std::numeric_limits<size_t>::max() ||
      summary->required_pair_occurrence_count >
          std::numeric_limits<size_t>::max())
    throw std::runtime_error("input exceeds compact field widths");
  return true;
}

static size_t checked_size(uint64_t value, char const* owner) {
  if (value > std::numeric_limits<size_t>::max())
    throw std::runtime_error(std::string("size overflow in ") + owner);
  return size_t(value);
}

static void prepare_candidate_storage(
    InputSummary const& summary, MemoryBudget* budget,
    std::unordered_map<std::string, WordId>* word_ids,
    std::vector<WordInfo>* words,
    std::vector<Candidate>* candidates,
    std::vector<WordId>* candidate_words,
    std::vector<PairKey>* pair_occurrences,
    size_t* word_map_charge,
    size_t* pair_occurrence_charge) {
  size_t const word_count = checked_size(
      summary.word_occurrence_count, "candidate words");
  size_t const answer_count = checked_size(
      summary.answer_count, "candidates");
  size_t const pair_count = checked_size(
      summary.required_pair_occurrence_count, "pair occurrences");

  size_t map_buckets;
  size_t map_elements;
  if (!checked_multiply_size(word_count, 2 * sizeof(void*), &map_buckets) ||
      !checked_multiply_size(word_count, 96, &map_elements))
    throw std::runtime_error("size overflow in word interning");
  size_t map_bytes;
  if (!checked_add_size(map_buckets, map_elements, &map_bytes) ||
      !checked_add_size(map_bytes, summary.total_word_text_bytes, &map_bytes))
    throw std::runtime_error("size overflow in word interning");
  charge(budget, map_bytes, "word interning");
  *word_map_charge = map_bytes;

  reserve_vector(candidates, answer_count, budget, "candidates");
  reserve_vector(candidate_words, word_count, budget, "candidate words");
  reserve_vector(words, word_count, budget, "word records");
  *pair_occurrence_charge = reserve_vector(
      pair_occurrences, pair_count, budget, "pair occurrences");
  word_ids->max_load_factor(1.0f);
  word_ids->reserve(word_count);
}

static void adjust_word_map_charge(
    std::unordered_map<std::string, WordId> const& word_ids,
    MemoryBudget* budget, size_t* charged) {
  size_t bucket_bytes;
  size_t element_bytes;
  if (!checked_multiply_size(
          word_ids.bucket_count(), sizeof(void*), &bucket_bytes) ||
      !checked_multiply_size(word_ids.size(), 96, &element_bytes))
    throw std::runtime_error("size overflow adjusting word interning");
  size_t actual;
  if (!checked_add_size(bucket_bytes, element_bytes, &actual))
    throw std::runtime_error("size overflow adjusting word interning");
  for (std::unordered_map<std::string, WordId>::const_iterator it =
           word_ids.begin(); it != word_ids.end(); ++it) {
    size_t with_nul;
    if (!checked_add_size(it->first.size(), 1, &with_nul) ||
        !checked_add_size(actual, with_nul, &actual))
      throw std::runtime_error("size overflow adjusting word interning");
  }
  if (actual < *charged) budget->release(*charged - actual);
  else if (actual > *charged)
    charge(budget, actual - *charged, "word interning adjustment");
  *charged = actual;
}

static bool load_candidates(
    FILE* fp, Args const& args, InputSummary const& summary,
    std::unordered_map<std::string, WordId>* word_ids,
    std::vector<WordInfo>* words,
    std::vector<Candidate>* candidates,
    std::vector<WordId>* candidate_words) {
  ParsedLine row;
  uint64_t line = 0;
  uint64_t occurrences = 0;
  size_t text_bytes = 0;
  while (true) {
    bool eof = false;
    if (!read_line(fp, args.input_path, line + 1, args.format, &row, &eof))
      return false;
    if (eof) break;
    ++line;

    Candidate candidate;
    candidate.input_rank = line;
    candidate.word_begin = uint32_t(candidate_words->size());
    candidate.word_count = uint8_t(row.words.size());
    candidate.has_legacy_score = row.has_legacy_score;
    candidate.displayed_legacy_score = row.displayed_legacy_score;
    candidate.legacy_log_score = row.legacy_log_score;
    candidates->push_back(candidate);

    for (size_t i = 0; i < row.words.size(); ++i) {
      size_t with_nul;
      if (!checked_add_size(row.words[i].size(), 1, &with_nul) ||
          !checked_add_size(text_bytes, with_nul, &text_bytes))
        throw std::runtime_error("word text size overflow");
      std::pair<std::unordered_map<std::string, WordId>::iterator, bool> const
          inserted = word_ids->emplace(row.words[i], WordId(words->size()));
      if (inserted.second) {
        if (words->size() >= UINT32_MAX - 1)
          throw std::runtime_error("too many unique words");
        WordInfo info;
        info.text = NULL;
        info.first_input_rank = line;
        info.aggregate_count = 0;
        info.position = {IndexReader::Node(-1), 0};
        words->push_back(info);
      }
      candidate_words->push_back(inserted.first->second);
      if (!checked_add_u64(occurrences, 1, &occurrences))
        throw std::runtime_error("word occurrence count overflow");
    }
  }

  if (line != summary.answer_count ||
      occurrences != summary.word_occurrence_count ||
      text_bytes != summary.total_word_text_bytes) {
    fputs("measure-coherence: input changed between parsing passes\n",
          stderr);
    return false;
  }
  for (std::unordered_map<std::string, WordId>::const_iterator it =
           word_ids->begin(); it != word_ids->end(); ++it)
    (*words)[it->second].text = &it->first;
  return true;
}

static void append_candidate_pairs(
    Candidate const& candidate, std::vector<WordId> const& candidate_words,
    bool all_directed, std::vector<PairKey>* occurrences) {
  WordId const* ids = &candidate_words[candidate.word_begin];
  size_t const count = candidate.word_count;
  if (all_directed) {
    for (size_t left = 0; left < count; ++left)
      for (size_t right = 0; right < count; ++right)
        if (left != right)
          occurrences->push_back(pair_key(ids[left], ids[right]));
  } else {
    for (size_t i = 1; i < count; ++i)
      occurrences->push_back(pair_key(ids[i - 1], ids[i]));
  }
}

static void build_pairs(
    Args const& args, std::vector<Candidate> const& candidates,
    std::vector<WordId> const& candidate_words, MemoryBudget* budget,
    size_t occurrence_charge, std::vector<PairKey>* occurrences,
    std::vector<PairRecord>* pairs) {
  bool const all_directed = args.order == ORDER_BEST;
  for (size_t i = 0; i < candidates.size(); ++i)
    append_candidate_pairs(
        candidates[i], candidate_words, all_directed, occurrences);
  std::sort(occurrences->begin(), occurrences->end());

  size_t unique = 0;
  for (size_t i = 0; i < occurrences->size(); ) {
    ++unique;
    PairKey const key = (*occurrences)[i];
    do { ++i; } while (i < occurrences->size() &&
                       (*occurrences)[i] == key);
  }
  reserve_vector(pairs, unique, budget, "pair records");
  for (size_t i = 0; i < occurrences->size(); ) {
    PairKey const key = (*occurrences)[i];
    do { ++i; } while (i < occurrences->size() &&
                       (*occurrences)[i] == key);
    PairRecord pair;
    pair.key = key;
    pair.observed_count = 0;
    pair.present = false;
    pair.exceeds_history_window = false;
    pair.expected_count = QUIET_NAN;
    pair.evidence = QUIET_NAN;
    pairs->push_back(pair);
  }

  std::vector<PairKey>().swap(*occurrences);
  budget->release(occurrence_charge);
}

static PairRecord const& find_pair(
    std::vector<PairRecord> const& pairs, PairKey key) {
  std::vector<PairRecord>::const_iterator const found = std::lower_bound(
      pairs.begin(), pairs.end(), key,
      [](PairRecord const& pair, PairKey wanted) {
        return pair.key < wanted;
      });
  if (found == pairs.end() || found->key != key)
    throw std::runtime_error("internal error: required pair is missing");
  return *found;
}

static bool resolve_index_data(
    IndexReader const& reader,
    std::vector<WordInfo>* words, std::vector<PairRecord>* pairs,
    double* word_seconds, double* pair_seconds) {
  std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now();
  for (size_t i = 0; i < words->size(); ++i) {
    WordInfo& word = (*words)[i];
    if (!reader.aggregate_entry_position(*word.text, &word.position)) {
      fprintf(stderr, "measure-coherence: word \"%s\" from input row %" PRIu64
              " is absent from the standalone-word index\n",
              word.text->c_str(), word.first_input_rank);
      return false;
    }
    word.aggregate_count = word.position.aggregate_count;
  }
  *word_seconds = seconds_since(started);

  started = std::chrono::steady_clock::now();
  for (size_t i = 0; i < pairs->size(); ++i) {
    PairRecord& pair = (*pairs)[i];
    WordInfo const& left = (*words)[left_id(pair.key)];
    WordInfo const& right = (*words)[right_id(pair.key)];
    IndexReader::EntryPosition position;
    pair.present = reader.continuation_entry_position(
        left.position, *right.text, &position);
    pair.observed_count = pair.present ? position.aggregate_count : 0;
    pair.exceeds_history_window =
        left.text->size() + 1 + right.text->size() + 1 > 40;
    PairObservation const observation = {
      left.aggregate_count,
      right.aggregate_count,
      pair.observed_count,
      pair.present,
    };
    PairScore const score = score_pair(observation, reader.count());
    pair.expected_count = score.expected_count;
    pair.evidence = score.evidence;
  }
  *pair_seconds = seconds_since(started);
  return true;
}

struct OrderingResult {
  uint8_t size;
  std::array<uint8_t, COHERENCE_MAX_WORDS> order;
};

class OrderingScratch {
 public:
  OrderingScratch(): max_words_(0), max_entries_(0) {}

  void prepare(size_t max_words, MemoryBudget* budget) {
    max_words_ = max_words;
    if (max_words < 2) return;
    if (max_words >= 8 * sizeof(size_t))
      throw std::runtime_error("exact ordering state count overflow");
    size_t const states = size_t(1) << max_words;
    if (!checked_multiply_size(states, max_words, &max_entries_))
      throw std::runtime_error("exact ordering state count overflow");
    size_t edge_count;
    if (!checked_multiply_size(max_words, max_words, &edge_count))
      throw std::runtime_error("exact ordering edge count overflow");
    reserve_vector(
        &minimums_, max_entries_, budget, "exact ordering minimums");
    reserve_vector(
        &sums_, max_entries_, budget, "exact ordering sums");
    reserve_vector(
        &parent_, max_entries_, budget, "exact ordering parents");
    reserve_vector(
        &edges_, edge_count, budget, "exact ordering edges");
  }

  OrderingResult order(
      Candidate const& candidate,
      std::vector<WordId> const& candidate_words,
      std::vector<PairRecord> const& pairs) {
    size_t const count = candidate.word_count;
    OrderingResult result;
    result.size = uint8_t(count);
    if (count < 2) {
      if (count == 1) result.order[0] = 0;
      return result;
    }
    assert(count <= max_words_);
    uint64_t const states = UINT64_C(1) << count;
    size_t entries;
    if (states > std::numeric_limits<size_t>::max() ||
        !checked_multiply_size(size_t(states), count, &entries) ||
        entries > max_entries_)
      throw std::runtime_error("exact ordering state count mismatch");
    minimums_.assign(entries, -std::numeric_limits<double>::infinity());
    sums_.assign(entries, -std::numeric_limits<double>::infinity());
    parent_.assign(entries, UINT8_MAX);
    edges_.assign(count * count, 0.0);
    WordId const* ids = &candidate_words[candidate.word_begin];
    uint64_t prerequisites[COHERENCE_MAX_WORDS] = {};
    for (size_t right = 0; right < count; ++right) {
      for (size_t earlier = 0; earlier < right; ++earlier)
        if (ids[earlier] == ids[right])
          prerequisites[right] |= UINT64_C(1) << earlier;
      for (size_t left = 0; left < count; ++left) {
        if (left == right) continue;
        PairRecord const& pair = find_pair(
            pairs, pair_key(ids[left], ids[right]));
        edges_[left * count + right] = pair.evidence;
      }
    }

    for (size_t first = 0; first < count; ++first) {
      if (prerequisites[first] != 0) continue;
      uint64_t const mask = UINT64_C(1) << first;
      size_t const index = size_t(mask) * count + first;
      minimums_[index] = std::numeric_limits<double>::infinity();
      sums_[index] = 0.0;
    }
    for (uint64_t mask = 1; mask < states; ++mask) {
      for (size_t last = 0; last < count; ++last) {
        if ((mask & (UINT64_C(1) << last)) == 0) continue;
        size_t const current_index = size_t(mask) * count + last;
        double const current_sum = sums_[current_index];
        if (!std::isfinite(current_sum)) continue;
        double const current_minimum = minimums_[current_index];
        for (size_t next = 0; next < count; ++next) {
          uint64_t const bit = UINT64_C(1) << next;
          if ((mask & bit) != 0 ||
              (mask & prerequisites[next]) != prerequisites[next])
            continue;
          uint64_t const next_mask = mask | bit;
          size_t const index = size_t(next_mask) * count + next;
          double const edge = edges_[last * count + next];
          double const minimum = std::min(current_minimum, edge);
          double const sum = current_sum + edge;
          if (!std::isfinite(minimum) || !std::isfinite(sum))
            throw std::runtime_error("non-finite exact ordering score");
          if (minimum > minimums_[index] ||
              (minimum == minimums_[index] && sum > sums_[index]) ||
              (minimum == minimums_[index] && sum == sums_[index] &&
               last < parent_[index])) {
            minimums_[index] = minimum;
            sums_[index] = sum;
            parent_[index] = uint8_t(last);
          }
        }
      }
    }

    uint64_t const full = states - 1;
    size_t best_last = 0;
    double best_minimum = -std::numeric_limits<double>::infinity();
    double best_sum = -std::numeric_limits<double>::infinity();
    for (size_t last = 0; last < count; ++last) {
      size_t const index = size_t(full) * count + last;
      double const minimum = minimums_[index];
      double const sum = sums_[index];
      if (minimum > best_minimum ||
          (minimum == best_minimum && sum > best_sum) ||
          (minimum == best_minimum && sum == best_sum && last < best_last)) {
        best_minimum = minimum;
        best_sum = sum;
        best_last = last;
      }
    }
    if (!std::isfinite(best_minimum) || !std::isfinite(best_sum))
      throw std::runtime_error("exact ordering did not find a path");
    uint64_t mask = full;
    size_t last_position = best_last;
    for (size_t remaining = count; remaining > 0; --remaining) {
      result.order[remaining - 1] = uint8_t(last_position);
      uint8_t const previous =
          parent_[size_t(mask) * count + last_position];
      mask ^= UINT64_C(1) << last_position;
      if (remaining > 1) {
        if (previous == UINT8_MAX)
          throw std::runtime_error("exact ordering parent is missing");
        last_position = previous;
      }
    }
    return result;
  }

 private:
  size_t max_words_;
  size_t max_entries_;
  std::vector<double> minimums_;
  std::vector<double> sums_;
  std::vector<uint8_t> parent_;
  std::vector<double> edges_;
};

static MeasuredAnswer measure_candidate(
    size_t candidate_index, Args const& args,
    std::vector<Candidate> const& candidates,
    std::vector<WordId> const& candidate_words,
    std::vector<PairRecord> const& pairs,
    OrderingScratch* scratch, std::vector<uint8_t>* selected_orders) {
  Candidate const& candidate = candidates[candidate_index];
  OrderingResult ordering;
  ordering.size = candidate.word_count;
  if (args.order == ORDER_BEST) {
    ordering = scratch->order(
        candidate, candidate_words, pairs);
  } else {
    for (size_t i = 0; i < candidate.word_count; ++i)
      ordering.order[i] = uint8_t(i);
  }

  MeasuredAnswer measured;
  measured.candidate_index = candidate_index;
  measured.coherence_defined = candidate.word_count > 1;
  measured.order_begin = uint32_t(selected_orders->size());
  measured.word_count = candidate.word_count;
  measured.present_pair_count = 0;
  measured.evidence_mean = QUIET_NAN;
  measured.evidence_minimum = QUIET_NAN;
  selected_orders->insert(
      selected_orders->end(), ordering.order.begin(),
      ordering.order.begin() + ordering.size);
  if (!measured.coherence_defined) return measured;

  std::vector<double> evidence;
  evidence.reserve(candidate.word_count - 1);
  WordId const* ids = &candidate_words[candidate.word_begin];
  for (size_t i = 1; i < ordering.size; ++i) {
    WordId const left = ids[ordering.order[i - 1]];
    WordId const right = ids[ordering.order[i]];
    PairRecord const& pair = find_pair(pairs, pair_key(left, right));
    evidence.push_back(pair.evidence);
    if (pair.present) ++measured.present_pair_count;
  }
  CoherenceSummary const summary = summarize_coherence(evidence);
  measured.evidence_mean = summary.mean;
  measured.evidence_minimum = summary.minimum;
  return measured;
}

static size_t reconstruct_answer(
    MeasuredAnswer const& measured,
    std::vector<Candidate> const& candidates,
    std::vector<WordId> const& candidate_words,
    std::vector<WordInfo> const& words,
    std::vector<uint8_t> const& selected_orders,
    char* text, size_t capacity) {
  Candidate const& candidate = candidates[measured.candidate_index];
  size_t length = 0;
  for (size_t i = 0; i < measured.word_count; ++i) {
    if (i != 0) {
      if (length == capacity)
        throw std::runtime_error("reconstructed answer exceeds line limit");
      text[length++] = ' ';
    }
    uint8_t const occurrence =
        selected_orders[measured.order_begin + i];
    WordId const word = candidate_words[candidate.word_begin + occurrence];
    std::string const& value = *words[word].text;
    if (value.size() > capacity - length)
      throw std::runtime_error("reconstructed answer exceeds line limit");
    memcpy(text + length, value.data(), value.size());
    length += value.size();
  }
  return length;
}

static void sort_answers(
    Args const& args, std::vector<MeasuredAnswer> const& measured,
    std::vector<Candidate> const& candidates,
    std::vector<WordId> const& candidate_words,
    std::vector<WordInfo> const& words,
    std::vector<uint8_t> const& selected_orders,
    std::vector<size_t>* order) {
  for (size_t i = 0; i < measured.size(); ++i) order->push_back(i);
  if (args.sort == SORT_INPUT) return;
  std::sort(order->begin(), order->end(),
            [&](size_t left_index, size_t right_index) {
    MeasuredAnswer const& left = measured[left_index];
    MeasuredAnswer const& right = measured[right_index];
    if (left.coherence_defined != right.coherence_defined)
      return left.coherence_defined;
    Candidate const& left_candidate = candidates[left.candidate_index];
    Candidate const& right_candidate = candidates[right.candidate_index];
    if (!left.coherence_defined)
      return left_candidate.input_rank < right_candidate.input_rank;
    if (left.evidence_minimum != right.evidence_minimum)
      return left.evidence_minimum > right.evidence_minimum;
    if (left.evidence_mean != right.evidence_mean)
      return left.evidence_mean > right.evidence_mean;
    if (left_candidate.has_legacy_score && right_candidate.has_legacy_score &&
        left_candidate.legacy_log_score != right_candidate.legacy_log_score)
      return left_candidate.legacy_log_score >
          right_candidate.legacy_log_score;
    std::array<char, COHERENCE_MAX_LINE_BYTES> left_text;
    std::array<char, COHERENCE_MAX_LINE_BYTES> right_text;
    size_t const left_length = reconstruct_answer(
        left, candidates, candidate_words, words, selected_orders,
        left_text.data(), left_text.size());
    size_t const right_length = reconstruct_answer(
        right, candidates, candidate_words, words, selected_orders,
        right_text.data(), right_text.size());
    int const comparison = memcmp(
        left_text.data(), right_text.data(), std::min(left_length, right_length));
    if (comparison != 0) return comparison < 0;
    if (left_length != right_length) return left_length < right_length;
    return left_candidate.input_rank < right_candidate.input_rank;
  });
}

static int formatted_double_width(double value, bool scientific) {
  if (std::isnan(value)) return 3;
  int const width = snprintf(
      NULL, 0, scientific ? "%.3e" : "%.3f", value);
  if (width < 0)
    throw std::runtime_error("cannot format floating-point output");
  return width;
}

static int formatted_u64_width(uint64_t value) {
  int const width = snprintf(NULL, 0, "%" PRIu64, value);
  if (width < 0) throw std::runtime_error("cannot format integer output");
  return width;
}

static int formatted_size_width(size_t value) {
  int const width = snprintf(NULL, 0, "%zu", value);
  if (width < 0) throw std::runtime_error("cannot format integer output");
  return width;
}

static int formatted_i64_width(int64_t value) {
  int const width = snprintf(NULL, 0, "%" PRId64, value);
  if (width < 0) throw std::runtime_error("cannot format integer output");
  return width;
}

static int formatted_pair_double_width(double value) {
  int const width = snprintf(NULL, 0, "%.2f", value);
  if (width < 0)
    throw std::runtime_error("cannot format compact floating-point output");
  return width;
}

static void print_double(double value, int width, bool scientific = false) {
  if (std::isnan(value)) printf("%*s", width, "nan");
  else if (scientific) printf("%*.3e", width, value);
  else printf("%*.3f", width, value);
}

static void print_legacy_score(Candidate const& candidate, int width) {
  print_double(candidate.has_legacy_score
                   ? candidate.displayed_legacy_score : QUIET_NAN,
               width, true);
}

static void print_pair_double(FILE* stream, double value, int width) {
  fprintf(stream, "%*.2f", width, value);
}

static void print_repeated(FILE* stream, char value, int count) {
  for (int i = 0; i < count; ++i) fputc(value, stream);
}

static void print_group_header(
    FILE* stream, char const* label, char const* compact_label, int width) {
  int label_width = int(strlen(label));
  if (label_width + 2 > width) {
    label = compact_label;
    label_width = int(strlen(label));
  }
  assert(label_width + 2 <= width);
  bool const padded = label_width + 4 <= width;
  int const dash_count = width - label_width - (padded ? 4 : 2);
  int const left_dashes = (dash_count + 1) / 2;
  int const right_dashes = dash_count - left_dashes;
  fputc('|', stream);
  print_repeated(stream, '-', left_dashes);
  if (padded) fputc(' ', stream);
  fputs(label, stream);
  if (padded) fputc(' ', stream);
  print_repeated(stream, '-', right_dashes);
  fputc('|', stream);
}

using PairDetailWidths = std::array<int, 11>;

static PairDetailWidths pair_detail_widths(
    size_t count, std::vector<MeasuredAnswer> const& measured,
    std::vector<size_t> const& output_order,
    std::vector<Candidate> const& candidates,
    std::vector<WordId> const& candidate_words,
    std::vector<WordInfo> const& words,
    std::vector<PairRecord> const& pairs,
    std::vector<uint8_t> const& selected_orders) {
  PairDetailWidths widths = {
    4, 8, 4, 5, 5, 5, 6, 6, 6, 8, 8,
  };
  for (size_t answer_index = 0; answer_index < count; ++answer_index) {
    MeasuredAnswer const& answer = measured[output_order[answer_index]];
    Candidate const& candidate = candidates[answer.candidate_index];
    for (size_t boundary = 1; boundary < answer.word_count; ++boundary) {
      uint8_t const left_occurrence =
          selected_orders[answer.order_begin + boundary - 1];
      uint8_t const right_occurrence =
          selected_orders[answer.order_begin + boundary];
      WordId const left_id_value =
          candidate_words[candidate.word_begin + left_occurrence];
      WordId const right_id_value =
          candidate_words[candidate.word_begin + right_occurrence];
      WordInfo const& left = words[left_id_value];
      WordInfo const& right = words[right_id_value];
      PairRecord const& pair = find_pair(
          pairs, pair_key(left_id_value, right_id_value));
      widths[0] = std::max(
          widths[0], formatted_u64_width(candidate.input_rank));
      widths[1] = std::max(widths[1], formatted_size_width(boundary));
      widths[2] = std::max(widths[2], int(left.text->size()));
      widths[3] = std::max(widths[3], int(right.text->size()));
      widths[4] = std::max(
          widths[4], formatted_i64_width(left.aggregate_count));
      widths[5] = std::max(
          widths[5], formatted_i64_width(right.aggregate_count));
      widths[6] = std::max(
          widths[6], formatted_i64_width(pair.observed_count));
      widths[9] = std::max(
          widths[9], formatted_pair_double_width(pair.expected_count));
      widths[10] = std::max(
          widths[10], formatted_pair_double_width(pair.evidence));
    }
  }
  return widths;
}

static void print_pair_header(PairDetailWidths const& widths) {
  print_group_header(stderr, "input", "input", widths[0] + 1 + widths[1]);
  fputc(' ', stderr);
  print_group_header(stderr, "words", "words", widths[2] + 1 + widths[3]);
  fputc(' ', stderr);
  print_group_header(
      stderr, "aggregate counts", "counts",
      widths[4] + 1 + widths[5] + 1 + widths[6]);
  fputc(' ', stderr);
  print_group_header(stderr, "flags", "flags", widths[7] + 1 + widths[8]);
  fputc(' ', stderr);
  print_group_header(
      stderr, "pair", "pair", widths[9] + 1 + widths[10]);
  fputc('\n', stderr);

  fprintf(stderr,
          "%-*s %-*s %-*s %-*s %-*s %-*s %-*s %-*s %-*s %-*s %-*s\n",
          widths[0], "rank", widths[1], "boundary",
          widths[2], "left", widths[3], "right",
          widths[4], "left", widths[5], "right",
          widths[6], "pair", widths[7], "stored",
          widths[8], "window", widths[9], "expected",
          widths[10], "evidence");
}

static void print_pair_details(
    MeasuredAnswer const& measured,
    std::vector<Candidate> const& candidates,
    std::vector<WordId> const& candidate_words,
    std::vector<WordInfo> const& words,
    std::vector<PairRecord> const& pairs,
    std::vector<uint8_t> const& selected_orders,
    PairDetailWidths const& widths) {
  Candidate const& candidate = candidates[measured.candidate_index];
  for (size_t boundary = 1; boundary < measured.word_count; ++boundary) {
    uint8_t const left_occurrence =
        selected_orders[measured.order_begin + boundary - 1];
    uint8_t const right_occurrence =
        selected_orders[measured.order_begin + boundary];
    WordId const left_id_value =
        candidate_words[candidate.word_begin + left_occurrence];
    WordId const right_id_value =
        candidate_words[candidate.word_begin + right_occurrence];
    WordInfo const& left = words[left_id_value];
    WordInfo const& right = words[right_id_value];
    PairRecord const& pair = find_pair(
        pairs, pair_key(left_id_value, right_id_value));
    fprintf(stderr,
            "%*" PRIu64 " %*zu %-*s %-*s %*" PRId64 " %*" PRId64
            " %*" PRId64 " %*d %*d ",
            widths[0], candidate.input_rank, widths[1], boundary,
            widths[2], left.text->c_str(), widths[3], right.text->c_str(),
            widths[4], left.aggregate_count,
            widths[5], right.aggregate_count,
            widths[6], pair.observed_count,
            widths[7], pair.present ? 1 : 0,
            widths[8], pair.exceeds_history_window ? 1 : 0);
    print_pair_double(stderr, pair.expected_count, widths[9]);
    fputc(' ', stderr);
    print_pair_double(stderr, pair.evidence, widths[10]);
    fputc('\n', stderr);
  }
}

static void write_output(
    Args const& args, std::vector<MeasuredAnswer> const& measured,
    std::vector<size_t> const& output_order,
    std::vector<Candidate> const& candidates,
    std::vector<WordId> const& candidate_words,
    std::vector<WordInfo> const& words,
    std::vector<PairRecord> const& pairs,
    std::vector<uint8_t> const& selected_orders) {
  size_t const count = std::min(args.top, output_order.size());
  PairDetailWidths pair_widths;
  if (args.pairs) {
    pair_widths = pair_detail_widths(
        count, measured, output_order, candidates, candidate_words,
        words, pairs, selected_orders);
    print_pair_header(pair_widths);
  }
  std::array<int, 7> widths = {
    5, 6, 5, 6, 5, 4, 6,
  };
  for (size_t i = 0; i < count; ++i) {
    MeasuredAnswer const& answer = measured[output_order[i]];
    Candidate const& candidate = candidates[answer.candidate_index];
    widths[0] = std::max(
        widths[0], formatted_u64_width(candidate.input_rank));
    widths[1] = std::max(
        widths[1], formatted_double_width(
            candidate.has_legacy_score
                ? candidate.displayed_legacy_score : QUIET_NAN,
            true));
    widths[2] = std::max(
        widths[2], formatted_u64_width(answer.word_count));
    widths[3] = std::max(
        widths[3], formatted_size_width(answer.present_pair_count));
    widths[4] = std::max(
        widths[4], formatted_double_width(answer.evidence_minimum, false));
    widths[5] = std::max(
        widths[5], formatted_double_width(answer.evidence_mean, false));
    std::array<char, COHERENCE_MAX_LINE_BYTES> text;
    size_t const text_length = reconstruct_answer(
        answer, candidates, candidate_words, words, selected_orders,
        text.data(), text.size());
    widths[6] = std::max(widths[6], int(text_length));
  }

  static char const* const upper_headers[] = {
    "input", "legacy", "word", "stored",
  };
  static char const* const lower_headers[] = {
    "rank", "score", "count", "pairs",
  };
  for (size_t i = 0; i < 4; ++i) {
    printf("%-*s", widths[i], upper_headers[i]);
    fputc(' ', stdout);
  }
  print_group_header(
      stdout, "evidence", "evidence", widths[4] + 1 + widths[5]);
  printf(" %-*s", widths[6], "text");
  fputc('\n', stdout);

  for (size_t i = 0; i < 4; ++i) {
    printf("%-*s ", widths[i], lower_headers[i]);
  }
  printf("%-*s %*s", widths[4], "min", widths[5], "mean");
  printf(" %-*s\n", widths[6], "answer");

  for (size_t i = 0; i < count; ++i) {
    MeasuredAnswer const& answer = measured[output_order[i]];
    Candidate const& candidate = candidates[answer.candidate_index];
    std::array<char, COHERENCE_MAX_LINE_BYTES> text;
    size_t const text_length = reconstruct_answer(
        answer, candidates, candidate_words, words, selected_orders,
        text.data(), text.size());
    printf("%*" PRIu64 " ", widths[0], candidate.input_rank);
    print_legacy_score(candidate, widths[1]);
    printf(" %*u %*zu ",
           widths[2], unsigned(answer.word_count),
           widths[3], answer.present_pair_count);
    print_double(answer.evidence_minimum, widths[4]);
    fputc(' ', stdout);
    print_double(answer.evidence_mean, widths[5]);
    fputc(' ', stdout);
    fwrite(text.data(), 1, text_length, stdout);
    printf("%*s", widths[6] - int(text_length), "");
    fputc('\n', stdout);
    if (args.pairs) {
      fflush(stdout);
      print_pair_details(
          answer, candidates, candidate_words, words, pairs,
          selected_orders, pair_widths);
    }
  }
}

static bool regular_file(char const* path) {
  struct stat status;
  if (stat(path, &status) != 0 || !S_ISREG(status.st_mode)) {
    fprintf(stderr, "measure-coherence: INPUT \"%s\" is not a regular file\n",
            path);
    return false;
  }
  return true;
}

int run_measure_coherence(int argc, char* argv[],
                          CoherenceResourceLimits const& limits) {
  (void) argc;
  Args args;
  bool requested_help;
  if (!parse_args(argv, &args, &requested_help)) return 2;
  if (requested_help) {
    help(argv[0]);
    return 0;
  }
  if (!regular_file(args.input_path)) return 1;
  if (limits.memory_bytes == 0 || limits.order_relaxations == 0) {
    fputs("measure-coherence: resource limits must be positive\n", stderr);
    return 1;
  }

  FILE* input = fopen(args.input_path, "rb");
  if (input == NULL) {
    fprintf(stderr, "measure-coherence: can't open INPUT \"%s\"\n",
            args.input_path);
    return 1;
  }
  struct stat opened_status;
  if (fstat(fileno(input), &opened_status) != 0 ||
      !S_ISREG(opened_status.st_mode)) {
    fprintf(stderr, "measure-coherence: INPUT \"%s\" is not a regular file\n",
            args.input_path);
    fclose(input);
    return 1;
  }
  FILE* index = NULL;

  try {
    std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();
    InputSummary summary;
    if (!summarize_input(input, args, limits, &summary)) {
      fclose(input);
      return 1;
    }
    double const parse1_seconds = seconds_since(started);

    MemoryBudget budget(limits.memory_bytes);
    std::unordered_map<std::string, WordId> word_ids;
    std::vector<WordInfo> words;
    std::vector<Candidate> candidates;
    std::vector<WordId> candidate_words;
    std::vector<PairKey> pair_occurrences;
    std::vector<PairRecord> pairs;
    size_t word_map_charge;
    size_t pair_occurrence_charge;
    prepare_candidate_storage(
        summary, &budget, &word_ids, &words, &candidates,
        &candidate_words, &pair_occurrences, &word_map_charge,
        &pair_occurrence_charge);

    clearerr(input);
    if (fseeko(input, 0, SEEK_SET) != 0)
      throw std::runtime_error("cannot seek INPUT to the second pass");
    started = std::chrono::steady_clock::now();
    if (!load_candidates(
            input, args, summary, &word_ids, &words,
            &candidates, &candidate_words)) {
      fclose(input);
      input = NULL;
      return 1;
    }
    adjust_word_map_charge(word_ids, &budget, &word_map_charge);
    double const parse2_seconds = seconds_since(started);
    int const input_close_status = fclose(input);
    input = NULL;
    if (input_close_status != 0)
      throw std::runtime_error("cannot close INPUT after parsing");

    started = std::chrono::steady_clock::now();
    build_pairs(
        args, candidates, candidate_words, &budget,
        pair_occurrence_charge, &pair_occurrences, &pairs);
    double const pair_build_seconds = seconds_since(started);

    index = fopen(args.index_path, "rb");
    if (index == NULL) {
      fprintf(stderr, "measure-coherence: can't open INDEX \"%s\"\n",
              args.index_path);
      return 1;
    }
    double word_lookup_seconds = 0.0;
    double pair_lookup_seconds = 0.0;
    double scoring_seconds = 0.0;
    {
      IndexReader reader(index);
      if (!resolve_index_data(
              reader, &words, &pairs,
              &word_lookup_seconds, &pair_lookup_seconds)) {
        fclose(index);
        index = NULL;
        return 1;
      }

      std::vector<MeasuredAnswer> measured;
      std::vector<uint8_t> selected_orders;
      reserve_vector(
          &measured, candidates.size(), &budget, "answer measurements");
      reserve_vector(
          &selected_orders, candidate_words.size(), &budget,
          "selected orders");
      OrderingScratch scratch;
      if (args.order == ORDER_BEST)
        scratch.prepare(summary.maximum_word_count, &budget);

      started = std::chrono::steady_clock::now();
      for (size_t i = 0; i < candidates.size(); ++i)
        measured.push_back(measure_candidate(
            i, args, candidates, candidate_words, pairs,
            &scratch, &selected_orders));
      std::vector<size_t> output_order;
      reserve_vector(
          &output_order, measured.size(), &budget, "output ordering");
      sort_answers(
          args, measured, candidates, candidate_words, words,
          selected_orders, &output_order);
      scoring_seconds = seconds_since(started);

      fprintf(stderr,
              "measure-coherence: input: %zu answers, %zu word "
              "occurrences, %zu unique words\n",
              candidates.size(), candidate_words.size(), words.size());
      fprintf(stderr,
              "measure-coherence: pairs: %" PRIu64
              " opportunities, %zu unique directed pairs\n",
              summary.required_pair_occurrence_count, pairs.size());
      fprintf(stderr,
              "measure-coherence: ordering: %" PRIu64
              " estimated relaxations, %zu maximum words\n",
              summary.order_relaxation_count, summary.maximum_word_count);
      fprintf(stderr,
              "measure-coherence: memory: %zu charged peak bytes of %zu\n",
              budget.peak(), limits.memory_bytes);
      fprintf(stderr,
              "measure-coherence: timing: parse1 %.3f, parse2 %.3f, "
              "pair-build %.3f\n",
              parse1_seconds, parse2_seconds, pair_build_seconds);
      fprintf(stderr,
              "measure-coherence: timing: word-lookup %.3f, "
              "pair-lookup %.3f, scoring %.3f\n",
              word_lookup_seconds, pair_lookup_seconds,
              scoring_seconds);
      write_output(
          args, measured, output_order, candidates, candidate_words,
          words, pairs, selected_orders);
    }
    int const index_close_status = fclose(index);
    index = NULL;
    if (index_close_status != 0)
      throw std::runtime_error("cannot close INDEX");
    return 0;
  } catch (std::bad_alloc const&) {
    if (input != NULL) fclose(input);
    if (index != NULL) fclose(index);
    fputs("measure-coherence: allocation failure\n", stderr);
    return 1;
  } catch (std::exception const& error) {
    if (input != NULL) fclose(input);
    if (index != NULL) fclose(index);
    fprintf(stderr, "measure-coherence: %s\n", error.what());
    return 1;
  }
}
