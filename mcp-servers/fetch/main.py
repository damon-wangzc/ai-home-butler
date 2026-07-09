"""Butler fetch-MCP server.

Exposes a single fetch_url tool via the MCP SSE transport so the orchestrator
can retrieve real-time web content (weather, public APIs, documentation).

Port: $MCP_PORT (default 8400)
SSE endpoint: http://fetch-mcp:8400/sse
"""

import os
import httpx
from mcp.server.fastmcp import FastMCP

_PORT = int(os.getenv("MCP_PORT", "8400"))
_MAX_BODY = int(os.getenv("FETCH_MAX_CHARS", "6000"))

mcp = FastMCP("butler-fetch")


@mcp.tool()
async def fetch_url(url: str, max_chars: int = _MAX_BODY) -> str:
    """Fetch the text body of any public URL.

    Use this to retrieve real-time information such as weather reports, news
    headlines, exchange rates from public APIs, or documentation pages.
    Returns up to max_chars characters of the response body.

    Args:
        url:       Full URL to fetch (must start with http:// or https://).
        max_chars: Maximum characters to return (default 6000).
    """
    if not url.startswith(("http://", "https://")):
        raise ValueError("url must start with http:// or https://")
    async with httpx.AsyncClient(
        follow_redirects=True, timeout=10.0
    ) as client:
        r = await client.get(url, headers={"User-Agent": "ButlerBot/1.0"})
        r.raise_for_status()
    return r.text[: max(1, min(max_chars, 32_000))]


if __name__ == "__main__":
    mcp.run(transport="sse", host="0.0.0.0", port=_PORT)
