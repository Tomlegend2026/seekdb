#!/usr/bin/env python3
"""Deterministic, local-only model service for seekdb integration tests."""

import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import threading


def embedding(text, dimension):
    seed = int.from_bytes(hashlib.sha256(text.encode("utf-8")).digest()[:4], "big")
    return [float((seed + i) % 1009) for i in range(dimension)]


class ModelServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address):
        super().__init__(address, ModelHandler)
        self.records = []
        self.record_lock = threading.Lock()

    def snapshot(self):
        with self.record_lock:
            return list(self.records)


class ModelHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def reply(self, status, body):
        payload = json.dumps(body, ensure_ascii=False, allow_nan=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_POST(self):
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if size <= 0 or size > 4 * 1024 * 1024:
                raise ValueError("invalid body size")
            body = json.loads(self.rfile.read(size))
            if not isinstance(body, dict) or not isinstance(body.get("model"), str):
                raise ValueError("model must be a string")
            # Never record authorization headers or other connection credentials.
            with self.server.record_lock:
                self.server.records.append((self.path, body))
            if self.path.endswith("/chat/completions"):
                text = body["messages"][-1]["content"]
                if not isinstance(text, str):
                    raise ValueError("content must be text")
                result = {"choices": [{"message": {"content": text}}]}
                if text == "__bad_root_array__":
                    result = []
                elif text == "__bad_root_scalar__":
                    result = "invalid response root"
            elif self.path == "/api/generate":
                if body.get("stream") is not False:
                    raise ValueError("stream must be false")
                result = {"response": body["prompt"], "done": True}
            elif self.path.endswith("/embeddings") or self.path == "/api/embed":
                texts = body["input"]
                if not isinstance(texts, list) or not texts or not all(isinstance(t, str) for t in texts):
                    raise ValueError("input must be a nonempty array of strings")
                dimension = body.get("dimensions", 768)
                if type(dimension) is not int or not 1 <= dimension <= 4096:
                    raise ValueError("unsupported dimension")
                vectors = [embedding(text, dimension) for text in texts]
                if self.path == "/api/embed":
                    result = {"embeddings": vectors}
                else:
                    # Reverse response order to expose incorrect positional mapping.
                    result = {"data": [{"index": i, "embedding": vectors[i]}
                                       for i in reversed(range(len(texts)))]}
                if "__bad_shape__" in texts:
                    result = {"data": {}, "embeddings": {}}
                elif "__bad_root_array__" in texts:
                    result = []
                elif "__bad_dimension__" in texts:
                    if self.path == "/api/embed":
                        result["embeddings"][0].append(0)
                    else:
                        result["data"][0]["embedding"].append(0)
            elif self.path == "/v1/rerank":
                documents = body["documents"]
                query = body["query"]
                if not isinstance(documents, list) or not all(isinstance(t, str) for t in documents):
                    raise ValueError("documents must contain strings")
                # Deliberately unsorted; ties span request boundaries.
                result = {"results": [
                    {"index": i, "relevance_score": 1.0 if query in text else 0.0}
                    for i, text in enumerate(documents)
                ]}
            else:
                self.reply(404, {"error": "unknown endpoint"})
                return
            self.reply(200, result)
        except (ValueError, TypeError, KeyError, IndexError) as error:
            self.reply(400, {"error": str(error)})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()
    with ModelServer((args.host, args.port)) as server:
        print(f"Mock model server: http://{args.host}:{server.server_port}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
