"""Memory retrieval — Phase 1: ChromaDB vector store with top-k=3 retrieval.

Graceful degradation: returns "" on any ChromaDB connection failure so the
orchestrator continues working without memory (Phase 0 behaviour).

Compression budget: adapter.build_messages() caps the injected summary at
_MEMORY_CHAR_LIMIT chars (≈150 tokens). Raw retrieval is done here; truncation
is the adapter's responsibility.
"""

import logging
import os
import uuid

logger = logging.getLogger(__name__)

CHROMA_URL = os.getenv("CHROMA_URL", "http://chromadb:8000")
_TOP_K = 3

_client = None  # lazy-initialised AsyncHttpClient
_embedding_fn = None  # cached DefaultEmbeddingFunction instance (ONNX model is heavy)


def _parse_chroma_host_port(url: str) -> tuple[str, int]:
    url = url.rstrip("/")
    if "://" in url:
        url = url.split("://", 1)[1]
    if ":" in url:
        host, port_str = url.rsplit(":", 1)
        return host, int(port_str)
    return url, 8000


def _get_embedding_fn():
    global _embedding_fn
    if _embedding_fn is None:
        from chromadb.utils.embedding_functions import DefaultEmbeddingFunction
        _embedding_fn = DefaultEmbeddingFunction()
    return _embedding_fn


async def _get_client():
    global _client
    if _client is None:
        import chromadb  # deferred — only required in Phase 1+

        host, port = _parse_chroma_host_port(CHROMA_URL)
        _client = await chromadb.AsyncHttpClient(host=host, port=port)
    return _client


async def _get_collection(user_id: str):
    client = await _get_client()
    return await client.get_or_create_collection(
        name=f"user_{user_id}",
        embedding_function=_get_embedding_fn(),
    )


async def store_turn(user_id: str, query: str, response: str) -> None:
    """Persist a Q&A turn to the user's ChromaDB collection."""
    try:
        col = await _get_collection(user_id)
        await col.add(
            documents=[f"Q: {query}\nA: {response}"],
            ids=[str(uuid.uuid4())],
        )
        logger.debug("memory.store_turn user_id=%s", user_id)
    except Exception:
        logger.warning("memory.store_turn failed — ChromaDB unavailable", exc_info=True)


async def get_memory_summary(user_id: str, text: str) -> str:
    """Return the top-3 relevant memory turns concatenated for context injection.

    Returns "" when ChromaDB is unavailable (degrades to Phase 0 behaviour).
    Truncation to ≤150 tokens is applied downstream in adapter.build_messages().
    """
    try:
        col = await _get_collection(user_id)
        count = await col.count()
        if count == 0:
            return ""
        results = await col.query(
            query_texts=[text],
            n_results=min(_TOP_K, count),
        )
        docs: list[str] = results.get("documents", [[]])[0]
        return " | ".join(docs) if docs else ""
    except Exception:
        logger.warning("memory.get_memory_summary failed", exc_info=True)
        return ""
