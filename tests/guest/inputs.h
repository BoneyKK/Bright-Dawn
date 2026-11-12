// The fixed input sequence every guest test is run on: all pairs of a set of
// edge values, then pseudo-random pairs. Shared so that every executor is fed
// exactly the same inputs as the reference.
#pragma once

#include <stdint.h>

#define GUEST_EDGE_COUNT 22
#define GUEST_RANDOM_PAIRS 2000
#define GUEST_PAIR_COUNT (GUEST_EDGE_COUNT * GUEST_EDGE_COUNT + GUEST_RANDOM_PAIRS)

static const uint32_t guest_edges[GUEST_EDGE_COUNT] = {
    0, 1, 2, 3, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF,
    0x80000000u, 0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu, 0x55555555u, 0xAAAAAAAAu, 31, 32, 33};

typedef struct {
  uint32_t state;
} guest_rng;

static inline uint32_t guest_rng_next(guest_rng* rng) {
  rng->state ^= rng->state << 13;
  rng->state ^= rng->state >> 17;
  rng->state ^= rng->state << 5;
  return rng->state;
}

// Input pair number `index` (0 .. GUEST_PAIR_COUNT-1). Call with increasing
// index and a generator that was seeded with guest_rng_seed() at index 0.
static inline void guest_input(guest_rng* rng, int index, uint32_t* a, uint32_t* b) {
  if (index < GUEST_EDGE_COUNT * GUEST_EDGE_COUNT) {
    *a = guest_edges[index / GUEST_EDGE_COUNT];
    *b = guest_edges[index % GUEST_EDGE_COUNT];
  } else {
    *a = guest_rng_next(rng);
    *b = guest_rng_next(rng);
  }
}

static inline guest_rng guest_rng_seed(void) {
  guest_rng rng = {0x12345678u};
  return rng;
}

// FNV-1a over the results of one test, in input order.
static inline uint32_t guest_hash_step(uint32_t hash, uint32_t result) {
  int i;
  for (i = 0; i < 4; ++i) {
    hash ^= (result >> (i * 8)) & 0xFFu;
    hash *= 16777619u;
  }
  return hash;
}
#define GUEST_HASH_START 2166136261u
