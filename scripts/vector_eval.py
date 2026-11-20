"""Compare Strata's HNSW and exact top-k on cached MiniLM embeddings."""
import argparse
import csv
from pathlib import Path
from time import perf_counter

import numpy as np
import strata


def timed_search(index, queries, k, ef=0):
    index.search(queries[:4], k, ef)
    start = perf_counter()
    hits = index.search(queries, k, ef)
    return hits, (perf_counter() - start) * 1e6 / len(queries)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cache", type=Path, default=Path("build-vector-cache"))
    parser.add_argument("--count", type=int, default=100_000)
    parser.add_argument("--queries", type=int, default=200)
    parser.add_argument("--output", type=Path, default=Path("results/vector_eval.csv"))
    args = parser.parse_args()
    if args.count < 10 or args.queries < 1:
        parser.error("at least 10 indexed vectors and one query are required")
    corpus = np.load(args.cache / f"train-{args.count}.npy", mmap_mode="r")
    queries = np.load(args.cache / f"test-{args.queries}.npy", mmap_mode="r")
    if corpus.shape != (args.count, 384) or queries.shape != (args.queries, 384):
        raise ValueError("cached embeddings have unexpected dimensions")

    k, m, ef_construction = 10, 16, 100
    flat = strata._FlatIndex(strata.Metric.COSINE)
    flat.add(corpus)
    graph = strata._HnswIndex(strata.Metric.COSINE, m, ef_construction)
    start = perf_counter()
    graph.add(corpus)
    build_seconds = perf_counter() - start
    print(f"Built HNSW on {len(corpus)} vectors in {build_seconds:.3f}s", flush=True)
    exact, flat_us = timed_search(flat, queries, k)

    fields = ("vectors", "queries", "dimensions", "m", "ef_construction", "ef_search",
              "recall_at_10", "flat_us_per_query", "hnsw_us_per_query", "speedup", "build_seconds")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        for ef in (16, 32, 64, 128, 256):
            approx, graph_us = timed_search(graph, queries, k, ef)
            recall = sum(len(set(a).intersection(b)) for a, b in zip(exact, approx)) / (len(queries) * k)
            result = dict(zip(fields, (len(corpus), len(queries), corpus.shape[1], m, ef_construction,
                                       ef, f"{recall:.4f}", f"{flat_us:.3f}", f"{graph_us:.3f}",
                                       f"{flat_us / graph_us:.3f}", f"{build_seconds:.3f}")))
            writer.writerow(result)
            print(result, flush=True)


if __name__ == "__main__":
    main()
