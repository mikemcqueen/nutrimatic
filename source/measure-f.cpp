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

// Phase 0 of plans/dfs.md: ancc's check_dict (findings/ancc-inspiration.md
// T2/T3/T5), ported to run over letter-multiset *classes* rather than
// spellings.  It emits nothing -- it is a node counter, to settle whether the
// collapsing DFS over the phrase-included class list costs what §3 of
// findings/phrase-recovery-cost.md predicts.  Deliberately faithful to ancc so
// its published node counts (5,488,296 at 19 letters, words only) reproduce.
//
// The 36 bag symbols, matching is_bag_char: a-z then 0-9.
static int const NSYM = 36;
static int sym_index(unsigned char c) {
  if (c >= 'a' && c <= 'z') return c - 'a';
  if (c >= '0' && c <= '9') return 26 + (c - '0');
  return -1;
}

class CollapseDFS {
 public:
  // class_keys: the distinct anagram-class keys (sorted letters, no spaces),
  //   from measure-f's class_key().
  // freq: per-symbol frequency over the extracted entry list, which fixes the
  //   global rarest-first priority order -- ancc's least-frequent-in-dictionary
  //   (calc_letter_freq): each entry counted once, letters with multiplicity.
  // max_depth: hard cap on the number of classes (segments) in a solution,
  //   = floor(letters / min_len), the emergent cap of plans/dfs.md §"The search".
  CollapseDFS(std::vector<std::string> const& class_keys,
              int64_t const freq[NSYM], int max_depth):
      max_depth(max_depth), nodes(0), fits(0), iters(0), solutions(0) {
    // Priority order: symbols ascending by frequency, ties by symbol index
    // (a<b<...), a symbol absent from every entry treated as 5000 exactly as
    // ancc's calc_letter_freq does.  rank 0 is the rarest letter.
    int order[NSYM];
    int64_t k[NSYM];
    for (int s = 0; s < NSYM; ++s) { order[s] = s; k[s] = freq[s] ? freq[s] : 5000; }
    std::sort(order, order + NSYM, [&](int a, int b) {
      return k[a] != k[b] ? k[a] < k[b] : a < b;
    });
    for (int r = 0; r < NSYM; ++r) { rank_to_sym[r] = order[r]; rank_of_sym[order[r]] = r; }

    // One record per class: its (symbol,count) letters, total length, and the
    // rank of its own rarest symbol.  Any class that is a subset of a bag whose
    // rarest present letter it contains has that letter as its own rarest (see
    // plans/dfs.md §"candidate index"), so bucketing on own-rarest makes "the
    // classes covering the forced letter" an O(1) range.
    classes.reserve(class_keys.size());
    for (size_t ci = 0; ci < class_keys.size(); ++ci) {
      std::string const& key = class_keys[ci];
      int cnt[NSYM] = { 0 };
      for (size_t i = 0; i < key.size(); ++i) {
        int s = sym_index((unsigned char) key[i]);
        if (s >= 0) ++cnt[s];
      }
      Class c;
      c.len = 0;
      int best = NSYM;
      for (int s = 0; s < NSYM; ++s) if (cnt[s] > 0) {
        c.letters.push_back(std::make_pair((uint8_t) s, (uint8_t) cnt[s]));
        c.len += cnt[s];
        if (rank_of_sym[s] < best) best = rank_of_sym[s];
      }
      c.rarest_rank = best;
      classes.push_back(c);
    }

    // Sort class indices into rarest-rank buckets.  Any fixed total order within
    // a bucket canonicalises correctly (plans/dfs.md §"The search"); sort by
    // (rank, length desc, key) to stay close to ancc's long->short order.
    order_idx.resize(classes.size());
    for (size_t i = 0; i < classes.size(); ++i) order_idx[i] = int(i);
    std::sort(order_idx.begin(), order_idx.end(), [&](int a, int b) {
      if (classes[a].rarest_rank != classes[b].rarest_rank)
        return classes[a].rarest_rank < classes[b].rarest_rank;
      if (classes[a].len != classes[b].len) return classes[a].len > classes[b].len;
      return class_keys[a] < class_keys[b];
    });
    int const n = int(order_idx.size());
    for (int r = 0; r <= NSYM; ++r) bucket_start[r] = n;
    for (int pos = n - 1; pos >= 0; --pos)
      bucket_start[classes[order_idx[pos]].rarest_rank] = pos;
    // Collapse empty ranks so bucket_start stays non-decreasing: an absent rank
    // r gets bucket_start[r] == bucket_start[r+1], i.e. an empty range.
    for (int r = NSYM - 1; r >= 0; --r)
      if (bucket_start[r] > bucket_start[r + 1]) bucket_start[r] = bucket_start[r + 1];
  }

  void run(std::string const& letters) {
    for (int s = 0; s < NSYM; ++s) bag[s] = 0;
    int left = 0;
    for (size_t i = 0; i < letters.size(); ++i) {
      int s = sym_index((unsigned char) letters[i]);
      if (s >= 0) { ++bag[s]; ++left; }
    }
    walk(left, 0, 0, 0);
  }

  int64_t nodes_visited() const { return nodes; }
  int64_t fits_count() const { return fits; }
  int64_t iters_count() const { return iters; }
  int64_t solutions_found() const { return solutions; }
  size_t class_count() const { return classes.size(); }

  // Independent oracle: enumerate class multisets by non-decreasing class index
  // over *all* classes (no forced-letter collapsing), which visits each multiset
  // exactly once.  This is the pre-collapse search (findings/ancc-inspiration.md:
  // the 7.7M-node 14-letter number) and its solution count is ground truth --
  // the forced-letter DFS above must agree with it exactly.
  int64_t reference_solutions() const { return ref_solutions; }
  int64_t reference_nodes() const { return ref_nodes; }
  void run_reference(std::string const& letters) {
    for (int s = 0; s < NSYM; ++s) bag[s] = 0;
    int left = 0;
    for (size_t i = 0; i < letters.size(); ++i) {
      int s = sym_index((unsigned char) letters[i]);
      if (s >= 0) { ++bag[s]; ++left; }
    }
    ref_nodes = ref_solutions = 0;
    ref_walk(left, 0, 0);
  }

 private:
  struct Class {
    std::vector<std::pair<uint8_t, uint8_t> > letters;
    int len;
    int rarest_rank;
  };

  void walk(int left, int level, int entry_point, int old_high) {
    ++nodes;

    // Forced letter L: the rarest symbol still in the bag.  The forced rank only
    // rises down a path (letters only leave), so resume the scan at the parent's
    // rank rather than from 0 -- ancc's old_high_letter_num.
    int r = old_high;
    while (r < NSYM && bag[rank_to_sym[r]] == 0) ++r;
    if (r == NSYM) return;

    // Only classes whose own rarest letter is L are candidates, and among those
    // covering the *same* forced letter require non-decreasing index to collapse
    // permutations (ancc's entry_point).  Passing the chosen pos (not pos+1)
    // lets a class repeat when L occurs more than once.
    int start = bucket_start[r];
    if (entry_point > start) start = entry_point;
    int const end = bucket_start[r + 1];

    for (int pos = start; pos < end; ++pos) {
      ++iters;
      Class const& c = classes[order_idx[pos]];
      bool fit = true;
      for (size_t i = 0; i < c.letters.size(); ++i)
        if (bag[c.letters[i].first] < c.letters[i].second) { fit = false; break; }
      if (!fit) continue;
      ++fits;

      int const nleft = left - c.len;
      if (nleft == 0) { ++solutions; continue; }  // bag empty: a solution
      if (level + 1 >= max_depth) continue;        // cap reached, cannot extend

      for (size_t i = 0; i < c.letters.size(); ++i) bag[c.letters[i].first] -= c.letters[i].second;
      walk(nleft, level + 1, pos, r);
      for (size_t i = 0; i < c.letters.size(); ++i) bag[c.letters[i].first] += c.letters[i].second;
    }
  }

  void ref_walk(int left, int level, int min_index) {
    ++ref_nodes;
    for (int i = min_index; i < int(classes.size()); ++i) {
      Class const& c = classes[i];
      bool fit = true;
      for (size_t j = 0; j < c.letters.size(); ++j)
        if (bag[c.letters[j].first] < c.letters[j].second) { fit = false; break; }
      if (!fit) continue;
      int const nleft = left - c.len;
      if (nleft == 0) { ++ref_solutions; continue; }
      if (level + 1 >= max_depth) continue;
      for (size_t j = 0; j < c.letters.size(); ++j) bag[c.letters[j].first] -= c.letters[j].second;
      ref_walk(nleft, level + 1, i);
      for (size_t j = 0; j < c.letters.size(); ++j) bag[c.letters[j].first] += c.letters[j].second;
    }
  }

  int const max_depth;
  int rank_to_sym[NSYM];
  int rank_of_sym[NSYM];
  std::vector<Class> classes;
  std::vector<int> order_idx;
  int bucket_start[NSYM + 1];
  int bag[NSYM];
  int64_t nodes, fits, iters, solutions;
  int64_t ref_nodes, ref_solutions;
};

int main(int argc, char* argv[]) {
  static struct optparse_long const long_options[] = {
    { "used-letters", 'u', OPTPARSE_REQUIRED },
    { "min-word-length", 'm', OPTPARSE_REQUIRED },
    { "max-words", 'x', OPTPARSE_REQUIRED },
    { "max-classes", 'k', OPTPARSE_REQUIRED },
    { "restart", 'r', OPTPARSE_REQUIRED },
    { "dump", 'd', OPTPARSE_REQUIRED },
    { "reference", 'R', OPTPARSE_NONE },
    { "dump-words", 'W', OPTPARSE_NONE },
    { NULL, 0, OPTPARSE_NONE },
  };

  int min_len = 4, max_words = 0, max_classes = 0;
  bool reference = false, dump_words = false;
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
      case 'k': max_classes = atoi(options.optarg); break;
      case 'r': restart = atof(options.optarg); break;
      case 'd': dump = atoll(options.optarg); break;
      case 'R': reference = true; break;
      case 'W': dump_words = true; break;
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
        " [-k max-classes] [-r restart] [-d dump-phrases]\n",
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

  if (dump_words) {
    for (std::unordered_map<std::string, int64_t>::const_iterator i =
             words.counts.begin(); i != words.counts.end(); ++i)
      printf("%s\n", i->first.c_str());
    return 0;
  }

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

  // Phase 0 (plans/dfs.md): the collapsing bag-subtraction DFS itself, over the
  // class lists just built.  This is the measurement F^depth was a proxy for --
  // the actual phase-2 node count, words only versus phrases included.
  //
  // The priority order is fixed once from the *word* entry list and reused for
  // both runs, so the only thing that changes between them is the candidate set
  // (words vs words+phrases).  That reproduces ancc's words-only node count as a
  // known target and isolates the phrase effect as a clean node multiplier.
  int64_t freq[NSYM] = { 0 };
  for (std::unordered_map<std::string, int64_t>::const_iterator i =
           words.counts.begin(); i != words.counts.end(); ++i) {
    std::string const& w = i->first;
    for (size_t j = 0; j < w.size(); ++j) {
      int s = sym_index((unsigned char) w[j]);
      if (s >= 0) ++freq[s];
    }
  }

  int const cap = max_classes > 0 ? max_classes : int(letters.size()) / min_len;

  std::vector<std::string> word_keys(words.classes.begin(), words.classes.end());
  std::vector<std::string> all_keys(phrases.classes.begin(), phrases.classes.end());

  double const p0 = now_seconds();
  CollapseDFS dfs_words(word_keys, freq, cap);
  dfs_words.run(letters);
  double const p1 = now_seconds();
  CollapseDFS dfs_all(all_keys, freq, cap);
  dfs_all.run(letters);
  double const p2 = now_seconds();

  printf("\nphase 2 DFS (max %d classes/solution):\n", cap);
  printf("  words only:   %zu classes, %" PRId64 " solutions, %" PRId64
         " nodes (%" PRId64 " fits, %" PRId64 " iters), %.2f s\n",
         dfs_words.class_count(), dfs_words.solutions_found(),
         dfs_words.nodes_visited(), dfs_words.fits_count(),
         dfs_words.iters_count(), p1 - p0);
  printf("  with phrases: %zu classes, %" PRId64 " solutions, %" PRId64
         " nodes (%" PRId64 " fits, %" PRId64 " iters), %.2f s\n",
         dfs_all.class_count(), dfs_all.solutions_found(),
         dfs_all.nodes_visited(), dfs_all.fits_count(),
         dfs_all.iters_count(), p2 - p1);
  if (dfs_words.nodes_visited() > 0)
    printf("  node multiplier (phrases / words): %.3fx\n",
           double(dfs_all.nodes_visited()) / double(dfs_words.nodes_visited()));

  if (reference) {
    double const r0 = now_seconds();
    dfs_words.run_reference(letters);
    double const r1 = now_seconds();
    printf("  reference (words, non-decreasing index, no forcing):"
           " %" PRId64 " solutions, %" PRId64 " nodes, %.2f s\n",
           dfs_words.reference_solutions(), dfs_words.reference_nodes(), r1 - r0);
  }

  return 0;
}
