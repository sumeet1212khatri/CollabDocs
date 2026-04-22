"""
Presence Manager
Tracks active users per document: cursor positions, selections, colors.
"""

import time
import random
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set
from fastapi import WebSocket

# Distinct user colors
USER_COLORS = [
    "#FF6B6B", "#4ECDC4", "#45B7D1", "#96CEB4", "#FFEAA7",
    "#DDA0DD", "#98D8C8", "#F7DC6F", "#BB8FCE", "#85C1E9",
    "#F8C471", "#82E0AA", "#F1948A", "#AED6F1", "#A9DFBF",
]

USER_NAMES = [
    "Alice", "Bob", "Carol", "Dave", "Eve",
    "Frank", "Grace", "Hank", "Iris", "Jack",
    "Kate", "Leo", "Mia", "Nick", "Olivia",
]


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
        # doc_id -> {user_id -> UserPresence}
        self._doc_users: Dict[str, Dict[str, UserPresence]] = {}
        # doc_id -> {user_id -> WebSocket}
        self._connections: Dict[str, Dict[str, WebSocket]] = {}
        self._used_colors: Dict[str, Set[str]] = {}

    def _get_color(self, doc_id: str) -> str:
        used = self._used_colors.get(doc_id, set())
        available = [c for c in USER_COLORS if c not in used]
        if not available:
            available = USER_COLORS
        color = random.choice(available)
        self._used_colors.setdefault(doc_id, set()).add(color)
        return color

    def join(self, doc_id: str, user_id: str, ws: WebSocket) -> UserPresence:
        if doc_id not in self._doc_users:
            self._doc_users[doc_id] = {}
            self._connections[doc_id] = {}

        if user_id not in self._doc_users[doc_id]:
            color = self._get_color(doc_id)
            name = random.choice(USER_NAMES)
            presence = UserPresence(
                user_id=user_id,
                name=name,
                color=color,
                websocket=ws,
            )
            self._doc_users[doc_id][user_id] = presence
        else:
            self._doc_users[doc_id][user_id].websocket = ws
            self._doc_users[doc_id][user_id].ping()

        self._connections[doc_id][user_id] = ws
        return self._doc_users[doc_id][user_id]

    def leave(self, doc_id: str, user_id: str):
        if doc_id in self._doc_users:
            self._doc_users[doc_id].pop(user_id, None)
            self._connections[doc_id].pop(user_id, None)
            color = None
            if color and doc_id in self._used_colors:
                self._used_colors[doc_id].discard(color)

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
        # Prune stale users (> 30s no ping)
        now = time.time()
        active = {
            uid: u for uid, u in users.items()
            if now - u.last_seen < 30
        }
        self._doc_users[doc_id] = active
        return [u.to_dict() for u in active.values()]

    def get_connections(self, doc_id: str) -> Dict[str, WebSocket]:
        return self._connections.get(doc_id, {})

    def get_user(self, doc_id: str, user_id: str) -> Optional[UserPresence]:
        return self._doc_users.get(doc_id, {}).get(user_id)


# Global singleton
presence_manager = PresenceManager()
