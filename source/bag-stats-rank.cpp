#include "bag-stats-rank.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <string_view>

#include "dfs-cli-help.h"
#include "pair-evidence.h"
#include "row-input.h"

bool parse_search(std::string_view program, char const* text, Search* out) {
    std::vector<std::string> steps(1);
    for (char const* c = text; *c; ++c) {
        if (*c == ',') steps.emplace_back();
        else steps.back() += *c;
    }
    Search search;
    search.pair_filter = steps.back() == "pf";
    if (steps.back() != "p" && !search.pair_filter) {
        fprintf(stderr, "%.*s: --ss has to end with p or pf\n",
                int(program.size()), program.data());
        return false;
    }
    steps.pop_back();

    for (std::string const& step : steps) {
        if (step == "ppb") ++search.prospects;
        else if (step == "pb") ++search.pairs;
        else if (step == "wb") ++search.words;
        else {
            fprintf(stderr, "%.*s: --ss step \"%s\" isn't ppb, pb or wb\n",
                    int(program.size()), program.data(), step.c_str());
            return false;
        }
    }
    *out = search;
    return true;
}

std::string search_text(Search const& search) {
    std::string out;
    for (int i = 0; i < search.prospects; ++i) out += "ppb,";
    for (int i = 0; i < search.pairs; ++i) out += "pb,";
    for (int i = 0; i < search.words; ++i) out += "wb,";
    return out + (search.pair_filter ? "pf" : "p");
}

RankOption parse_rank_option(std::string_view program, int option,
                             char const* arg, RankArgs* out) {
    constexpr int SORT_FLAG_COUNT =
        sizeof(BAG_STATS_SORT_FLAGS) / sizeof(BAG_STATS_SORT_FLAGS[0]);
    static_assert(SORT_FLAG_COUNT == 6);
    if (option >= OPT_RANK_SORT && option < OPT_RANK_SORT + SORT_FLAG_COUNT) {
        out->sorts.push_back(&BAG_STATS_SORT_FLAGS[option - OPT_RANK_SORT]);
        return RankOption::PARSED;
    }
    switch (option) {
      case 'n':
        if (!parse_count(arg, "-n", &out->top)) return RankOption::BAD;
        if (out->top == 0) {
            fprintf(stderr, "%.*s: -n needs N of at least 1\n",
                    int(program.size()), program.data());
            return RankOption::BAD;
        }
        break;
      case OPT_RANK_BY_LENGTH:
        out->by_length = true;
        break;
      case OPT_RANK_TOP_SEGMENTS:
        out->top_segments = true;
        break;
      case OPT_RANK_SCORE:
        out->score.push_back(arg);
        break;
      case OPT_RANK_PRIOR:
        out->prior_file = arg;
        break;
      case OPT_RANK_BY_PICK:
        out->by_pick = true;
        break;
      case OPT_RANK_SCORE_DETAIL:
        out->score_detail = true;
        break;
      case OPT_RANK_BEST_PAIRS:
        if (strcmp(arg, "appear") == 0) {
            out->best_pairs = BestPairs::APPEAR;
        } else if (strcmp(arg, "lead") == 0) {
            out->best_pairs = BestPairs::LEAD;
        } else {
            fprintf(stderr, "%.*s: --best-pairs is appear or lead\n",
                    int(program.size()), program.data());
            return RankOption::BAD;
        }
        break;
      case OPT_RANK_P1:
        out->p1 = arg;
        break;
      case OPT_RANK_CLUES:
        out->clues = arg;
        break;
      case OPT_RANK_PRIOR_WEIGHTS:
        out->prior_weights = arg;
        break;
      default:
        return RankOption::NOT_ONE;
    }
    return RankOption::PARSED;
}

bool check_rank_args(std::string_view program, RankArgs const& args,
                     Search const& search) {
    if (args.top > 0 && !args.score.empty()) {
        fprintf(stderr, "%.*s: -n and --score don't go together\n",
                int(program.size()), program.data());
        return false;
    }
    if (args.by_pick &&
        (args.by_length || search.prospects + search.pairs < 2)) {
        fprintf(stderr, "%.*s: --by-pick goes without --by-length and with two "
                "or more ppb and pb steps\n",
                int(program.size()), program.data());
        return false;
    }
    bool const evidence = args.p1 != NULL || args.clues != NULL;
    if ((args.prior_file != NULL || evidence) && !args.by_pick) {
        fprintf(stderr, "%.*s: --prior, --p1 and --clues go with --by-pick\n",
                int(program.size()), program.data());
        return false;
    }
    if (args.prior_file != NULL && evidence) {
        fprintf(stderr, "%.*s: --prior doesn't go with --p1 or --clues\n",
                int(program.size()), program.data());
        return false;
    }
    if (args.prior_weights != NULL && !evidence) {
        fprintf(stderr, "%.*s: --prior-weights goes with --p1 or --clues\n",
                int(program.size()), program.data());
        return false;
    }
    if (!args.score.empty() &&
        (args.top_segments ||
         int(args.score.size()) != search.prospects + search.pairs)) {
        fprintf(stderr, "%.*s: --score names one pick per ppb and pb step, "
                "without --top-segments\n",
                int(program.size()), program.data());
        return false;
    }
    if (args.score_detail && (args.score.empty() || !args.by_pick)) {
        fprintf(stderr,
                "%.*s: --score-detail goes with --score and --by-pick\n",
                int(program.size()), program.data());
        return false;
    }
    if (args.best_pairs != BestPairs::NONE &&
        (args.top == 0 || args.top_segments ||
         (args.best_pairs == BestPairs::LEAD && !args.by_pick))) {
        fprintf(stderr, "%.*s: --best-pairs goes with -n, without "
                "--top-segments, and lead with --by-pick\n",
                int(program.size()), program.data());
        return false;
    }
    return true;
}

void help_rank_output_options() {
    dfs_help_option("--top-segments",
                    "print no header, and each row as \"0 PAIR,PAIR...\", "
                    "its pair picks, for top-segments; bag-stats-sort can't "
                    "read it");
    dfs_help_option("--score W1,W2",
                    "print only the row whose pair picks are these, one "
                    "--score per ppb and pb step in PAIR order, ppb picks "
                    "first, and below it \"rank R of N\": where -n, given "
                    "the same SORT options and --by-length, would print it; "
                    "when other rows have its sort key, \", tied with T other "
                    "rows for ranks A-B\", among which -n prints in walk "
                    "order; each pick is the bag of its two words");
    dfs_help_option("--score-detail",
                    "with --score and --by-pick, below the rank print the "
                    "row's KEY and SUM, the smallest and the sum of its "
                    "picks' terms, then each pick's PAIR, PRIOR, P, ROWS "
                    "sharing it, TERM, its P * Q * sqrt(P) * Q^(1/4) of each "
                    "other pick, and Q, then each SORT option's value, and "
                    "among the pick's rows its RANK, A-B when tied, its Q, "
                    "and TERM with that Q, the other picks' Q their best");
    dfs_help_option("--best-pairs appear|lead",
                    "with -n, print the -n rows' pair picks instead, each "
                    "once as PAIRS writes it, at the first row it is in "
                    "(appear) or leads, its own term sqrt(P) * Q^(3/4) the "
                    "smallest of the row's, the one in its key (lead, only "
                    "with --by-pick)");
}

void help_rank_sort_options() {
    bag_stats_help_sort_flags();
    dfs_help_option("--by-length",
                    "rank each row's sort value only among the rows whose "
                    "pair picks have as many letters, so the best of each "
                    "length come first");
    dfs_help_option("--by-pick",
                    "rank each row's sort value among the rows that share "
                    "each of its pair picks, Q the best mid-rank percentile "
                    "from the top over picks and SORT options; with --prior, "
                    "a pick's Q is weighted by P, its prior's percentile from "
                    "the top among its list's bags, as P * Q * sqrt(P) * "
                    "Q^(1/4) of each other pick, and rows sort by the "
                    "smallest; needs two or more ppb and pb steps");
    dfs_help_option("--prior FILE",
                    "\"SCORE word,word\" lines, as pair-prior prints them, "
                    "larger better; a pick bag scores its best row, below "
                    "every score when none is in FILE; only with --by-pick");
    dfs_help_option("--p1 INDEX",
                    "score each pair's prior from its phase 1 verdicts in "
                    "INDEX, as make-p1-index writes it, as pair-prior does; "
                    "only with --by-pick, not with --prior");
    dfs_help_option("--clues CLUEDATA",
                    "score each pair's prior from the cluer clue index "
                    "beside CLUEDATA too, or alone");
    dfs_help_option("--prior-weights SPEC",
                    "how --p1 and --clues score a pair, as pair-prior "
                    "--prior-weights (default %s, less the terms of an index "
                    "not given)", PRIOR_WEIGHTS_DEFAULT);
    fputs("\n--prior-weights terms:\n", stdout);
    help_prior_terms();
}

bool load_priors(std::string_view program, char const* path,
                 std::unordered_map<std::string, double>* out) {
    return read_lines(program, path,
        [&](std::string const& line, size_t number) {
            char* end;
            double const score = strtod(line.c_str(), &end);
            size_t const space = size_t(end - line.c_str());
            if (end == line.c_str() || !std::isfinite(score) ||
                space >= line.size() || line[space] != ' ') {
                fprintf(stderr, "%.*s: %s:%zu isn't \"SCORE word,word\" with "
                        "a finite SCORE\n", int(program.size()), program.data(),
                        path, number);
                return false;
            }
            DfsPairRow row;
            if (!parse_pair_row(line.substr(space + 1), "prior", path, number,
                                false, &row))
                return false;
            auto const [entry, inserted] =
                out->emplace(pair_key(row.left, row.right), score);
            if (!inserted) entry->second = std::max(entry->second, score);
            return true;
        });
}

void PairBagLoader::add(DfsPairRow const& row) {
    PairsCounts const counts =
        pairs_letter_counts((row.left + row.right).c_str());
    if (!pairs_fits(bag_, counts)) return;
    if (!rows_.insert(pair_key(row.left, row.right)).second) return;
    auto const inserted = indices_.emplace(counts, out_->size());
    if (inserted.second) out_->push_back({counts, row.left + " " + row.right});
    if (bag_rows_) {
        if (inserted.second) bag_rows_->emplace_back();
        (*bag_rows_)[inserted.first->second].push_back(
            row.left + "," + row.right);
    }
    PairBag& pair_bag = (*out_)[inserted.first->second];
    ++pair_bag.pairs;
    double score;
    if (prior_ && prior_(row, &score))
        pair_bag.prior = std::max(pair_bag.prior, score);
}

int letter_total(PairsCounts const& counts) {
    int total = 0;
    for (uint8_t value : counts.values) total += value;
    return total;
}

bool load_pair_bags(std::string_view program, char const* path,
                    PairsCounts const& bag, DfsPairSet const& rejected,
                    PairPrior const& prior, std::vector<PairBag>* out,
                    std::vector<std::vector<std::string>>* rows) {
    PairBagLoader loader(bag, prior, out, rows);
    return read_rows(program, path, "pairs", false,
        [&](DfsPairRow const& row, std::string const&) {
            if (!rejected.contains(row.entry())) loader.add(row);
            return true;
        });
}

PairPickFilter::PairPickFilter(std::vector<PairBag> const& prospect_bags,
                               int min_length)
        : min_length_(min_length) {
    for (PairBag const& prospect : prospect_bags)
        prospects_.insert(prospect.counts);
}

bool PairPickFilter::dropped(PairBag const& pair_bag) const {
    return prospects_.contains(pair_bag.counts) ||
        letter_total(pair_bag.counts) < min_length_;
}

bool load_pick_bags(std::string_view program, RankArgs const& args,
                    char const* prospects_file, char const* pairs_file,
                    PairsCounts const& bag, DfsPairSet const& rejected,
                    int min_pair_length, std::vector<PairBag>* prospect_bags,
                    std::vector<PairBag>* pair_bags,
                    std::vector<PairBag>* final_bags,
                    std::vector<std::vector<std::string>>* final_rows,
                    bool* has_prior) {
    std::unordered_map<std::string, double> priors;
    PairEvidenceSource evidence;
    PriorWeights weights;
    PairPrior prior;
    if (args.prior_file != NULL) {
        if (!load_priors(program, args.prior_file, &priors)) return false;
        prior = [&](DfsPairRow const& row, double* out) {
            auto const found = priors.find(pair_key(row.left, row.right));
            if (found == priors.end()) return false;
            *out = found->second;
            return true;
        };
    } else if (args.p1 != NULL || args.clues != NULL) {
        std::string error;
        if (!open_prior(args.p1, args.clues, args.prior_weights, false,
                        &weights, &evidence, &error)) {
            fprintf(stderr, "%.*s: %s\n", int(program.size()), program.data(),
                    error.c_str());
            return false;
        }
        prior = [&](DfsPairRow const& row, double* out) {
            *out = prior_score(evidence.lookup(row.left, row.right), weights);
            return true;
        };
    }
    *has_prior = bool(prior);
    std::vector<PairBag>* const loaded = final_bags ? final_bags : pair_bags;
    if ((prospects_file != NULL &&
         !load_pair_bags(program, prospects_file, bag, rejected, prior,
                         prospect_bags)) ||
        (loaded != nullptr &&
         !load_pair_bags(program, pairs_file, bag, rejected, prior, loaded,
                         final_rows)))
        return false;
    if (pair_bags == nullptr) return true;
    PairPickFilter const filter(*prospect_bags, min_pair_length);
    auto const dropped = [&](PairBag const& pair_bag) {
        return filter.dropped(pair_bag);
    };
    if (final_bags)
        std::remove_copy_if(final_bags->begin(), final_bags->end(),
                            std::back_inserter(*pair_bags), dropped);
    else
        std::erase_if(*pair_bags, dropped);
    return true;
}

Distribution::Distribution(std::vector<std::pair<double, uint64_t>> counts) {
    std::sort(counts.begin(), counts.end());
    size_t distinct = 0;
    for (size_t i = 0; i < counts.size(); ++i)
        if (i == 0 || counts[i].first != counts[i - 1].first) ++distinct;
    values_.reserve(distinct);
    below_.reserve(distinct + 1);
    uint64_t size = 0;
    for (auto const& [value, count] : counts) {
        if (values_.empty() || values_.back() != value) {
            values_.push_back(value);
            below_.push_back(size);
        }
        size += count;
    }
    below_.push_back(size);
}

double Distribution::percentile(double value) const {
    size_t const i = size_t(
        std::lower_bound(values_.begin(), values_.end(), value) -
        values_.begin());
    return bag_stats_percentile(below_[i], below_[i + 1], below_.back());
}

std::pair<uint64_t, uint64_t> Distribution::above_and_tied(
        double value) const {
    size_t const i = size_t(
        std::lower_bound(values_.begin(), values_.end(), value) -
        values_.begin());
    return {below_.back() - below_[i + 1], below_[i + 1] - below_[i]};
}

void Ranker::add(Part* part, BagStatsRow const& row, size_t length,
                 std::span<uint32_t const>) const {
    for (size_t k = 0; k < keys_.size(); ++k)
        ++(*part)[k][{group(length), bag_stats_sort_value(keys_[k], row)}];
}

void Ranker::finish(std::vector<Part>& parts) {
    for (size_t k = 0; k < keys_.size(); ++k) {
        std::unordered_map<size_t,
            std::vector<std::pair<double, uint64_t>>> by_group;
        std::unordered_map<std::pair<size_t, double>, uint64_t,
                           LengthValueHash> merged;
        for (SortValueCounts const& part : parts)
            for (auto const& [key, count] : part[k]) merged[key] += count;
        for (auto const& [key, count] : merged)
            by_group[key.first].emplace_back(key.second, count);
        groups_.emplace_back();
        for (auto& [length, counts] : by_group)
            groups_.back().emplace(length, Distribution(std::move(counts)));
        if (keys_.size() == 1) continue;

        std::unordered_map<double, uint64_t> ranked;
        for (auto const& [key, count] : merged)
            ranked[first_rank(k, key.first, key.second)] += count;
        overall_.emplace_back(std::vector<std::pair<double, uint64_t>>(
            ranked.begin(), ranked.end()));
    }
}

std::pair<double, double> Ranker::key(BagStatsRow const& row, size_t length,
                                      std::span<uint32_t const>) const {
    std::pair<double, double> out{-1, 0};
    for (size_t k = 0; k < keys_.size(); ++k) {
        double value = first_rank(k, group(length),
                                  bag_stats_sort_value(keys_[k], row));
        if (keys_.size() > 1) value = overall_[k].percentile(value);
        out.first = std::max(out.first, value);
        out.second += value;
    }
    return out;
}

void PickRanker::add(Part* part, BagStatsRow const& row, size_t,
                     std::span<uint32_t const> picks) const {
    for (size_t k = 0; k < keys_.size(); ++k) {
        double const value = bag_stats_sort_value(keys_[k], row);
        for (uint32_t pick : picks)
            part->entries.push_back(
                {value, uint32_t(k * ranks_.size() + pick), 1});
    }
    if (part->entries.size() < part->limit) return;
    compact(&part->entries);
    if (part->entries.size() > part->limit / 2) part->limit *= 2;
}

void PickRanker::finish(std::vector<Part>& parts) {
    for (Part& part : parts) {
        compact(&part.entries);
        part.entries.shrink_to_fit();
    }
    size_t const groups = keys_.size() * ranks_.size();
    groups_.assign(groups, Distribution());
    std::vector<size_t> next(parts.size(), 0);
    for (size_t group = 0; group < groups; ++group) {
        std::vector<std::pair<double, uint64_t>> counts;
        for (size_t p = 0; p < parts.size(); ++p) {
            std::vector<Entry> const& entries = parts[p].entries;
            for (; next[p] < entries.size() &&
                   entries[next[p]].group == group; ++next[p])
                counts.emplace_back(entries[next[p]].value,
                                    entries[next[p]].count);
        }
        if (!counts.empty()) groups_[group] = Distribution(std::move(counts));
    }
    for (Part& part : parts) std::vector<Entry>().swap(part.entries);
}

std::pair<double, double> PickRanker::key(
        BagStatsRow const& row, size_t,
        std::span<uint32_t const> picks) const {
    double all = 1;
    double best = INFINITY;
    double sum = 0;
    for (uint32_t pick : picks) {
        auto const [own, shared] = pick_factors(row, pick);
        all *= shared;
        best = std::min(best, own);
        sum += own;
    }
    best *= all;
    sum *= all;
    return {-best, -sum};
}

std::vector<uint32_t> PickRanker::leaders(
        BagStatsRow const& row, std::span<uint32_t const> picks) const {
    std::vector<double> own;
    for (uint32_t pick : picks) own.push_back(pick_factors(row, pick).first);
    double const least = *std::min_element(own.begin(), own.end());
    std::vector<uint32_t> out;
    for (size_t i = 0; i < picks.size(); ++i)
        if (own[i] == least) out.push_back(picks[i]);
    return out;
}

std::vector<PickRanker::PickDetail> PickRanker::detail(
        BagStatsRow const& row, std::span<uint32_t const> picks) const {
    std::vector<PickDetail> out;
    for (uint32_t pick : picks) {
        PickDetail part{ranks_[pick],
                        groups_[0 * ranks_.size() + pick].size(), {}, {},
                        {}, {}, 1, 0};
        for (size_t k = 0; k < keys_.size(); ++k) {
            double const value = bag_stats_sort_value(keys_[k], row);
            part.values.push_back(value);
            part.above_and_tied.push_back(
                groups_[k * ranks_.size() + pick].above_and_tied(value));
            part.qs.push_back(pick_q(row, pick, k));
            part.q = std::min(part.q, part.qs.back());
        }
        out.push_back(std::move(part));
    }
    auto const term = [&](size_t i, double q) {
        double all = 1;
        for (size_t j = 0; j < out.size(); ++j)
            all *= factors(out[j].p, j == i ? q : out[j].q).second;
        return factors(out[i].p, q).first * all;
    };
    for (size_t i = 0; i < out.size(); ++i) {
        out[i].term = term(i, out[i].q);
        for (double q : out[i].qs) out[i].terms.push_back(term(i, q));
    }
    return out;
}

// A pick's two factors in key(), from its P and Q: sqrt(P) * Q^(3/4), its
// own, and sqrt(P) * Q^(1/4), which every pick's term has.
std::pair<double, double> PickRanker::factors(double p, double q) {
    double const root = std::sqrt(p);
    double const quarter = std::sqrt(std::sqrt(q));
    return {root * q / quarter, root * quarter};
}

// factors() of `pick` in `row`, Q its smallest over the sort keys.
std::pair<double, double> PickRanker::pick_factors(BagStatsRow const& row,
                                                   uint32_t pick) const {
    double q = 1;
    for (size_t k = 0; k < keys_.size(); ++k)
        q = std::min(q, pick_q(row, pick, k));
    return factors(ranks_[pick], q);
}

// Q of sort key `k` for `row` among the rows sharing `pick`.
double PickRanker::pick_q(BagStatsRow const& row, uint32_t pick,
                          size_t k) const {
    return 1 - groups_[k * ranks_.size() + pick].percentile(
        bag_stats_sort_value(keys_[k], row));
}

// Sorts `entries` and merges those of one group and value while their count
// fits.
void PickRanker::compact(std::vector<Entry>* entries) {
    std::sort(entries->begin(), entries->end(),
              [](Entry const& a, Entry const& b) {
                  return a.group != b.group ? a.group < b.group
                                            : a.value < b.value;
              });
    size_t out = 0;
    for (Entry const& entry : *entries) {
        if (out > 0 && (*entries)[out - 1].group == entry.group &&
            (*entries)[out - 1].value == entry.value &&
            (*entries)[out - 1].count <= UINT32_MAX - entry.count)
            (*entries)[out - 1].count += entry.count;
        else
            (*entries)[out++] = entry;
    }
    entries->resize(out);
}

std::vector<double> pick_ranks(std::vector<PairBag> const& pair_bags,
                               std::vector<PairBag> const& prospect_bags,
                               bool prior) {
    std::vector<double> out;
    for (std::vector<PairBag> const* bags : {&pair_bags, &prospect_bags}) {
        if (!prior) {
            out.insert(out.end(), bags->size(), 1.0);
            continue;
        }
        std::map<double, uint64_t> priors;
        for (PairBag const& bag : *bags) ++priors[bag.prior];
        Distribution const distribution(
            std::vector<std::pair<double, uint64_t>>(priors.begin(),
                                                     priors.end()));
        for (PairBag const& bag : *bags)
            out.push_back(1 - distribution.percentile(bag.prior));
    }
    return out;
}

std::vector<BagStatsCount const*> sort_flag_keys(
        std::vector<BagStatsSortFlag const*> const& sorts) {
    std::vector<BagStatsCount const*> out;
    for (BagStatsSortFlag const* sort : sorts)
        out.push_back(bag_stats_sort_key(*sort));
    return out;
}

bool ranks_before(RankedRow const& a, RankedRow const& b) {
    if (a.key != b.key) return a.key > b.key;
    if (a.unit != b.unit) return a.unit < b.unit;
    return a.seq < b.seq;
}

bool score_picks(std::string_view program,
                 std::vector<std::string> const& score, Search const& search,
                 std::vector<std::vector<PairBag> const*> const& steps,
                 PairsCounts const& letters, std::vector<size_t>* out) {
    size_t const prospects = size_t(search.prospects);
    std::vector<size_t> picks(steps.size());
    for (size_t i = 0; i < score.size(); ++i) {
        std::string const& name = score[i];
        size_t const comma = name.find(',');
        // ppb picks come first in --score and last in the steps.
        size_t const step = i < prospects ? steps.size() - prospects + i
            : i - prospects;
        std::vector<PairBag> const& bags = *steps[step];
        PairsCounts const counts = pairs_letter_counts(name.c_str());
        auto const found = std::find_if(bags.begin(), bags.end(),
            [&](PairBag const& bag) { return bag.counts == counts; });
        if (comma == std::string::npos || comma == 0 ||
            comma + 1 == name.size() ||
            name.find(',', comma + 1) != std::string::npos ||
            found == bags.end()) {
            fprintf(stderr, "%.*s: --score %s isn't two words whose bag is "
                    "in %s and fits the letters\n",
                    int(program.size()), program.data(), name.c_str(),
                    i < prospects
                        ? "--pp" : "PAIRS, of at least --min-pb-len letters,");
            return false;
        }
        picks[step] = size_t(found - bags.begin());
    }
    PairsCounts rest = letters;
    for (size_t j = 0; j < steps.size(); ++j) {
        PairsCounts const& counts = (*steps[j])[picks[j]].counts;
        bool repeats = false;
        for (size_t i = 0; i < j; ++i)
            repeats = repeats || (*steps[i])[picks[i]].counts == counts;
        if (repeats || !pairs_subtract(rest, counts, &rest)) {
            fprintf(stderr, "%.*s: the --score picks don't fit the letters "
                    "together\n", int(program.size()), program.data());
            return false;
        }
    }
    std::sort(picks.begin(), picks.begin() + search.pairs);
    std::sort(picks.begin() + search.pairs, picks.end());
    *out = std::move(picks);
    return true;
}

void append_row(std::string* out, BagStatsRow const& row, bool top_segments) {
    if (!top_segments) {
        append_bag_stats_row(out, row);
        return;
    }
    *out += "0 ";
    for (size_t i = 0; i < row.pair.size();) {
        if (row.pair.compare(i, 3, " + ") == 0) {
            *out += ',';
            i += 3;
        } else {
            *out += row.pair[i++];
        }
    }
    *out += '\n';
}

void print_score_rank(BagStatsRow const& row, ScoreRank const& rank) {
    write_bag_stats_row(stdout, row);
    printf("rank %llu of %llu", (unsigned long long)(rank.before + 1),
           (unsigned long long)rank.rows);
    if (rank.tied > 1)
        printf(", tied with %llu other row%s for ranks %llu-%llu",
               (unsigned long long)(rank.tied - 1), rank.tied == 2 ? "" : "s",
               (unsigned long long)(rank.larger + 1),
               (unsigned long long)(rank.larger + rank.tied));
    putchar('\n');
}

void print_score_detail(std::vector<BagStatsSortFlag const*> const& sorts,
                        PickRanker const& ranker, RankedRow const& target,
                        std::vector<PairBag const*> const& bags,
                        std::span<uint32_t const> row_picks) {
    printf("KEY %.6g SUM %.6g\n", -target.key.first, -target.key.second);
    std::vector<PickRanker::PickDetail> const parts =
        ranker.detail(target.row, row_picks);
    // Each sort line's columns, padded to the widest of every pick's lines.
    constexpr size_t COLUMNS = 5;
    std::vector<std::vector<std::array<std::string, COLUMNS>>> lines;
    std::array<int, COLUMNS> widths{};
    char text[64];
    for (PickRanker::PickDetail const& part : parts) {
        lines.emplace_back();
        for (size_t k = 0; k < part.qs.size(); ++k) {
            auto const [above, tied] = part.above_and_tied[k];
            std::array<std::string, COLUMNS>& line =
                lines.back().emplace_back();
            BagStatsSortFlag const& sort = *sorts[k];
            line[0] = std::string("--") + sort.option;
            snprintf(text, sizeof text,
                     sort.column == NULL ? "%.3f" : "%.6g",
                     part.values[k]);
            line[1] = text;
            if (tied > 1)
                snprintf(text, sizeof text, "RANK %llu-%llu",
                         (unsigned long long)(above + 1),
                         (unsigned long long)(above + tied));
            else
                snprintf(text, sizeof text, "RANK %llu",
                         (unsigned long long)(above + 1));
            line[2] = text;
            snprintf(text, sizeof text, "Q %.5f", part.qs[k]);
            line[3] = text;
            snprintf(text, sizeof text, "TERM %.6g", part.terms[k]);
            line[4] = text;
            for (size_t c = 0; c + 1 < COLUMNS; ++c)
                widths[c] = std::max(widths[c], int(line[c].size()));
        }
    }
    for (size_t j = 0; j < parts.size(); ++j) {
        PairBag const& bag = *bags[j];
        PickRanker::PickDetail const& part = parts[j];
        snprintf(text, sizeof text, "%.6g", bag.prior);
        printf("PAIR %s PRIOR %s P %.5f ROWS %llu TERM %.6g Q %.5f\n",
               bag.pair.c_str(), std::isinf(bag.prior) ? "none" : text, part.p,
               (unsigned long long)part.rows, part.term, part.q);
        for (auto const& line : lines[j])
            printf("  %-*s %*s %-*s %-*s %s\n", widths[0], line[0].c_str(),
                   widths[1], line[1].c_str(), widths[2], line[2].c_str(),
                   widths[3], line[3].c_str(), line[4].c_str());
    }
}

void append_best_pairs(std::string* out, std::unordered_set<uint32_t>* seen,
                       std::vector<uint32_t> const& best,
                       std::vector<PairBag> const& pair_bags,
                       std::vector<PairBag> const& prospect_bags) {
    for (uint32_t pick : best) {
        if (!seen->insert(pick).second) continue;
        std::string const& pair = pick < pair_bags.size()
            ? pair_bags[pick].pair
            : prospect_bags[pick - pair_bags.size()].pair;
        *out += pair.substr(0, pair.find(' ')) + ',' +
            pair.substr(pair.find(' ') + 1) + '\n';
    }
}
