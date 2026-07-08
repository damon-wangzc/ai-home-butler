"""Home Knowledge RAG service.

Indexes .txt and .md files from DOCS_DIR into a ChromaDB 'home_knowledge'
collection on startup. New files dropped into DOCS_DIR can be indexed via
POST /v1/index without restarting.

GET  /health       → {"status":"ok","chunks":N}
POST /v1/query     → {"query":"...","top_k":3}
                   → {"results":[{"content":"...","source":"...","score":0.9}]}
POST /v1/index     → {"source":"filename.txt","content":"..."}
                   → {"indexed":true,"chunks":N}
"""

import logging
import os
from contextlib import asynccontextmanager

import asyncio
import chromadb
from chromadb.utils.embedding_functions import DefaultEmbeddingFunction
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel

logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s %(message)s")
logger = logging.getLogger(__name__)

CHROMA_HOST = os.getenv("CHROMA_HOST", "chromadb")
CHROMA_PORT = int(os.getenv("CHROMA_PORT", "8000"))
DOCS_DIR = os.getenv("DOCS_DIR", "/data/home-docs")
RAG_TOP_K = int(os.getenv("RAG_TOP_K", "3"))
CHUNK_SIZE = int(os.getenv("CHUNK_SIZE", "400"))

_collection = None


def _chunk_text(text: str) -> list[str]:
    """Split on blank lines; merge short paragraphs up to CHUNK_SIZE chars."""
    paras = [p.strip() for p in text.split("\n\n") if p.strip()]
    chunks, buf = [], ""
    for p in paras:
        if buf and len(buf) + len(p) + 2 > CHUNK_SIZE:
            chunks.append(buf)
            buf = p
        else:
            buf = f"{buf}\n\n{p}" if buf else p
    if buf:
        chunks.append(buf)
    return chunks or [text[:CHUNK_SIZE]]


async def _get_collection():
    global _collection
    if _collection is None:
        ef = DefaultEmbeddingFunction()
        client = await chromadb.AsyncHttpClient(host=CHROMA_HOST, port=CHROMA_PORT)
        _collection = await client.get_or_create_collection(
            "home_knowledge", embedding_function=ef
        )
    return _collection


async def _ingest(source: str, content: str) -> int:
    """Index content into home_knowledge; skips already-indexed chunk IDs."""
    col = await _get_collection()
    chunks = _chunk_text(content)
    ids = [f"{source}::chunk{i}" for i in range(len(chunks))]
    metas = [{"source": source, "chunk": i} for i in range(len(chunks))]
    existing = await col.get(ids=ids, include=[])
    existing_ids = set(existing.get("ids") or [])
    new = [(ids[i], chunks[i], metas[i]) for i in range(len(ids)) if ids[i] not in existing_ids]
    if new:
        await col.add(
            ids=[t[0] for t in new],
            documents=[t[1] for t in new],
            metadatas=[t[2] for t in new],
        )
    logger.info("ingest source=%s new_chunks=%d total=%d", source, len(new), len(chunks))
    return len(new)


@asynccontextmanager
async def lifespan(app: FastAPI):
    # Pre-warm ONNX embedding model (same cache volume as orchestrator)
    await asyncio.to_thread(lambda: DefaultEmbeddingFunction()(["warmup"]))
    logger.info("ONNX embedding model ready")

    # Auto-index all docs in DOCS_DIR
    os.makedirs(DOCS_DIR, exist_ok=True)
    total_new = 0
    for fname in sorted(os.listdir(DOCS_DIR)):
        if not fname.endswith((".txt", ".md")):
            continue
        path = os.path.join(DOCS_DIR, fname)
        try:
            content = open(path, encoding="utf-8").read()
            n = await _ingest(fname, content)
            total_new += n
        except Exception as e:
            logger.warning("Failed to index %s: %s", fname, e)

    col = await _get_collection()
    total = await col.count()
    logger.info("RAG ready — %d chunks in home_knowledge (%d new this boot)", total, total_new)
    yield


app = FastAPI(title="RAG Service", lifespan=lifespan)


class QueryRequest(BaseModel):
    query: str
    top_k: int = RAG_TOP_K


class IndexRequest(BaseModel):
    source: str
    content: str


@app.post("/v1/query")
async def query(req: QueryRequest):
    try:
        col = await _get_collection()
        count = await col.count()
        if count == 0:
            return {"results": []}
        n = min(req.top_k, count)
        res = await col.query(
            query_texts=[req.query],
            n_results=n,
            include=["documents", "metadatas", "distances"],
        )
        results = [
            {
                "content": doc,
                "source": meta.get("source", ""),
                "score": round(1.0 - dist, 4),  # cosine distance → similarity
            }
            for doc, meta, dist in zip(
                res["documents"][0], res["metadatas"][0], res["distances"][0]
            )
        ]
        return {"results": results}
    except Exception as e:
        logger.error("query error: %s", e)
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/v1/index")
async def index_doc(req: IndexRequest):
    try:
        n = await _ingest(req.source, req.content)
        return {"indexed": True, "chunks": n}
    except Exception as e:
        logger.error("index error: %s", e)
        raise HTTPException(status_code=500, detail=str(e))


@app.get("/health")
async def health():
    try:
        col = await _get_collection()
        count = await col.count()
        return {"status": "ok", "chunks": count}
    except Exception:
        return {"status": "degraded", "chunks": 0}
