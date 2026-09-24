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

    // SplitMix64 finalizer. Identity hashing would be faster for the sequential
    // ids a real sequencer emits (consecutive ids land in consecutive buckets with
    // zero collisions), and the optimization arc measures that as a candidate. It
    // is not the starting point because the fuzzer supplies scattered ids and the
    // structure must not degrade under them.
    [[nodiscard]] static std::uint64_t hash(OrderId id) noexcept {
        std::uint64_t z = id;
        z ^= z >> 33;
        z *= 0xFF51'AFD7'ED55'8CCDULL;
        z ^= z >> 33;
        z *= 0xC4CE'B9FE'1A85'EC53ULL;
        z ^= z >> 33;
        return z;
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
    // A separate prefault would be a no-op with a misleading name.
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
