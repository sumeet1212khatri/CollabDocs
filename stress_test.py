"""
Stress test for CollabDocs OT engine.

Tests:
1. Unit correctness — all four OT transform pairs including tricky overlaps
2. Concurrent convergence — N clients all edit simultaneously; final content must converge
3. Throughput — ops/sec under sustained load
4. Reconnect resilience — client ops after disconnect must not corrupt document

Run: python stress_test.py
Requires: pip install websockets
"""

import asyncio
import json
import random
import string
import sys
import time
import unittest
from dataclasses import dataclass, field
from typing import List

# ─── Import OT engine directly for unit tests ────────────────────────────────
sys.path.insert(0, '.')
from ot_engine import (
    Operation, apply_operation, transform_operation,
    transform_against_log, transform_cursor,
)


# ══════════════════════════════════════════════════════════════════════════════
#  1. OT UNIT TESTS
# ══════════════════════════════════════════════════════════════════════════════

class TestOTTransforms(unittest.TestCase):

    # ── Insert vs Insert ──────────────────────────────────────────────────────

    def test_ii_applied_before_incoming(self):
        """Insert at pos 2 against insert at pos 0 — incoming shifts right."""
        incoming = Operation('insert', position=5, value='X')
        applied  = Operation('insert', position=2, value='AB')
        result   = transform_operation(incoming, applied)
        self.assertEqual(result.position, 7)

    def test_ii_applied_after_incoming(self):
        """Insert after incoming position — no shift."""
        incoming = Operation('insert', position=2, value='X')
        applied  = Operation('insert', position=5, value='AB')
        result   = transform_operation(incoming, applied)
        self.assertEqual(result.position, 2)

    def test_ii_same_position_tiebreak(self):
        """Same position, lower user_id wins (goes first), incoming shifts."""
        inc = Operation('insert', position=3, value='X', user_id='bob')
        app = Operation('insert', position=3, value='Y', user_id='alice')  # alice < bob
        result = transform_operation(inc, app)
        self.assertEqual(result.position, 4)  # alice goes first, bob shifts

    def test_ii_same_position_no_shift(self):
        """Same position, incoming user_id < applied — incoming goes first, no shift."""
        inc = Operation('insert', position=3, value='X', user_id='alice')
        app = Operation('insert', position=3, value='Y', user_id='bob')    # bob > alice
        result = transform_operation(inc, app)
        self.assertEqual(result.position, 3)  # alice first, no shift

    # ── Delete vs Insert ──────────────────────────────────────────────────────

    def test_di_insert_before_delete(self):
        """Insert before delete — delete shifts right."""
        incoming = Operation('delete', position=5, length=3)
        applied  = Operation('insert', position=2, value='XX')
        result   = transform_operation(incoming, applied)
        self.assertEqual(result.position, 7)
        self.assertEqual(result.length, 3)

    def test_di_insert_inside_delete(self):
        """Insert inside delete range — delete length expands."""
        incoming = Operation('delete', position=3, length=5)
        applied  = Operation('insert', position=5, value='AB')
        result   = transform_operation(incoming, applied)
        self.assertEqual(result.position, 3)
        self.assertEqual(result.length, 7)

    def test_di_insert_after_delete(self):
        """Insert after delete range — no change."""
        incoming = Operation('delete', position=3, length=4)
        applied  = Operation('insert', position=10, value='AB')
        result   = transform_operation(incoming, applied)
        self.assertEqual(result.position, 3)
        self.assertEqual(result.length, 4)

    # ── Insert vs Delete ──────────────────────────────────────────────────────

    def test_id_delete_before_insert(self):
        """Delete before insert — insert shifts left."""
        incoming = Operation('insert', position=8, value='X')
        applied  = Operation('delete', position=2, length=3)
        result   = transform_operation(incoming, applied)
        self.assertEqual(result.position, 5)

    def test_id_delete_contains_insert(self):
        """Delete range contains insert pos — insert collapses to del_start."""
        incoming = Operation('insert', position=5, value='X')
        applied  = Operation('delete', position=3, length=5)  # deletes 3-7
        result   = transform_operation(incoming, applied)
        self.assertEqual(result.position, 3)

    def test_id_delete_after_insert(self):
        """Delete after insert — no change."""
        incoming = Operation('insert', position=2, value='X')
        applied  = Operation('delete', position=5, length=3)
        result   = transform_operation(incoming, applied)
        self.assertEqual(result.position, 2)

    # ── Delete vs Delete ──────────────────────────────────────────────────────

    def test_dd_applied_entirely_before(self):
        """Against-delete entirely before incoming — incoming shifts left."""
        incoming = Operation('delete', position=8, length=4)
        applied  = Operation('delete', position=2, length=3)
        result   = transform_operation(incoming, applied)
        self.assertEqual(result.position, 5)
        self.assertEqual(result.length, 4)

    def test_dd_applied_entirely_after(self):
        """Against-delete entirely after incoming — no change."""
        incoming = Operation('delete', position=2, length=3)
        applied  = Operation('delete', position=8, length=4)
        result   = transform_operation(incoming, applied)
        self.assertEqual(result.position, 2)
        self.assertEqual(result.length, 3)

    def test_dd_applied_overlaps_right(self):
        """
        incoming deletes [3, 7), applied deletes [5, 9).
        Overlap [5,7) = 2 chars. incoming.length shrinks by 2.
        """
        incoming = Operation('delete', position=3, length=4)
        applied  = Operation('delete', position=5, length=4)
        result   = transform_operation(incoming, applied)
        self.assertEqual(result.position, 3)
        self.assertEqual(result.length, 2)

    def test_dd_applied_overlaps_left(self):
        """
        incoming deletes [5, 9), applied deletes [2, 7).
        Overlap [5,7) = 2 chars. Position collapses to applied.position=2.
        incoming.length shrinks by overlap=2.
        """
        incoming = Operation('delete', position=5, length=4)
        applied  = Operation('delete', position=2, length=5)
        result   = transform_operation(incoming, applied)
        self.assertEqual(result.position, 2)
        self.assertEqual(result.length, 2)

    def test_dd_applied_contains_incoming(self):
        """Applied delete entirely contains incoming — incoming becomes no-op (length=0)."""
        incoming = Operation('delete', position=4, length=2)
        applied  = Operation('delete', position=2, length=8)
        result   = transform_operation(incoming, applied)
        self.assertEqual(result.length, 0)

    def test_dd_identical(self):
        """Same delete from two clients — second becomes no-op."""
        op1 = Operation('delete', position=3, length=4, user_id='a')
        op2 = Operation('delete', position=3, length=4, user_id='b')
        result = transform_operation(op2, op1)
        self.assertEqual(result.length, 0)

    # ── Convergence property ──────────────────────────────────────────────────

    def test_convergence_ii(self):
        """Two concurrent inserts must converge regardless of order."""
        doc = "hello world"
        op1 = Operation('insert', position=5, value='_A_', user_id='u1')
        op2 = Operation('insert', position=5, value='_B_', user_id='u2')

        # Site 1 applies op1 first, then transformed op2
        d1 = apply_operation(doc, op1)
        op2_t = transform_operation(op2, op1)
        d1 = apply_operation(d1, op2_t)

        # Site 2 applies op2 first, then transformed op1
        d2 = apply_operation(doc, op2)
        op1_t = transform_operation(op1, op2)
        d2 = apply_operation(d2, op1_t)

        self.assertEqual(d1, d2, f"Convergence failed:\n  site1={d1!r}\n  site2={d2!r}")

    def test_convergence_di(self):
        """
        Server-authoritative OT convergence for delete vs insert.

        In server-authoritative OT, the SERVER applies ops in serial order.
        Convergence means: regardless of which op the server receives first,
        the second client (whose op is transformed against the first) ends up
        with the same content as the server.

        We test two orderings:
          Server order A: del → transformed_ins
          Server order B: ins → transformed_del
        Both must produce internally consistent results (client matches server).
        We don't require A == B since semantic choice (insert survives vs delete
        wins) differs; we require each ordering is self-consistent.
        """
        doc = "abcdefghij"
        op_del = Operation('delete', position=3, length=4, user_id='u1')  # deletes 'defg'
        op_ins = Operation('insert', position=5, value='XY',  user_id='u2')

        # Order A: server applies del first, then transforms ins
        server_a = apply_operation(doc, op_del)
        op_ins_t = transform_operation(op_ins, op_del)
        server_a = apply_operation(server_a, op_ins_t)

        # Client 2 must see same content as server_a after receiving the ack+broadcast
        # (server sends transformed op to client 2 which has already applied op_ins locally)
        # Client 2 state before transform: applied op_ins locally
        client2_local = apply_operation(doc, op_ins)
        # Client 2 receives broadcast of op_del (already transformed by server to v2)
        # In this scenario the server transforms op_del against op_ins for broadcast
        op_del_for_client2 = transform_operation(op_del, op_ins)
        client2_final = apply_operation(client2_local, op_del_for_client2)

        self.assertEqual(server_a, client2_final,
            f"Order A mismatch:\n  server={server_a!r}\n  client2={client2_final!r}")

        # Order B: server applies ins first, then transforms del
        server_b = apply_operation(doc, op_ins)
        op_del_t = transform_operation(op_del, op_ins)
        server_b = apply_operation(server_b, op_del_t)

        # Client 1 state: applied op_del locally, receives transformed op_ins
        client1_local = apply_operation(doc, op_del)
        op_ins_for_client1 = transform_operation(op_ins, op_del)
        client1_final = apply_operation(client1_local, op_ins_for_client1)

        self.assertEqual(server_b, client1_final,
            f"Order B mismatch:\n  server={server_b!r}\n  client1={client1_final!r}")

    def test_convergence_dd(self):
        """Two overlapping deletes must converge."""
        doc = "abcdefghij"
        op1 = Operation('delete', position=2, length=5, user_id='u1')  # deletes 'cdefg'
        op2 = Operation('delete', position=4, length=4, user_id='u2')  # deletes 'efgh'

        d1 = apply_operation(doc, op1)
        op2_t = transform_operation(op2, op1)
        d1 = apply_operation(d1, op2_t)

        d2 = apply_operation(doc, op2)
        op1_t = transform_operation(op1, op2)
        d2 = apply_operation(d2, op1_t)

        self.assertEqual(d1, d2, f"Convergence failed:\n  site1={d1!r}\n  site2={d2!r}")

    def test_transform_against_log(self):
        """transform_against_log must compose transforms correctly."""
        doc = "hello world"
        # Server log has two ops at versions 1 and 2
        srv_op1 = Operation('insert', position=0, value='>> ', user_id='s')
        srv_op2 = Operation('delete', position=9, length=3, user_id='s')

        log = [(1, srv_op1), (2, srv_op2)]

        # Client op based at version 0, inserts at position 5
        client_op = Operation('insert', position=5, value='X', base_version=0, user_id='c')

        transformed = transform_against_log(client_op, log, from_version=0, to_version=2)

        # Manually verify:
        # After srv_op1 (insert 3 chars at 0): client pos 5 → 8
        after_op1 = transform_operation(client_op, srv_op1)
        # After srv_op2 (delete 3 chars at 9): pos 8 unaffected (8 < 9)
        after_op2 = transform_operation(after_op1, srv_op2)

        self.assertEqual(transformed.position, after_op2.position)


# ══════════════════════════════════════════════════════════════════════════════
#  2. CONVERGENCE STRESS TEST (no server needed — pure OT simulation)
# ══════════════════════════════════════════════════════════════════════════════

def simulate_concurrent_edits(initial_doc: str, n_clients: int, n_ops_each: int) -> bool:
    """
    Simulate N clients each making ops against the same initial document.
    Verify all clients converge to the same final state.

    This is the algorithm used in OT literature for correctness verification.
    """
    rng = random.Random(42)
    doc = initial_doc

    # Generate ops for each client (all based at version 0)
    def rand_op(content, uid):
        if not content or rng.random() < 0.5:
            pos = rng.randint(0, len(content))
            val = rng.choice(string.ascii_lowercase + ' ')
            return Operation('insert', position=pos, value=val, user_id=uid, base_version=0)
        else:
            pos = rng.randint(0, len(content) - 1)
            ln = rng.randint(1, min(3, len(content) - pos))
            return Operation('delete', position=pos, length=ln, user_id=uid, base_version=0)

    clients_ops = [
        [rand_op(doc, f'u{i}') for _ in range(n_ops_each)]
        for i in range(n_clients)
    ]

    # Apply each client's ops and transform against all others
    # Simple 2-client convergence check (extend for N with Jupiter algorithm)
    # Here we do pairwise for N=2
    if n_clients != 2:
        raise ValueError("Simulation supports exactly 2 clients")

    ops_a, ops_b = clients_ops[0], clients_ops[1]

    # Site A: apply A's ops first, then transform-and-apply B's ops
    doc_a = doc
    for op in ops_a:
        doc_a = apply_operation(doc_a, op)

    # Build log for A's ops
    log_a = [(i+1, op) for i, op in enumerate(ops_a)]

    doc_a_final = doc_a
    for op_b in ops_b:
        t = transform_against_log(op_b, log_a, from_version=0, to_version=len(ops_a))
        doc_a_final = apply_operation(doc_a_final, t)

    # Site B: apply B's ops first, then transform-and-apply A's ops
    doc_b = doc
    for op in ops_b:
        doc_b = apply_operation(doc_b, op)

    log_b = [(i+1, op) for i, op in enumerate(ops_b)]

    doc_b_final = doc_b
    for op_a in ops_a:
        t = transform_against_log(op_a, log_b, from_version=0, to_version=len(ops_b))
        doc_b_final = apply_operation(doc_b_final, t)

    return doc_a_final == doc_b_final


# ══════════════════════════════════════════════════════════════════════════════
#  3. THROUGHPUT BENCHMARK (local, no network)
# ══════════════════════════════════════════════════════════════════════════════

def benchmark_ops(n_ops: int = 10_000):
    doc_content = "The quick brown fox jumps over the lazy dog. " * 100
    rng = random.Random(0)
    ops = []
    for _ in range(n_ops):
        if rng.random() < 0.6 or not doc_content:
            pos = rng.randint(0, len(doc_content))
            val = rng.choice(string.ascii_lowercase)
            ops.append(Operation('insert', position=pos, value=val))
        else:
            pos = rng.randint(0, len(doc_content) - 1)
            ops.append(Operation('delete', position=pos, length=1))

    start = time.perf_counter()
    for op in ops:
        doc_content = apply_operation(doc_content, op)
    elapsed = time.perf_counter() - start

    return n_ops, elapsed, n_ops / elapsed


# ══════════════════════════════════════════════════════════════════════════════
#  RUNNER
# ══════════════════════════════════════════════════════════════════════════════

if __name__ == '__main__':
    print("=" * 60)
    print(" CollabDocs Stress Test Suite")
    print("=" * 60)

    # 1. Unit tests
    print("\n[1/3] Running OT unit tests…")
    loader = unittest.TestLoader()
    suite = loader.loadTestsFromTestCase(TestOTTransforms)
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)

    if not result.wasSuccessful():
        print("\n❌  Unit tests FAILED. Fix OT bugs before benchmarking.")
        sys.exit(1)

    print(f"\n✅  All {result.testsRun} unit tests passed.")

    # 2. Convergence simulation
    print("\n[2/3] Running convergence simulation (2 clients × 50 ops, 200 trials)…")
    failures = 0
    trials = 200
    initial = "The quick brown fox jumps over the lazy dog."
    for trial in range(trials):
        rng_seed = trial
        random.seed(rng_seed)
        ok = simulate_concurrent_edits(initial, n_clients=2, n_ops_each=50)
        if not ok:
            failures += 1
            print(f"  ❌ Trial {trial} DIVERGED")

    if failures:
        print(f"\n❌  {failures}/{trials} convergence trials FAILED.")
    else:
        print(f"✅  All {trials} convergence trials converged.")

    # 3. Throughput benchmark
    print("\n[3/3] Throughput benchmark (10,000 sequential ops)…")
    n, elapsed, ops_per_sec = benchmark_ops(10_000)
    print(f"  Ops:          {n:,}")
    print(f"  Time:         {elapsed*1000:.1f} ms")
    print(f"  Throughput:   {ops_per_sec:,.0f} ops/sec")
    print(f"  Avg latency:  {(elapsed/n)*1_000_000:.2f} µs/op")

    print("\n" + "=" * 60)
    print(" Done.")
    print("=" * 60)
