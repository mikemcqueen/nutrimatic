# rebuild a sentence's pair file
#
# usage: make -f rebuild.mk [universe|wiki|cluer|combine|subsets|all]
#        S=N|S=N,M
# requires: source ./setup.sh
#
# S may also be lowercase, and is read only from the command line, since
# setup.sh sets it. The pair files depend on universe order-only, so removing
# dictionary words never rebuilds them; after adding words, run
# make -f rebuild.mk -B universe wiki cluer combine S=N. Add -B to rebuild
# targets that are up to date. subsets writes pairs.sN.NAME for each word
# list words.sN.NAME, keeping the pairs of pairs.sN whose words are both in
# it. S=N,M, N less than M, is a combined sentence: the letters of $SN and
# $SM, written to idx.2.sNsM, cluer.sNsM and pairs.sNsM, with the
# hand-added pairs of sN.pairs and sM.pairs.

SHELL := bash
.SHELLFLAGS := -euo pipefail -c
.DELETE_ON_ERROR:
.DEFAULT_GOAL := all

usage := usage: make -f rebuild.mk \
	[universe|wiki|cluer|combine|subsets|all] S=N|S=N,M \
	(N and M are 1-9, N less than M)

# the value of $1, or else $2, counting only command-line values
arg = $(strip $(or $(if $(filter command line,$(origin $1)),$($1)),\
	$(if $(filter command line,$(origin $2)),$($2))))

comma := ,
space := $(empty) $(empty)
sentence := $(call arg,S,s)
nums := $(patsubst s%,%,$(patsubst S%,%,$(subst $(comma), ,$(sentence))))
ifneq ($(words $(sentence)) $(filter 1 2 3 4 5 6 7 8 9,$(nums)),1 $(nums))
$(error $(usage))
endif
ifeq ($(words $(nums)),1)
ifneq ($(findstring $(comma),$(sentence)),)
$(error $(usage))
endif
else ifeq ($(words $(nums)),2)
ifneq ($(sentence) $(sort $(nums)),$(firstword $(nums))$(comma)$(lastword $(nums)) $(nums))
$(error $(usage))
endif
else
$(error $(usage))
endif
$(foreach num,$(nums),$(if $(S$(num)),,\
	$(error $$S$(num) is unset or empty; source ./setup.sh)))
id := $(subst $(space),,$(addprefix s,$(nums)))
letters := $(subst $(space),,$(foreach num,$(nums),$(S$(num))))
ifeq ($(IDX_DIR),)
$(error $$IDX_DIR is unset; source ./setup.sh)
endif
ifeq ($(wildcard $(IDX_DIR)/.),)
$(error IDX_DIR $(IDX_DIR) is not a directory)
endif
$(foreach var,BIG DICT,$(if $($(var)),,\
	$(error $$$(var) is unset; source ./setup.sh)))
$(foreach tool,query-index pcomm python,\
	$(if $(shell command -v $(tool)),,\
		$(error $(tool) not found on PATH; source ./setup.sh)))

here := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
wf := $(here)/wf
cluer := $(dir $(realpath $(wf)))cluer
tokens := $(here)/cluer/data/index/tokens.txt
bigrams := $(here)/cluer/data/index/bigrams.npy
ifeq ($(shell test -x '$(wf)' && echo ok),)
$(error $(wf) not found)
endif
ifeq ($(wildcard $(cluer)/query_index.py),)
$(error $(cluer)/query_index.py not found)
endif

universe := $(IDX_DIR)/universe
idx_out := $(IDX_DIR)/idx.2.$(id)
cluer_out := $(IDX_DIR)/cluer.$(id)
pairs_out := $(IDX_DIR)/pairs.$(id)
extra_pairs := $(wildcard $(foreach num,$(nums),$(IDX_DIR)/s$(num).pairs))
subset_words := $(wildcard $(IDX_DIR)/words.$(id).*)
subset_pairs := $(patsubst $(IDX_DIR)/words.%,$(IDX_DIR)/pairs.%,\
	$(subset_words))

.PHONY: all universe wiki cluer combine subsets
all: combine subsets
universe: $(universe)
wiki: $(idx_out)
cluer: $(cluer_out)
combine: $(pairs_out)
subsets: $(subset_pairs)

$(universe): $(IDX_DIR)/words $(BIG) $(DICT)
	LC_ALL=C sort -u $^ | LC_ALL=C awk 'length >= 3 && length <= 13' \
		> $@.tmp
	mv $@.tmp $@

$(idx_out): | $(universe)
	query-index '$(letters)' --wf -w2 -n0 --csv -m3 --dict $(universe) \
		> $@.tmp
	mv $@.tmp $@

# universe word pairs adjacent in some cluer clue that fit in the letters
$(cluer_out): $(tokens) $(bigrams) | $(universe)
	python $(cluer)/query_index.py --letters '$(letters)' \
		--dict $(universe) -m3 > $@.tmp
	mv $@.tmp $@

$(pairs_out): $(cluer_out) $(idx_out) $(extra_pairs)
	pcomm $(cluer_out) $(idx_out) \
		$(foreach file,$(extra_pairs),| pcomm - $(file)) | sort -u > $@.tmp
	mv $@.tmp $@

$(IDX_DIR)/pairs.$(id).%: $(IDX_DIR)/words.$(id).% $(pairs_out)
	LC_ALL=C awk -F, 'NR == FNR { w[$$1]; next } ($$1 in w) && ($$2 in w)' \
		$^ > $@.tmp
	mv $@.tmp $@

# the sentence seed dfs-anagrams -t sN resolves; not generated, so the
# existing links under $WFROOT/.wf/best/sN stay as they are
#
# seed := $(WFROOT)/.wf/best/$(id)/seed.m3.85.pairs
# .PHONY: seed
# seed: $(seed)
# $(seed): $(pairs_out) $(IDX_DIR)/p1.idx
# 	seed-pairs --p1 $(IDX_DIR)/p1.idx --pairs $(pairs_out) > $@.tmp
# 	mv $@.tmp $@
