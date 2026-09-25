# Phase 4: Wire Protocol and Ingest — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Accept orders as a fixed-layout binary protocol decoded with zero copying, driven by a busy-poll ingest loop, with the parser itself fuzzed.

**Architecture:** Fixed-size little-endian records with a 4-byte header, ITCH/OUCH in shape. The decoder never allocates, never copies the payload, and validates every length before reading. A busy-poll loop reads a framed file into a buffer and feeds the engine without blocking. This is what turns the project from a data-structure exercise into something shaped like exchange infrastructure.

**Tech Stack:** C++20, standard library only. libFuzzer for the parser target.

**Spec:** [`../specs/2026-09-22-order-book-matching-engine-design.md`](../specs/2026-09-22-order-book-matching-engine-design.md) — open question **O1**, and section 10's "hostile or corrupt replay file" row.

## Global Constraints

Everything in [`README.md`](README.md). The ones this phase turns on:

- **The decoder parses untrusted bytes.** Every length is validated before any read. "It's just a local file" is how parsers ship without review.
- **No allocation in the decode path.** It fills a caller-provided `Command`.
- **Little-endian on the wire, `memcpy` to load.** A `reinterpret_cast` onto the buffer would be a strict-aliasing and alignment violation; `memcpy` is free after optimization and is correct.
- **Every malformed input is a typed error, never UB.** The fuzz target exists to prove it.

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `include/ob/wire.hpp` | Message layout, encode, decode, error codes | 1 |
| `tools/wire_gen.cpp` | Writes a scenario as a binary message file | 2 |
| `tools/wire_ingest.cpp` | Busy-poll ingest loop: file to engine | 2 |
| `fuzz/fuzz_wire_parser.cpp` | Decoder against arbitrary bytes | 3 |
| `tests/test_wire.cpp` | Round-trip, truncation, corruption, every error path | 1 |

---

## Task 1: The wire format

**Produces:** `ob::wire::MsgType`, `ob::wire::Header`, `ob::wire::kMaxMsgSize`, `ob::wire::DecodeError`, `ob::wire::DecodeResult`, `encode(const Command&, std::span<std::byte>) -> std::size_t`, `decode(std::span<const std::byte>, Command&) -> DecodeResult`.

**Layout.** Every field little-endian, every message a whole number of bytes with explicit padding, no implicit struct layout on the wire:

```
Header (4 bytes)   : u16 length (whole message, including header)
                     u8  type
                     u8  version
New (24 bytes)     : header + u64 id + i32 price + u32 qty + u8 side + u8 order_type + u16 pad
Cancel (12 bytes)  : header + u64 id
```

`length` is in the header rather than implied by type so a decoder can skip an unknown message instead of losing frame sync — the property that matters when a venue adds a message type you do not know yet.

- [ ] **Step 1: Write the failing test** covering: round-trip of every order type and side; a truncated buffer at *every* prefix length; a length field that disagrees with the type; an unknown type; an unknown version; a zero length; a length larger than the buffer; and that `decode` never reads past `size()`.
- [ ] **Step 2: Build, watch it fail.**
- [ ] **Step 3: Write `include/ob/wire.hpp`.**
- [ ] **Step 4: Tests pass; run under ASan and UBSan.**
- [ ] **Step 5: Commit.**

---

## Task 2: Generator and busy-poll ingest

**Produces:** `ob_wire_gen` and `ob_wire_ingest` executables.

`ob_wire_gen` writes a scenario to a binary file. `ob_wire_ingest` reads it into a fixed buffer and feeds the engine in a **busy-poll loop**: no blocking read, no sleep, drain what is available and spin when it is not. Reports decode errors by kind rather than dying.

- [ ] **Step 1: Write both tools.**
- [ ] **Step 2: Round-trip a scenario through the file and assert the engine ends in the same state as feeding it directly.** This is the acceptance test for the whole phase: the wire path must be behaviourally identical to the in-process path.
- [ ] **Step 3: Commit.**

---

## Task 3: Fuzz the parser

**Produces:** `ob_fuzz_wire_parser`.

Feeds libFuzzer's bytes straight at `decode` with no sanitisation, asserting it always terminates with a typed result and never reads out of bounds. Then feeds the decoded commands to the engine and checks the invariants, so a malformed frame cannot corrupt the book.

- [ ] **Step 1: Write the target.**
- [ ] **Step 2: Run it for several minutes in Docker; commit a minimised corpus.**
- [ ] **Step 3: Commit.**

---

## Task 4: CI and docs

- [ ] **Step 1: Add the wire fuzz target to the CI fuzz job.**
- [ ] **Step 2: Update README and the phase index.**
- [ ] **Step 3: Commit.**

---

## Self-review

**Hazards.** (1) `decode` must validate `length` against both the buffer size *and* the expected size for the type, in that order — checking only the type's size loses frame sync on a truncated buffer. (2) No `reinterpret_cast` onto the byte buffer; `memcpy` only. (3) The busy-poll loop must have a termination condition that does not depend on the file being well-formed.
