---
title: CollabDocs
emoji: 📝
colorFrom: green
colorTo: blue      
sdk: docker
pinned: false
app_port: 7860
---

# CollabDocs

> Collaborative document tool combining a C++ backend with an HTML frontend.  
> Purpose-built for low-latency editing and content sync across peers.

Badges: [Build Status] [License] [Language: C++, HTML]
Keywords: collaborative editing, real-time sync, C++, CMake, Docker, low-latency

Table of contents
- [Overview](#overview)
- [Why this project](#why-this-project)
- [Tech stack](#tech-stack)
- [Architecture & design](#architecture--design)
- [Build & run (local)](#build--run-local)
- [Run with Docker](#run-with-docker)
- [Testing](#testing)
- [Project structure](#project-structure)
- [Design decisions & trade-offs](#design-decisions--trade-offs)
- [Performance characteristics](#performance-characteristics)
- [Security & privacy considerations](#security--privacy-considerations)
- [Contributing](#contributing)
- [Roadmap & TODOs](#roadmap--todos)
- [License & contact](#license--contact)

Overview
--------
CollabDocs is a collaborative document editor that demonstrates a compact C++ core (sync/merge engine and server) with an HTML/JS frontend. The repository uses CMake for builds and includes a Dockerfile for containerized runs. The primary interface listens on port 7860 by default.

Why this project
----------------
- Shows system-level programming (C++) and web integration (HTML).
- Exercises algorithms for concurrent editing and conflict resolution.
- Suitable as a portfolio project to demonstrate engineering fundamentals: architecture, testing, performance evaluation, and deployment.

Tech stack
----------
- Languages: C++ (core logic and server), HTML (frontend)
- Build: CMake
- Container: Docker
- Port: 7860 (default)
- Other: (add any frameworks/libraries used, e.g., Boost, WebSocket lib)

Architecture & design
---------------------
High-level components:
- Sync engine (C++): core CRDT/OT-like data structures and merge logic.
- Networking server (C++): HTTP/WebSocket endpoints for clients.
- Frontend (HTML/JS): UI, editor, and synchronization client.

Key flows:
1. Client connects via WebSocket.
2. Local edits are batched and sent as operations.
3. Server merges operations using deterministic merge rules, returns acknowledgements/deltas.
4. Server persists snapshots and broadcasts diffs to connected peers.

Design points for reviewers:
- Data structures used for document model (rope/gap-buffer, CRDT variant).
- Complexity of common operations (insert/delete/search).
- Failure modes and recovery: snapshotting, operation logs, client reconnection strategy.

Build & run (local)
-------------------
Prerequisites:
- CMake >= 3.16
- A modern C++ compiler (g++/clang) with C++17 support
- Docker (optional)

Typical local build:
```bash
# create build directory
mkdir -p build && cd build
cmake ..
cmake --build . -- -j$(nproc)
# binary expected at build/bin/collabdocs-server (adjust if different)
```

Run server (local):
```bash
# in repo root or build/bin location
./build/bin/collabdocs-server --port 7860
# then open the frontend: http://localhost:7860
```

Run with Docker
---------------
Assuming a Dockerfile exists at project root:

Build:
```bash
docker build -t collabdocs:latest .
```

Run:
```bash
docker run --rm -p 7860:7860 collabdocs:latest
```

Testing
-------
- Unit tests: run via CTest (if configured)
```bash
cd build
ctest --output-on-failure
```
- Linting and static analysis: run clang-tidy / cppcheck rules (add CI config)
- End-to-end: a simple script to start server, open a headless browser, and simulate concurrent edits.

Project structure (high level)
------------------------------
- /src               — C++ source (server, sync engine)
- /include           — C++ headers (public APIs)
- /web or /static    — HTML/CSS/JS frontend
- /cmake or CMakeLists.txt — build config
- /Dockerfile
- /tests             — unit & integration tests
- /docs              — design docs, protocol spec, API reference

(If you want I can replace these with exact file names from the repo.)

Design decisions & trade-offs
----------------------------
- CRDT vs OT: choose CRDT for simpler conflict-free semantics at the cost of larger operation metadata.
- In-memory vs durable-first: server keeps recent state in memory with periodic snapshots for performance and fast merging; snapshot frequency trades off durability vs throughput.
- Batch size and network frequency: batching reduces traffic but increases visible latency; configurable via server flags.

Performance characteristics
---------------------------
- Common operation complexities:
  - Insert/delete at position: O(log n) (if using a balanced tree/rope) or O(1) amortized (gap-buffer local).
  - Merge cost: linear in the number of concurrent operations to reconcile.
- Benchmarks (suggested):
  - Throughput: ops/sec with N concurrent clients (e.g., 50, 100).
  - Latency: median & 95th-percentile edit-to-ack time.
Add measurement scripts under /bench or CI.

Security & privacy considerations
-------------------------------
- Transport: use TLS for production (WSS/HTTPS).
- Authentication: add token-based auth for private documents.
- Input validation: server must sanitize frontend inputs to avoid injection in any persisted logs.
- Rate limiting & DoS protection: limit edits per client and apply quotas.

Contributing
------------
- Follow the project's C++ style (e.g., clang-format settings).
- Run unit tests and static analysis before opening a PR.
- Use topic branches and a clear PR description: what, why, perf impact, tests added.
- Code review checklist: correctness, O(n) complexity checks, memory/resource leak checks, thread-safety.

Roadmap & TODOs
---------------
- Add persistence layer (LevelDB/SQLite) for operation logs.
- Implement server-side auth and permission model.
- Add CRDT performance tuning and memory profiling.
- Improve frontend UX for large documents and merge conflicts.

How to evaluate (for SWE-1 reviewers)
-------------------------------------
- Read core sync algorithm (src/sync_engine.*) and comment on correctness and complexity.
- Run unit tests and end-to-end tests; measure ops/sec under simulated load.
- Review memory safety and concurrency handling (locks/atomics/lock-free structures).
- Verify clear API boundary between server and frontend.

License & contact
-----------------
- License: [Choose a license — e.g., MIT] (add LICENSE file)
- Maintainer: sumeet1212khatri — open to questions and PRs.

Appendix: SEO summary for Google (single-paragraph)
---------------------------------------------------
CollabDocs is a C++-based collaborative document editor with an HTML frontend, designed for real-time low-latency editing and deterministic conflict resolution using CRDT-like techniques; the repository includes CMake build scripts and a Dockerfile for easy deployment on port 7860 — ideal for engineers evaluating real-time sync algorithms, performance benchmarking, and system-level design in C++.
