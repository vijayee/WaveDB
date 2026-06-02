#include <stdio.h>
#include <string.h>
#include "../src/Layers/graph/graph_internal.h"

static int tests_passed = 0, tests_failed = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { fprintf(stderr, "FAIL: %s\n", name); tests_failed++; } \
    else { tests_passed++; printf("PASS: %s\n", name); } \
} while(0)

int main(void) {
    vertex_set_t set;
    vertex_set_init(&set, 8);

    TEST("empty set has count 0", set.count == 0);
    TEST("contains returns 0 for missing", vertex_set_contains(&set, "alice") == 0);

    vertex_set_add(&set, "alice");
    TEST("add increases count", set.count == 1);
    TEST("contains finds added item", vertex_set_contains(&set, "alice") == 1);
    TEST("contains returns 0 for different", vertex_set_contains(&set, "bob") == 0);

    // Duplicate add
    vertex_set_add(&set, "alice");
    TEST("duplicate add does not increase count", set.count == 1);

    // Bulk add
    vertex_set_add(&set, "bob");
    vertex_set_add(&set, "charlie");
    TEST("multiple adds work", set.count == 3);

    // Intersection
    vertex_set_t a, b, result;
    vertex_set_init(&a, 8);
    vertex_set_init(&b, 8);
    vertex_set_init(&result, 8);
    vertex_set_add(&a, "alice"); vertex_set_add(&a, "bob"); vertex_set_add(&a, "charlie");
    vertex_set_add(&b, "bob"); vertex_set_add(&b, "dave");
    vertex_set_intersect(&result, &a, &b);
    TEST("intersect has correct count", result.count == 1);
    TEST("intersect contains bob", vertex_set_contains(&result, "bob") == 1);
    TEST("intersect does not contain alice", vertex_set_contains(&result, "alice") == 0);

    // Union
    vertex_set_t uresult;
    vertex_set_init(&uresult, 8);
    vertex_set_union(&uresult, &a, &b);
    TEST("union has correct count", uresult.count == 4);
    TEST("union contains alice", vertex_set_contains(&uresult, "alice") == 1);
    TEST("union contains dave", vertex_set_contains(&uresult, "dave") == 1);

    // Difference
    vertex_set_t dresult;
    vertex_set_init(&dresult, 8);
    vertex_set_difference(&dresult, &a, &b);
    TEST("difference has correct count", dresult.count == 2);
    TEST("difference contains alice", vertex_set_contains(&dresult, "alice") == 1);
    TEST("difference contains charlie", vertex_set_contains(&dresult, "charlie") == 1);
    TEST("difference does not contain bob", vertex_set_contains(&dresult, "bob") == 0);
    TEST("difference does not contain dave", vertex_set_contains(&dresult, "dave") == 0);

    // Difference: empty result when b contains all of a
    vertex_set_t c, dempty;
    vertex_set_init(&c, 8);
    vertex_set_init(&dempty, 8);
    vertex_set_add(&c, "bob");
    vertex_set_difference(&dempty, &c, &b);
    TEST("difference empty when b covers a", dempty.count == 0);

    // Copy
    vertex_set_t cpy;
    vertex_set_init(&cpy, 8);
    vertex_set_copy(&cpy, &a);
    TEST("copy has same count", cpy.count == a.count);
    TEST("copy contains alice", vertex_set_contains(&cpy, "alice") == 1);
    TEST("copy contains charlie", vertex_set_contains(&cpy, "charlie") == 1);

    vertex_set_destroy(&c);
    vertex_set_destroy(&dempty);
    vertex_set_destroy(&cpy);
    vertex_set_destroy(&dresult);
    vertex_set_destroy(&a);
    vertex_set_destroy(&b);
    vertex_set_destroy(&result);
    vertex_set_destroy(&uresult);
    vertex_set_destroy(&set);

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
