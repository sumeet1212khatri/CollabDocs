"""
CollabDocs – FastAPI Backend
Real-time collaborative document editing with Operational Transformation.

Fixes over original:
- document.apply_op() is now awaited (async with lock) — race condition fixed
- broadcast_to_doc() removes dead connections atomically after fan-out
- WebSocket disconnect and error paths both cleanly remove presence
- lifespan uses async get_or_create
- Input validation on all incoming messages
- /api/docs POST returns consistent shape
"""

from __future__ import annotations

import asyncio
import json
import logging
import uuid
from contextlib import asynccontextmanager
from typing import Any, Dict

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

from document_store import store
from ot_engine import Operation
from presence import presence_manager

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger(__name__)


# ─── Broadcast helpers ────────────────────────────────────────────────────────

async def broadcast_to_doc(doc_id: str, message: dict, exclude_user: str | None = None):
    """Fan-out a message to all connections for a document.

    Uses a snapshot of connections so the dict can't be mutated mid-iteration.
    Dead connections are removed after the fan-out pass.
    """
    connections = presence_manager.get_connections(doc_id)  # snapshot copy
    dead: list[str] = []

    send_tasks = []
    recipients = []
    for user_id, ws in connections.items():
        if user_id == exclude_user:
            continue
        send_tasks.append(ws.send_json(message))
        recipients.append(user_id)

    results = await asyncio.gather(*send_tasks, return_exceptions=True)

    for user_id, result in zip(recipients, results):
        if isinstance(result, Exception):
            logger.warning(f"Dead connection for {user_id}, removing")
            dead.append(user_id)

    for uid in dead:
        presence_manager.leave(doc_id, uid)


async def send_to_user(doc_id: str, user_id: str, message: dict):
    user = presence_manager.get_user(doc_id, user_id)
    if user and user.websocket:
        try:
            await user.websocket.send_json(message)
        except Exception as e:
            logger.warning(f"Failed to send to {user_id}: {e}")


# ─── Message handlers ─────────────────────────────────────────────────────────

async def handle_operation(doc_id: str, user_id: str, data: dict):
    """Process an incoming OT operation: validate → lock+apply → ack → broadcast."""
    # Validate required fields
    op_type = data.get("op_type")
    if op_type not in ("insert", "delete"):
        logger.warning(f"Invalid op_type from {user_id}: {op_type!r}")
        return

    position = data.get("position")
    if not isinstance(position, int) or position < 0:
        logger.warning(f"Invalid position from {user_id}: {position!r}")
        return

    base_version = data.get("base_version")
    if not isinstance(base_version, int):
        logger.warning(f"Invalid base_version from {user_id}: {base_version!r}")
        return

    doc = await store.get_or_create(doc_id)

    op = Operation(
        op_type=op_type,
        position=position,
        value=data.get("value", ""),
        length=data.get("length", len(data.get("value", "")) or 1),
        base_version=base_version,
        user_id=user_id,
        op_id=data.get("op_id") or str(uuid.uuid4()),
    )

    # apply_op is async and acquires the per-doc lock internally
    transformed_op = await doc.apply_op(op)

    # Ack to sender
    await send_to_user(doc_id, user_id, {
        "type": "ack",
        "op_id": op.op_id,
        "server_version": doc.version,
    })

    # Broadcast transformed op to everyone else
    await broadcast_to_doc(doc_id, {
        "type": "operation",
        "op_type": transformed_op.op_type,
        "position": transformed_op.position,
        "value": transformed_op.value,
        "length": transformed_op.length,
        "server_version": doc.version,
        "user_id": user_id,
        "op_id": transformed_op.op_id,
    }, exclude_user=user_id)


async def handle_cursor(doc_id: str, user_id: str, data: dict):
    cursor_pos = data.get("cursor_pos", 0)
    if not isinstance(cursor_pos, int):
        return

    presence_manager.update_cursor(
        doc_id,
        user_id,
        cursor_pos=cursor_pos,
        sel_start=data.get("selection_start", -1),
        sel_end=data.get("selection_end", -1),
    )

    user = presence_manager.get_user(doc_id, user_id)
    if not user:
        return

    await broadcast_to_doc(doc_id, {
        "type": "cursor",
        "user_id": user_id,
        "name": user.name,
        "color": user.color,
        "cursor_pos": cursor_pos,
        "selection_start": data.get("selection_start", -1),
        "selection_end": data.get("selection_end", -1),
    }, exclude_user=user_id)


async def handle_title_change(doc_id: str, user_id: str, data: dict):
    title = str(data.get("title", "Untitled Document"))[:200]
    store.update_title(doc_id, title)
    await broadcast_to_doc(doc_id, {
        "type": "title_change",
        "title": title,
        "user_id": user_id,
    }, exclude_user=user_id)


async def handle_ping(doc_id: str, user_id: str):
    user = presence_manager.get_user(doc_id, user_id)
    if user:
        user.ping()


# ─── App lifecycle ────────────────────────────────────────────────────────────

@asynccontextmanager
async def lifespan(app: FastAPI):
    doc = await store.get_or_create("welcome")
    if not doc.content:
        doc.title = "Welcome to CollabDocs"
        doc.content = (
            "Welcome to CollabDocs!\n\n"
            "This is a real-time collaborative document editor.\n"
            "Share the URL with anyone — they can edit simultaneously.\n\n"
            "✦ Changes appear instantly for all collaborators\n"
            "✦ Each user gets a unique color and cursor\n"
            "✦ Conflicts resolved with Operational Transformation (OT)\n\n"
            "Start typing to collaborate…"
        )
        doc.version = 1
    yield


app = FastAPI(title="CollabDocs", lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


# ─── WebSocket ────────────────────────────────────────────────────────────────

@app.websocket("/ws/{doc_id}")
async def websocket_endpoint(websocket: WebSocket, doc_id: str):
    await websocket.accept()

    user_id = websocket.query_params.get("user_id") or str(uuid.uuid4())
    user = presence_manager.join(doc_id, user_id, websocket)
    doc = await store.get_or_create(doc_id)

    logger.info(f"[{doc_id}] {user.name} ({user_id}) connected")

    # Send initial state
    await websocket.send_json({
        "type": "init",
        "user_id": user_id,
        "name": user.name,
        "color": user.color,
        "doc_state": doc.get_state(),
        "users": presence_manager.get_users(doc_id),
    })

    # Announce join
    await broadcast_to_doc(doc_id, {
        "type": "user_joined",
        "user_id": user_id,
        "name": user.name,
        "color": user.color,
        "users": presence_manager.get_users(doc_id),
    }, exclude_user=user_id)

    try:
        while True:
            raw = await websocket.receive_text()
            try:
                data = json.loads(raw)
            except json.JSONDecodeError:
                continue

            msg_type = data.get("type")

            if msg_type == "operation":
                await handle_operation(doc_id, user_id, data)
            elif msg_type == "cursor":
                await handle_cursor(doc_id, user_id, data)
            elif msg_type == "title_change":
                await handle_title_change(doc_id, user_id, data)
            elif msg_type == "ping":
                await handle_ping(doc_id, user_id)

    except WebSocketDisconnect:
        logger.info(f"[{doc_id}] {user.name} ({user_id}) disconnected")
    except Exception as e:
        logger.error(f"[{doc_id}] WebSocket error for {user_id}: {e}", exc_info=True)
    finally:
        # Always clean up — covers both normal disconnect and exceptions
        presence_manager.leave(doc_id, user_id)
        await broadcast_to_doc(doc_id, {
            "type": "user_left",
            "user_id": user_id,
            "name": user.name,
            "users": presence_manager.get_users(doc_id),
        })


# ─── REST API ─────────────────────────────────────────────────────────────────

@app.get("/api/docs")
async def list_documents():
    return store.list_docs()


@app.get("/api/docs/{doc_id}")
async def get_document(doc_id: str):
    doc = store.get(doc_id)
    if not doc:
        raise HTTPException(status_code=404, detail="Document not found")
    return doc.get_state()


@app.post("/api/docs", status_code=201)
async def create_document():
    doc_id = str(uuid.uuid4())[:8]
    await store.get_or_create(doc_id)
    return {"doc_id": doc_id, "url": f"/?doc={doc_id}"}


@app.get("/health")
async def health():
    return {
        "status": "ok",
        "docs": len(store._docs),
        "active_users": sum(
            len(presence_manager.get_connections(doc_id))
            for doc_id in store._docs
        ),
    }


# ─── Static files ─────────────────────────────────────────────────────────────

@app.get("/")
async def serve_index():
    return FileResponse("index.html")
