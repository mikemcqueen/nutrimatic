Q: can you go into more depth on maximum vowel/consonant run constraints plus common syllable-pattern likelihood,

A: For an anagram solver, those are much stronger signals than a raw consonant/vowel ratio because they ask whether the letters can be arranged into English-like local structure, not merely whether the totals look normal.

The basic idea is to score an arrangement according to how plausible its runs and syllable-shaped chunks are.

A vowel run is a sequence of consecutive vowels; a consonant run is the same for consonants. English tolerates some runs quite well and others poorly. For example, boat has a vowel run of length 2, strengths has a fairly extreme consonant run, while something like aeiou... or bcdfgh... is usually implausible unless a word boundary intervenes.

For normal English words, rough tendencies are:

vowel runs of 1–2 are very common
vowel runs of 3 occur, but much less often: beautiful, queueing
vowel runs of 4+ are rare
consonant runs of 1–2 are extremely common
consonant runs of 3 are common enough: str, spl, nch
consonant runs of 4 are uncommon but legitimate: strength, texts
runs of 5+ are rare and usually depend on morphology or unusual spelling

But the key is that length alone isn't sufficient. str is a very plausible three-consonant sequence; qzx is not. Likewise ea is much more English-like than uo in most contexts.

So instead of merely calculating:

$$ \text{max consonant run}=4 $$

you can calculate probabilities such as

$$ P(\text{next character class}\mid\text{previous classes}) $$

For example, after CC, English may have a high probability of transitioning to a vowel, while after CCCC, continuing with another consonant is extremely unlikely.

A simple class-level model could encode each word as a sequence of C and V:

strength → CCCVCCVC

beautiful → something like CVVVCVCVC, depending on whether you treat y specially.

Then train frequencies of patterns from a corpus. You could score a candidate word using bigrams or trigrams over these abstract classes:

$$ P(V\mid C),\quad P(C\mid V),\quad P(C\mid CC),\quad P(V\mid CC) $$

That already captures a surprising amount of structure.

The next level is syllable-pattern likelihood.

English syllables are commonly described as:

$$ (\text{onset}) + \text{nucleus} + (\text{coda}) $$

where the nucleus is normally a vowel or vowel-like sound.

Typical abstract patterns include:

CV — go
CVC — cat
CCVC — stop
CVCC — hand
CCVCC — plant

These are much more common than arbitrary mixtures like:

VCCCCV

or

CCCCVCCC

although real words can occasionally have those shapes.

For an anagram heuristic, you don't even need a true phonological syllabifier. You could train the frequency of spelling-level patterns such as:

CV
CVC
CVCC
CCVC
CCVCC
VC
VCC

Then ask whether your available letters could plausibly be partitioned into high-frequency patterns.

For example, suppose your letter inventory has:

6 vowels
14 consonants

The raw ratio is:

$$ 14/6=2.33 $$

That's consonant-heavy, but potentially quite usable.

You might form something structurally like:

CCVC CVC CCVC CVCC

which is entirely English-looking.

Compare that with:

2 vowels
18 consonants

The ratio is 9.0. Even before knowing the actual letters, you know that you cannot make many normal syllables because most English syllables require a vowel nucleus.

This gives you a stronger quantity than C/V ratio: syllable capacity.

A crude approximation is that the number of conventional syllables you can form is bounded roughly by the number of usable vowels:

$$ N_{\text{syllables}}\lesssim N_V $$

Ignoring syllabic consonants and diphthongs.

Then you can compare the consonants that must be packed around those vowel nuclei.

For example, with 12 consonants and 4 vowels, you need on average:

$$ 12/4=3 $$

consonants per vowel nucleus.

That's possible.

With 20 consonants and 3 vowels:

$$ 20/3\approx6.7 $$

consonants around each nucleus. That becomes extremely difficult for normal English.

This is where the global ratio becomes meaningful: not because English has a hard ratio threshold, but because each vowel can only support so much surrounding consonantal structure.

You can model this directly. Suppose your corpus tells you the empirical distribution of consonants associated with a syllable:

0 consonants : rare
1 consonant  : common
2 consonants : very common
3 consonants : common
4 consonants : less common
5 consonants : rare
6+           : extremely rare

Then if an anagram requires an average of, say, 5.8 consonants per vowel, you can assign it a very low structural probability.

There's another important improvement: distinguish legal clusters from merely possible run lengths.

English onset clusters are highly constrained. For example:

pl-
pr-
bl-
br-
tr-
dr-
kr-
gr-
fl-
fr-
sl-
sm-
sn-
sp-
st-
sk-
spl-
spr-
str-
skr-

are familiar.

But clusters such as:

bn-
dk-
fp-
gx-
zm-

are essentially nonexistent in native English word beginnings.

Similarly, codas have their own preferences:

-nd
-nt
-st
-sk
-mp
-lt
-rd
-rk
-ŋk

are common.

So the strongest heuristic is really not "how many consonants?" but:

$$ \text{Can these consonants be grouped into legal English clusters?} $$

Imagine two inventories with the same C/V ratio:

A: s t r p l n a e i
B: q x z j v b a e i

Both have six consonants and three vowels, so:

$$ C/V=2 $$

But A is massively more usable because it contains consonants that naturally form clusters like str, pl, st, tr, etc. B is structurally awful.

You can formalize that using character n-gram likelihood.

Given a corpus, estimate:

$$ P(c_i \mid c_{i-1},c_{i-2}) $$

or simply frequency counts of bigrams/trigrams such as:

th
he
ing
str
tion
ea
ou

Then score a candidate:

$$ \log P(w) = \sum_i \log P(c_i \mid c_{i-n+1}\ldots c_{i-1}) $$

For your application, though, there's an interesting twist: you may want to evaluate the letter inventory before you've solved the anagram.

In that case, I would derive several inventory-level features rather than trying to predict one particular ordering:

$$ \text{C/V ratio} $$ $$ \text{vowel diversity} $$ $$ \text{consonant-cluster compatibility} $$ $$ \text{estimated syllable capacity} $$ $$ \text{rare-letter burden} $$ $$ \text{minimum unavoidable run length} $$

That last one is especially useful.

Suppose you have \(C\) consonants and \(V\) vowels. If you arrange the vowels optimally to break up consonants, they create at most:

$$ V+1 $$

consonant slots.

So the smallest possible maximum consonant run is approximately:

$$ R_C^{\min} = \left\lceil \frac{C}{V+1} \right\rceil $$

This is a very useful hard combinatorial bound.

For example, with:

$$ C=12,\quad V=4 $$

you have 5 consonant slots:

$$ \lceil12/5\rceil=3 $$

So you can arrange the letters such that no consonant run exceeds 3:

CCC V CC V CCC V CC V CC

Perfectly plausible structurally.

But if:

$$ C=20,\quad V=2 $$

you only have 3 consonant slots:

$$ \lceil20/3\rceil=7 $$

Therefore every possible ordering must contain at least one run of seven consonants.

That's devastating for English plausibility.

Likewise for vowels:

$$ R_V^{\min} = \left\lceil \frac{V}{C+1} \right\rceil $$

although vowel-heavy inventories don't become problematic nearly as quickly.

For an anagram system, I would therefore use the ratio only as a weak feature and put considerably more weight on this minimum unavoidable run length.

You could make a cheap prefilter something like:

min_required_C_run <= 3   very plausible
min_required_C_run == 4   somewhat unusual
min_required_C_run == 5   strongly suspicious
min_required_C_run >= 6   extremely unlikely

Then combine it with whether the actual consonants can form known clusters.

If you're scoring multiword anagrams, word boundaries change the picture somewhat because each word boundary gives you another place to break a bad run. That actually makes the problem interesting: an inventory that looks impossible for one word may be perfectly usable as a phrase.

For the anagram problem you've been working on, I'd probably build a structural prior something roughly like:

$$ S = a\,S_{\text{CV-ratio}} + b\,S_{\text{min-run}} + c\,S_{\text{cluster}} + d\,S_{\text{ngram}} $$

with min-run and cluster compatibility weighted more heavily than raw C/V ratio.

In other words, I'd expect

"Can I distribute these consonants around the vowels without forcing impossible clusters?"

to be substantially more predictive than

"Is C/V near 1.6?"

That distinction is probably the useful one for your anagram scoring problem.
