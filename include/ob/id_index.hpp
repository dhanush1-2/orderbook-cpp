// include/ob/id_index.hpp
#pragma once

// Open-addressed OrderId -> Slot map. This is what makes cancel O(1).
//
// Linear probing, because the probe sequence is contiguous and therefore
// cache-friendly, which is the entire reason not to use a chained map.
//
// BACKWARD-SHIFT DELETION rather than tombstones. Under the realistic workload
// (roughly 90% cancels) tombstones accumulate and probe lengths grow without
// bound until a rehash, and rehashing is forbidden on a no-allocation hot path.
// Keeping the table tombstone-free is also what lets find() stop at the first
// empty slot, forever.
//
// Never rehashes. Capacity is fixed at construction; reaching the load ceiling is
// a capacity rejection (spec E40), not a resize.

#include <cassert>
#include <cstddef>
#include <ob/types.hpp>
#include <vector>

namespace ob {

class IdIndex {
public:
    // Capacity is rounded up to a power of two of at least 2 * min_entries, so
    // min_entries live ids sit at a 0.5 load factor and probe lengths stay ~1.5.
    explicit IdIndex(std::size_t min_entries) {
        std::size_t cap = 1;
        while (cap < min_entries * 2) {
            cap <<= 1;
        }
        table_.assign(cap, Entry{});
        mask_     = cap - 1;
        max_load_ = cap / 2;
    }

    // Hash selection, A/B-able at compile time via OB_IDINDEX_IDENTITY_HASH.
    //
    // MEASURED MOTIVATION: varying only the engine capacity, with an identical
    // workload, moves cancel_heavy from 7.6 ns/op (0.5 MB index) to 33.0 ns/op
    // (128 MB index) - a 4.3x swing. The engine is memory-bound on this table, and
    // SplitMix64 is what scatters sequential ids across the whole of it.
    //
    // Identity hashing maps consecutive ids to consecutive buckets, so a sequencer's
    // output touches a contiguous window regardless of allocated capacity. The risk
    // is scattered or adversarial ids clustering, which is why both cases are
    // measured before a decision is recorded.
    // Block size for the blocked hash. 16 ids x 16 bytes = 256 bytes, so a block
    // spans two 128-byte cache lines, and a contiguous run is bounded at ~16.
    static constexpr unsigned kBlockShift = 4;

    static std::uint64_t splitmix(std::uint64_t z) noexcept {
        z ^= z >> 33;
        z *= 0xFF51'AFD7'ED55'8CCDULL;
        z ^= z >> 33;
        z *= 0xC4CE'B9FE'1A85'EC53ULL;
        z ^= z >> 33;
        return z;
    }

    [[nodiscard]] static std::uint64_t hash(OrderId id) noexcept {
#if defined(OB_IDINDEX_HASH_SPLITMIX) && OB_IDINDEX_HASH_SPLITMIX
        // SplitMix64: scatters everything. O(1) deletion, worst locality. Kept
        // selectable because it was the original default and the A/B needs it.
        return splitmix(id);
#elif defined(OB_IDINDEX_HASH_IDENTITY) && OB_IDINDEX_HASH_IDENTITY
        // Pure identity: best possible locality, WORST possible deletion. A run of
        // consecutive live ids is a contiguous run of buckets, and backward-shift
        // deletion over a contiguous run is O(run length). Measured: 30-67% faster
        // on five scenarios, 284% SLOWER on worst_case_sweep, which holds 400+
        // consecutive live ids.
        return id;
#else
        // DEFAULT, chosen by measurement. Blocked: low kBlockShift bits come from the
        // id, so 16 consecutive ids land in 16 consecutive buckets (locality). The
        // block index is scrambled, so different blocks land far apart and a
        // contiguous run is bounded at ~16 instead of growing with the number of live
        // orders (bounded deletion cost).
        //
        // Measured against SplitMix64: faster on ALL SIX scenarios, -17% to -54%,
        // 30.3% better on the sum of per-op costs, with no regression anywhere.
        return (splitmix(id >> kBlockShift) << kBlockShift) | (id & ((1u << kBlockShift) - 1));
#endif
    }

    [[nodiscard]] bool insert(OrderId id, Slot slot) noexcept {
        if (id == 0) {
            return false;  // 0 is the empty marker; the engine rejects it earlier
        }
        if (size_ >= max_load_) {
            return false;  // capacity, never a rehash
        }
        std::size_t i = hash(id) & mask_;
        while (table_[i].id != 0) {
            if (table_[i].id == id) {
                return false;  // duplicate
            }
            i = (i + 1) & mask_;
        }
        table_[i].id   = id;
        table_[i].slot = slot;
        ++size_;
        return true;
    }

    // Stops at the first empty slot, which is only correct because deletion keeps
    // the table tombstone-free.
    [[nodiscard]] Slot find(OrderId id) const noexcept {
        if (id == 0) {
            return kInvalidSlot;
        }
        std::size_t i = hash(id) & mask_;
        while (table_[i].id != 0) {
            if (table_[i].id == id) {
                return table_[i].slot;
            }
            i = (i + 1) & mask_;
        }
        return kInvalidSlot;
    }

    bool erase(OrderId id) noexcept {
        if (id == 0) {
            return false;
        }
        std::size_t i = hash(id) & mask_;
        while (table_[i].id != 0 && table_[i].id != id) {
            i = (i + 1) & mask_;
        }
        if (table_[i].id == 0) {
            return false;
        }

        // Backward-shift deletion. Walk forward from the hole; any entry whose
        // ideal bucket does NOT lie cyclically in (i, j] must move back into the
        // hole, or find() would stop early at it.
        table_[i]     = Entry{};
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (table_[j].id == 0) {
                break;
            }
            const std::size_t k        = hash(table_[j].id) & mask_;
            const bool        leave_it = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
            if (leave_it) {
                continue;
            }
            table_[i] = table_[j];
            table_[j] = Entry{};
            i         = j;
        }
        --size_;
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return table_.size(); }
    [[nodiscard]] std::size_t max_load() const noexcept { return max_load_; }
    [[nodiscard]] bool        full() const noexcept { return size_ >= max_load_; }

    // No prefault() here on purpose: the constructor's assign() writes every byte
    // of the table, so every page is already resident before any measurement runs.
    void reset() noexcept {
        table_.assign(table_.size(), Entry{});
        size_ = 0;
    }

private:
    struct Entry {
        OrderId id   = 0;  // 0 means empty
        Slot    slot = kInvalidSlot;
    };
    static_assert(sizeof(Entry) == 16);

    std::vector<Entry> table_;
    std::size_t        mask_     = 0;
    std::size_t        size_     = 0;
    std::size_t        max_load_ = 0;
};

}  // namespace ob
