# rebuild a sentence's pair file
#
# usage: make -f rebuild.mk [universe|wiki|cluer|combine|all] S=N
# requires: source ./setup.sh
#
# S may also be lowercase, and is read only from the command line, since
# setup.sh sets it. The pair files depend on universe order-only, so removing
# dictionary words never rebuilds them; after adding words, run
# make -f rebuild.mk -B universe wiki cluer combine S=N. Add -B to rebuild
# targets that are up to date.

SHELL := bash
.SHELLFLAGS := -euo pipefail -c
.DELETE_ON_ERROR:
.DEFAULT_GOAL := all

usage := usage: make -f rebuild.mk [universe|wiki|cluer|combine|all] S=N \
	(N is 1-9)

# the value of $1, or else $2, counting only command-line values
arg = $(strip $(or $(if $(filter command line,$(origin $1)),$($1)),\
	$(if $(filter command line,$(origin $2)),$($2))))

sentence := $(call arg,S,s)
n := $(patsubst s%,%,$(patsubst S%,%,$(sentence)))
ifneq ($(words $(sentence)) $(words $(n)) $(filter 1 2 3 4 5 6 7 8 9,$(n)),1 1 $(n))
$(error $(usage))
endif
letters := $(S$(n))
ifeq ($(letters),)
$(error $$S$(n) is unset or empty; source ./setup.sh)
endif
ifeq ($(IDX_DIR),)
$(error $$IDX_DIR is unset; source ./setup.sh)
endif
ifeq ($(wildcard $(IDX_DIR)/.),)
$(error IDX_DIR $(IDX_DIR) is not a directory)
endif
$(foreach var,BIG DICT,$(if $($(var)),,\
	$(error $$$(var) is unset; source ./setup.sh)))
$(foreach tool,query-index pairs pcomm python,\
	$(if $(shell command -v $(tool)),,\
		$(error $(tool) not found on PATH; source ./setup.sh)))

here := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
wf := $(here)/wf
cluer := $(dir $(realpath $(wf)))cluer
tokens := $(here)/cluer/data/index/tokens.txt
ifeq ($(shell test -x '$(wf)' && echo ok),)
$(error $(wf) not found)
endif
ifeq ($(wildcard $(cluer)/query_index.py),)
$(error $(cluer)/query_index.py not found)
endif

universe := $(IDX_DIR)/universe
idx_out := $(IDX_DIR)/idx.2.s$(n)
cluer_out := $(IDX_DIR)/cluer.s$(n)
pairs_out := $(IDX_DIR)/pairs.s$(n)
extra_pairs := $(wildcard $(IDX_DIR)/s$(n).pairs)

.PHONY: all universe wiki cluer combine
all: combine
universe: $(universe)
wiki: $(idx_out)
cluer: $(cluer_out)
combine: $(pairs_out)

$(universe): $(IDX_DIR)/words $(BIG) $(DICT)
	LC_ALL=C sort -u $^ | LC_ALL=C awk 'length >= 3 && length <= 13' \
		> $@.tmp
	mv $@.tmp $@

$(idx_out): | $(universe)
	query-index '$(letters)' --wf -w2 -n0 --csv -m3 --dict $(universe) \
		> $@.tmp
	mv $@.tmp $@

# filter out non-cluer-index dict words before sending to pairs
$(cluer_out): $(tokens) | $(universe)
	@echo "Generating $(@F), this might take a while..." >&2
	LC_ALL=C comm -12 $(tokens) $(universe) | \
		pairs '$(letters)' -d - -m3 | \
		python $(cluer)/query_index.py -f - -j --threads 2 > $@.tmp
	mv $@.tmp $@

$(pairs_out): $(cluer_out) $(idx_out) $(extra_pairs)
	pcomm $(cluer_out) $(idx_out) \
		$(if $(extra_pairs),| pcomm - $(extra_pairs)) | sort -u > $@.tmp
	mv $@.tmp $@

# the sentence seed dfs-anagrams -t sN resolves; not generated, so the
# existing links under $WFROOT/.wf/best/sN stay as they are
#
# seed := $(WFROOT)/.wf/best/s$(n)/seed.m3.85.pairs
# .PHONY: seed
# seed: $(seed)
# $(seed): $(pairs_out) $(IDX_DIR)/p1.idx
# 	seed-pairs --p1 $(IDX_DIR)/p1.idx --pairs $(pairs_out) > $@.tmp
# 	mv $@.tmp $@
