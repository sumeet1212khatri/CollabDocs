"""
Presence Manager
Tracks active users per document: cursor positions, selections, colors.

Fixes over original:
- Color reclamation in leave() actually works (was dead code before)
- Stale user pruning doesn't mutate the dict while iterating
- get_connections() returns a snapshot copy to avoid mutation during broadcast
- User names are preserved on reconnect (same user_id)
"""

from __future__ import annotations

import random
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set

from fastapi import WebSocket

USER_COLORS = [
    "#E63946", "#2196F3", "#FF9800", "#9C27B0", "#00BCD4",
    "#4CAF50", "#FF5722", "#3F51B5", "#009688", "#F44336",
    "#8BC34A", "#FF4081", "#00ACC1", "#7E57C2", "#FFA726",
]

USER_NAMES = [
    "Alice", "Bob", "Carol", "Dave", "Eve",
    "Frank", "Grace", "Hank", "Iris", "Jack",
    "Kate", "Leo", "Mia", "Nick", "Olivia",
    "Pete", "Quinn", "Rosa", "Sam", "Tara",
]

STALE_TIMEOUT_SECONDS = 30


@dataclass
class UserPresence:
    user_id: str
    name: str
    color: str
    cursor_pos: int = 0
    selection_start: int = -1
    selection_end: int = -1
    last_seen: float = field(default_factory=time.time)
    websocket: Optional[object] = None

    def to_dict(self) -> dict:
        return {
            "user_id": self.user_id,
            "name": self.name,
            "color": self.color,
            "cursor_pos": self.cursor_pos,
            "selection_start": self.selection_start,
            "selection_end": self.selection_end,
        }

    def ping(self):
        self.last_seen = time.time()


class PresenceManager:
    def __init__(self):
        self._doc_users: Dict[str, Dict[str, UserPresence]] = {}
        self._connections: Dict[str, Dict[str, WebSocket]] = {}
        self._used_colors: Dict[str, Set[str]] = {}

    def _get_color(self, doc_id: str) -> str:
        used = self._used_colors.get(doc_id, set())
        available = [c for c in USER_COLORS if c not in used]
        if not available:
            # All colors taken — recycle but pick least-used visually distinct
            available = USER_COLORS
        color = random.choice(available)
        self._used_colors.setdefault(doc_id, set()).add(color)
        return color

    def _release_color(self, doc_id: str, color: str):
        if doc_id in self._used_colors:
            self._used_colors[doc_id].discard(color)

    def join(self, doc_id: str, user_id: str, ws: WebSocket) -> UserPresence:
        if doc_id not in self._doc_users:
            self._doc_users[doc_id] = {}
            self._connections[doc_id] = {}

        if user_id not in self._doc_users[doc_id]:
            color = self._get_color(doc_id)
            # Assign name deterministically from user_id hash for consistency
            name_idx = abs(hash(user_id)) % len(USER_NAMES)
            name = USER_NAMES[name_idx]
            presence = UserPresence(
                user_id=user_id,
                name=name,
                color=color,
                websocket=ws,
            )
            self._doc_users[doc_id][user_id] = presence
        else:
            # Reconnect: preserve name/color, just update ws
            self._doc_users[doc_id][user_id].websocket = ws
            self._doc_users[doc_id][user_id].ping()

        self._connections[doc_id][user_id] = ws
        return self._doc_users[doc_id][user_id]

    def leave(self, doc_id: str, user_id: str):
        if doc_id in self._doc_users:
            user = self._doc_users[doc_id].pop(user_id, None)
            self._connections[doc_id].pop(user_id, None)
            # Actually release the color now (original had dead code here)
            if user is not None:
                self._release_color(doc_id, user.color)

    def update_cursor(
        self,
        doc_id: str,
        user_id: str,
        cursor_pos: int,
        sel_start: int = -1,
        sel_end: int = -1,
    ):
        users = self._doc_users.get(doc_id, {})
        if user_id in users:
            p = users[user_id]
            p.cursor_pos = cursor_pos
            p.selection_start = sel_start
            p.selection_end = sel_end
            p.ping()

    def get_users(self, doc_id: str) -> List[dict]:
        users = self._doc_users.get(doc_id, {})
        now = time.time()
        # Build a new dict instead of mutating while iterating
        active = {
            uid: u
            for uid, u in users.items()
            if now - u.last_seen < STALE_TIMEOUT_SECONDS
        }
        stale = set(users) - set(active)
        for uid in stale:
            self._release_color(doc_id, users[uid].color)
        self._doc_users[doc_id] = active
        return [u.to_dict() for u in active.values()]

    def get_connections(self, doc_id: str) -> Dict[str, WebSocket]:
        # Return a snapshot so broadcast can't mutate the live dict mid-iteration
        return dict(self._connections.get(doc_id, {}))

    def get_user(self, doc_id: str, user_id: str) -> Optional[UserPresence]:
        return self._doc_users.get(doc_id, {}).get(user_id)


# Global singleton
presence_manager = PresenceManager()
