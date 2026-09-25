// include/ob/wire.hpp
#pragma once

// Fixed-layout binary order-entry protocol, ITCH/OUCH in shape.
//
// THIS CODE PARSES UNTRUSTED BYTES. Every length is validated against both the
// buffer and the message type before a single field is read. "It is only a local
// file" is how parsers ship without review.
//
// Little-endian on the wire; loaded with std::memcpy. A reinterpret_cast onto the
// buffer would be a strict-aliasing violation and would assume an alignment the
// wire does not guarantee. memcpy compiles to the same load and is correct.
//
// The header carries an explicit LENGTH rather than implying it from the type, so a
// decoder can skip a message type it does not know instead of losing frame sync -
// the property that matters the day a venue adds a message you have never seen.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ob/command.hpp>
#include <span>

namespace ob::wire {

inline constexpr std::uint8_t kVersion = 1;

enum class MsgType : std::uint8_t { New = 1, Cancel = 2 };

inline constexpr std::size_t kHeaderSize = 4;   // u16 length, u8 type, u8 version
inline constexpr std::size_t kNewSize    = 24;  // header + u64 + i32 + u32 + u8 + u8 + u16
inline constexpr std::size_t kCancelSize = 12;  // header + u64
inline constexpr std::size_t kMaxMsgSize = kNewSize;

enum class DecodeError : std::uint8_t {
    None = 0,
    Truncated,       // fewer bytes available than the message claims
    BadLength,       // length disagrees with the type, or is below the header
    UnknownType,     // well-formed frame, unrecognised type
    UnknownVersion,  // well-formed frame, unrecognised version
};

struct DecodeResult {
    DecodeError error    = DecodeError::None;
    std::size_t consumed = 0;  // bytes to advance; nonzero for a skippable frame

    [[nodiscard]] bool ok() const noexcept { return error == DecodeError::None; }
};

[[nodiscard]] constexpr const char* to_string(DecodeError e) noexcept {
    switch (e) {
        case DecodeError::None:
            return "None";
        case DecodeError::Truncated:
            return "Truncated";
        case DecodeError::BadLength:
            return "BadLength";
        case DecodeError::UnknownType:
            return "UnknownType";
        case DecodeError::UnknownVersion:
            return "UnknownVersion";
    }
    return "?";
}

namespace detail {

template <class T>
void put(std::byte* p, std::size_t off, T v) noexcept {
    std::memcpy(p + off, &v, sizeof(T));
}

template <class T>
[[nodiscard]] T get(const std::byte* p, std::size_t off) noexcept {
    T v{};
    std::memcpy(&v, p + off, sizeof(T));
    return v;
}

}  // namespace detail

// Returns bytes written, or 0 if the buffer is too small. Never writes past `out`.
//
// The frame is assembled in a LOCAL fixed-size array and copied out in one guarded
// memcpy, rather than by storing each field straight into the caller's span.
//
// That is not a stylistic preference. Writing fields directly into a runtime-sized
// span makes GCC's -Warray-bounds analyse each store against a size it cannot
// always bound, and it reports the padding store at offset 22 as potentially out of
// bounds of a 23-byte buffer - a false positive, because the guard above makes that
// store unreachable, but a build failure under -Werror all the same. One copy of a
// compile-time-constant size is analysable, and it also puts the whole frame layout
// in one place, which is easier to check against the protocol table.
[[nodiscard]] inline std::size_t encode(const Command& c, std::span<std::byte> out) noexcept {
    if (c.type == CommandType::Cancel) {
        if (out.size() < kCancelSize) {
            return 0;
        }
        std::array<std::byte, kCancelSize> f{};
        detail::put<std::uint16_t>(f.data(), 0, static_cast<std::uint16_t>(kCancelSize));
        detail::put<std::uint8_t>(f.data(), 2, static_cast<std::uint8_t>(MsgType::Cancel));
        detail::put<std::uint8_t>(f.data(), 3, kVersion);
        detail::put<std::uint64_t>(f.data(), 4, c.id);
        std::memcpy(out.data(), f.data(), kCancelSize);
        return kCancelSize;
    }

    if (out.size() < kNewSize) {
        return 0;
    }
    std::array<std::byte, kNewSize> f{};
    detail::put<std::uint16_t>(f.data(), 0, static_cast<std::uint16_t>(kNewSize));
    detail::put<std::uint8_t>(f.data(), 2, static_cast<std::uint8_t>(MsgType::New));
    detail::put<std::uint8_t>(f.data(), 3, kVersion);
    detail::put<std::uint64_t>(f.data(), 4, c.id);
    detail::put<std::int32_t>(f.data(), 12, c.price);
    detail::put<std::uint32_t>(f.data(), 16, c.qty);
    detail::put<std::uint8_t>(f.data(), 20, static_cast<std::uint8_t>(c.side));
    detail::put<std::uint8_t>(f.data(), 21, static_cast<std::uint8_t>(c.order_type));
    detail::put<std::uint16_t>(f.data(), 22, 0);  // explicit pad; never implicit layout
    std::memcpy(out.data(), f.data(), kNewSize);
    return kNewSize;
}

// Decodes one message from the front of `in`.
//
// Validation order is deliberate: header presence, then the declared length against
// the BUFFER, then the length against the TYPE. Checking the type's size first would
// lose frame sync on a truncated buffer, because the caller would not know how far
// to advance.
[[nodiscard]] inline DecodeResult decode(std::span<const std::byte> in, Command& out) noexcept {
    if (in.size() < kHeaderSize) {
        return {DecodeError::Truncated, 0};
    }
    const std::byte* p       = in.data();
    const auto       len     = detail::get<std::uint16_t>(p, 0);
    const auto       type    = detail::get<std::uint8_t>(p, 2);
    const auto       version = detail::get<std::uint8_t>(p, 3);

    if (len < kHeaderSize || len > kMaxMsgSize) {
        // Unframeable: the caller cannot know how far to skip, so consume nothing.
        return {DecodeError::BadLength, 0};
    }
    if (in.size() < len) {
        return {DecodeError::Truncated, 0};
    }
    // From here the frame is intact, so every remaining error is SKIPPABLE: report
    // `len` consumed so the caller stays in sync.
    if (version != kVersion) {
        return {DecodeError::UnknownVersion, len};
    }

    switch (static_cast<MsgType>(type)) {
        case MsgType::New: {
            if (len != kNewSize) {
                return {DecodeError::BadLength, len};
            }
            out           = Command{};
            out.type      = CommandType::New;
            out.id        = detail::get<std::uint64_t>(p, 4);
            out.price     = detail::get<std::int32_t>(p, 12);
            out.qty       = detail::get<std::uint32_t>(p, 16);
            const auto s  = detail::get<std::uint8_t>(p, 20);
            const auto ot = detail::get<std::uint8_t>(p, 21);
            // Enum ranges are validated here, not trusted. An out-of-range value in
            // an enum is UB the moment anything switches on it.
            if (s > static_cast<std::uint8_t>(Side::Sell)) {
                return {DecodeError::BadLength, len};
            }
            if (ot > static_cast<std::uint8_t>(OrderType::PostOnly)) {
                return {DecodeError::BadLength, len};
            }
            out.side       = static_cast<Side>(s);
            out.order_type = static_cast<OrderType>(ot);
            return {DecodeError::None, len};
        }
        case MsgType::Cancel: {
            if (len != kCancelSize) {
                return {DecodeError::BadLength, len};
            }
            out       = Command{};
            out.type  = CommandType::Cancel;
            out.id    = detail::get<std::uint64_t>(p, 4);
            out.price = kNoPrice;
            out.qty   = 0;
            return {DecodeError::None, len};
        }
        default:
            // A well-framed message of an unknown type is skippable, which is the
            // whole reason the length lives in the header.
            return {DecodeError::UnknownType, len};
    }
}

}  // namespace ob::wire
