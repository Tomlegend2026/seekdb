#!/usr/bin/env python3
"""Run AI SQL against a real seekdb binary and an in-process mock model service."""

import argparse
import json
import subprocess
import threading
import uuid

from ai_function_mock import ModelServer, embedding


def literal(value):
    # UTF-8 hex literals avoid dependence on the connection's SQL escape mode.
    return "CONVERT(X'" + value.encode("utf-8").hex() + "' USING utf8mb4)"


class Regression:
    def __init__(self, args, server):
        self.args = args
        self.server = server
        self.prefix = "ai_reg_" + uuid.uuid4().hex[:10]
        self.models = []
        self.table = self.prefix + "_rows"
        self.base = f"http://{args.model_host}:{server.server_port}"

    def sql(self, statement, error=False):
        command = [self.args.mysql_client, "--protocol=tcp", "--host=" + self.args.host,
                   "--port=" + str(self.args.port), "--user=" + self.args.user,
                   "--default-character-set=utf8mb4", "--batch", "--raw", "--skip-column-names",
                   self.args.database]
        result = subprocess.run(command, input=statement, text=True, capture_output=True, timeout=90)
        if error:
            assert result.returncode != 0, "Invalid SQL unexpectedly succeeded"
        elif result.returncode:
            raise RuntimeError(result.stderr.strip())
        return result.stdout.rstrip("\n")

    def model(self, suffix, kind, provider, path):
        name = self.prefix + "_" + suffix
        self.sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(" + literal(name) + "," +
                 literal(json.dumps({"type": kind, "model_name": "regression-model"})) + ");")
        self.models.append(name)
        config = {"ai_model_name": name, "scope": "all", "url": self.base + path,
                  "provider": provider, "access_key": "" if provider.lower() == "ollama" else "mock-key"}
        self.sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(" + literal(name + "_ep") + "," +
                 literal(json.dumps(config)) + ");")
        return literal(name)

    def complete(self, model, prompt, config="NULL"):
        output = self.sql(f"SELECT HEX(AI_COMPLETE({model},{prompt},{config}));")
        decoded = bytes.fromhex(output).decode("utf-8")
        # Current seekdb serializes the model's JSON string as its SQL result.
        return json.loads(decoded)

    def run(self):
        complete = self.model("complete", "completion", "openai", "/v1/chat/completions")
        embed = self.model("embed", "dense_embedding", "openai", "/v1/embeddings")
        rerank = self.model("rerank", "rerank", "siliconflow", "/v1/rerank")
        ollama = self.model("ollama", "completion", "oLlAmA", "/api/generate")
        ollama_embed = self.model("ollama_embed", "dense_embedding", "OLLAMA", "/api/embed")

        text = "你好\\\"\n" + "x" * 70000
        self.sql(f"CREATE TABLE {self.table}(id INT PRIMARY KEY, content LONGTEXT);")
        self.sql(f"INSERT INTO {self.table} VALUES(1,{literal(text)});")
        prompt = f"(SELECT AI_PROMPT('{{0}}/{{0}}',content) FROM {self.table} WHERE id=1)"
        assert self.complete(complete, prompt) == text + "/" + text
        nested = "AI_PROMPT('<{0}>',AI_PROMPT('hi {0}'," + literal("世界") + "))"
        assert self.complete(complete, nested, "JSON_OBJECT('temperature',0.2)") == "<hi 世界>"
        structured = "AI_PROMPT('{0}|{1}',JSON_OBJECT('k','v'),JSON_ARRAY(1,TRUE))"
        value = self.complete(complete, structured)
        left, right = value.split("|")
        assert json.loads(left) == {"k": "v"} and json.loads(right) == [1, True]
        assert self.complete(ollama, literal("Ollama 你好")) == "Ollama 你好"
        print("PASS prompt / LONGTEXT / JSON / completion / Ollama", flush=True)

        for provider in (embed, ollama_embed):
            actual = json.loads(self.sql(f"SELECT AI_EMBED({provider},'single',4);"))
            assert actual == embedding("single", 4)
            for dimension in ("0", "-1", "NULL", "1.5"):
                before = len(self.server.snapshot())
                self.sql(f"SELECT AI_EMBED({provider},'single',{dimension});", error=True)
                assert len(self.server.snapshot()) == before, "Invalid dimension reached the model"
            for bad in ("__bad_shape__", "__bad_dimension__"):
                self.sql(f"SELECT AI_EMBED({provider},'{bad}',4);", error=True)
        print("PASS embedding dimensions / malformed responses", flush=True)

        self.sql(f"DELETE FROM {self.table};")
        contents = {i: None if i % 7 == 0 else f"row-{i}" for i in range(1, 131)}
        values = ",".join(f"({i},{'NULL' if text is None else literal(text)})"
                          for i, text in contents.items())
        self.sql(f"INSERT INTO {self.table} VALUES {values};")
        marker = len(self.server.snapshot())
        rows = self.sql(f"SELECT id,AI_EMBED({embed},content,4) FROM {self.table} ORDER BY id;")
        returned = {}
        for row in rows.splitlines():
            key, vector = row.split("\t")
            returned[int(key)] = None if vector == "NULL" else json.loads(vector)
        assert set(returned) == set(contents), "Rows lost during batch processing"
        for key, content in contents.items():
            assert returned[key] == (None if content is None else embedding(content, 4)), key
        requests = [body["input"] for path, body in self.server.snapshot()[marker:]
                    if path.endswith("/embeddings")]
        assert any(len(batch) > 1 for batch in requests), "Every row made its own HTTP request"
        assert all(1 <= len(batch) <= 64 for batch in requests)
        assert sorted(text for batch in requests for text in batch) == sorted(
            text for text in contents.values() if text is not None), "Null, duplicate or missing model input"
        print(f"PASS batch embedding: {len(contents)} rows, {len(requests)} HTTP requests", flush=True)

        documents = [f"{'hit' if i % 3 == 0 else 'miss'}-{i}" for i in range(61)]
        expected = sorted(range(len(documents)), key=lambda i: (i % 3 != 0, i))
        result = json.loads(self.sql(f"SELECT AI_RERANK({rerank},'hit'," +
                                    literal(json.dumps(documents)) + ",NULL);"))
        assert [entry["index"] for entry in result] == expected, "Rerank global order or tie stability failed"
        objects = [{"id": i, "text": text} for i, text in enumerate(documents)]
        result = json.loads(self.sql(f"SELECT AI_RERANK({rerank},'hit'," +
                                    literal(json.dumps(objects)) + ",'text');"))
        assert [entry["id"] for entry in result] == expected
        assert all("_model_score" in entry for entry in result)
        print("PASS rerank: unequal batch sizes, global indices, stable ties, object documents", flush=True)

    def cleanup(self):
        self.sql(f"DROP TABLE IF EXISTS {self.table};")
        for name in reversed(self.models):
            # An endpoint may not exist if setup failed midway.
            try:
                self.sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(" + literal(name + "_ep") + ");")
            except RuntimeError:
                pass
            self.sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL(" + literal(name) + ");")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mysql-client", default="mysql")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--user", default="root")
    parser.add_argument("--database", default="test")
    parser.add_argument("--mock-bind", default="127.0.0.1")
    parser.add_argument("--model-host", default="127.0.0.1")
    args = parser.parse_args()
    # mysql reads MYSQL_PWD or its normal client config; no secrets in arguments.
    with ModelServer((args.mock_bind, 0)) as server:
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        regression = Regression(args, server)
        try:
            regression.run()
        finally:
            try:
                regression.cleanup()
            finally:
                server.shutdown()
                worker.join(timeout=5)


if __name__ == "__main__":
    main()
