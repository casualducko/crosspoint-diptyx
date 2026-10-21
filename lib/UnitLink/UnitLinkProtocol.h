#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Framing and messages for the two-unit link (UART over the pogo data pads).
// Frame: [SOF][type][len][payload (len bytes)][crc16 lo][crc16 hi], CRC-16/CCITT-FALSE over type, len and payload.
// The parser resynchronises on the next SOF after any error, so line noise costs only the frames it overlaps.

namespace unitlink {

constexpr uint8_t SOF = 0xA5;
constexpr size_t MAX_PAYLOAD = 200;  // room for a book path
constexpr size_t FRAME_OVERHEAD = 5;
constexpr uint8_t PROTOCOL_VERSION = 3;

enum class Type : uint8_t {
  Hello = 1,  // pairing and heartbeat: role, version, book id, layout hash, whether the sender considers itself linked
  State = 2,  // left -> right: chapter and page the pair shows
  Key = 3,    // reserved: a forwarded button press
  Sleep = 4,  // both units go to sleep
  Ack = 5,    // echoes the seq of a State
  Book = 6,   // left -> right: path of the open book, repeated while the right unit has another one open
  Marks = 7,  // left -> right: one chunk of the book's bookmark file (see MarksChunk)
  MarksAck = 8,  // right -> left: the crc of a bookmark file received whole
};

constexpr size_t HELLO_LEN = 11;
constexpr size_t PAGE_STATE_LEN = 6;
constexpr size_t MARKS_HEADER_LEN = 6;  // crc of the whole file, offset of this chunk, size of the whole file
constexpr size_t MARKS_CHUNK_DATA = MAX_PAYLOAD - MARKS_HEADER_LEN;
constexpr size_t MARKS_MAX_TOTAL = 16384;

// Every message has a fixed payload size except Book (a path). Checking the size as soon as it arrives lets the parser
// drop a stray SOF from line noise at once instead of waiting for a long payload that will never come.
inline bool lengthAllowed(Type type, size_t len) {
  switch (type) {
    case Type::Hello:
      return len == HELLO_LEN;
    case Type::State:
      return len == PAGE_STATE_LEN;
    case Type::Ack:
      return len == 2;
    case Type::Sleep:
      return len == 0;
    case Type::Key:
      return len <= 8;
    case Type::Book:
      return len >= 1 && len <= MAX_PAYLOAD;
    case Type::Marks:
      return len > MARKS_HEADER_LEN && len <= MAX_PAYLOAD;
    case Type::MarksAck:
      return len == 2;
  }
  return false;
}

// Left shows page N, right shows page N + 1; the roles also fix which pogo pad each unit sends on.
enum class Role : uint8_t { Left = 0, Right = 1 };

struct Frame {
  Type type = Type::Hello;
  uint8_t len = 0;
  uint8_t payload[MAX_PAYLOAD] = {};
};

inline uint16_t crc16(const uint8_t* data, size_t n, uint16_t crc = 0xFFFF) {
  for (size_t i = 0; i < n; i++) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

// Writes the frame to out (needs len + FRAME_OVERHEAD bytes); returns the byte count, 0 if the payload is too long.
inline size_t encode(Type type, const uint8_t* payload, size_t len, uint8_t* out) {
  if (len > MAX_PAYLOAD) return 0;
  out[0] = SOF;
  out[1] = static_cast<uint8_t>(type);
  out[2] = static_cast<uint8_t>(len);
  if (len) memcpy(out + 3, payload, len);
  const uint16_t crc = crc16(out + 1, len + 2);
  out[3 + len] = static_cast<uint8_t>(crc & 0xFF);
  out[4 + len] = static_cast<uint8_t>(crc >> 8);
  return len + FRAME_OVERHEAD;
}

// Byte-at-a-time parser; push() returns true when a CRC-valid frame is available from frame().
// A false start (a stray SOF byte in line noise) must not swallow the real frame behind it, so when a candidate frame
// fails, its bytes after the SOF are fed through the parser again.
class Parser {
 public:
  bool push(uint8_t c) {
    if (state == Step::Sof) {
      if (c == SOF) {
        raw[0] = c;
        rawLen = 1;
        state = Step::Type;
      }
      return false;
    }
    raw[rawLen++] = c;
    switch (state) {
      case Step::Sof:
        return false;
      case Step::Type:
        f.type = static_cast<Type>(c);
        crc = crc16(&c, 1);
        state = Step::Len;
        return false;
      case Step::Len:
        if (c > MAX_PAYLOAD || !lengthAllowed(f.type, c)) {
          errors++;
          return resync();
        }
        f.len = c;
        crc = crc16(&c, 1, crc);
        got = 0;
        state = c ? Step::Payload : Step::CrcLo;
        return false;
      case Step::Payload:
        f.payload[got++] = c;
        crc = crc16(&c, 1, crc);
        if (got == f.len) state = Step::CrcLo;
        return false;
      case Step::CrcLo:
        crcLo = c;
        state = Step::CrcHi;
        return false;
      case Step::CrcHi: {
        const uint16_t rx = static_cast<uint16_t>(crcLo | (static_cast<uint16_t>(c) << 8));
        if (rx == crc) {
          state = Step::Sof;
          return true;
        }
        errors++;
        return resync();
      }
    }
    return false;
  }
  const Frame& frame() const { return f; }
  uint32_t crcErrors() const { return errors; }
  void reset() {
    state = Step::Sof;
    rawLen = 0;
  }

 private:
  enum class Step : uint8_t { Sof, Type, Len, Payload, CrcLo, CrcHi };

  // Drops the candidate's SOF and feeds the rest back in, which finds a frame that began inside it.
  bool resync() {
    uint8_t again[MAX_PAYLOAD + FRAME_OVERHEAD];
    const size_t n = rawLen - 1;
    memcpy(again, raw + 1, n);
    state = Step::Sof;
    rawLen = 0;
    for (size_t i = 0; i < n; i++) {
      if (push(again[i])) return true;
    }
    return false;
  }

  uint8_t raw[MAX_PAYLOAD + FRAME_OVERHEAD] = {};
  size_t rawLen = 0;
  Step state = Step::Sof;
  Frame f;
  uint16_t crc = 0;
  uint8_t crcLo = 0;
  uint8_t got = 0;
  uint32_t errors = 0;
};

// FNV-1a, used to fold the book and the layout settings into the 32-bit ids that Hello carries.
constexpr uint32_t FNV_OFFSET = 2166136261u;
inline uint32_t fnv1a(const void* data, size_t n, uint32_t h = FNV_OFFSET) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}

inline void put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}
inline uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

struct Hello {
  Role role = Role::Left;
  uint8_t version = PROTOCOL_VERSION;
  uint32_t bookId = 0;      // hash of the book: both SD cards must hold the same book
  uint32_t layoutHash = 0;  // hash of everything that changes pagination (fonts, size, margins, orientation, firmware)
  bool linked = false;      // the sender has heard a compatible Hello and considers the link up
};

inline size_t encodeHello(const Hello& h, uint8_t* out) {
  uint8_t p[HELLO_LEN];
  p[0] = static_cast<uint8_t>(h.role);
  p[1] = h.version;
  put32(p + 2, h.bookId);
  put32(p + 6, h.layoutHash);
  p[10] = h.linked ? 1 : 0;
  return encode(Type::Hello, p, sizeof(p), out);
}

inline bool decodeHello(const Frame& f, Hello& h) {
  if (f.type != Type::Hello || f.len != HELLO_LEN || f.payload[0] > 1) return false;
  h.role = static_cast<Role>(f.payload[0]);
  h.version = f.payload[1];
  h.bookId = get32(f.payload + 2);
  h.layoutHash = get32(f.payload + 6);
  h.linked = f.payload[10] != 0;
  return true;
}

struct PageState {
  uint16_t seq = 0;    // grows with every new position; the right unit acks it
  uint16_t spine = 0;  // chapter index
  uint16_t page = 0;   // page shown by the left unit; the right unit shows page + 1
};

inline size_t encodePageState(const PageState& s, uint8_t* out) {
  uint8_t p[PAGE_STATE_LEN];
  put16(p, s.seq);
  put16(p + 2, s.spine);
  put16(p + 4, s.page);
  return encode(Type::State, p, sizeof(p), out);
}

inline bool decodePageState(const Frame& f, PageState& s) {
  if (f.type != Type::State || f.len != PAGE_STATE_LEN) return false;
  s.seq = get16(f.payload);
  s.spine = get16(f.payload + 2);
  s.page = get16(f.payload + 4);
  return true;
}

// Ack (a State's seq) and MarksAck (a file's crc) share one shape: a 16-bit number.
inline size_t encodeAck(uint16_t seq, uint8_t* out, Type type = Type::Ack) {
  uint8_t p[2];
  put16(p, seq);
  return encode(type, p, sizeof(p), out);
}

inline bool decodeAck(const Frame& f, uint16_t& seq, Type type = Type::Ack) {
  if (f.type != type || f.len != 2) return false;
  seq = get16(f.payload);
  return true;
}

// A chunk of the file: [crc16 of the whole file][offset u16][total size u16][data]. The crc names the file, so the
// receiver can tell a repeat from news and check the file once it is whole.
inline size_t encodeMarksChunk(uint16_t fileCrc, uint16_t offset, uint16_t total, const uint8_t* data, size_t n,
                               uint8_t* out) {
  uint8_t p[MAX_PAYLOAD];
  if (n > MARKS_CHUNK_DATA) return 0;
  put16(p, fileCrc);
  put16(p + 2, offset);
  put16(p + 4, total);
  memcpy(p + MARKS_HEADER_LEN, data, n);
  return encode(Type::Marks, p, MARKS_HEADER_LEN + n, out);
}

// Why two units cannot pair; a bit mask so the screen can name every cause.
enum IncompatibleReason : uint8_t {
  INCOMPATIBLE_VERSION = 1,
  INCOMPATIBLE_BOOK = 2,
  INCOMPATIBLE_LAYOUT = 4,
  INCOMPATIBLE_ROLE = 8,  // both units chose the same side
};

inline uint8_t incompatibleReasons(const Hello& a, const Hello& b) {
  uint8_t r = 0;
  if (a.version != b.version) r |= INCOMPATIBLE_VERSION;
  if (a.bookId != b.bookId) r |= INCOMPATIBLE_BOOK;
  if (a.layoutHash != b.layoutHash) r |= INCOMPATIBLE_LAYOUT;
  if (a.role == b.role) r |= INCOMPATIBLE_ROLE;
  return r;
}

}  // namespace unitlink
