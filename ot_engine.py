"""
Operational Transformation Engine
Implements insert-insert, insert-delete, delete-insert, delete-delete,
delete-range-insert, and insert-inside-delete-range transformations.
Based on the architecture from the Google Docs system design.
"""

from dataclasses import dataclass, field
from typing import Literal, Optional
import copy


@dataclass
class Operation:
    op_type: Literal["insert", "delete"]
    position: int
    value: str = ""          # For insert
    length: int = 1          # For delete (range support)
    base_version: int = 0
    user_id: str = ""
    op_id: str = ""


@dataclass
class ServerState:
    version: int = 0
    content: str = ""
    # Operation log: list of (version, Operation)
    op_log: list = field(default_factory=list)


def transform_insert_insert(incoming: Operation, applied: Operation) -> Operation:
    """
    Two concurrent inserts. If applied insert position <= incoming position,
    shift incoming position right by length of applied insert.
    Tie-break by user_id to ensure consistency.
    """
    result = copy.copy(incoming)
    app_len = len(applied.value)

    if applied.position < incoming.position:
        result.position += app_len
    elif applied.position == incoming.position:
        # Tie-break: lexicographically larger user_id goes later
        if applied.user_id <= incoming.user_id:
            result.position += app_len
    return result


def transform_delete_insert(incoming: Operation, applied: Operation) -> Operation:
    """
    incoming = delete, applied = insert (already on server).
    Insert before delete pos: shift delete right.
    Insert inside/after: no change.
    """
    result = copy.copy(incoming)
    app_len = len(applied.value)

    if applied.position <= incoming.position:
        result.position += app_len
    elif applied.position < incoming.position + incoming.length:
        # Insert is inside the delete range — expand delete range
        result.length += app_len
    return result


def transform_insert_delete(incoming: Operation, applied: Operation) -> Operation:
    """
    incoming = insert, applied = delete (already on server).
    Delete before insert pos: shift insert left by deleted length.
    Delete range overlaps insert pos: push insert to start of deleted range.
    """
    result = copy.copy(incoming)

    del_end = applied.position + applied.length

    if del_end <= incoming.position:
        # Delete entirely before insert — shift left
        result.position -= applied.length
    elif applied.position <= incoming.position < del_end:
        # Insert falls inside deleted region — push to start
        result.position = applied.position
    # If delete is entirely after insert pos, no change
    return result


def transform_delete_delete(incoming: Operation, applied: Operation) -> Operation:
    """
    Two concurrent deletes.
    """
    result = copy.copy(incoming)

    app_end = applied.position + applied.length
    inc_end = incoming.position + incoming.length

    if app_end <= incoming.position:
        # Applied delete entirely before incoming delete — shift left
        result.position -= applied.length

    elif applied.position >= inc_end:
        # Applied delete entirely after incoming delete — no change
        pass

    else:
        # Overlapping deletes — trim incoming to remove already-deleted chars
        overlap_start = max(applied.position, incoming.position)
        overlap_end = min(app_end, inc_end)
        overlap = overlap_end - overlap_start

        # Shift position if applied starts before incoming
        if applied.position < incoming.position:
            result.position = applied.position
        
        result.length = max(0, result.length - overlap)

    return result


def transform_operation(incoming: Operation, applied: Operation) -> Operation:
    """
    Transform incoming operation against an already-applied operation.
    """
    if incoming.op_type == "insert" and applied.op_type == "insert":
        return transform_insert_insert(incoming, applied)
    elif incoming.op_type == "delete" and applied.op_type == "insert":
        return transform_delete_insert(incoming, applied)
    elif incoming.op_type == "insert" and applied.op_type == "delete":
        return transform_insert_delete(incoming, applied)
    elif incoming.op_type == "delete" and applied.op_type == "delete":
        return transform_delete_delete(incoming, applied)
    return incoming


def transform_against_log(
    incoming: Operation,
    op_log: list,
    from_version: int,
    to_version: int,
) -> Operation:
    """
    Transform incoming against all operations from from_version to to_version.
    op_log is list of (version, Operation) tuples.
    """
    result = copy.copy(incoming)
    for ver, applied_op in op_log:
        if from_version < ver <= to_version:
            result = transform_operation(result, applied_op)
    return result


def apply_operation(content: str, op: Operation) -> str:
    """
    Apply a single operation to the document content string.
    Returns new content string.
    """
    if op.op_type == "insert":
        pos = max(0, min(op.position, len(content)))
        return content[:pos] + op.value + content[pos:]
    elif op.op_type == "delete":
        pos = max(0, min(op.position, len(content)))
        end = max(0, min(pos + op.length, len(content)))
        return content[:pos] + content[end:]
    return content


def transform_cursor(cursor_pos: int, op: Operation) -> int:
    """
    Adjust a cursor position based on an operation.
    Same logic as OT but for cursor position.
    """
    if op.op_type == "insert":
        ins_len = len(op.value)
        if op.position <= cursor_pos:
            return cursor_pos + ins_len
    elif op.op_type == "delete":
        del_end = op.position + op.length
        if del_end <= cursor_pos:
            return cursor_pos - op.length
        elif op.position <= cursor_pos < del_end:
            return op.position
    return cursor_pos
