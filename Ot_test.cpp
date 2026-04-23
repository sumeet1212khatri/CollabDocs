/*
 * CollabDocs C++ — OT Engine Test Suite
 *
 * Tests all 4 transform cases + convergence + log-transform.
 * No external test framework needed — pure C++17.
 */

#include "ot_engine.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <random>
#include <string>

using namespace collab;

// ─── Mini test framework ─────────────────────────────────────────────────────

static int g_pass = 0, g_fail = 0;

#define EXPECT_EQ(a, b) do { \
    if ((a) == (b)) { g_pass++; } \
    else { g_fail++; \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ \
                  << "  expected=" << (b) << "  got=" << (a) << "\n"; } \
} while(0)

#define EXPECT_STR_EQ(a, b) do { \
    if (std::string(a) == std::string(b)) { g_pass++; } \
    else { g_fail++; \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ \
                  << "\n  expected: " << (b) \
                  << "\n  got:      " << (a) << "\n"; } \
} while(0)

// ─── Helpers ─────────────────────────────────────────────────────────────────

Operation ins(int pos, const std::string& val,
              const std::string& uid = "", int base = 0) {
    Operation op;
    op.type = OpType::INSERT;
    op.position = pos;
    op.value = val;
    op.length = static_cast<int>(val.size());
    op.user_id = uid;
    op.base_version = base;
    return op;
}

Operation del(int pos, int len,
              const std::string& uid = "", int base = 0) {
    Operation op;
    op.type = OpType::DELETE;
    op.position = pos;
    op.length = len;
    op.user_id = uid;
    op.base_version = base;
    return op;
}

// ─── Insert-Insert ────────────────────────────────────────────────────────────

void test_ii_before() {
    auto r = transform_operation(ins(5,"X"), ins(2,"AB"));
    EXPECT_EQ(r.position, 7);
}

void test_ii_after() {
    auto r = transform_operation(ins(2,"X"), ins(5,"AB"));
    EXPECT_EQ(r.position, 2);
}

void test_ii_same_tiebreak_shift() {
    // alice < bob → alice wins → bob shifts
    auto r = transform_operation(ins(3,"X","bob"), ins(3,"Y","alice"));
    EXPECT_EQ(r.position, 4);
}

void test_ii_same_tiebreak_no_shift() {
    auto r = transform_operation(ins(3,"X","alice"), ins(3,"Y","bob"));
    EXPECT_EQ(r.position, 3);
}

// ─── Delete-Insert ────────────────────────────────────────────────────────────

void test_di_insert_before_delete() {
    auto r = transform_operation(del(5,3), ins(2,"XX"));
    EXPECT_EQ(r.position, 7);
    EXPECT_EQ(r.length, 3);
}

void test_di_insert_inside_delete() {
    auto r = transform_operation(del(3,5), ins(5,"AB"));
    EXPECT_EQ(r.position, 3);
    EXPECT_EQ(r.length, 7); // expanded
}

void test_di_insert_after_delete() {
    auto r = transform_operation(del(3,4), ins(10,"AB"));
    EXPECT_EQ(r.position, 3);
    EXPECT_EQ(r.length, 4);
}

// ─── Insert-Delete ────────────────────────────────────────────────────────────

void test_id_delete_before_insert() {
    auto r = transform_operation(ins(8,"X"), del(2,3));
    EXPECT_EQ(r.position, 5);
}

void test_id_delete_contains_insert() {
    auto r = transform_operation(ins(5,"X"), del(3,5));
    EXPECT_EQ(r.position, 3);
}

void test_id_delete_after_insert() {
    auto r = transform_operation(ins(2,"X"), del(5,3));
    EXPECT_EQ(r.position, 2);
}

// ─── Delete-Delete ────────────────────────────────────────────────────────────

void test_dd_applied_before() {
    auto r = transform_operation(del(8,4), del(2,3));
    EXPECT_EQ(r.position, 5);
    EXPECT_EQ(r.length, 4);
}

void test_dd_applied_after() {
    auto r = transform_operation(del(2,3), del(8,4));
    EXPECT_EQ(r.position, 2);
    EXPECT_EQ(r.length, 3);
}

void test_dd_overlap_right() {
    // incoming [3,7), applied [5,9): overlap [5,7)=2
    auto r = transform_operation(del(3,4), del(5,4));
    EXPECT_EQ(r.position, 3);
    EXPECT_EQ(r.length, 2);
}

void test_dd_overlap_left() {
    // incoming [5,9), applied [2,7): overlap [5,7)=2
    auto r = transform_operation(del(5,4), del(2,5));
    EXPECT_EQ(r.position, 2);
    EXPECT_EQ(r.length, 2);
}

void test_dd_applied_contains_incoming() {
    auto r = transform_operation(del(4,2), del(2,8));
    EXPECT_EQ(r.length, 0);
}

void test_dd_identical() {
    auto r = transform_operation(del(3,4,"b"), del(3,4,"a"));
    EXPECT_EQ(r.length, 0);
}

// ─── Convergence ─────────────────────────────────────────────────────────────

void test_convergence_ii() {
    std::string doc = "hello world";
    auto op1 = ins(5,"_A_","u1");
    auto op2 = ins(5,"_B_","u2");

    auto d1 = apply_operation(doc, op1);
    d1 = apply_operation(d1, transform_operation(op2, op1));

    auto d2 = apply_operation(doc, op2);
    d2 = apply_operation(d2, transform_operation(op1, op2));

    EXPECT_STR_EQ(d1, d2);
}

void test_convergence_dd() {
    std::string doc = "abcdefghij";
    auto op1 = del(2,5,"u1");
    auto op2 = del(4,4,"u2");

    auto d1 = apply_operation(doc, op1);
    d1 = apply_operation(d1, transform_operation(op2, op1));

    auto d2 = apply_operation(doc, op2);
    d2 = apply_operation(d2, transform_operation(op1, op2));

    EXPECT_STR_EQ(d1, d2);
}

void test_transform_against_log() {
    std::string doc = "hello world";
    Operation srv1 = ins(0,">> ","s",0);
    Operation srv2 = del(9,3,"s",1);

    std::vector<std::pair<int,Operation>> log = {{1,srv1},{2,srv2}};

    Operation client_op = ins(5,"X","c",0);
    auto result = transform_against_log(client_op, log, 0, 2);

    // Manual:
    // After srv1 (ins 3 chars at 0): pos 5 → 8
    auto after1 = transform_operation(client_op, srv1);
    // After srv2 (del 3 at 9): pos 8 < 9 → no change
    auto after2 = transform_operation(after1, srv2);

    EXPECT_EQ(result.position, after2.position);
}

// ─── Apply tests ──────────────────────────────────────────────────────────────

void test_apply_insert() {
    EXPECT_STR_EQ(apply_operation("abcde", ins(2,"XY")), "abXYcde");
}

void test_apply_delete() {
    EXPECT_STR_EQ(apply_operation("abcde", del(1,3)), "ae");
}

void test_apply_insert_end() {
    EXPECT_STR_EQ(apply_operation("abc", ins(3,"Z")), "abcZ");
}

void test_apply_delete_out_of_range() {
    EXPECT_STR_EQ(apply_operation("abc", del(2,100)), "ab");
}

// ─── Cursor sync ──────────────────────────────────────────────────────────────

void test_cursor_insert_before() {
    auto c = transform_cursor(8, ins(3,"AB"));
    EXPECT_EQ(c, 10);
}

void test_cursor_insert_after() {
    auto c = transform_cursor(2, ins(5,"AB"));
    EXPECT_EQ(c, 2);
}

void test_cursor_delete_before() {
    auto c = transform_cursor(8, del(2,3));
    EXPECT_EQ(c, 5);
}

void test_cursor_delete_over() {
    auto c = transform_cursor(4, del(2,5));
    EXPECT_EQ(c, 2);
}

// ─── Stress convergence ───────────────────────────────────────────────────────

void stress_convergence(int trials = 500) {
    std::mt19937 rng(42);
    const std::string alphabet = "abcdefghijklmnopqrstuvwxyz ";
    int failures = 0;

    auto rand_op = [&](const std::string& content, const std::string& uid) {
        if (content.empty() || std::uniform_real_distribution<>()(rng) < 0.55) {
            int pos = std::uniform_int_distribution<>(0, static_cast<int>(content.size()))(rng);
            char c = alphabet[std::uniform_int_distribution<>(0,static_cast<int>(alphabet.size())-1)(rng)];
            return ins(pos, std::string(1,c), uid);
        } else {
            int pos = std::uniform_int_distribution<>(0, static_cast<int>(content.size())-1)(rng);
            int len = std::uniform_int_distribution<>(1, std::min(3, static_cast<int>(content.size())-pos))(rng);
            return del(pos, len, uid);
        }
    };

    std::string initial = "The quick brown fox jumps.";
    for (int t = 0; t < trials; ++t) {
        std::string doc = initial;
        auto op1 = rand_op(doc, "u1");
        auto op2 = rand_op(doc, "u2");

        auto d1 = apply_operation(doc, op1);
        d1 = apply_operation(d1, transform_operation(op2, op1));

        auto d2 = apply_operation(doc, op2);
        d2 = apply_operation(d2, transform_operation(op1, op2));

        if (d1 != d2) failures++;
    }

    if (failures > 0) {
        g_fail++;
        std::cerr << "FAIL stress_convergence: " << failures << "/" << trials << " diverged\n";
    } else {
        g_pass++;
        std::cout << "  stress_convergence: " << trials << " trials OK\n";
    }
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== CollabDocs C++ OT Test Suite ===\n\n";

    // Insert-Insert
    test_ii_before();
    test_ii_after();
    test_ii_same_tiebreak_shift();
    test_ii_same_tiebreak_no_shift();

    // Delete-Insert
    test_di_insert_before_delete();
    test_di_insert_inside_delete();
    test_di_insert_after_delete();

    // Insert-Delete
    test_id_delete_before_insert();
    test_id_delete_contains_insert();
    test_id_delete_after_insert();

    // Delete-Delete
    test_dd_applied_before();
    test_dd_applied_after();
    test_dd_overlap_right();
    test_dd_overlap_left();
    test_dd_applied_contains_incoming();
    test_dd_identical();

    // Convergence
    test_convergence_ii();
    test_convergence_dd();
    test_transform_against_log();

    // Apply
    test_apply_insert();
    test_apply_delete();
    test_apply_insert_end();
    test_apply_delete_out_of_range();

    // Cursor
    test_cursor_insert_before();
    test_cursor_insert_after();
    test_cursor_delete_before();
    test_cursor_delete_over();

    // Stress
    stress_convergence(500);

    std::cout << "\n=== Results: "
              << g_pass << " passed, " << g_fail << " failed ===\n";
    return g_fail > 0 ? 1 : 0;
}
