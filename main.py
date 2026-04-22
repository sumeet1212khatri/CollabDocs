"""
Google Docs Clone - FastAPI Backend
Real-time collaborative document editing with Operational Transformation.

Architecture:
- WebSocket gateway for real-time bidirectional comms
- OT Engine for conflict resolution
- Presence manager for cursor/user tracking
- Document store for persistence (in-memory, swap for Redis+PG in prod)
"""

import asyncio
import json
import logging
import uuid
from contextlib import asynccontextmanager
from typing import Any, Dict

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, HTMLResponse
from fastapi.staticfiles import StaticFiles

from document_store import store
from ot_engine import Operation
from presence import presence_manager

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)


# ─── Broadcast helpers ────────────────────────────────────────────────────────

async def broadcast_to_doc(doc_id: str, message: dict, exclude_user: str = None):
    """Fan out a message to all connected clients for a document."""
    connections = presence_manager.get_connections(doc_id)
    dead = []
    for user_id, ws in connections.items():
        if user_id == exclude_user:
            continue
        try:
            await ws.send_json(message)
        except Exception:
            dead.append(user_id)
    for uid in dead:
        presence_manager.leave(doc_id, uid)


async def send_to_user(doc_id: str, user_id: str, message: dict):
    """Send a message to a specific user."""
    user = presence_manager.get_user(doc_id, user_id)
    if user and user.websocket:
        try:
            await user.websocket.send_json(message)
        except Exception:
            pass


# ─── Message handlers ─────────────────────────────────────────────────────────

async def handle_operation(doc_id: str, user_id: str, data: dict):
    """
    Process an incoming OT operation from a client.
    Transform → Apply → Broadcast.
    """
    doc = store.get_or_create(doc_id)

    op = Operation(
        op_type=data["op_type"],
        position=data["position"],
        value=data.get("value", ""),
        length=data.get("length", len(data.get("value", "")) or 1),
        base_version=data["base_version"],
        user_id=user_id,
        op_id=data.get("op_id", str(uuid.uuid4())),
    )

    # Apply with OT (thread-safety note: use asyncio.Lock in prod)
    transformed_op = doc.apply_op(op)

    # Ack to sender
    await send_to_user(doc_id, user_id, {
        "type": "ack",
        "op_id": op.op_id,
        "server_version": doc.version,
    })

    # Broadcast transformed op to all others
    broadcast_msg = {
        "type": "operation",
        "op_type": transformed_op.op_type,
        "position": transformed_op.position,
        "value": transformed_op.value,
        "length": transformed_op.length,
        "server_version": doc.version,
        "user_id": user_id,
        "op_id": transformed_op.op_id,
    }
    await broadcast_to_doc(doc_id, broadcast_msg, exclude_user=user_id)


async def handle_cursor(doc_id: str, user_id: str, data: dict):
    """Update and broadcast cursor position."""
    presence_manager.update_cursor(
        doc_id,
        user_id,
        cursor_pos=data.get("cursor_pos", 0),
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
        "cursor_pos": data.get("cursor_pos", 0),
        "selection_start": data.get("selection_start", -1),
        "selection_end": data.get("selection_end", -1),
    }, exclude_user=user_id)


async def handle_title_change(doc_id: str, user_id: str, data: dict):
    """Sync document title change."""
    title = data.get("title", "Untitled Document")[:200]
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


# ─── WebSocket endpoint ───────────────────────────────────────────────────────

@asynccontextmanager
async def lifespan(app: FastAPI):
    # Create a default welcome document
    doc = store.get_or_create("welcome")
    if not doc.content:
        doc.title = "Welcome Document"
        welcome_text = (
            "Welcome to CollabDocs!\n\n"
            "This is a real-time collaborative document editor.\n"
            "Share the URL with anyone — they can edit simultaneously.\n\n"
            "✦ Changes appear instantly for all users\n"
            "✦ Each user gets a unique color cursor\n"
            "✦ Conflicts are resolved via Operational Transformation (OT)\n\n"
            "Start typing to collaborate..."
        )
        from ot_engine import Operation, apply_operation
        doc.content = welcome_text
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


@app.websocket("/ws/{doc_id}")
async def websocket_endpoint(websocket: WebSocket, doc_id: str):
    await websocket.accept()

    # Generate or get user_id from query params
    user_id = websocket.query_params.get("user_id") or str(uuid.uuid4())

    # Register presence
    user = presence_manager.join(doc_id, user_id, websocket)
    doc = store.get_or_create(doc_id)

    logger.info(f"User {user.name} ({user_id}) joined doc {doc_id}")

    # Send initial state to connecting user
    await websocket.send_json({
        "type": "init",
        "user_id": user_id,
        "name": user.name,
        "color": user.color,
        "doc_state": doc.get_state(),
        "users": presence_manager.get_users(doc_id),
    })

    # Announce join to others
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
        logger.info(f"User {user.name} ({user_id}) left doc {doc_id}")
        presence_manager.leave(doc_id, user_id)

        await broadcast_to_doc(doc_id, {
            "type": "user_left",
            "user_id": user_id,
            "name": user.name,
            "users": presence_manager.get_users(doc_id),
        })

    except Exception as e:
        logger.error(f"WebSocket error for {user_id}: {e}")
        presence_manager.leave(doc_id, user_id)


# ─── REST endpoints ───────────────────────────────────────────────────────────

@app.get("/api/docs")
async def list_documents():
    return store.list_docs()


@app.get("/api/docs/{doc_id}")
async def get_document(doc_id: str):
    doc = store.get(doc_id)
    if not doc:
        raise HTTPException(status_code=404, detail="Document not found")
    return doc.get_state()


@app.post("/api/docs")
async def create_document():
    doc_id = str(uuid.uuid4())[:8]
    doc = store.get_or_create(doc_id)
    return {"doc_id": doc_id, "url": f"/?doc={doc_id}"}


@app.get("/health")
async def health():
    return {"status": "ok", "docs": len(store._docs)}


# ─── Serve frontend ───────────────────────────────────────────────────────────

@app.get("/")
async def serve_index():
    return FileResponse("index.html")


