#pragma once

// The edge-case table IS the specification. Every row cites the spec edge case it
// covers (docs/superpowers/specs/...-design.md section 5.3). Keep them in sync.

#include <limits>
#include <ob/command.hpp>
#include <ob/events.hpp>
#include <optional>
#include <vector>

namespace obtest {

using namespace ob;  // NOLINT(google-build-using-namespace) - test-local convenience

// Expected event, without `seq`. Sequence monotonicity is asserted separately so
// that inserting a case does not renumber every later expectation.
struct Expect {
    EventType    type{};
    OrderId      id     = 0;
    OrderId      maker  = 0;
    Ticks        price  = kNoPrice;
    Qty          qty    = 0;
    RejectReason reject = RejectReason::None;
    CancelReason cancel = CancelReason::None;
};

struct Case {
    const char*          name;
    std::vector<Command> setup;    // events ignored
    Command              subject;  // the command under test
    std::vector<Expect>  expect;   // exactly the events `subject` must produce
    std::optional<Ticks> bid;      // expected best_bid() afterwards, if checked
    std::optional<Ticks> ask;      // expected best_ask() afterwards, if checked
};

// ---- command builders ----
inline Command buy(OrderId id, Ticks px, Qty q, OrderType t = OrderType::Limit) {
    return make_new(id, Side::Buy, t, px, q);
}
inline Command sell(OrderId id, Ticks px, Qty q, OrderType t = OrderType::Limit) {
    return make_new(id, Side::Sell, t, px, q);
}
inline Command cxl(OrderId id) {
    return make_cancel(id);
}

// ---- expectation builders ----
inline Expect acc(OrderId id) {
    return {EventType::Accepted, id, 0, kNoPrice, 0, RejectReason::None, CancelReason::None};
}
inline Expect fil(OrderId id) {
    return {EventType::Filled, id, 0, kNoPrice, 0, RejectReason::None, CancelReason::None};
}
inline Expect rej(OrderId id, RejectReason r) {
    return {EventType::Rejected, id, 0, kNoPrice, 0, r, CancelReason::None};
}
inline Expect trd(OrderId taker, OrderId maker, Ticks px, Qty q) {
    return {EventType::Trade, taker, maker, px, q, RejectReason::None, CancelReason::None};
}
inline Expect can(OrderId id, CancelReason r, Qty q) {
    return {EventType::Cancelled, id, 0, kNoPrice, q, RejectReason::None, r};
}

inline constexpr Ticks NONE = kNoPrice;

// The example book used by several cases:
//   asks  10050 -> [300 (id 1)][100 (id 2)],  10100 -> [200 (id 3)]
inline std::vector<Command> example_book() {
    return {sell(1, 10050, 300), sell(2, 10050, 100), sell(3, 10100, 200)};
}

inline std::vector<Case> all_edge_cases() {
    return {
        // ---- empty and boundary book states ----
        {"E1_limit_into_empty_book_rests", {}, buy(1, 10000, 100), {acc(1)}, 10000, NONE},

        {"E2_market_into_empty_book_cancels_no_liquidity",
         {},
         buy(1, NONE, 100, OrderType::Market),
         {acc(1), can(1, CancelReason::NoLiquidity, 100)},
         NONE,
         NONE},

        {"E3_fok_into_empty_book_unfillable",
         {},
         buy(1, 10000, 100, OrderType::Fok),
         {acc(1), can(1, CancelReason::Unfillable, 100)},
         NONE,
         NONE},

        {"E4_last_order_filled_yields_sentinel",
         {sell(1, 10000, 100)},
         buy(2, 10000, 100),
         {acc(2), trd(2, 1, 10000, 100), fil(1), fil(2)},
         NONE,
         NONE},

        {"E5_last_order_cancelled_yields_sentinel",
         {sell(1, 10000, 100)},
         cxl(1),
         {can(1, CancelReason::UserRequested, 100)},
         NONE,
         NONE},

        {"E6_both_sides_emptied_then_reused",
         {sell(1, 10000, 100), buy(2, 10000, 100)},
         buy(3, 9000, 10),
         {acc(3)},
         9000,
         NONE},

        {"E7_rests_at_lowest_valid_tick", {}, buy(1, kMinTick, 1), {acc(1)}, kMinTick, NONE},

        {"E8_rests_at_highest_valid_tick", {}, sell(1, kMaxTick, 1), {acc(1)}, NONE, kMaxTick},

        // ---- price and quantity validation ----
        {"E10_zero_quantity_rejected",
         {},
         buy(1, 10000, 0),
         {rej(1, RejectReason::InvalidQuantity)},
         NONE,
         NONE},

        {"E11_oversize_quantity_rejected",
         {},
         buy(1, 10000, kMaxOrderQty + 1),
         {rej(1, RejectReason::InvalidQuantity)},
         NONE,
         NONE},

        {"E12_price_below_ladder_rejected",
         {},
         buy(1, kMinTick - 1, 100),
         {rej(1, RejectReason::PriceOutOfRange)},
         NONE,
         NONE},

        {"E13_price_above_ladder_rejected",
         {},
         buy(1, kMaxTick + 1, 100),
         {rej(1, RejectReason::PriceOutOfRange)},
         NONE,
         NONE},

        {"E15_market_price_field_ignored_not_validated",
         {},
         buy(1, std::numeric_limits<Ticks>::max(), 100, OrderType::Market),
         {acc(1), can(1, CancelReason::NoLiquidity, 100)},
         NONE,
         NONE},

        // ---- identity and lifecycle ----
        {"E16_duplicate_live_id_rejected",
         {buy(1, 10000, 100)},
         sell(1, 20000, 5),
         {rej(1, RejectReason::DuplicateOrderId)},
         10000,
         NONE},

        {"E17_duplicate_retired_id_rejected",
         {sell(1, 10000, 100), buy(2, 10000, 100)},
         sell(1, 30000, 5),
         {rej(1, RejectReason::DuplicateOrderId)},
         NONE,
         NONE},

        {"E48_id_at_or_below_high_water_rejected_even_if_never_used",
         {buy(5, 10000, 100)},
         buy(3, 10000, 100),
         {rej(3, RejectReason::DuplicateOrderId)},
         10000,
         NONE},

        {"E48b_rejected_command_does_not_advance_the_high_water_mark",
         {buy(5, 10000, 100), buy(3, 10000, 100)},
         buy(6, 10000, 100),
         {acc(6)},
         10000,
         NONE},

        {"E49_order_id_zero_rejected",
         {},
         buy(0, 10000, 100),
         {rej(0, RejectReason::DuplicateOrderId)},
         NONE,
         NONE},

        {"E18_cancel_unknown_id_rejected",
         {},
         cxl(999),
         {rej(999, RejectReason::UnknownOrderId)},
         NONE,
         NONE},

        {"E19_cancel_filled_id_rejected",
         {sell(1, 10000, 100), buy(2, 10000, 100)},
         cxl(1),
         {rej(1, RejectReason::UnknownOrderId)},
         NONE,
         NONE},

        {"E20_double_cancel_rejected",
         {buy(1, 10000, 100), cxl(1)},
         cxl(1),
         {rej(1, RejectReason::UnknownOrderId)},
         NONE,
         NONE},

        {"E21_cancel_partially_filled_removes_remainder_only",
         {sell(1, 10000, 100), buy(2, 10000, 40)},
         cxl(1),
         {can(1, CancelReason::UserRequested, 60)},
         NONE,
         NONE},

        {"E22_cancel_fifo_head",
         {sell(1, 10000, 10), sell(2, 10000, 10), sell(3, 10000, 10)},
         cxl(1),
         {can(1, CancelReason::UserRequested, 10)},
         NONE,
         10000},

        {"E23_cancel_fifo_tail",
         {sell(1, 10000, 10), sell(2, 10000, 10), sell(3, 10000, 10)},
         cxl(3),
         {can(3, CancelReason::UserRequested, 10)},
         NONE,
         10000},

        {"E24_cancel_only_order_at_best_level_moves_best",
         {buy(1, 10010, 10), buy(2, 10000, 10)},
         cxl(1),
         {can(1, CancelReason::UserRequested, 10)},
         10000,
         NONE},

        {"E25_cancel_fifo_middle",
         {sell(1, 10000, 10), sell(2, 10000, 10), sell(3, 10000, 10)},
         cxl(2),
         {can(2, CancelReason::UserRequested, 10)},
         NONE,
         10000},

        // ---- matching mechanics ----
        {"E26_exact_quantity_match",
         {sell(1, 10000, 100)},
         buy(2, 10000, 100),
         {acc(2), trd(2, 1, 10000, 100), fil(1), fil(2)},
         NONE,
         NONE},

        {"E27_incoming_smaller_maker_survives_without_filled_event",
         {sell(1, 10000, 100)},
         buy(2, 10000, 30),
         {acc(2), trd(2, 1, 10000, 30), fil(2)},
         NONE,
         10000},

        {"E28_fifo_within_a_level",
         {sell(1, 10000, 10), sell(2, 10000, 10), sell(3, 10000, 10), sell(4, 10000, 10)},
         buy(9, 10000, 40),
         {acc(9), trd(9, 1, 10000, 10), fil(1), trd(9, 2, 10000, 10), fil(2), trd(9, 3, 10000, 10),
          fil(3), trd(9, 4, 10000, 10), fil(4), fil(9)},
         NONE,
         NONE},

        {"E29_sweeps_levels_best_price_first_at_each_makers_price",
         {sell(1, 10020, 10), sell(2, 10000, 10), sell(3, 10010, 10)},
         buy(9, 10020, 30),
         {acc(9), trd(9, 2, 10000, 10), fil(2), trd(9, 3, 10010, 10), fil(3), trd(9, 1, 10020, 10),
          fil(1), fil(9)},
         NONE,
         NONE},

        {"E30_equal_price_crosses",
         {sell(1, 10000, 100)},
         buy(2, 10000, 100),
         {acc(2), trd(2, 1, 10000, 100), fil(1), fil(2)},
         NONE,
         NONE},

        {"E31_one_tick_apart_does_not_cross",
         {sell(1, 10001, 100)},
         buy(2, 10000, 100),
         {acc(2)},
         10000,
         10001},

        {"E32_sweeps_level_then_rests_remainder",
         example_book(),
         buy(9, 10050, 1000),
         {acc(9), trd(9, 1, 10050, 300), fil(1), trd(9, 2, 10050, 100), fil(2)},
         10050,
         10100},

        {"E33_post_only_that_would_cross_is_rejected_with_no_accepted",
         {sell(1, 10000, 100)},
         buy(2, 10000, 100, OrderType::PostOnly),
         {rej(2, RejectReason::WouldCross)},
         NONE,
         10000},

        {"E34_post_only_that_does_not_cross_rests",
         {sell(1, 10010, 100)},
         buy(2, 10000, 100, OrderType::PostOnly),
         {acc(2)},
         10000,
         10010},

        {"E35_fok_exactly_fillable",
         {sell(1, 10000, 60), sell(2, 10010, 40)},
         buy(9, 10010, 100, OrderType::Fok),
         {acc(9), trd(9, 1, 10000, 60), fil(1), trd(9, 2, 10010, 40), fil(2), fil(9)},
         NONE,
         NONE},

        {"E36_fok_one_unit_short_mutates_nothing",
         {sell(1, 10000, 60), sell(2, 10010, 39)},
         buy(9, 10010, 100, OrderType::Fok),
         {acc(9), can(9, CancelReason::Unfillable, 100)},
         NONE,
         10000},

        {"E37_ioc_partial_fill_then_cancel_remainder",
         {sell(1, 10000, 30)},
         buy(9, 10000, 100, OrderType::Ioc),
         {acc(9), trd(9, 1, 10000, 30), fil(1), can(9, CancelReason::IocRemainder, 70)},
         NONE,
         NONE},

        {"E38_equal_price_ties_broken_by_arrival_sequence",
         {sell(1, 10000, 10), sell(2, 10000, 10)},
         buy(9, 10000, 20),
         {acc(9), trd(9, 1, 10000, 10), fil(1), trd(9, 2, 10000, 10), fil(2), fil(9)},
         NONE,
         NONE},

        // ---- worked example from the spec ----
        {"spec_worked_example_350_at_10050",
         example_book(),
         buy(99, 10050, 350),
         {acc(99), trd(99, 1, 10050, 300), fil(1), trd(99, 2, 10050, 50), fil(99)},
         NONE,
         10050},
    };
}

// Coverage accounting for edge cases NOT in the table above. Every spec case in
// section 5.3 appears either in all_edge_cases() or in this list.
//
//   E9  ladder fully occupied      -> tests/test_stress.cpp (Task 12): 65,536 levels
//   E14 price not a whole tick     -> not reachable through this API; the API takes
//                                     integer ticks. Enforced at the file boundary by
//                                     the replay parser (Phase 3).
//   E39 order pool exhausted       -> test_reference_resting.cpp,
//                                     test_reference_cancel.cpp (capacity ctor)
//   E40 id index at capacity       -> Phase 2, tests/test_id_index.cpp
//   E41 level quantity overflow    -> invariants.hpp (Task 11), asserted continuously
//   E42 sequence number overflow   -> accepted, not guarded (spec 5.3); ~5800 years
//   E43 replay determinism         -> tests/test_determinism.cpp (Task 12)
//   E44 determinism across builds  -> CI matrix golden-file comparison (Task 13)
//   E45 reference vs fast engine   -> Phase 2, tests/test_differential.cpp
//   E46 buffer sized exactly       -> test_event_buffer.cpp (Task 4)
//   E47 buffer overflow aborts     -> test_event_buffer.cpp death test (Task 4)
//   E48 strictly increasing ids    -> all_edge_cases() rows E48, E48b, and
//                                     test_reference_validation.cpp
//   E49 order id 0 rejected        -> all_edge_cases() row E49

}  // namespace obtest
