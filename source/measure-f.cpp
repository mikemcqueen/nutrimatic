// Throwaway measurement tool for "F", the phrase-recovery cost described in
// findings/ancc-inspiration-summary.md section 5 and listed as item 2 of the
// order of work in section 9.
//
// Approach A replaces the trie search with a two-phase design: phase 1 pulls
// every word makeable from the bag out of the index, phase 2 runs ancc's
// bag-subtraction DFS over that list.  Phase 2's speed comes from collapsing
// entries that share a letter multiset into one branch, so its branching factor
// is the number of distinct *classes*, not the number of entries -- and its
// node count therefore scales as (classes)^depth.
//
// Contiguous corpus phrases ("pen built" scoring 7 as one segment rather than
// 2.1e-05 as two words) have to survive into A, which means phase 1 must also
// emit multi-word strings as entries.  Those entries bring new classes with
// them.  F is the factor by which they grow the class count, and F^depth is
// what it costs.
//
// So this tool extracts twice from the same bag -- once stopping at the first
// space, once walking past it -- and reports the class counts side by side.
// It is deliberately separate from find-anagrams: nothing here is meant to
// ship, it exists to decide whether the next piece of work is approach A or
// approach B.

#include "index.h"
#include "optparse.h"

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The letters a bag can hold, matching find-anagrams: lowercase and digits.
static bool is_bag_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

static double now_seconds() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

// The sorted letters of "s", ignoring spaces: the identity of its anagram
// class, and the key phase 2 would collapse on.
static std::string class_key(std::string const& s) {
  std::string key;
  for (size_t i = 0; i < s.size(); ++i)
    if (s[i] != ' ') key.push_back(s[i]);
  std::sort(key.begin(), key.end());
  return key;
}

// Walks the index depth-first under a bag constraint, exactly the space
// find-anagrams' AnagramFilter admits, and hands every completed segment to
// emit().  A "segment" is a string ending in a space; with max_words 1 that is
// a single word, and above that it is a contiguous corpus phrase whose every
// word is at least min_len long.
//
// Recursion rather than a frontier is the whole point: this is what phase 1 of
// approach A looks like, and its cost is what section 5 asserts is "bounded by
// the bag filter" without having measured it.
class Extractor {
 public:
  Extractor(IndexReader const* r, std::string const& letters,
            int min_word_len, int max_word_count):
      reader(r), min_len(min_word_len), max_words(max_word_count), nodes(0) {
    for (int i = 0; i < 256; ++i) bag[i] = 0;
    for (size_t i = 0; i < letters.size(); ++i)
      ++bag[(unsigned char) letters[i]];

    letters_left = int(letters.size());
    // One level per character consumed plus one per space, and nothing else
    // pushes a level, so this is the exact depth bound.
    tmp.resize(letters.size() + size_t(max_word_count) + 2);
    word_start = 0;
  }

  // Called once per completed segment with the string (including its trailing
  // space), the count of the trie node it ends on -- which is exactly the score
  // the search would give that segment -- and how many words it holds.
  virtual void emit(std::string const& text, int64_t count, int words) = 0;

  virtual ~Extractor() { }

  void run() {
    cur.clear();
    word_start = 0;
    walk(reader->root(), reader->count(), 0, 0, 0);
  }

  int64_t nodes_visited() const { return nodes; }

 protected:
  // The offset in "cur" where the word now in progress starts, so emit() can
  // recover the word that just finished without rescanning.
  size_t word_start;

 private:
  void walk(IndexReader::Node node, int64_t count, int word_len, int words,
            size_t depth) {
    ++nodes;

    // Asking for only what the bag can accept is what keeps this cheap: the
    // reader skips decoding the count and offset of every other child.  A
    // space is offered only where AnagramFilter would allow one.
    IndexReader::CharSet allowed;
    allowed.clear();
    for (int c = 0; c < 256; ++c)
      if (bag[c] > 0) allowed.set((unsigned char) c);
    if (word_len > 0 && word_len >= min_len) allowed.set(' ');

    std::vector<IndexReader::Choice>& here = tmp[depth];
    here.clear();
    reader->children(node, count, allowed, &here);

    for (size_t i = 0; i < here.size(); ++i) {
      IndexReader::Choice const& c = here[i];
      if (c.ch == ' ') {
        size_t const saved_start = word_start;
        cur.push_back(' ');
        emit(cur, c.count, words + 1);

        // Only keep going if another word of min_len could still fit; the
        // corpus has no phrase whose next word is empty, so this is a pruning
        // step, not a correctness one.
        if (words + 1 < max_words && letters_left >= min_len &&
            c.next != IndexReader::Node(-1)) {
          word_start = cur.size();
          walk(c.next, c.count, 0, words + 1, depth + 1);
        }

        cur.resize(cur.size() - 1);
        word_start = saved_start;
      } else {
        unsigned char const ch = (unsigned char) c.ch;
        --bag[ch];
        --letters_left;
        cur.push_back(c.ch);
        walk(c.next, c.count, word_len + 1, words, depth + 1);
        cur.resize(cur.size() - 1);
        ++letters_left;
        ++bag[ch];
      }
    }
  }

  IndexReader const* const reader;
  int const min_len;
  int const max_words;

  int bag[256];
  int letters_left;
  std::string cur;
  std::vector<std::vector<IndexReader::Choice> > tmp;
  int64_t nodes;
};

// Pass 1: words only.  Records each word's standalone count, which pass 2 needs
// to score a phrase against its own words taken separately.
class WordPass: public Extractor {
 public:
  WordPass(IndexReader const* r, std::string const& letters, int min_len):
      Extractor(r, letters, min_len, 1) { }

  void emit(std::string const& text, int64_t count, int words) {
    std::string const word(text, 0, text.size() - 1);  // drop trailing space
    counts[word] = count;
    classes.insert(class_key(word));
  }

  std::unordered_map<std::string, int64_t> counts;
  std::unordered_set<std::string> classes;
};

// Pass 2: phrases.  Every segment of two or more words is a candidate entry;
// the question is how many *new* classes they contribute on top of pass 1's.
//
// Section 5 proposes a mitigation: keep a phrase only where being contiguous
// actually beats spelling the same words out as separate segments, since a
// phrase that doesn't beat its own words adds a class for nothing.  Both
// figures are reported, because the mitigation turns out to prune almost
// nothing (see the score model below).
class PhrasePass: public Extractor {
 public:
  PhrasePass(IndexReader const* r, std::string const& letters, int min_len,
             int max_words, double restart, int64_t total,
             std::unordered_map<std::string, int64_t> const* word_counts,
             std::unordered_set<std::string> const* word_classes,
             int64_t dump_count):
      Extractor(r, letters, min_len, max_words),
      counts(word_counts),
      log_restart(log(restart) - log(double(total))),
      phrases(0), kept(0), kept_literal(0), missing_words(0),
      min_margin(1e300), margin_sum(0.0), narrow(0), dump(dump_count) {
    classes = *word_classes;
    kept_classes = *word_classes;
    literal_classes = *word_classes;
    log_prod.assign(1, 0.0);
  }

  void emit(std::string const& text, int64_t count, int words) {
    // The word that just ended, whose standalone count closes out the product.
    std::string const word(text, word_start, text.size() - 1 - word_start);

    double word_log;
    std::unordered_map<std::string, int64_t>::const_iterator it =
        counts->find(word);
    if (it == counts->end()) {
      // Can't happen: every word of an extractable phrase is itself at least
      // min_len long and spelled from a subset of the bag, so pass 1 found it.
      ++missing_words;
      word_log = 0.0;
    } else {
      word_log = log(double(it->second));
    }

    // The product of the counts of every word in this phrase, in logs because
    // four counts of 10^7 overflow nothing but are awkward to compare against
    // a restart^3 that underflows a double at depth.
    double const prod = log_prod[size_t(words - 1)] + word_log;
    if (size_t(words) >= log_prod.size()) log_prod.resize(size_t(words) + 1);
    log_prod[size_t(words)] = prod;

    if (words < 2) return;  // pass 1 already has the single words

    ++phrases;
    if (dump > 0) {
      --dump;
      printf("  phrase \"%.*s\" scores %" PRId64 " contiguous,"
             " %.4g split into %d segments\n",
             int(text.size() - 1), text.c_str(), count,
             exp(prod + log_restart * double(words - 1)), words);
    }
    std::string const key = class_key(text);
    classes.insert(key);

    // Score model, from SearchDriver::step().  One segment scores the count of
    // the node it ends on.  The same words as k separate segments score
    //
    //   prod(count(w_i)) * (restart / total)^(k-1)
    //
    // because each restart multiplies the running score by "restart" and then
    // re-seeds the count at the corpus total, which the next segment's count
    // divides back out.  Section 5 writes the test without that /total, which
    // makes it far stricter than the search it is meant to model.
    double const split = prod + log_restart * double(words - 1);
    double const mine = log(double(count));
    if (mine > split) {
      ++kept;
      kept_classes.insert(key);
      // By how much, in orders of magnitude.  If this is never small the test
      // is not a filter, it is a tautology.
      double const margin = (mine - split) / log(10.0);
      if (margin < min_margin) min_margin = margin;
      if (margin < 3.0) ++narrow;
      margin_sum += margin;
    }
    if (mine > prod + log(1e-6) * double(words - 1)) {
      ++kept_literal;
      literal_classes.insert(key);
    }
  }

  std::unordered_map<std::string, int64_t> const* counts;
  double log_restart;

  std::vector<double> log_prod;  // by word index along the current path
  std::unordered_set<std::string> classes;          // words + every phrase
  std::unordered_set<std::string> kept_classes;     // words + phrases that pay
  std::unordered_set<std::string> literal_classes;  // section 5's test, as written
  int64_t phrases, kept, kept_literal, missing_words;
  double min_margin, margin_sum;
  int64_t narrow;
  int64_t dump;  // remaining phrases to print, for eyeballing the score model
};

int main(int argc, char* argv[]) {
  static struct optparse_long const long_options[] = {
    { "used-letters", 'u', OPTPARSE_REQUIRED },
    { "min-word-length", 'm', OPTPARSE_REQUIRED },
    { "max-words", 'x', OPTPARSE_REQUIRED },
    { "restart", 'r', OPTPARSE_REQUIRED },
    { "dump", 'd', OPTPARSE_REQUIRED },
    { NULL, 0, OPTPARSE_NONE },
  };

  int min_len = 4, max_words = 0;
  double restart = 1e-6;
  int64_t dump = 0;
  std::string used;

  struct optparse options;
  optparse_init(&options, argv);

  int opt;
  while ((opt = optparse_long(&options, long_options, NULL)) != -1) {
    switch (opt) {
      case 'u': used += options.optarg; break;
      case 'm': min_len = atoi(options.optarg); break;
      case 'x': max_words = atoi(options.optarg); break;
      case 'r': restart = atof(options.optarg); break;
      case 'd': dump = atoll(options.optarg); break;
      default:
        fprintf(stderr, "error: %s\n", options.errmsg);
        return 2;
    }
  }

  char const* index_file = optparse_arg(&options);
  char const* letters_arg = optparse_arg(&options);
  if (index_file == NULL || letters_arg == NULL) {
    fprintf(stderr,
        "usage: %s input.index letters"
        " [-u used-letters] [-m min-word-length] [-x max-words]"
        " [-r restart] [-d dump-phrases]\n",
        argv[0]);
    return 2;
  }

  int have[256] = { 0 };
  for (char const* p = letters_arg; *p; ++p) {
    if (*p == ' ') continue;
    if (!is_bag_char(*p)) {
      fprintf(stderr, "error: bad character '%c' in letters\n", *p);
      return 2;
    }
    ++have[(unsigned char) *p];
  }
  for (size_t i = 0; i < used.size(); ++i) {
    if (used[i] == ' ') continue;
    if (have[(unsigned char) used[i]]-- <= 0) {
      fprintf(stderr, "error: no '%c' left to use\n", used[i]);
      return 2;
    }
  }

  std::string letters;
  for (int c = 0; c < 256; ++c)
    letters.append(size_t(have[c] > 0 ? have[c] : 0), char(c));

  if (min_len < 1) min_len = 1;
  if (max_words <= 0) max_words = int(letters.size()) / min_len;

  FILE* fp = fopen(index_file, "rb");
  if (fp == NULL) {
    fprintf(stderr, "error: can't open \"%s\"\n", index_file);
    return 1;
  }
  IndexReader reader(fp);

  printf("bag: %zu letters \"%s\", -m %d, -x %d, restart %g, corpus total %"
         PRId64 "\n",
         letters.size(), letters.c_str(), min_len, max_words, restart,
         reader.count());

  double const t0 = now_seconds();
  WordPass words(&reader, letters, min_len);
  words.run();
  double const t1 = now_seconds();

  PhrasePass phrases(&reader, letters, min_len, max_words, restart,
                     reader.count(), &words.counts, &words.classes, dump);
  phrases.run();
  double const t2 = now_seconds();

  size_t const wc = words.classes.size(), ac = phrases.classes.size();
  size_t const kc = phrases.kept_classes.size();

  printf("\n");
  printf("words only:   %zu entries, %zu classes (collapse %.2fx),"
         " %" PRId64 " nodes, %.2f s\n",
         words.counts.size(), wc,
         wc ? double(words.counts.size()) / double(wc) : 0.0,
         words.nodes_visited(), t1 - t0);
  printf("with phrases: %" PRId64 " phrase entries (%" PRId64 " pay for"
         " themselves, %" PRId64 " under section 5's literal test),"
         " %" PRId64 " nodes, %.2f s\n",
         phrases.phrases, phrases.kept, phrases.kept_literal,
         phrases.nodes_visited(), t2 - t1);
  if (phrases.missing_words)
    printf("WARNING: %" PRId64 " phrase words absent from the word pass\n",
           phrases.missing_words);

  printf("\n");
  printf("classes, words only:      %zu\n", wc);
  printf("classes, + all phrases:   %zu   F = %.3f\n", ac,
         wc ? double(ac) / double(wc) : 0.0);
  printf("classes, + phrases kept:  %zu   F = %.3f\n", kc,
         wc ? double(kc) / double(wc) : 0.0);
  printf("classes, + literal test:  %zu   F = %.3f\n",
         phrases.literal_classes.size(),
         wc ? double(phrases.literal_classes.size()) / double(wc) : 0.0);

  // Where the new classes sit in the length distribution.  This matters
  // because F^depth silently assumes the extra classes are usable at every
  // level, and a phrase is at least 2*min_len letters long -- so most of them
  // can only be picked while the bag is still nearly full.
  std::vector<size_t> by_len_w(letters.size() + 1, 0), by_len_a(by_len_w);
  for (std::unordered_set<std::string>::const_iterator i = words.classes.begin();
       i != words.classes.end(); ++i)
    if (i->size() <= letters.size()) ++by_len_w[i->size()];
  for (std::unordered_set<std::string>::const_iterator i = phrases.classes.begin();
       i != phrases.classes.end(); ++i)
    if (i->size() <= letters.size()) ++by_len_a[i->size()];

  printf("\nclasses by letters used (words -> words+phrases):\n");
  for (size_t n = 1; n <= letters.size(); ++n) {
    if (by_len_a[n] == 0) continue;
    printf("  %2zu: %8zu -> %8zu  %5.2fx\n", n, by_len_w[n], by_len_a[n],
           by_len_w[n] ? double(by_len_a[n]) / double(by_len_w[n]) : 0.0);
  }

  if (phrases.kept > 0) {
    printf("\nphrases beat their own words split apart by %.1f orders of"
           " magnitude on average, %.1f at the closest;"
           " %" PRId64 " of %" PRId64 " are within 3\n",
           phrases.margin_sum / double(phrases.kept), phrases.min_margin,
           phrases.narrow, phrases.kept);
  }

  // What F actually costs: phase 2's node count scales as F^depth, and
  // findings/ancc-inspiration-summary.md section 0 measures node count growing
  // 3.2x per additional letter, so F^depth converts straight into bag length.
  if (wc && ac > wc) {
    double const f = double(ac) / double(wc);
    int const depth = max_words;
    printf("\nat depth %d: %.1fx nodes, %.1f letters of reach lost\n",
           depth, pow(f, depth), log(pow(f, depth)) / log(3.2));
  }
  return 0;
}
