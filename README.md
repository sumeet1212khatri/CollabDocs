# CollabDocs

Lightweight real-time collaborative text editor and server implemented in modern C++ (C++17) with a self-contained single-file frontend. CollabDocs provides an operational-transformation (OT) based backend, presence/cursor sync, and a small browser client (index.html) to demonstrate real-time collaboration across WebSocket connections.

Highlights
- Pure C++ server using Boost.Asio / Boost.Beast for HTTP + WebSocket.
- OT engine implementing insert/delete transforms and cursor sync (ot_engine.hpp).
- Per-document store with batched broadcasts to reduce message overhead (document_store.hpp).
- Presence manager for user names, colors, and cursor positions (presence.hpp).
- Single-file HTML/JS frontend (index.html) that implements the client-side OT logic and UI.
- Multi-stage Dockerfile for easy containerized builds and runtime.

Stack
- Language: C++ (primary), HTML/JS (frontend)
- Build system: CMake (requires >= 3.16)
- Runtime libraries: Boost (system, coroutine, context), OpenSSL, nlohmann_json
- Notable libraries: Boost.Asio/Beast, nlohmann/json

Repository layout (top-level)

```
CMakeLists.txt        # CMake project + build rules (creates `collabdocs` and `ot_test`)
Dockerfile            # Multi-stage image: build + minimal runtime
docker-compose.yml    # Example compose (small)
index.html            # Single-file frontend client (UI + OT client)
main.cpp              # HTTP + WebSocket server, session management
ot_engine.hpp         # OT primitives, transforms, apply, cursor transform
document_store.hpp    # Document model, op log, batching and snapshots
presence.hpp          # Presence manager (user names/colors/cursors)
Ot_test.cpp           # OT stress / unit test executable
LICENSE               # Project license
README.md             # (this file)
```

What the system provides
- HTTP endpoints:
  - GET / or /index.html → serves the bundled frontend
  - GET /health → { status, docs, conns }
  - GET /api/docs → list documents (JSON)
  - GET /api/docs/<doc_id> → document state (title, content, version)
  - POST /api/docs → creates a new document and returns { doc_id, url }
- WebSocket endpoint:
  - ws://<host>/ws/<doc_id>?user_id=<id>
  - Message types used by client/server: init, operation, batch, ack, pong, cursor, user_joined, user_left, title_change
- Built-in "welcome" document seeded on startup (doc id: `welcome`, content: "Start typing…").

Prerequisites
- Linux (or compatible) development environment
- CMake >= 3.16
- A C++17-capable compiler (g++ / clang++)
- Boost >= 1.74 with components: system, coroutine, context
- libssl (OpenSSL)
- nlohmann-json (or the distro package)
If you want to use Docker, Docker engine is required.

Build and run (local)
1. Clone and build with CMake:
   ```bash
   git clone https://github.com/sumeet1212khatri/CollabDocs.git
   cd CollabDocs
   mkdir build && cd build
   cmake -DCMAKE_BUILD_TYPE=Release ..
   cmake --build . -j$(nproc)
   ```
   This produces two binaries:
   - `collabdocs` — the server
   - `ot_test` — OT stress/test executable

2. Run the server:
   ```bash
   # default port is 7860; override with PORT environment variable
   PORT=7860 ./collabdocs
   ```
   Open http://localhost:7860 in a browser to load the demo frontend.

Build and run (Docker)
- Build image (multi-stage builder included):
  ```bash
  docker build -t collabdocs:latest .
  ```
- Run container:
  ```bash
  docker run --rm -p 7860:7860 collabdocs:latest
  ```
- Or with docker-compose:
  ```bash
  docker-compose up --build
  ```

Notes on the Dockerfile
- The Dockerfile uses a builder stage (Ubuntu 22.04) to compile the C++ binaries and a smaller runtime stage that installs the runtime Boost/OpenSSL libraries. The runtime binary is copied to `/app/app_server` and the frontend `index.html` is served alongside it. The server runs as a non-root user in the image and exposes port `7860`.

API examples
- Create a new document:
  ```bash
  curl -X POST http://localhost:7860/api/docs
  # -> {"doc_id":"<id>","url":"/?doc=<id>"}
  ```
- List documents:
  ```bash
  curl http://localhost:7860/api/docs
  ```
- Get a document state:
  ```bash
  curl http://localhost:7860/api/docs/<doc_id>
  ```

WebSocket client (quick)
- Connect a browser client (the included index.html) or a custom client to:
  ```
  ws://<host>:<port>/ws/<doc_id>?user_id=<your_id>
  ```
- The client expects the server to send an `init` message with doc state and user list. Operations are exchanged as `operation` messages; the server batches outgoing operations into `batch` messages when appropriate.

Development notes
- The OT logic is in `ot_engine.hpp` and implements pairwise transforms and transform-against-log. The server's `Document` class applies operations while transforming them correctly against the stored operation log.
- `document_store.hpp` contains the per-document op log, snapshots, and a batched broadcast mechanism to reduce outgoing message volume.
- Presence and cursor handling live in `presence.hpp` and are decoupled by using send callbacks (so presence manager does not directly depend on Boost.Beast types).
- The main server (`main.cpp`) uses strand-serialized write queues to avoid locking and posts deferred broadcast callbacks after releasing document locks to avoid deadlocks.

Running the OT test
- After building:
  ```bash
  ./ot_test
  ```
  (This binary is provided as a stress / validation harness for the OT implementation.)

Contributing
- Bug reports and pull requests are welcome. For code changes:
  - Follow the same code style: modern C++ (RAII, std containers), small header-only engine components.
  - Add tests where appropriate (OT transforms are sensitive — tests are highly appreciated).

License
- See the LICENSE file in the repository root.

Maintainer
- Repository owner: @sumeet1212khatri

If you want, I can:
- Add a short Quick Start section to the README with screenshots and example sessions.
- Produce a development checklist (setup, common debug commands, how to reproduce race conditions).
- Draft CI actions to build the project and run ot_test on each PR.
