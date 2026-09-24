#include "cases/edge_cases.hpp"

#include <ob/reference_engine.hpp>

#include <gtest/gtest.h>

namespace {

// Phase 2 adds ob::FastEngine to this list, and every case below then runs against
// it unchanged. That one-line extension is the whole reason the table exists.
using EngineTypes = ::testing::Types<ob::ReferenceEngine>;

template <class E>
class EdgeCases : public ::testing::Test {};

TYPED_TEST_SUITE(EdgeCases, EngineTypes);

TYPED_TEST(EdgeCases, AllCasesProduceTheSpecifiedEvents) {
    for (const obtest::Case& tc : obtest::all_edge_cases()) {
        SCOPED_TRACE(tc.name);
        TypeParam engine;

        ob::FixedEventBuffer<4096> scratch;
        for (const ob::Command& c : tc.setup) {
            scratch.clear();
            engine.submit(c, scratch);
        }

        ob::FixedEventBuffer<4096> out;
        engine.submit(tc.subject, out);

        ASSERT_EQ(out.size(), tc.expect.size()) << "event count for " << tc.name;
        for (std::size_t i = 0; i < tc.expect.size(); ++i) {
            const ob::Event& got = out[i];
            const obtest::Expect& want = tc.expect[i];
            EXPECT_EQ(got.type, want.type) << "event " << i;
            EXPECT_EQ(got.order_id, want.id) << "event " << i;
            EXPECT_EQ(got.reject, want.reject) << "event " << i;
            EXPECT_EQ(got.cancel, want.cancel) << "event " << i;
            if (want.type == ob::EventType::Trade) {
                EXPECT_EQ(got.maker_id, want.maker) << "event " << i;
                EXPECT_EQ(got.price, want.price) << "event " << i;
            }
            if (want.qty != 0) {
                EXPECT_EQ(got.qty, want.qty) << "event " << i;
            }
        }

        if (tc.bid.has_value()) {
            EXPECT_EQ(engine.best_bid(), *tc.bid) << "best_bid for " << tc.name;
        }
        if (tc.ask.has_value()) {
            EXPECT_EQ(engine.best_ask(), *tc.ask) << "best_ask for " << tc.name;
        }
    }
}

// Asserted separately from the table so that inserting a case does not renumber
// every later expectation.
TYPED_TEST(EdgeCases, SequenceNumbersAreMonotonicAndGapFreeAcrossEveryCase) {
    for (const obtest::Case& tc : obtest::all_edge_cases()) {
        SCOPED_TRACE(tc.name);
        TypeParam engine;
        ob::Seq expected = 0;
        ob::FixedEventBuffer<4096> buf;

        for (const ob::Command& c : tc.setup) {
            buf.clear();
            engine.submit(c, buf);
            for (const ob::Event& e : buf) {
                ASSERT_EQ(e.seq, expected++);
            }
        }
        buf.clear();
        engine.submit(tc.subject, buf);
        for (const ob::Event& e : buf) {
            ASSERT_EQ(e.seq, expected++);
        }
    }
}

TYPED_TEST(EdgeCases, TheBookIsNeverCrossedAfterAnyCase) {
    for (const obtest::Case& tc : obtest::all_edge_cases()) {
        SCOPED_TRACE(tc.name);
        TypeParam engine;
        ob::FixedEventBuffer<4096> buf;
        for (const ob::Command& c : tc.setup) {
            buf.clear();
            engine.submit(c, buf);
        }
        buf.clear();
        engine.submit(tc.subject, buf);

        if (engine.best_bid() != ob::kNoPrice && engine.best_ask() != ob::kNoPrice) {
            EXPECT_LT(engine.best_bid(), engine.best_ask());
        }
    }
}

TYPED_TEST(EdgeCases, EveryCaseProducesAtLeastOneEvent) {
    for (const obtest::Case& tc : obtest::all_edge_cases()) {
        SCOPED_TRACE(tc.name);
        EXPECT_GE(tc.expect.size(), 1u) << "the table itself is wrong for " << tc.name;
    }
}

}  // namespace
