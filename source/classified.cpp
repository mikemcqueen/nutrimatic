#include "classified.h"

#include <stdio.h>

#include <charconv>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "log.h"
#include "workflow-paths.h"

void classified_help_sentence_no() {
  dfs_help_option("-s, --sentence N",
      "drop pairs listed in $WFROOT/%s/sN/no/no.pairs",
      WORKFLOW_CLASSIFIED_PATH);
}

void classified_help_sentence_pairs() {
  dfs_help_option("-s, --sentence N",
      "load $WFROOT/%s/sN/yes/yes.pairs as YES pairs and exclude "
      "sN/no/no.pairs as if it were a pairs-only --reject file; both must "
      "exist; --wfroot DIR replaces $WFROOT; does not require --wf; a "
      "--target must be in sentence N",
      WORKFLOW_CLASSIFIED_PATH);
}

bool parse_classified_sentence(std::string_view arg, int* sentence) {
  return parse_count(arg, "--sentence", sentence);
}

bool classified_pair_file(
    std::string_view program, std::filesystem::path const& root,
    int sentence, std::string_view kind, std::string* path) {
  std::filesystem::path dir = root / WORKFLOW_CLASSIFIED_PATH;
  if (sentence != CLASSIFIED_NO_SENTENCE)
    dir /= "s" + std::to_string(sentence);
  std::filesystem::path const file =
      dir / kind / (std::string(kind) + ".pairs");
  path->clear();
  std::error_code error;
  if (sentence == CLASSIFIED_NO_SENTENCE) {
    if (!std::filesystem::exists(file, error) && !error) {
      warn(program, "classified pair file \"%s\" is not present",
          file.c_str());
      return true;
    }
  } else if (!std::filesystem::is_regular_file(file, error)) {
    report(program, "\"%s\" is not a file", file.c_str());
    return false;
  }
  *path = file.string();
  return true;
}

bool load_classified_pairs(
    std::string_view program, std::filesystem::path const& root,
    int sentence, std::string_view kind, DfsPairSet* pairs) {
  std::string path;
  if (!classified_pair_file(program, root, sentence, kind, &path))
    return false;
  return path.empty() ||
      load_pair_file(path, "classified pair list", pairs, true, true);
}

bool load_no_pairs(
    std::string_view program, std::filesystem::path const& root,
    int sentence, DfsPairSet* pairs) {
  return load_classified_pairs(
             program, root, CLASSIFIED_NO_SENTENCE, "no", pairs) &&
      (sentence == CLASSIFIED_NO_SENTENCE ||
       load_classified_pairs(program, root, sentence, "no", pairs));
}

std::shared_ptr<DfsPairSet const> ClassifiedPairCache::get(
    std::string_view program, std::filesystem::path const& root,
    int sentence, std::string_view kind) {
  std::string path;
  if (!classified_pair_file(program, root, sentence, kind, &path))
    return nullptr;
  if (path.empty()) return std::make_shared<DfsPairSet const>();

  std::error_code error;
  std::filesystem::file_time_type const mtime =
      std::filesystem::last_write_time(path, error);
  std::uintmax_t const size =
      error ? 0 : std::filesystem::file_size(path, error);
  if (error) {
    report(program, "can't stat \"%s\": %s", path.c_str(),
           error.message().c_str());
    return nullptr;
  }

  std::lock_guard<std::mutex> const lock(mutex_);
  Entry& entry = entries_[path];
  if (entry.pairs == nullptr || entry.mtime != mtime || entry.size != size) {
    auto pairs = std::make_shared<DfsPairSet>();
    if (!load_pair_file(path, "classified pair list", pairs.get(),
            true, true)) {
      entries_.erase(path);
      return nullptr;
    }
    entry = {mtime, size, std::move(pairs)};
  }
  return entry.pairs;
}

bool add_classified_sentence_pairs(
    std::string_view program, int sentence, DfsCommonArgs* args,
    std::vector<std::string>* reject_files) {
  if (args->pair_file) {
    report(program, "--pairs cannot be combined with --sentence");
    return false;
  }
  std::filesystem::path root;
  if (args->workflow_root.empty()) {
    char const* const env_root = require_workflow_root(program);
    if (env_root == NULL) return false;
    root = env_root;
  } else {
    root = args->workflow_root;
    if (!check_workflow_root(program, root)) return false;
  }
  std::string yes;
  std::string no;
  if (!classified_pair_file(program, root, sentence, "yes", &yes) ||
      !classified_pair_file(program, root, sentence, "no", &no))
    return false;
  args->yes_pair_files.push_back(yes);
  reject_files->push_back(no);
  return true;
}

bool check_classified_sentence_target(
    std::string_view program, int sentence, std::string const& target) {
  if (sentence == CLASSIFIED_NO_SENTENCE || target.empty()) return true;
  int target_sentence = -1;
  if (target[0] == 's')
    std::from_chars(
        target.data() + 1, target.data() + target.size(), target_sentence);
  if (target_sentence == sentence) return true;
  report(program, "--sentence %d does not match target \"%s\"", sentence,
         target.c_str());
  return false;
}
