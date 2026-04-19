#include <doctest.h>

#include "Fs/IBinaryStream.h"
#include "Fs/MemBinaryStream.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace wallpaper;
using namespace wallpaper::fs;

// Helper: build a little-endian byte vector of assorted int sizes + floats.
static std::vector<uint8_t> makeBytes() {
    std::vector<uint8_t> v;
    // int8: 0x42  (66)
    v.push_back(0x42);
    // int8: 0xFF  (-1)
    v.push_back(0xFF);
    // uint16 little-endian: 0x1234
    v.push_back(0x34); v.push_back(0x12);
    // int16 little-endian: -1 → 0xFFFF
    v.push_back(0xFF); v.push_back(0xFF);
    // uint32 little-endian: 0xDEADBEEF
    v.push_back(0xEF); v.push_back(0xBE); v.push_back(0xAD); v.push_back(0xDE);
    // int32 little-endian: -2 → 0xFFFFFFFE
    v.push_back(0xFE); v.push_back(0xFF); v.push_back(0xFF); v.push_back(0xFF);
    // uint64 little-endian: 0x0001020304050607
    v.push_back(0x07); v.push_back(0x06); v.push_back(0x05); v.push_back(0x04);
    v.push_back(0x03); v.push_back(0x02); v.push_back(0x01); v.push_back(0x00);
    return v;
}

TEST_SUITE("IBinaryStream.ReadInts") {

TEST_CASE("Read*Int8/16/32/64 in little-endian") {
    auto            data = makeBytes();
    MemBinaryStream f(std::move(data));
    CHECK(f.ReadUint8() == 0x42u);
    CHECK(f.ReadInt8() == -1);
    CHECK(f.ReadUint16() == 0x1234u);
    CHECK(f.ReadInt16() == -1);
    CHECK(f.ReadUint32() == 0xDEADBEEFu);
    CHECK(f.ReadInt32() == -2);
    CHECK(f.ReadUint64() == 0x0001020304050607ull);
}

TEST_CASE("ReadInt32 at EOF returns 0 default") {
    std::vector<uint8_t> v;
    MemBinaryStream      f(std::move(v));
    CHECK(f.ReadInt32() == 0);
    CHECK(f.ReadInt64() == 0);
}

TEST_CASE("ReadFloat reads raw bytes as a float") {
    std::vector<uint8_t> v;
    float                val = 1.5f;
    uint32_t             bits;
    std::memcpy(&bits, &val, 4);
    for (int i = 0; i < 4; i++) v.push_back((bits >> (i * 8)) & 0xff);

    MemBinaryStream f(std::move(v));
    CHECK(f.ReadFloat() == doctest::Approx(1.5f));
}

TEST_CASE("ReadInt64 / ReadInt8 signed paths") {
    std::vector<uint8_t> v;
    // int64 = -5 = 0xFFFFFFFFFFFFFFFB
    for (int i = 0; i < 8; i++) v.push_back(i == 0 ? 0xFB : 0xFF);
    // int8 = -10 = 0xF6
    v.push_back(0xF6);

    MemBinaryStream f(std::move(v));
    CHECK(f.ReadInt64() == -5);
    CHECK(f.ReadInt8() == -10);
}

} // ReadInts

TEST_SUITE("IBinaryStream.Strings") {

TEST_CASE("ReadStr reads up to null terminator") {
    std::vector<uint8_t> v = { 'h', 'i', '\0', 'j', 'k' };
    MemBinaryStream      f(std::move(v));
    CHECK(f.ReadStr() == "hi");
    // Next ReadStr continues from after the null.
    CHECK(f.ReadStr() == "jk");
}

TEST_CASE("ReadStr at EOF returns empty") {
    std::vector<uint8_t> v;
    MemBinaryStream      f(std::move(v));
    CHECK(f.ReadStr().empty());
}

TEST_CASE("ReadAllStr returns entire contents as string") {
    std::vector<uint8_t> v = { 'a', 'b', 'c', 'd' };
    MemBinaryStream      f(std::move(v));
    std::string s = f.ReadAllStr();
    CHECK(s.size() == 4);
    CHECK(s == "abcd");
}

TEST_CASE("Gets forwards to Read") {
    std::vector<uint8_t> v = { 'x', 'y', 'z' };
    MemBinaryStream      f(std::move(v));
    char                 buf[4] { 0, 0, 0, 0 };
    f.Gets(buf, 3);
    CHECK(buf[0] == 'x');
    CHECK(buf[1] == 'y');
    CHECK(buf[2] == 'z');
}

} // Strings

TEST_SUITE("IBinaryStream.Seek") {

TEST_CASE("SeekSet / Tell") {
    MemBinaryStream f(std::vector<uint8_t>(10, 0));
    CHECK(f.Tell() == 0);
    CHECK(f.SeekSet(5));
    CHECK(f.Tell() == 5);
    CHECK(f.SeekSet(0));
    CHECK(f.Tell() == 0);
}

TEST_CASE("SeekSet beyond end returns false") {
    MemBinaryStream f(std::vector<uint8_t>(4, 0));
    CHECK_FALSE(f.SeekSet(100));
    CHECK(f.Tell() == 0); // unchanged on failure
}

TEST_CASE("SeekCur is relative to current position") {
    MemBinaryStream f(std::vector<uint8_t>(10, 0));
    f.SeekSet(3);
    CHECK(f.SeekCur(2));
    CHECK(f.Tell() == 5);
    CHECK(f.SeekCur(-1));
    CHECK(f.Tell() == 4);
    CHECK_FALSE(f.SeekCur(100));
}

TEST_CASE("SeekEnd uses Size as anchor") {
    MemBinaryStream f(std::vector<uint8_t>(10, 0));
    CHECK(f.SeekEnd(0));
    CHECK(f.Tell() == 10);
    CHECK(f.SeekEnd(-3));
    CHECK(f.Tell() == 7);
    CHECK_FALSE(f.SeekEnd(-100));
}

TEST_CASE("Rewind returns to offset 0") {
    MemBinaryStream f(std::vector<uint8_t>(10, 0));
    f.SeekSet(5);
    CHECK(f.Rewind());
    CHECK(f.Tell() == 0);
}

TEST_CASE("Size / Usize are consistent") {
    MemBinaryStream f(std::vector<uint8_t>(42, 0));
    CHECK(f.Size() == 42);
    CHECK(f.Usize() == 42u);
}

} // Seek

TEST_SUITE("IBinaryStream.ByteOrder") {

TEST_CASE("SetByteOrder BigEndian swaps subsequent reads") {
    // Raw bytes 0x12 0x34 little-endian = 0x3412; with BigEndian mode → 0x1234
    std::vector<uint8_t> v = { 0x12, 0x34 };
    MemBinaryStream      f(std::move(v));
    f.SetByteOrder(IBinaryStream::ByteOrder::BigEndian);
    CHECK(f.ReadUint16() == 0x1234u);
}

TEST_CASE("SetByteOrder LittleEndian keeps raw order") {
    std::vector<uint8_t> v = { 0x12, 0x34 };
    MemBinaryStream      f(std::move(v));
    f.SetByteOrder(IBinaryStream::ByteOrder::LittleEndian);
    CHECK(f.ReadUint16() == 0x3412u);
}

} // ByteOrder

TEST_SUITE("MemBinaryStream.StreamConstructor") {

TEST_CASE("construct from another IBinaryStream copies data") {
    std::vector<uint8_t> src { 1, 2, 3, 4, 5 };
    MemBinaryStream      a(std::move(src));
    IBinaryStream&       a_as_base = a;
    MemBinaryStream      b(a_as_base); // IBinaryStream& ctor — reads into new buffer
    CHECK(b.Size() == 5);
    CHECK(b.ReadUint8() == 1u);
    CHECK(b.ReadUint8() == 2u);
}

} // StreamConstructor
