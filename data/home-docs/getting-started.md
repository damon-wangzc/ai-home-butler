# AI Home Butler — Getting Started

AI Home Butler is your local AI assistant. It runs entirely on your own hardware with no cloud required.

## What Butler Can Do

Butler answers questions, manages your portfolio, and controls home devices.
You can talk to Butler using text via the /ask API or voice via the /audio WebSocket.

## How to Ask Questions

Send a POST request to http://localhost:8000/ask with JSON body:
{"user_id": "default", "text": "your question here"}

Butler will respond with a JSON object containing the response, cost estimate, and token usage.

## Voice Interface

Connect to the WebSocket at ws://localhost:8000/audio.
Send an optional JSON config frame first: {"user_id": "default"}
Then send binary WAV audio frames. Butler will reply with WAV audio.

## Portfolio Tools

Butler knows about your investment portfolio. Ask questions like:
- "How is our AAPL position?"
- "What is the current price of MSFT?"
- "Show me my holdings"

## Home Knowledge

Butler can answer questions about your home using this knowledge base.
Add .txt or .md files to the data/home-docs/ folder to teach Butler about your home.

## Available Voice Languages

- English US (Lessac) — default
- English US (Amy)
- Swedish (NST)
- Chinese Simplified (Xiao Ya)

Change the voice by setting VOICE_NAME in your .env file.
