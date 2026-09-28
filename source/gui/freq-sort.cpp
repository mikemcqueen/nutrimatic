#include "freq-sort.h"

#include <cstdio>

bool FreqSort::run(std::vector<SharedLines> const& inputs,
                   GlobalSettings const& settings, Lines* output) const {
  if (inputs.size() != 1) {
    fprintf(stderr, "pgui: freqsort takes 1 input, not %zu\n", inputs.size());
    return false;
  }

  FreqsortOptions call = options;
  call.letters = settings.letters;
  call.used_letters = settings.used_letters;
  Freqsort freqsort;
  if (!freqsort.load(call)) return false;

  for (std::string const& line : *inputs[0]) freqsort.add(line);
  *output = freqsort.lines();
  return true;
}
