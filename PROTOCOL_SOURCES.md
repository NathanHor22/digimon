# Protocol sources

The embedded client was implemented against OpenAI's published tunnel protocol at commit **3e52fa0400c9126dbc92c6b764b4e712506927cf**:

- [Protocol](https://github.com/openai/tunnel-client/blob/3e52fa0400c9126dbc92c6b764b4e712506927cf/docs/protocol.md), SHA-256 `2bee030f8e7f2de6635a58404c2e2a893bbbf28fd9aefb90b1c52f3bb4dd90cc`.
- [OpenAPI](https://github.com/openai/tunnel-client/blob/3e52fa0400c9126dbc92c6b764b4e712506927cf/docs/openapi.json), SHA-256 `30221a0209250e5ad4e0231c7a93e6e987e208ace8368c2013e8f3fb2b737940`.

MCP protocol version is `2026-07-28`. The official source JSON schema used for six core fixture validations has SHA-256 `ef70b61f99b6d2e5e3b46863822eab08dff6a45bedc7a08914e0e5b133f40203`. The Events draft used during implementation has SHA-256 `4a3f75a1db48d99f7baa38f4a0129ec5876dcf81935f5ab7bf43715016491f59`. These were retrieved on 17 October 2026; they are not included as vendored implementation code.

Product behavior and setup references: [Secure MCP Tunnel](https://developers.openai.com/api/docs/guides/secure-mcp-tunnels), [MCP server](https://developers.openai.com/plugins/build/mcp-server), [MCP Events](https://developers.openai.com/plugins/build/mcp-events), [connecting ChatGPT](https://developers.openai.com/plugins/deploy/connect-chatgpt), [custom MCP setup](https://developers.openai.com/api/docs/guides/custom-mcp-server).

The display driver and original board pin definitions were reused from the user's local Quipus firmware. ESP-IDF and its linked third-party components retain their upstream licenses; consult the SDK's license files before redistributing the firmware.
