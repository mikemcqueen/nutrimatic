#ifndef NUTRIMATIC_CLASSIFIED_H
#define NUTRIMATIC_CLASSIFIED_H

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dfs-class-list.h"
#include "dfs-cli-args.h"
#include "optparse.h"

// -s, --sentence N selects the pairs classified for sentence N, kept under
// $WFROOT/.wf/classified/sN. CLASSIFIED_NO_SENTENCE means none was given.
inline constexpr int CLASSIFIED_NO_SENTENCE = -1;

#define CLASSIFIED_SENTENCE_LONG_OPTION \
  { "sentence", 's', OPTPARSE_REQUIRED }

// Help for -s, --sentence N in tools that only drop sentence N's NO pairs.
void classified_help_sentence_no();

// Help for -s, --sentence N in tools that load sentence N's YES and NO pairs
// through add_classified_sentence_pairs().
void classified_help_sentence_pairs();

// Parses the --sentence argument into `sentence`. Errors are diagnosed.
bool parse_classified_sentence(char const* arg, int* sentence);

// Sets `path` to KIND/KIND.pairs, KIND being "no" or "yes", in $WFROOT's
// classified directory: .wf/classified/sN for `sentence`, or .wf/classified
// itself for CLASSIFIED_NO_SENTENCE. A sentence's file must exist. A workflow
// need not have classified anything globally yet, so an absent global file is
// warned about and leaves `path` empty. Errors are diagnosed, prefixed by
// `program`.
bool classified_pair_file(
    char const* program, char const* root, int sentence, char const* kind,
    std::string* path);

// Loads the classified_pair_file() for `sentence` and `kind` into `pairs`,
// both word orders; an absent global file loads nothing.
bool load_classified_pairs(
    char const* program, char const* root, int sentence, char const* kind,
    DfsPairSet* pairs);

// load_classified_pairs() of the global NO pairs.
bool load_global_no_pairs(
    char const* program, char const* root, DfsPairSet* pairs);

// load_classified_pairs() of sentence N's NO pairs.
bool load_sentence_no_pairs(
    char const* program, char const* root, int sentence, DfsPairSet* pairs);

// Classified pair files by path, each kept after it is loaded and loaded
// again only when its modification time or size differs from when it was
// last loaded. Safe to share between threads.
class ClassifiedPairCache {
 public:
  // The pairs load_classified_pairs() would load for `sentence` and `kind`,
  // or null on an error, which is diagnosed, prefixed by `program`.
  std::shared_ptr<DfsPairSet const> get(
      char const* program, char const* root, int sentence, char const* kind);

 private:
  struct Entry {
    std::filesystem::file_time_type mtime;
    std::uintmax_t size = 0;
    std::shared_ptr<DfsPairSet const> pairs;
  };

  std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
};

// Adds sentence N's classified YES pairs to `args`' YES pair files and its NO
// pairs to `reject_files`, as the workflow root's global files are. The root
// is `args`' --wfroot DIR when given, otherwise $WFROOT, so --wf is not
// needed. Both files must exist, and --pairs cannot be combined with it;
// errors are diagnosed, prefixed by `program`.
bool add_classified_sentence_pairs(
    char const* program, int sentence, DfsCommonArgs* args,
    std::vector<std::string>* reject_files);

// Diagnoses a workflow `target`, sN/..., whose sentence is not `sentence`.
// CLASSIFIED_NO_SENTENCE and an empty target always agree.
bool check_classified_sentence_target(
    char const* program, int sentence, std::string const& target);

#endif
