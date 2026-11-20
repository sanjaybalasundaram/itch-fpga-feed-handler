// messages.hpp - NASDAQ TotalView-ITCH 5.0 message layouts used for book building.
// All integers on the wire are big-endian. Field offsets follow the ITCH 5.0 spec.
#pragma once
#include <cstdint>
#include <cstring>

namespace itch {

// ---- big-endian loads (memcpy keeps it alignment-safe; compiles to mov + bswap)
inline uint16_t be16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return __builtin_bswap16(v); }
inline uint32_t be32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v); }
inline uint64_t be64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return __builtin_bswap64(v); }
inline uint64_t be48(const uint8_t* p) { return (uint64_t(be16(p)) << 32) | be32(p + 2); }

enum class Side : uint8_t { Buy = 'B', Sell = 'S' };

struct Header {
    uint16_t locate;      // stock locate code
    uint64_t timestamp;   // ns since midnight (48-bit on the wire)
};

struct AddOrder     { Header h; uint64_t ref; Side side; uint32_t shares; uint32_t price; };
struct OrderExecuted{ Header h; uint64_t ref; uint32_t shares; };          // 'E' and 'C'
struct OrderCancel  { Header h; uint64_t ref; uint32_t shares; };
struct OrderDelete  { Header h; uint64_t ref; };
struct OrderReplace { Header h; uint64_t ref; uint64_t new_ref; uint32_t shares; uint32_t price; };

// Fixed message lengths from the ITCH 5.0 spec (0 = not a book message we decode).
constexpr uint16_t kLenAdd = 36, kLenAddMpid = 40, kLenExec = 31, kLenExecPx = 36,
                   kLenCancel = 23, kLenDelete = 19, kLenReplace = 35;

inline Header decode_header(const uint8_t* m) { return {be16(m + 1), be48(m + 5)}; }

}  // namespace itch
