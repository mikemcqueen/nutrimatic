#include "command.h"

#include "freq-sort.h"
#include "pair-filter.h"

std::optional<Command> make_command(std::string const& name) {
  if (name == "pfilter") return Command(PairFilter());
  if (name == "freqsort") return Command(FreqSort());
  return std::nullopt;
}
