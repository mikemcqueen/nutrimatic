#include "seed-pairs-impl.h"

#include "dfs-cli-args.h"
#include "log.h"

#include <utility>

bool SeedPairs::load(std::string_view program,
                     SeedPairsOptions const& options) {
  program_ = program;
  options_ = options;
  std::string error;
  std::string stale;
  if (!p1_.open(options_.p1, &error, &stale)) {
    report(program_, "%s", error.c_str());
    return false;
  }
  if (!stale.empty()) alert(program_, "%s", stale.c_str());
  return true;
}

bool SeedPairs::run(SeedPairsInput const& input,
                    std::vector<SeedPair>* out) const {
  size_t const threads = resolve_search_threads(options_.threads);
  std::string error;
  bool const read = std::visit(
      [&](auto const& source) {
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(source)>,
                                     std::filesystem::path>)
          return read_seed_pairs(source, &p1_, threads, out, &error);
        else
          return read_seed_pairs(source, "pairs", &p1_, threads, out, &error);
      },
      input);
  if (!read ||
      (!options_.forced.empty() &&
       !add_forced_pairs(options_.forced, &p1_, out, &error))) {
    report(program_, "%s", error.c_str());
    out->clear();
    return false;
  }
  if (!options_.forced.empty() && options_.filter.dictionary)
    warn_forced_words_missing(program_, *out, options_.forced,
                              *options_.filter.dictionary,
                              options_.dictionary_path);
  std::erase_if(*out, [&](SeedPair const& pair) {
    return !seed_pair_passes(pair, options_.filter);
  });
  return true;
}
