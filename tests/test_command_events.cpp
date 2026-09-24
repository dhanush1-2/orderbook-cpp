#include <gtest/gtest.h>

#include <ob/command.hpp>
#include <ob/events.hpp>
#include <sstream>
#include <string>

namespace {

TEST(Command, NewFactorySetsEveryField) {
    const ob::Command c = ob::make_new(42, ob::Side::Buy, ob::OrderType::Limit, 10050, 350);
    EXPECT_EQ(c.type, ob::CommandType::New);
    EXPECT_EQ(c.id, 42u);
    EXPECT_EQ(c.side, ob::Side::Buy);
    EXPECT_EQ(c.order_type, ob::OrderType::Limit);
    EXPECT_EQ(c.price, 10050);
    EXPECT_EQ(c.qty, 350u);
}

TEST(Command, CancelFactoryLeavesPriceAndQtyNeutral) {
    const ob::Command c = ob::make_cancel(42);
    EXPECT_EQ(c.type, ob::CommandType::Cancel);
    EXPECT_EQ(c.id, 42u);
    EXPECT_EQ(c.price, ob::kNoPrice);
    EXPECT_EQ(c.qty, 0u);
}

TEST(Event, EqualityComparesEveryField) {
    ob::Event a{};
    a.seq       = 1;
    a.type      = ob::EventType::Trade;
    a.order_id  = 9;
    a.maker_id  = 7;
    a.price     = 10050;
    a.qty       = 300;
    ob::Event b = a;
    EXPECT_EQ(a, b);

    b.qty = 299;
    EXPECT_NE(a, b);

    b          = a;
    b.maker_id = 8;
    EXPECT_NE(a, b);

    b     = a;
    b.seq = 2;
    EXPECT_NE(a, b);
}

TEST(Event, PrintToProducesReadableOutput) {
    ob::Event e{};
    e.seq      = 5;
    e.type     = ob::EventType::Trade;
    e.order_id = 99;
    e.maker_id = 7;
    e.price    = 10050;
    e.qty      = 300;

    std::ostringstream os;
    PrintTo(e, &os);
    const std::string s = os.str();

    EXPECT_NE(s.find("Trade"), std::string::npos);
    EXPECT_NE(s.find("99"), std::string::npos);
    EXPECT_NE(s.find("10050"), std::string::npos);
}

TEST(Event, RejectedPrintIncludesTheReasonName) {
    ob::Event e{};
    e.type     = ob::EventType::Rejected;
    e.order_id = 3;
    e.reject   = ob::RejectReason::PriceOutOfRange;

    std::ostringstream os;
    PrintTo(e, &os);
    EXPECT_NE(os.str().find("PriceOutOfRange"), std::string::npos);
}

}  // namespace
