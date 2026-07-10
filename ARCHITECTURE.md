# Architecture of `turf-lsp`

The Turf Language Server (`turf-lsp`) is a C++ application designed to provide IDE features (like autocompletion, diagnostics, go-to-definition) to any LSP-compatible text editor.

## The JSON-RPC Layer

The Language Server Protocol is based on JSON-RPC 2.0. 
Our server communicates with the client (the editor) via `stdin` and `stdout`. 

1. **Input**: We continuously read from `stdin`. An LSP message consists of a header (e.g., `Content-Length: ...\r\n\r\n`) and a JSON payload.
2. **Parsing**: We parse the JSON payload using `nlohmann/json`.
3. **Dispatching**: The `LSPHandler` inspects the `"method"` field of the request/notification and routes it to the appropriate function.
4. **Output**: When a response or server-to-client notification is required, we construct a JSON object, serialize it, prepend the `Content-Length` header, and flush it to `stdout`.

## Components

### 1. Main Event Loop (`main.cpp`)
Responsible for reading the raw bytes from standard input, extracting the `Content-Length`, and buffering the JSON payload until a complete message is formed. It then passes the string to the `LSPHandler`.

### 2. The Protocol Handler (`LSPHandler.hpp` / `LSPHandler.cpp`)
Parses the raw JSON string into a JSON object and dispatches it based on the `"method"` field. It keeps track of the server state (e.g., `initialize`, `initialized`, `shutdown`, `exit`).

### 3. The Document Store (To Be Implemented)
A component that keeps track of the open files (`textDocument/didOpen`, `textDocument/didChange`, `textDocument/didClose`). It synchronizes the text buffer with the editor.

### 4. Language Core Integration
This will link against the main `turflang` compiler libraries (parser, AST, semantic analyzer) to answer complex queries like `textDocument/hover` or `textDocument/definition`.

## Threading Model

Currently, the server is single-threaded and handles requests synchronously. For a production-ready server, a worker thread pool will be introduced to handle slow compiler queries without blocking the main event loop handling fast notifications (like typing events).
