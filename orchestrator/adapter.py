"""Compact prompt builder and model-server caller.

Design rules (token budget):
  - System prompt: ≤ 120 tokens (hard limit)
  - Memory injection: ≤ 150 tokens (≈ _MEMORY_CHAR_LIMIT chars), truncated here
  - stream=False for WSL/llama-cpp-rs compat (streaming unsupported in current build)

Phase 1:
  - call_model() accepts optional tools list for OpenAI-style native function calling
  - extract_response() checks tool_calls first, falls back to custom JSON format
"""

import json
import logging
import os
from typing import Any

import httpx

logger = logging.getLogger(__name__)

MODEL_SERVER_URL = os.getenv("MODEL_SERVER_URL", "http://model-server:8080/v1")
MODEL_NAME = os.getenv("MODEL_NAME", "local")
MAX_TOKENS = int(os.getenv("MAX_TOKENS", "512"))
TEMPERATURE = float(os.getenv("TEMPERATURE", "0.2"))

# Kept deliberately under 120 tokens.
# Tool-call protocol: model should use native function calling when supported.
# Fallback: respond ONLY with JSON {"call": "name", "args": {...}} for tools.
_SYSTEM_PROMPT = (
    "You are Butler, a home AI assistant. Reply concisely. "
    'Fallback tool-call format: {"call":"tool_name","args":{...}}. '
    "Otherwise reply in plain text."
)

# Budget: ≤ 150 tokens injected as context (1 token ≈ 6 chars, conservative)
_MEMORY_CHAR_LIMIT = 900


def build_messages(user_id: str, text: str, memory_summary: str = "") -> list[dict]:
    """Build the compact message list sent to model-server.

    memory_summary is injected as a bracketed prefix inside the user turn
    so it never inflates the system prompt. Capped at _MEMORY_CHAR_LIMIT chars
    (task 1.3: memory compression budget ≤ 150 tokens).
    """
    if memory_summary:
        memory_summary = memory_summary[:_MEMORY_CHAR_LIMIT]
    user_content = f"[context: {memory_summary}]\n{text}" if memory_summary else text
    return [
        {"role": "system", "content": _SYSTEM_PROMPT},
        {"role": "user", "content": user_content},
    ]


async def call_model(
    messages: list[dict],
    tools: list[dict] | None = None,
) -> dict:
    """POST /v1/chat/completions and return the raw JSON response dict.

    Uses stream=False for compatibility with the current llama-cpp-rs build.
    Timeout is 120 s to accommodate slow CPU inference on Gemma 4B.
    Pass tools to enable OpenAI-style native function calling (Phase 1+).
    """
    payload: dict[str, Any] = {
        "model": MODEL_NAME,
        "messages": messages,
        "temperature": TEMPERATURE,
        "max_tokens": MAX_TOKENS,
        "stream": False,
        # Disable Qwen3 extended thinking: without this, content is always
        # empty and all output goes into reasoning_content, exhausting
        # max_tokens before any real answer is written.
        "chat_template_kwargs": {"enable_thinking": False},
    }
    if tools:
        payload["tools"] = tools
        payload["tool_choice"] = "auto"
    logger.info(
        "call_model url=%s model=%s prompt_turns=%d",
        MODEL_SERVER_URL,
        MODEL_NAME,
        len(messages),
    )
    async with httpx.AsyncClient(timeout=120.0) as client:
        resp = await client.post(
            f"{MODEL_SERVER_URL}/chat/completions",
            json=payload,
        )
        resp.raise_for_status()
        data = resp.json()

    usage = data.get("usage", {})
    logger.info(
        "call_model done prompt_tokens=%s completion_tokens=%s",
        usage.get("prompt_tokens", "?"),
        usage.get("completion_tokens", "?"),
    )
    return data


def extract_response(raw: dict) -> dict:
    """Parse model output into a typed result dict.

    Returns one of:
      {"type": "text",          "text": "..."}
      {"type": "call_function", "name": "...", "args": {...}}

    Checks for native OpenAI tool_calls first (Phase 1+), then falls back
    to the custom JSON format for models without function-calling support.
    """
    try:
        message = raw["choices"][0]["message"]
    except (KeyError, IndexError):
        return {"type": "text", "text": ""}

    # Native OpenAI function calling — preferred path
    tool_calls = message.get("tool_calls")
    if tool_calls:
        tc = tool_calls[0]
        fn = tc.get("function", {})
        try:
            args = json.loads(fn.get("arguments", "{}"))
        except json.JSONDecodeError:
            args = {}
        return {"type": "call_function", "name": fn.get("name", ""), "args": args}

    # Custom JSON fallback for models without native tool support.
    # Qwen3 puts thinking in reasoning_content; actual answer is in content.
    content: str = message.get("content") or ""
    stripped = content.strip()
    if stripped.startswith("{"):
        try:
            obj = json.loads(stripped)
            if "call" in obj and isinstance(obj["call"], str):
                return {
                    "type": "call_function",
                    "name": obj["call"],
                    "args": obj.get("args", {}),
                }
        except json.JSONDecodeError:
            pass

    return {"type": "text", "text": content}
