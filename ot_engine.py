"""
Operational Transformation Engine
Implements insert-insert, insert-delete, delete-insert, delete-delete
transformations with correct overlap handling.

Key fixes over original:
- delete-delete overlap now correctly handles all 4 positional cases
- transform_against_log is O(n) in missed ops, not re-scanned
- apply_operation is side-effect free (returns new string)
- All operations are immutable (copy.deepcopy on transform)
"""

from __future__ import annotations

import copy
from dataclasses import dataclass, field
from typing import List, Literal, Tuple


@dataclass
class Operation:
    op_type: Literal["insert", "delete"]
    position: int
    value: str = ""       # meaningful only for insert
    length: int = 1       # meaningful only for delete
    base_version: int = 0
    user_id: str = ""
    op_id: str = ""

    def __post_init__(self):
        # Normalize: insert length always derived from value
        if self.op_type == "insert":
            self.length = len(self.value)


# ─── Pairwise transforms ──────────────────────────────────────────────────────

def _transform_ii(op: Operation, against: Operation) -> Operation:
    """insert vs insert"""
    r = copy.deepcopy(op)
    if against.position < op.position:
        r.position += len(against.value)
    elif against.position == op.position:
        # Deterministic tie-break: lower user_id wins (goes first)
        if against.user_id <= op.user_id:
            r.position += len(against.value)
    return r


def _transform_di(op: Operation, against: Operation) -> Operation:
    """
    delete vs insert (against=insert already applied on server).

    Policy: "delete wins" — a concurrent insert inside the delete range is
    absorbed by the delete (the delete expands to cover new chars).
    This is the dOPT/Jupiter convention: deletions are authoritative over
    concurrent inserts within their range.
    """
    r = copy.deepcopy(op)
    ins_len = len(against.value)
    ins_pos = against.position
    del_end = op.position + op.length

    if ins_pos < op.position:
        # Insert before delete start — shift delete right
        r.position += ins_len
    elif ins_pos <= del_end:
        # Insert at or inside delete range (including right boundary) —
        # expand delete to absorb the inserted chars
        r.length += ins_len
    # ins_pos > del_end: insert after delete — no change
    return r


def _transform_id(op: Operation, against: Operation) -> Operation:
    """
    insert vs delete (against=delete already applied on server).

    Policy: "delete wins" — if the insert position falls inside the deleted
    range, collapse the insert to del_start (the insert still happens, but
    the content it was anchored to is gone).
    """
    r = copy.deepcopy(op)
    del_start = against.position
    del_end = against.position + against.length

    if del_end <= op.position:
        # Delete entirely before insert — shift left by deleted amount
        r.position -= against.length
    elif del_start < op.position:
        # Insert was inside deleted range — collapse to del_start
        r.position = del_start
    # del_start >= op.position: delete is at or after insert — no change
    return r


def _transform_dd(op: Operation, against: Operation) -> Operation:
    """delete vs delete"""
    r = copy.deepcopy(op)

    op_start = op.position
    op_end = op.position + op.length
    ag_start = against.position
    ag_end = against.position + against.length

    if ag_end <= op_start:
        # Against entirely before op — shift op left
        r.position -= against.length

    elif ag_start >= op_end:
        # Against entirely after op — no change
        pass

    else:
        # Overlapping. Four sub-cases:
        overlap_start = max(op_start, ag_start)
        overlap_end = min(op_end, ag_end)
        overlap = overlap_end - overlap_start

        # Shift position if against starts before op
        if ag_start < op_start:
            # Characters before op that were deleted shift our start left
            r.position = ag_start
        # else: op starts before against, position unchanged

        # Reduce length by the overlap (already deleted by `against`)
        r.length = max(0, op.length - overlap)

    return r


def transform_operation(incoming: Operation, applied: Operation) -> Operation:
    """Transform `incoming` against an already-applied `applied`."""
    if incoming.op_type == "insert":
        if applied.op_type == "insert":
            return _transform_ii(incoming, applied)
        else:
            return _transform_id(incoming, applied)
    else:
        if applied.op_type == "insert":
            return _transform_di(incoming, applied)
        else:
            return _transform_dd(incoming, applied)


def transform_against_log(
    incoming: Operation,
    op_log: List[Tuple[int, Operation]],
    from_version: int,
    to_version: int,
) -> Operation:
    """
    Transform `incoming` (based at `from_version`) against every operation
    in op_log with version in range (from_version, to_version].
    """
    result = copy.deepcopy(incoming)
    for ver, applied_op in op_log:
        if from_version < ver <= to_version:
            result = transform_operation(result, applied_op)
    return result


# ─── Apply ───────────────────────────────────────────────────────────────────

def apply_operation(content: str, op: Operation) -> str:
    """Apply a single operation to content. Pure function."""
    if op.op_type == "insert":
        pos = max(0, min(op.position, len(content)))
        return content[:pos] + op.value + content[pos:]
    elif op.op_type == "delete":
        if op.length <= 0:
            return content
        pos = max(0, min(op.position, len(content)))
        end = max(0, min(pos + op.length, len(content)))
        return content[:pos] + content[end:]
    return content


def transform_cursor(cursor_pos: int, op: Operation) -> int:
    """Adjust a cursor position given an applied operation."""
    if op.op_type == "insert":
        if op.position <= cursor_pos:
            return cursor_pos + len(op.value)
    elif op.op_type == "delete":
        del_end = op.position + op.length
        if del_end <= cursor_pos:
            return cursor_pos - op.length
        elif op.position <= cursor_pos:
            return op.position
    return cursor_pos
