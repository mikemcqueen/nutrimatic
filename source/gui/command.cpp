#include "command.h"

#include "find-pairs.h"
#include "freq-sort.h"
#include "pair-filter.h"

std::optional<Command> make_command(std::string const& name) {
  if (name == "pfilter") return Command(PairFilter());
  if (name == "freqsort") return Command(FreqSort());
  if (name == "pairs") return Command(FindPairs());
  return std::nullopt;
}
