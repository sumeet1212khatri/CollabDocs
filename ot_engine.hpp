#pragma once
/*
 * CollabDocs C++ — Operational Transformation Engine
 *
 * Implements:
 *   insert-insert, insert-delete, delete-insert, delete-delete
 *   with cursor sync and transform_against_log.
 *
 * All operations are VALUE types (copyable, no heap allocation).
 */

#include <string>
#include <vector>
#include <algorithm>
#include <stdexcept>

namespace collab {

// ─── Operation ───────────────────────────────────────────────────────────────

enum class OpType { INSERT, DELETE };

struct Operation {
    OpType      type;
    int         position   = 0;
    std::string value;          // meaningful for INSERT
    int         length     = 0; // meaningful for DELETE (insert: derived)
    int         base_version = 0;
    std::string user_id;
    std::string op_id;

    // Normalize: insert.length always == value.size()
    void normalize() {
        if (type == OpType::INSERT)
            length = static_cast<int>(value.size());
    }
};

// ─── Pairwise transform functions ────────────────────────────────────────────

// insert vs insert
inline Operation transform_ii(Operation op, const Operation& against) {
    if (against.position < op.position) {
        op.position += static_cast<int>(against.value.size());
    } else if (against.position == op.position) {
        // Tie-break: lower user_id wins (goes first)
        if (against.user_id <= op.user_id)
            op.position += static_cast<int>(against.value.size());
    }
    return op;
}

// delete vs insert (delete incoming, insert already applied)
// "delete wins" — absorbs chars inserted inside its range
inline Operation transform_di(Operation op, const Operation& against) {
    int ins_pos = against.position;
    int ins_len = static_cast<int>(against.value.size());
    int del_end = op.position + op.length;

    if (ins_pos < op.position) {
        op.position += ins_len;
    } else if (ins_pos <= del_end) {
        op.length += ins_len;
    }
    return op;
}

// insert vs delete (insert incoming, delete already applied)
// if insert fell inside deleted range → collapse to del_start
inline Operation transform_id(Operation op, const Operation& against) {
    int del_start = against.position;
    int del_end   = against.position + against.length;

    if (del_end <= op.position) {
        op.position -= against.length;
    } else if (del_start < op.position) {
        op.position = del_start;
    }
    return op;
}

// delete vs delete
inline Operation transform_dd(Operation op, const Operation& against) {
    int op_start = op.position;
    int op_end   = op.position + op.length;
    int ag_start = against.position;
    int ag_end   = against.position + against.length;

    if (ag_end <= op_start) {
        op.position -= against.length;
    } else if (ag_start >= op_end) {
        // no change
    } else {
        int overlap_start = std::max(op_start, ag_start);
        int overlap_end   = std::min(op_end,   ag_end);
        int overlap       = overlap_end - overlap_start;

        if (ag_start < op_start)
            op.position = ag_start;

        op.length = std::max(0, op.length - overlap);
    }
    return op;
}

// ─── Dispatch ────────────────────────────────────────────────────────────────

inline Operation transform_operation(Operation incoming, const Operation& applied) {
    if (incoming.type == OpType::INSERT) {
        if (applied.type == OpType::INSERT) return transform_ii(incoming, applied);
        else                               return transform_id(incoming, applied);
    } else {
        if (applied.type == OpType::INSERT) return transform_di(incoming, applied);
        else                                return transform_dd(incoming, applied);
    }
}

// Transform `incoming` (based at `from_version`) against log entries
// with version in range (from_version, to_version].
inline Operation transform_against_log(
    Operation incoming,
    const std::vector<std::pair<int, Operation>>& op_log,
    int from_version,
    int to_version)
{
    for (auto& [ver, applied_op] : op_log) {
        if (ver > from_version && ver <= to_version)
            incoming = transform_operation(incoming, applied_op);
    }
    return incoming;
}

// ─── Apply ───────────────────────────────────────────────────────────────────

inline std::string apply_operation(const std::string& content, const Operation& op) {
    int len = static_cast<int>(content.size());
    if (op.type == OpType::INSERT) {
        int pos = std::max(0, std::min(op.position, len));
        return content.substr(0, pos) + op.value + content.substr(pos);
    } else {
        if (op.length <= 0) return content;
        int pos = std::max(0, std::min(op.position, len));
        int end = std::max(0, std::min(pos + op.length, len));
        return content.substr(0, pos) + content.substr(end);
    }
}

// ─── Cursor sync ─────────────────────────────────────────────────────────────

inline int transform_cursor(int cursor, const Operation& op) {
    if (op.type == OpType::INSERT) {
        if (op.position <= cursor)
            cursor += static_cast<int>(op.value.size());
    } else {
        int del_end = op.position + op.length;
        if (del_end <= cursor)
            cursor -= op.length;
        else if (op.position <= cursor)
            cursor = op.position;
    }
    return cursor;
}

} // namespace collab
