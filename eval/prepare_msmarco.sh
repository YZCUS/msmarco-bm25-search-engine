#!/usr/bin/env bash
# prepare_msmarco.sh: download the MS MARCO Passage v1 collection together
# with the dev queries (extracted from queries.tar.gz) and the TREC DL 2019 /
# 2020 evaluation sets.
#
# All artifacts land under ./data/ which is git-ignored.
set -euo pipefail

mkdir -p data
cd data

if [[ ! -f collection.tsv ]]; then
    echo "[prepare] downloading collection.tar.gz (~1 GB)"
    curl -L --fail -o collection.tar.gz \
        https://msmarco.z22.web.core.windows.net/msmarcoranking/collection.tar.gz
    echo "[prepare] extracting collection.tsv"
    tar -xzf collection.tar.gz
    rm collection.tar.gz
fi

# Dev queries + qrels live inside queries.tar.gz; qrels.dev.small.tsv is
# also published at a top-level URL but we extract from the archive to keep
# everything aligned.
if [[ ! -f qrels.dev.small.tsv ]]; then
    echo "[prepare] downloading qrels.dev.small.tsv"
    curl -L --fail -o qrels.dev.small.tsv \
        https://msmarco.z22.web.core.windows.net/msmarcoranking/qrels.dev.small.tsv
fi

# queries.dev.small.tsv is published as a subset of queries.dev.tsv (6,980
# queries with at least one relevant passage). The MS MARCO mirror does not
# host the small variant directly, so we filter queries.dev.tsv ourselves
# using the qids present in qrels.dev.small.tsv.
if [[ ! -f queries.dev.small.tsv ]]; then
    if [[ ! -f queries.dev.tsv ]]; then
        echo "[prepare] downloading queries.tar.gz (~18 MB)"
        curl -L --fail -o queries.tar.gz \
            https://msmarco.z22.web.core.windows.net/msmarcoranking/queries.tar.gz
        echo "[prepare] extracting queries.dev.tsv"
        tar -xzf queries.tar.gz queries.dev.tsv
        rm queries.tar.gz
    fi
    echo "[prepare] filtering queries.dev.small.tsv from queries.dev.tsv"
    python3 - <<'PY'
qids = set()
with open('qrels.dev.small.tsv') as f:
    for line in f:
        qids.add(line.split()[0])
with open('queries.dev.tsv') as src, open('queries.dev.small.tsv', 'w') as dst:
    n = 0
    for line in src:
        if line.split('\t', 1)[0] in qids:
            dst.write(line)
            n += 1
print(f'[prepare] {n} dev queries written')
PY
    rm -f queries.dev.tsv queries.eval.tsv queries.train.tsv
fi

# TREC DL 2019
if [[ ! -f 2019qrels-pass.txt ]]; then
    echo "[prepare] downloading DL19 qrels"
    curl -L --fail -O https://trec.nist.gov/data/deep/2019qrels-pass.txt
fi
if [[ ! -f msmarco-test2019-queries.tsv ]]; then
    echo "[prepare] downloading DL19 queries"
    curl -L --fail -O https://msmarco.z22.web.core.windows.net/msmarcoranking/msmarco-test2019-queries.tsv.gz
    gunzip -k msmarco-test2019-queries.tsv.gz
fi

# TREC DL 2020
if [[ ! -f 2020qrels-pass.txt ]]; then
    echo "[prepare] downloading DL20 qrels"
    curl -L --fail -O https://trec.nist.gov/data/deep/2020qrels-pass.txt
fi
if [[ ! -f msmarco-test2020-queries.tsv ]]; then
    echo "[prepare] downloading DL20 queries"
    curl -L --fail -O https://msmarco.z22.web.core.windows.net/msmarcoranking/msmarco-test2020-queries.tsv.gz
    gunzip -k msmarco-test2020-queries.tsv.gz
fi

echo "[prepare] done. Files in $(pwd):"
ls -lh collection.tsv queries.dev.small.tsv qrels.dev.small.tsv \
       2019qrels-pass.txt msmarco-test2019-queries.tsv \
       2020qrels-pass.txt msmarco-test2020-queries.tsv 2>/dev/null || true
