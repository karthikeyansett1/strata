"""Generate reproducible MiniLM embeddings for the Strata evaluation."""
import argparse
import csv
import hashlib
import platform
import shutil
import time
from pathlib import Path
from urllib.request import urlopen

import numpy as np
import onnxruntime as ort
from tokenizers import Tokenizer

MODEL_REVISION = "1110a243fdf4706b3f48f1d95db1a4f5529b4d41"
MODEL = f"https://huggingface.co/sentence-transformers/all-MiniLM-L6-v2/resolve/{MODEL_REVISION}"
NEWS = "https://raw.githubusercontent.com/mhjabreel/CharCnn_Keras/master/data/ag_news_csv"
NEWS_SHA256 = {
    "train": "76a0a2d2f92b286371fe4d4044640910a04a803fdd2538e0f3f29a5c6f6b672e",
    "test": "521465c2428ed7f02f8d6db6ffdd4b5447c1c701962353eb2c40d548c3c85699",
}


def download(url, path):
    if not path.exists():
        temporary = path.with_suffix(path.suffix + ".tmp")
        # A fresh redirect avoids stale signed URLs cached by intermediate proxies.
        if "huggingface.co" in url:
            url += f"?download=true&t={time.time_ns()}"
        with urlopen(url, timeout=120) as source, temporary.open("wb") as output:
            shutil.copyfileobj(source, output)
        temporary.replace(path)
    return path


class Encoder:
    def __init__(self, cache, max_tokens=24, threads=2, graph=None):
        cache.mkdir(parents=True, exist_ok=True)
        if graph is None:
            graph = ("model_qint8_arm64.onnx" if platform.machine() == "aarch64"
                     else "model_quint8_avx2.onnx")
        self.tokenizer = Tokenizer.from_file(str(download(f"{MODEL}/tokenizer.json", cache / "tokenizer.json")))
        self.tokenizer.enable_truncation(max_length=max_tokens)
        self.tokenizer.enable_padding(pad_id=0, pad_token="[PAD]")
        options = ort.SessionOptions()
        options.intra_op_num_threads = threads
        options.inter_op_num_threads = 1
        model = download(f"{MODEL}/onnx/{graph}", cache / graph)
        self.session = ort.InferenceSession(str(model), options, providers=["CPUExecutionProvider"])

    def encode(self, texts):
        encodings = self.tokenizer.encode_batch(texts)
        data = {
            "input_ids": np.asarray([item.ids for item in encodings], dtype=np.int64),
            "attention_mask": np.asarray([item.attention_mask for item in encodings], dtype=np.int64),
            "token_type_ids": np.asarray([item.type_ids for item in encodings], dtype=np.int64),
        }
        hidden = self.session.run(None, {i.name: data[i.name] for i in self.session.get_inputs()})[0]
        mask = data["attention_mask"][..., None].astype(np.float32)
        pooled = (hidden * mask).sum(axis=1) / mask.sum(axis=1)
        return pooled / np.maximum(np.linalg.norm(pooled, axis=1, keepdims=True), 1e-12)


def titles(cache, split, count):
    path = download(f"{NEWS}/{split}.csv", cache / f"ag-news-{split}.csv")
    if hashlib.sha256(path.read_bytes()).hexdigest() != NEWS_SHA256[split]:
        raise ValueError(f"AG News {split} checksum mismatch")
    with path.open(newline="", encoding="utf-8") as stream:
        import itertools
        result = [row[1] for row in itertools.islice(csv.reader(stream), count)]
    if len(result) != count:
        raise ValueError(f"AG News {split} contains fewer than {count} titles")
    return result


def encode_split(encoder, cache, split, count, batch):
    target = cache / f"{split}-{count}.npy"
    if target.exists():
        return
    samples = titles(cache, split, count)
    temporary = target.with_suffix(".tmp.npy")
    output = np.lib.format.open_memmap(temporary, mode="w+", dtype=np.float32, shape=(count, 384))
    for first in range(0, count, batch):
        end = min(first + batch, count)
        output[first:end] = encoder.encode(samples[first:end])
        if first == 0 or end % (batch * 64) == 0 or end == count:
            print(f"{split}: {end}/{count} embeddings", flush=True)
    output.flush()
    del output
    temporary.replace(target)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cache", type=Path, default=Path("build-vector-cache"))
    parser.add_argument("--count", type=int, default=100_000)
    parser.add_argument("--queries", type=int, default=200)
    parser.add_argument("--batch", type=int, default=128)
    parser.add_argument("--threads", type=int, default=2)
    args = parser.parse_args()
    if min(args.count, args.queries, args.batch, args.threads) < 1:
        parser.error("counts, batch size, and thread count must be positive")
    encoder = Encoder(args.cache, threads=args.threads)
    encode_split(encoder, args.cache, "train", args.count, args.batch)
    encode_split(encoder, args.cache, "test", args.queries, args.batch)


if __name__ == "__main__":
    main()
