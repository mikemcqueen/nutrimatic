#include "dfs-class-list.h"
#include "dfs-output.h"
#include "dfs-search.h"
#include "index.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

static void check(bool ok, char const* message) {
  if (!ok) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

static void check_close(double actual, double expected,
                        char const* message) {
  if (fabs(actual - expected) >= 1e-12) {
    fprintf(stderr, "FAIL: %s: expected %.17g, got %.17g\n",
            message, expected, actual);
    exit(1);
  }
}

static size_t find_class(DfsClassList const& classes,
                         std::string const& key) {
  for (size_t i = 0; i < classes.classes().size(); ++i)
    if (classes.classes()[i].key == key) return i;
  fprintf(stderr, "FAIL: class \"%s\" is missing\n", key.c_str());
  exit(1);
}

static int64_t member_count(DfsClassList const& classes, size_t class_index,
                            std::string const& text) {
  std::vector<DfsClassMember> const& members =
      classes.classes()[class_index].members;
  for (size_t i = 0; i < members.size(); ++i)
    if (members[i].text == text) return members[i].count;
  fprintf(stderr, "FAIL: member \"%s\" is missing\n", text.c_str());
  exit(1);
}

static double representative_score(
    DfsClassList const& classes, std::vector<size_t> const& path,
    double segment_penalty, int64_t corpus_total) {
  double score = 0.0;
  for (size_t i = 0; i < path.size(); ++i) {
    if (i != 0)
      score -= log(segment_penalty) + log(double(corpus_total));
    score += log(double(classes.classes()[path[i]].members[0].count));
  }
  return score;
}

static std::string word_set_key(std::vector<std::string> words) {
  std::sort(words.begin(), words.end());
  std::string key;
  for (size_t i = 0; i < words.size(); ++i) {
    if (i != 0) key.push_back(' ');
    key += words[i];
  }
  return key;
}

struct Expected {
  double log_score;
  std::string key;
};

static bool expected_order(Expected const& a, Expected const& b) {
  if (a.log_score != b.log_score) return a.log_score > b.log_score;
  return a.key < b.key;
}

static void exhaustive_product_test() {
  FILE* fp = tmpfile();
  check(fp != NULL, "could not create Cartesian-product index");
  {
    IndexWriter writer(fp);
    writer.next("ab ", 0, 11);
    writer.next("ba ", 0, 7);
    writer.next("cde ", 0, 13);
    writer.next("dec ", 0, 5);
    writer.next("ecd ", 0, 3);
    writer.next("fg ", 0, 17);
    writer.next("gf ", 0, 2);
    writer.next(NULL, 0, 0);
  }
  fflush(fp);
  rewind(fp);

  {
    IndexReader reader(fp);
    DfsClassList classes(&reader, "abcdefg", 2, false);
    size_t const ab = find_class(classes, "ab");
    size_t const cde = find_class(classes, "cde");
    size_t const fg = find_class(classes, "fg");
    std::vector<size_t> path;
    path.push_back(ab);
    path.push_back(cde);
    path.push_back(fg);

    double const segment_penalty = DFS_DEFAULT_SEGMENT_PENALTY;
    DfsTopN output(&classes, 14);
    output.emit(
        path, representative_score(
            classes, path, segment_penalty, reader.count()));
    std::vector<DfsSpelling> const actual = output.take_sorted_results();

    std::vector<Expected> expected;
    std::vector<DfsClassMember> const& ab_members =
        classes.classes()[ab].members;
    std::vector<DfsClassMember> const& cde_members =
        classes.classes()[cde].members;
    std::vector<DfsClassMember> const& fg_members =
        classes.classes()[fg].members;
    double const segment_boundary =
        -log(segment_penalty) - log(double(reader.count()));
    for (size_t ai = 0; ai < ab_members.size(); ++ai)
      for (size_t ci = 0; ci < cde_members.size(); ++ci)
        for (size_t fi = 0; fi < fg_members.size(); ++fi) {
          Expected row;
          row.log_score =
              log(double(ab_members[ai].count)) +
              log(double(cde_members[ci].count)) +
              log(double(fg_members[fi].count)) + 2.0 * segment_boundary;
          std::vector<std::string> words;
          words.push_back(ab_members[ai].text);
          words.push_back(cde_members[ci].text);
          words.push_back(fg_members[fi].text);
          row.key = word_set_key(words);
          expected.push_back(row);
        }
    std::sort(expected.begin(), expected.end(), expected_order);

    check(expected.size() == 12, "reference product has the wrong size");
    check(actual.size() == expected.size(),
          "lazy expansion did not exhaust the 2x3x2 product");
    for (size_t i = 0; i < expected.size(); ++i) {
      check(actual[i].word_set_key == expected[i].key,
            "Cartesian-product spelling order is wrong");
      check_close(actual[i].log_score, expected[i].log_score,
                  "Cartesian-product score is wrong");
    }
  }

  fclose(fp);
}

static void heap_churn_test() {
  FILE* fp = tmpfile();
  check(fp != NULL, "could not create heap-churn index");
  {
    IndexWriter writer(fp);
    writer.next("aa ", 0, 1);
    writer.next("bb ", 0, 1);
    writer.next("cc ", 0, 1);
    writer.next("dd ", 0, 1);
    writer.next("ee ", 0, 1);
    writer.next("ff ", 0, 1);
    writer.next(NULL, 0, 0);
  }
  fflush(fp);
  rewind(fp);

  {
    IndexReader reader(fp);
    DfsClassList classes(&reader, "aabbccddeeff", 2, false);
    size_t indexes[6] = {
      find_class(classes, "aa"),
      find_class(classes, "bb"),
      find_class(classes, "cc"),
      find_class(classes, "dd"),
      find_class(classes, "ee"),
      find_class(classes, "ff"),
    };

    DfsTopN output(&classes, 5);
    for (size_t i = 0; i < 5; ++i)
      output.emit(std::vector<size_t>(1, indexes[i]),
                  log(double(10 * (i + 1))));

    // Increase the root, then a non-root with children, forcing index updates
    // through two different sift-down paths.
    output.emit(std::vector<size_t>(1, indexes[0]), log(45.0));
    output.emit(std::vector<size_t>(1, indexes[3]), log(70.0));

    // Evict the root, then improve the new root twice after the heap has moved.
    output.emit(std::vector<size_t>(1, indexes[5]), log(60.0));
    // Improve the score of the key that just recycled the evicted node. If
    // eviction left a stale heap_pos or slot pointer behind, this either
    // corrupts the wrong slot or is silently dropped.
    output.emit(std::vector<size_t>(1, indexes[5]), log(65.0));
    output.emit(std::vector<size_t>(1, indexes[2]), log(80.0));
    output.emit(std::vector<size_t>(1, indexes[0]), log(55.0));

    std::vector<DfsSpelling> const results = output.take_sorted_results();
    char const* expected_keys[] = { "cc", "dd", "ff", "aa", "ee" };
    double const expected_scores[] = { 80.0, 70.0, 65.0, 55.0, 50.0 };
    check(results.size() == 5, "heap churn changed the top-N size");
    for (size_t i = 0; i < results.size(); ++i) {
      check(results[i].word_set_key == expected_keys[i],
            "heap index pointed at the wrong spelling");
      check_close(results[i].log_score, log(expected_scores[i]),
                  "heap retained the wrong score");
    }

    double stale_floor = 0.0;
    check(!output.score_floor(&stale_floor),
          "drained top-N output retained its published floor");
    output.emit(std::vector<size_t>(1, indexes[1]), log(1.0));
    std::vector<DfsSpelling> const reused_results =
        output.take_sorted_results();
    check(reused_results.size() == 1 &&
              reused_results[0].word_set_key == "bb",
          "drained top-N output rejected a lower-scoring refill");

    DfsTopN zero_output(&classes, 0);
    zero_output.emit(std::vector<size_t>(1, indexes[0]), log(40.0));
    zero_output.emit(std::vector<size_t>(1, indexes[1]), log(20.0));
    zero_output.emit(std::vector<size_t>(1, indexes[2]), log(30.0));
    zero_output.emit(std::vector<size_t>(1, indexes[1]), log(50.0));
    zero_output.emit(std::vector<size_t>(1, indexes[2]), log(10.0));
    check(zero_output.size() == 3,
          "zero-limit output did not retain every unique key");
    check(!zero_output.score_floor(&stale_floor),
          "zero-limit output unexpectedly published a score floor");

    std::vector<DfsSpelling> const zero_results =
        zero_output.take_sorted_results();
    char const* zero_expected_keys[] = { "bb", "aa", "cc" };
    double const zero_expected_scores[] = { 50.0, 40.0, 30.0 };
    check(zero_results.size() == 3,
          "zero-limit output returned the wrong result count");
    for (size_t i = 0; i < zero_results.size(); ++i) {
      check(zero_results[i].word_set_key == zero_expected_keys[i],
            "zero-limit output order is wrong");
      check_close(zero_results[i].log_score, log(zero_expected_scores[i]),
                  "zero-limit duplicate retained the wrong score");
    }
  }

  fclose(fp);
}

static void concurrent_top_n_test() {
  FILE* fp = tmpfile();
  check(fp != NULL, "could not create concurrent top-N index");
  {
    IndexWriter writer(fp);
    writer.next("ab ", 0, 997);
    writer.next("ba ", 0, 991);
    writer.next("cd ", 0, 997);
    writer.next("dc ", 0, 991);
    writer.next("ef ", 0, 997);
    writer.next("fe ", 0, 991);
    writer.next("gh ", 0, 997);
    writer.next("hg ", 0, 991);
    writer.next("ij ", 0, 997);
    writer.next("ji ", 0, 991);
    writer.next("kl ", 0, 997);
    writer.next("lk ", 0, 991);
    writer.next("mn ", 0, 997);
    writer.next("nm ", 0, 991);
    writer.next("op ", 0, 997);
    writer.next("po ", 0, 991);
    writer.next(NULL, 0, 0);
  }
  fflush(fp);
  rewind(fp);

  {
    IndexReader reader(fp);
    DfsClassList classes(&reader, "abcdefghijklmnop", 2, false);
    std::vector<size_t> indexes;
    char const* keys[] = {
      "ab", "cd", "ef", "gh", "ij", "kl", "mn", "op",
    };
    for (size_t i = 0; i < 8; ++i)
      indexes.push_back(find_class(classes, keys[i]));

    // Each emit expands both members of its anagram class. This exercises
    // concurrent worker-local expansion as well as shared heap updates.
    size_t const limit = 16;
    size_t const event_count = 256;
    DfsTopN serial(&classes, limit);
    for (size_t event = 0; event < event_count; ++event)
      serial.emit(
          std::vector<size_t>(1, indexes[event % indexes.size()]),
          log(double(event + 1)));
    std::vector<DfsSpelling> const expected =
        serial.take_sorted_results();

    DfsTopN concurrent(&classes, limit);
    check(concurrent.supports_parallel_search(),
          "top-N output did not opt in to parallel search");
    for (size_t event = 0; event < limit; ++event)
      concurrent.emit(
          std::vector<size_t>(1, indexes[event % indexes.size()]),
          log(double(event + 1)));
    std::atomic<bool> start(false);
    std::atomic<size_t> producers_done(0);
    std::atomic<bool> floor_decreased(false);
    double reader_max[2] = {
      -HUGE_VAL,
      -HUGE_VAL,
    };
    std::vector<std::thread> producers;
    for (size_t thread = 0; thread < 4; ++thread) {
      producers.push_back(std::thread([&, thread]() {
        while (!start.load(std::memory_order_acquire))
          std::this_thread::yield();
        for (size_t event = limit + thread;
             event < event_count; event += 4)
          concurrent.emit(
              std::vector<size_t>(
                  1, indexes[event % indexes.size()]),
              log(double(event + 1)));
        producers_done.fetch_add(1, std::memory_order_release);
      }));
    }
    std::vector<std::thread> readers;
    for (size_t thread = 0; thread < 2; ++thread) {
      readers.push_back(std::thread([&, thread]() {
        double previous = -HUGE_VAL;
        do {
          double floor;
          if (concurrent.score_floor(&floor)) {
            if (floor < previous)
              floor_decreased.store(true, std::memory_order_relaxed);
            previous = std::max(previous, floor);
          }
          std::this_thread::yield();
        } while (
            producers_done.load(std::memory_order_acquire) != 4);
        reader_max[thread] = previous;
      }));
    }
    start.store(true, std::memory_order_release);
    for (size_t i = 0; i < producers.size(); ++i)
      producers[i].join();
    for (size_t i = 0; i < readers.size(); ++i)
      readers[i].join();

    double final_floor = 0.0;
    check(concurrent.score_floor(&final_floor),
          "concurrent top-N output did not publish a floor");
    check(!floor_decreased.load(std::memory_order_relaxed),
          "concurrent score floor decreased");
    for (size_t i = 0; i < 2; ++i)
      check(reader_max[i] <= final_floor,
            "concurrent score floor exceeded the final floor");
    std::vector<DfsSpelling> const actual =
        concurrent.take_sorted_results();
    check(actual.size() == expected.size(),
          "concurrent top-N retained the wrong result count");
    for (size_t i = 0; i < expected.size(); ++i) {
      check(actual[i].word_set_key == expected[i].word_set_key,
            "concurrent top-N retained the wrong spelling");
      check_close(actual[i].log_score, expected[i].log_score,
                  "concurrent top-N retained the wrong score");
    }
  }

  fclose(fp);
}

static void repeated_class_test() {
  FILE* fp = tmpfile();
  check(fp != NULL, "could not create repeated-class index");
  {
    IndexWriter writer(fp);
    writer.next("ab ", 0, 11);
    writer.next("ba ", 0, 7);
    writer.next("cd ", 0, 13);
    writer.next("dc ", 0, 5);
    writer.next(NULL, 0, 0);
  }
  fflush(fp);
  rewind(fp);

  {
    IndexReader reader(fp);
    DfsClassList classes(&reader, "aaabbbcd", 2, false);
    size_t const ab = find_class(classes, "ab");
    size_t const cd = find_class(classes, "cd");
    std::vector<size_t> path(3, ab);
    double const segment_penalty = DFS_DEFAULT_SEGMENT_PENALTY;

    DfsTopN output(&classes, 14);
    output.emit(
        path, representative_score(
            classes, path, segment_penalty, reader.count()));
    std::vector<DfsSpelling> const results = output.take_sorted_results();

    char const* expected_keys[] = {
      "ab ab ab", "ab ab ba", "ab ba ba", "ba ba ba",
    };
    int64_t const high = member_count(classes, ab, "ab");
    int64_t const low = member_count(classes, ab, "ba");
    double const segment_boundary =
        -log(segment_penalty) - log(double(reader.count()));
    double const expected_scores[] = {
      3.0 * log(double(high)) + 2.0 * segment_boundary,
      2.0 * log(double(high)) + log(double(low)) + 2.0 * segment_boundary,
      log(double(high)) + 2.0 * log(double(low)) + 2.0 * segment_boundary,
      3.0 * log(double(low)) + 2.0 * segment_boundary,
    };

    check(results.size() == 4,
          "repeated-class member permutations were not deduplicated");
    check(output.spellings_expanded() == 4,
          "repeated-class permutations were still expanded");
    for (size_t i = 0; i < results.size(); ++i) {
      check(results[i].word_set_key == expected_keys[i],
            "repeated-class word set is wrong");
      check_close(results[i].log_score, expected_scores[i],
                  "repeated-class score is wrong");
    }

    std::vector<size_t> mixed_path;
    mixed_path.push_back(ab);
    mixed_path.push_back(ab);
    mixed_path.push_back(cd);
    DfsTopN mixed_output(&classes, 14);
    mixed_output.emit(
        mixed_path,
        representative_score(
            classes, mixed_path, segment_penalty, reader.count()));
    std::vector<DfsSpelling> const mixed_results =
        mixed_output.take_sorted_results();

    std::vector<Expected> mixed_expected;
    std::vector<DfsClassMember> const& ab_members =
        classes.classes()[ab].members;
    std::vector<DfsClassMember> const& cd_members =
        classes.classes()[cd].members;
    for (size_t first = 0; first < ab_members.size(); ++first)
      for (size_t second = first; second < ab_members.size(); ++second)
        for (size_t third = 0; third < cd_members.size(); ++third) {
          Expected row;
          row.log_score =
              log(double(ab_members[first].count)) +
              log(double(ab_members[second].count)) +
              log(double(cd_members[third].count)) + 2.0 * segment_boundary;
          std::vector<std::string> words;
          words.push_back(ab_members[first].text);
          words.push_back(ab_members[second].text);
          words.push_back(cd_members[third].text);
          row.key = word_set_key(words);
          mixed_expected.push_back(row);
        }
    std::sort(
        mixed_expected.begin(), mixed_expected.end(), expected_order);

    check(mixed_results.size() == 6,
          "mixed repeated-class expansion has the wrong result count");
    check(mixed_output.spellings_expanded() == 6,
          "mixed repeated-class permutations were still expanded");
    for (size_t i = 0; i < mixed_expected.size(); ++i) {
      check(mixed_results[i].word_set_key == mixed_expected[i].key,
            "mixed repeated-class word set is wrong");
      check_close(mixed_results[i].log_score, mixed_expected[i].log_score,
                  "mixed repeated-class score is wrong");
    }
  }

  fclose(fp);
}

static void large_repeated_class_test() {
  FILE* fp = tmpfile();
  check(fp != NULL, "could not create large repeated-class index");
  {
    IndexWriter writer(fp);
    writer.next("abcd ", 0, 10);
    writer.next("abdc ", 0, 9);
    writer.next("acbd ", 0, 8);
    writer.next("acdb ", 0, 7);
    writer.next("adbc ", 0, 6);
    writer.next("adcb ", 0, 5);
    writer.next("bacd ", 0, 4);
    writer.next("badc ", 0, 3);
    writer.next("bcad ", 0, 2);
    writer.next("bcda ", 0, 1);
    writer.next(NULL, 0, 0);
  }
  fflush(fp);
  rewind(fp);

  {
    IndexReader reader(fp);
    DfsClassList classes(&reader, "abcd", 4, false);
    size_t const abcd = find_class(classes, "abcd");
    std::vector<size_t> path(6, abcd);
    double const segment_penalty = DFS_DEFAULT_SEGMENT_PENALTY;

    DfsTopN output(&classes, 10000);
    output.emit(
        path, representative_score(
            classes, path, segment_penalty, reader.count()));
    std::vector<DfsSpelling> const results = output.take_sorted_results();

    // Ten members chosen six times with repetition:
    // C(10 + 6 - 1, 6) = 5,005 canonical multisets, versus 10^6 ordered
    // tuples without the repeated-class symmetry break.
    check(results.size() == 5005,
          "large repeated-class result count is wrong");
    check(output.spellings_expanded() == 5005,
          "large repeated-class expansion regressed to permutations");
  }

  fclose(fp);
}

static void search_output_integration_test() {
  FILE* fp = tmpfile();
  check(fp != NULL, "could not create integration index");
  {
    IndexWriter writer(fp);
    writer.next("ab ", 0, 10);
    writer.next("ab cd ", 0, 70);
    writer.next("ba ", 0, 5);
    writer.next("cd ", 0, 7);
    writer.next("dc ", 0, 2);
    writer.next(NULL, 0, 0);
  }
  fflush(fp);
  rewind(fp);

  {
    IndexReader reader(fp);
    std::string const letters = "abcd";
    double const segment_penalty = DFS_DEFAULT_SEGMENT_PENALTY;
    DfsClassList classes(&reader, letters, 2);
    DfsAnagramSearch search(
        &classes, letters, segment_penalty, reader.count());
    DfsTopN output(&classes, 14);
    search.run(&output);

    std::vector<DfsSpelling> const results = output.take_sorted_results();
    check(search.solutions_found() == 2,
          "integration search emitted the wrong solution count");
    check(results.size() == 4,
          "integration output emitted the wrong spelling count");

    char const* expected_keys[] = { "ab cd", "ab dc", "ba cd", "ba dc" };
    size_t const ab = find_class(classes, "ab");
    size_t const cd = find_class(classes, "cd");
    double const segment_boundary =
        -log(segment_penalty) - log(double(reader.count()));
    double const expected_scores[] = {
      log(70.0),
      log(double(member_count(classes, ab, "ab"))) +
          log(double(member_count(classes, cd, "dc"))) + segment_boundary,
      log(double(member_count(classes, ab, "ba"))) +
          log(double(member_count(classes, cd, "cd"))) + segment_boundary,
      log(double(member_count(classes, ab, "ba"))) +
          log(double(member_count(classes, cd, "dc"))) + segment_boundary,
    };
    for (size_t i = 0; i < results.size(); ++i) {
      check(results[i].word_set_key == expected_keys[i],
            "integration output order is wrong");
      check_close(results[i].log_score, expected_scores[i],
                  "integration output score is wrong");
    }
  }

  fclose(fp);
}

int main() {
  exhaustive_product_test();
  heap_churn_test();
  concurrent_top_n_test();
  repeated_class_test();
  large_repeated_class_test();
  search_output_integration_test();
  return 0;
}
