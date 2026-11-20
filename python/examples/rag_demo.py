"""A local retrieval-augmented, extractive answer backed by persisted vectors."""
import argparse
import json
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

import strata

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from vector_data import Encoder

DOCUMENTS = {
    "wal": "Strata acknowledges a write only after its write-ahead log is synced. "
           "On restart, it replays the log and repairs a torn final record.",
    "sstables": "Strata flushes sorted in-memory rows to immutable SSTables. A manifest identifies "
                "the live tables and the logs required to recover the database.",
    "hnsw": "HNSW searches a layered proximity graph for nearby embeddings. Increasing ef_search "
            "lets the search consider more candidates and usually improves recall.",
    "bloom": "A Bloom filter lets Strata skip tables that cannot contain a key. "
             "False positives cause an extra lookup but do not hide stored keys.",
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("question", nargs="?", default="When does a write become durable?")
    parser.add_argument("--cache", type=Path, default=Path("build-vector-cache"))
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    encoder = Encoder(args.cache, max_tokens=128)
    with TemporaryDirectory(prefix="strata-rag-") as directory:
        path = Path(directory) / "db"
        db = strata.DB(path)
        ids = list(DOCUMENTS)
        for name, embedding in zip(ids, encoder.encode(list(DOCUMENTS.values()))):
            db.put("doc/" + name, DOCUMENTS[name])
            db.vput(name, embedding)
        del db

        db = strata.DB(path)
        hits = db.vsearch(encoder.encode([args.question])[0], 2)
        sources = [{"id": name, "distance": distance, "passage": db.get("doc/" + name)}
                   for name, distance in hits]
        result = {"question": args.question, "answer": sources[0]["passage"], "sources": sources}
        del db
        if args.json:
            print(json.dumps(result, indent=2))
        else:
            print(f"Question: {result['question']}\nAnswer: {result['answer']}\n")
            for source in sources:
                print(f"[{source['id']}] {source['passage']}")


if __name__ == "__main__":
    main()
