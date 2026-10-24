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
constexpr uint8_t PROTOCOL_VERSION = 5;

enum class Type : uint8_t {
  Hello = 1,  // pairing and heartbeat: role, version, book id, layout hash, whether the sender considers itself linked
  State = 2,  // left -> right: chapter and page the pair shows
  Key = 3,    // right -> left: a button press the left unit carries out (see KeyAction)
  Sleep = 4,  // both units go to sleep
  Ack = 5,    // echoes the seq of a State
  Book = 6,   // left -> right: path of the open book, repeated while the right unit has another one open
  Marks = 7,  // left -> right: one chunk of the book's bookmark file (see MarksChunk)
  MarksAck = 8,    // right -> left: the crc of a bookmark file received whole
  KeyAck = 9,      // left -> right: echoes the seq of a Key
  Layout = 10,     // either way: the sender's text settings, for the other unit to adopt (see LAYOUT_BLOB_LEN)
  LayoutAck = 11,  // echoes the crc of a Layout
  Goto = 12,       // right -> left: jump to a place in the book (see JumpRequest)
  GotoAck = 13,    // echoes the seq of a Goto
  End = 15,        // either way: the sender ended the link; the peer ends it too
  Close = 14,      // either way: the sender left this book (its id); a peer reading it leaves too
};

constexpr size_t HELLO_LEN = 16;
constexpr size_t PAGE_STATE_LEN = 6;
constexpr size_t LAYOUT_BLOB_LEN = 46;  // 14 one-byte text settings and a 32-byte SD font name
constexpr size_t LAYOUT_LEN = 2 + LAYOUT_BLOB_LEN;
constexpr size_t KEY_LEN = 3;           // seq, action
constexpr size_t CLOSE_LEN = 4;         // book id
constexpr size_t GOTO_LEN = 11;         // seq, spine, page, flags, visible text offset
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
    case Type::End:
      return len == 0;
    case Type::Key:
      return len == KEY_LEN;
    case Type::KeyAck:
      return len == 2;
    case Type::Goto:
      return len == GOTO_LEN;
    case Type::GotoAck:
      return len == 2;
    case Type::Close:
      return len == CLOSE_LEN;
    case Type::Layout:
      return len == LAYOUT_LEN;
    case Type::LayoutAck:
      return len == 2;
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

// What the two screens share: night mode and the frontlight. Carried on every Hello; the units follow each other's.
struct DisplayState {
  bool night = false;
  bool lightOn = false;
  uint8_t brightness = 0;  // 0..100
  uint8_t warmth = 0;      // 0..100
  bool locked = false;     // touch lock (see HalGPIO::setTouchLocked)
  bool operator==(const DisplayState& o) const {
    return night == o.night && lightOn == o.lightOn && brightness == o.brightness && warmth == o.warmth &&
           locked == o.locked;
  }
  bool operator!=(const DisplayState& o) const { return !(*this == o); }
};

// Which parts of a DisplayState differ. The units follow each other part by part, so a change of one part does not
// undo a different change made on this unit at the same moment.
enum DisplayPart : uint8_t { DISPLAY_NIGHT = 1, DISPLAY_LIGHT = 2, DISPLAY_LOCK = 4 };

inline uint8_t displayDiff(const DisplayState& a, const DisplayState& b) {
  uint8_t m = 0;
  if (a.night != b.night) m |= DISPLAY_NIGHT;
  if (a.lightOn != b.lightOn || a.brightness != b.brightness || a.warmth != b.warmth) m |= DISPLAY_LIGHT;
  if (a.locked != b.locked) m |= DISPLAY_LOCK;
  return m;
}

struct Hello {
  Role role = Role::Left;
  uint8_t version = PROTOCOL_VERSION;
  uint32_t bookId = 0;      // hash of the book: both SD cards must hold the same book
  uint32_t layoutHash = 0;  // hash of everything that changes pagination (fonts, size, margins, orientation, firmware)
  bool linked = false;      // the sender has heard a compatible Hello and considers the link up
  DisplayState display;
};

inline size_t encodeHello(const Hello& h, uint8_t* out) {
  uint8_t p[HELLO_LEN];
  p[0] = static_cast<uint8_t>(h.role);
  p[1] = h.version;
  put32(p + 2, h.bookId);
  put32(p + 6, h.layoutHash);
  p[10] = h.linked ? 1 : 0;
  p[11] = h.display.night ? 1 : 0;
  p[12] = h.display.lightOn ? 1 : 0;
  p[13] = h.display.brightness;
  p[14] = h.display.warmth;
  p[15] = h.display.locked ? 1 : 0;
  return encode(Type::Hello, p, sizeof(p), out);
}

inline bool decodeHello(const Frame& f, Hello& h) {
  if (f.type != Type::Hello || f.len != HELLO_LEN || f.payload[0] > 1 || f.payload[10] > 1 || f.payload[11] > 1 ||
      f.payload[12] > 1 || f.payload[13] > 100 || f.payload[14] > 100 || f.payload[15] > 1) {
    return false;
  }
  h.role = static_cast<Role>(f.payload[0]);
  h.version = f.payload[1];
  h.bookId = get32(f.payload + 2);
  h.layoutHash = get32(f.payload + 6);
  h.linked = f.payload[10] != 0;
  h.display.night = f.payload[11] != 0;
  h.display.lightOn = f.payload[12] != 0;
  h.display.brightness = f.payload[13];
  h.display.warmth = f.payload[14];
  h.display.locked = f.payload[15] != 0;
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

inline size_t encodeLayout(uint16_t crc, const uint8_t* blob, uint8_t* out) {
  uint8_t p[LAYOUT_LEN];
  put16(p, crc);
  memcpy(p + 2, blob, LAYOUT_BLOB_LEN);
  return encode(Type::Layout, p, sizeof(p), out);
}

inline size_t encodeClose(uint32_t bookId, uint8_t* out) {
  uint8_t p[CLOSE_LEN];
  put32(p, bookId);
  return encode(Type::Close, p, sizeof(p), out);
}

inline bool decodeClose(const Frame& f, uint32_t& bookId) {
  if (f.type != Type::Close || f.len != CLOSE_LEN) return false;
  bookId = get32(f.payload);
  return true;
}

// A place in the book the right unit wants to see (a bookmark or chapter picked there). The left unit owns the position
// and moves to it: the chapter, and the page holding the text offset when there is one, else the page number.
enum class JumpKind : uint8_t {
  Page = 0,      // the chapter's page number
  Offset = 1,    // the page holding this visible text offset
  Fraction = 2,  // this part of the chapter, in millionths (go to percent)
};

struct JumpRequest {
  uint16_t spine = 0;
  uint16_t page = 0;
  JumpKind kind = JumpKind::Page;
  uint32_t value = 0;  // the text offset or the fraction, by kind
};

inline size_t encodeGoto(uint16_t seq, const JumpRequest& j, uint8_t* out) {
  uint8_t p[GOTO_LEN];
  put16(p, seq);
  put16(p + 2, j.spine);
  put16(p + 4, j.page);
  p[6] = static_cast<uint8_t>(j.kind);
  put32(p + 7, j.value);
  return encode(Type::Goto, p, sizeof(p), out);
}

inline bool decodeGoto(const Frame& f, uint16_t& seq, JumpRequest& j) {
  if (f.type != Type::Goto || f.len != GOTO_LEN || f.payload[6] > static_cast<uint8_t>(JumpKind::Fraction))
    return false;
  seq = get16(f.payload);
  j.spine = get16(f.payload + 2);
  j.page = get16(f.payload + 4);
  j.kind = static_cast<JumpKind>(f.payload[6]);
  j.value = get32(f.payload + 7);
  return true;
}

// What the right unit asks the left unit to do. The left unit owns the position, so the right unit's page buttons are
// requests, not page turns.
// ToggleBookmark: toggle the bookmark of the page after the left unit's, the one the right unit shows.
enum class KeyAction : uint8_t { NextPage = 1, PrevPage = 2, ToggleBookmark = 3 };

inline size_t encodeKey(uint16_t seq, KeyAction action, uint8_t* out) {
  uint8_t p[KEY_LEN];
  put16(p, seq);
  p[2] = static_cast<uint8_t>(action);
  return encode(Type::Key, p, sizeof(p), out);
}

inline bool decodeKey(const Frame& f, uint16_t& seq, KeyAction& action) {
  if (f.type != Type::Key || f.len != KEY_LEN) return false;
  if (f.payload[2] != static_cast<uint8_t>(KeyAction::NextPage) &&
      f.payload[2] != static_cast<uint8_t>(KeyAction::PrevPage) &&
      f.payload[2] != static_cast<uint8_t>(KeyAction::ToggleBookmark)) {
    return false;
  }
  seq = get16(f.payload);
  action = static_cast<KeyAction>(f.payload[2]);
  return true;
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
