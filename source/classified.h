#ifndef NUTRIMATIC_CLASSIFIED_H
#define NUTRIMATIC_CLASSIFIED_H

#include <filesystem>

#include "dfs-class-list.h"
#include "optparse.h"

// -s, --sentence N selects the pairs classified for sentence N, kept under
// $WFROOT/.wf/classified/sN. CLASSIFIED_NO_SENTENCE means none was given.
inline constexpr int CLASSIFIED_NO_SENTENCE = -1;

#define CLASSIFIED_SENTENCE_LONG_OPTION \
  { "sentence", 's', OPTPARSE_REQUIRED }

void classified_help_sentence();

// Parses the --sentence argument into `sentence`. Errors are diagnosed.
bool parse_classified_sentence(char const* arg, int* sentence);

// $WFROOT/.wf/classified/sN for `root` and `sentence`.
std::filesystem::path classified_sentence_dir(char const* root, int sentence);

// Loads sentence N's no/no.pairs into `pairs`, both word orders. The file must
// exist; errors are diagnosed, prefixed by `program`.
bool load_classified_no_pairs(
    char const* program, char const* root, int sentence, DfsPairSet* pairs);

#endif
