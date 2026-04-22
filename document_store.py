"""
Document Store
Manages in-memory document state with snapshot support.
In production this would be backed by Redis + Postgres.
"""

import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple
from ot_engine import Operation, apply_operation, ServerState, transform_against_log

SNAPSHOT_EVERY_N_OPS = 50  # Take snapshot every 50 ops


@dataclass
class Snapshot:
    version: int
    content: str
    timestamp: float = field(default_factory=time.time)


@dataclass
class Document:
    doc_id: str
    title: str = "Untitled Document"
    content: str = ""
    version: int = 0
    op_log: List[Tuple[int, Operation]] = field(default_factory=list)
    snapshots: List[Snapshot] = field(default_factory=list)
    created_at: float = field(default_factory=time.time)
    updated_at: float = field(default_factory=time.time)

    def get_latest_snapshot(self) -> Optional[Snapshot]:
        if self.snapshots:
            return self.snapshots[-1]
        return None

    def maybe_take_snapshot(self):
        if self.version > 0 and self.version % SNAPSHOT_EVERY_N_OPS == 0:
            snap = Snapshot(version=self.version, content=self.content)
            self.snapshots.append(snap)

    def apply_op(self, op: Operation) -> Operation:
        """
        Apply an incoming operation with OT.
        Returns the transformed operation that was actually applied.
        """
        # If client is behind, transform against missed ops
        if op.base_version < self.version:
            op = transform_against_log(
                op,
                self.op_log,
                from_version=op.base_version,
                to_version=self.version,
            )

        # Apply to content
        self.content = apply_operation(self.content, op)
        self.version += 1
        op.base_version = self.version  # stamp with server version

        # Store in log
        self.op_log.append((self.version, op))

        # Keep op_log bounded (last 1000 ops)
        if len(self.op_log) > 1000:
            self.op_log = self.op_log[-1000:]

        self.updated_at = time.time()
        self.maybe_take_snapshot()

        return op

    def get_state(self) -> dict:
        return {
            "doc_id": self.doc_id,
            "title": self.title,
            "content": self.content,
            "version": self.version,
        }


class DocumentStore:
    def __init__(self):
        self._docs: Dict[str, Document] = {}

    def get_or_create(self, doc_id: str) -> Document:
        if doc_id not in self._docs:
            self._docs[doc_id] = Document(doc_id=doc_id)
        return self._docs[doc_id]

    def get(self, doc_id: str) -> Optional[Document]:
        return self._docs.get(doc_id)

    def update_title(self, doc_id: str, title: str):
        doc = self.get(doc_id)
        if doc:
            doc.title = title

    def list_docs(self) -> List[dict]:
        return [
            {
                "doc_id": d.doc_id,
                "title": d.title,
                "version": d.version,
                "updated_at": d.updated_at,
            }
            for d in self._docs.values()
        ]


# Global singleton
store = DocumentStore()
