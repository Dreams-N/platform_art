/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "utf.h"

#include "common_runtime_test.h"
#include "utf-inl.h"

#include <vector>

namespace art {

class UtfTest : public CommonRuntimeTest {};

TEST_F(UtfTest, GetLeadingUtf16Char) {
  EXPECT_EQ(0xffff, GetLeadingUtf16Char(0xeeeeffff));
}

TEST_F(UtfTest, GetTrailingUtf16Char) {
  EXPECT_EQ(0xffff, GetTrailingUtf16Char(0xffffeeee));
  EXPECT_EQ(0, GetTrailingUtf16Char(0x0000aaaa));
}

#define EXPECT_ARRAY_POSITION(expected, end, start) \
  EXPECT_EQ(static_cast<uintptr_t>(expected), \
            reinterpret_cast<uintptr_t>(end) - reinterpret_cast<uintptr_t>(start));

// A test string containing one, two, three and four byte UTF-8 sequences.
static const uint8_t kAllSequences[] = {
    0x24,
    0xc2, 0xa2,
    0xe2, 0x82, 0xac,
    0xf0, 0x9f, 0x8f, 0xa0,
    0x00
};

// A test string that contains a UTF-8 encoding of a surrogate pair
// (code point = U+10400)
static const uint8_t kSurrogateEncoding[] = {
    0xed, 0xa0, 0x81,
    0xed, 0xb0, 0x80,
    0x00
};

TEST_F(UtfTest, GetUtf16FromUtf8) {
  const char* const start = reinterpret_cast<const char*>(kAllSequences);
  const char* ptr = start;
  uint32_t pair = 0;

  // Single byte sequence.
  pair = GetUtf16FromUtf8(&ptr);
  EXPECT_EQ(0x24, GetLeadingUtf16Char(pair));
  EXPECT_EQ(0, GetTrailingUtf16Char(pair));
  EXPECT_ARRAY_POSITION(1, ptr, start);

  // Two byte sequence
  pair = GetUtf16FromUtf8(&ptr);
  EXPECT_EQ(0xa2, GetLeadingUtf16Char(pair));
  EXPECT_EQ(0, GetTrailingUtf16Char(pair));
  EXPECT_ARRAY_POSITION(3, ptr, start);

  // Three byte sequence
  pair = GetUtf16FromUtf8(&ptr);
  EXPECT_EQ(0x20ac, GetLeadingUtf16Char(pair));
  EXPECT_EQ(0, GetTrailingUtf16Char(pair));
  EXPECT_ARRAY_POSITION(6, ptr, start);

  // Four byte sequence
  pair = GetUtf16FromUtf8(&ptr);
  EXPECT_EQ(0xd83c, GetLeadingUtf16Char(pair));
  EXPECT_EQ(0xdfe0, GetTrailingUtf16Char(pair));
  EXPECT_ARRAY_POSITION(10, ptr, start);

  // Null terminator
  pair = GetUtf16FromUtf8(&ptr);
  EXPECT_EQ(0, GetLeadingUtf16Char(pair));
  EXPECT_EQ(0, GetTrailingUtf16Char(pair));
  EXPECT_ARRAY_POSITION(11, ptr, start);
}

TEST_F(UtfTest, GetUtf16FromUtf8_SurrogatesPassThrough) {
  const char* const start = reinterpret_cast<const char *>(kSurrogateEncoding);
  const char* ptr = start;
  uint32_t pair = 0;

  pair = GetUtf16FromUtf8(&ptr);
  EXPECT_EQ(0xd801, GetLeadingUtf16Char(pair));
  EXPECT_EQ(0, GetTrailingUtf16Char(pair));
  EXPECT_ARRAY_POSITION(3, ptr, start);

  pair = GetUtf16FromUtf8(&ptr);
  EXPECT_EQ(0xdc00, GetLeadingUtf16Char(pair));
  EXPECT_EQ(0, GetTrailingUtf16Char(pair));
  EXPECT_ARRAY_POSITION(6, ptr, start);
}

TEST_F(UtfTest, CountModifiedUtf8Chars) {
  EXPECT_EQ(5u, CountModifiedUtf8Chars(reinterpret_cast<const char *>(kAllSequences)));
  EXPECT_EQ(2u, CountModifiedUtf8Chars(reinterpret_cast<const char *>(kSurrogateEncoding)));
}

static void AssertConversion(const std::vector<uint16_t> input,
                             const std::vector<uint8_t> expected) {
  ASSERT_EQ(expected.size(), CountUtf8Bytes(&input[0], input.size()));

  std::vector<uint8_t> output(expected.size());
  ConvertUtf16ToModifiedUtf8(reinterpret_cast<char*>(&output[0]), &input[0],
                             expected.size(), input.size());
  EXPECT_EQ(expected, output);
}

TEST_F(UtfTest, CountAndConvertUtf8Bytes) {
  // Surrogate pairs will be converted into 4 byte sequences.
  AssertConversion({ 0xd801, 0xdc00 }, { 0xf0, 0x90, 0x90, 0x80 });

  // Three byte encodings that are below & above the leading surrogate
  // range respectively.
  AssertConversion({ 0xdef0 }, { 0xed, 0xbb, 0xb0 });
  AssertConversion({ 0xdcff }, { 0xed, 0xb3, 0xbf });
  // Two byte encoding.
  AssertConversion({ 0x0101 }, { 0xc4, 0x81 });

  // Two byte special case : 0 must use an overlong encoding.
  AssertConversion({ 0x0101, 0x0000 }, { 0xc4, 0x81, 0xc0, 0x80 });

  // One byte encoding.
  AssertConversion({ 'h', 'e', 'l', 'l', 'o' }, { 0x68, 0x65, 0x6c, 0x6c, 0x6f });

  AssertConversion({
      0xd802, 0xdc02,  // Surrogate pair
      0xdef0, 0xdcff,  // Three byte encodings
      0x0101, 0x0000,  // Two byte encodings
      'p'   , 'p'      // One byte encoding
    }, {
      0xf0, 0x90, 0xa0, 0x82,
      0xed, 0xbb, 0xb0, 0xed, 0xb3, 0xbf,
      0xc4, 0x81, 0xc0, 0x80,
      0x70, 0x70
    });
}

TEST_F(UtfTest, CountAndConvertUtf8Bytes_UnpairedSurrogate) {
  // Unpaired trailing surrogate at the end of input.
  AssertConversion({ 'h', 'e', 0xd801 }, { 'h', 'e', 0xed, 0xa0, 0x81 });
  // Unpaired (or incorrectly paired) surrogates in the middle of the input.
  AssertConversion({ 'h', 0xd801, 'e' }, { 'h', 0xed, 0xa0, 0x81, 'e' });
  AssertConversion({ 'h', 0xd801, 0xd801, 'e' }, { 'h', 0xed, 0xa0, 0x81, 0xed, 0xa0, 0x81, 'e' });
  AssertConversion({ 'h', 0xdc00, 0xdc00, 'e' }, { 'h', 0xed, 0xb0, 0x80, 0xed, 0xb0, 0x80, 'e' });
}

// Old versions of functions, here to compare answers with optimized versions

static size_t CountUtf8Bytes_reference(const uint16_t* chars, size_t char_count) {
  size_t result = 0;
  while (char_count--) {
    const uint16_t ch = *chars++;
    if (ch > 0 && ch <= 0x7f) {
      ++result;
    } else if (ch >= 0xd800 && ch <= 0xdbff) {
      if (char_count > 0) {
        const uint16_t ch2 = *chars;
        // If we find a properly paired surrogate, we emit it as a 4 byte
        // UTF sequence. If we find an unpaired leading or trailing surrogate,
        // we emit it as a 3 byte sequence like would have done earlier.
        if (ch2 >= 0xdc00 && ch2 <= 0xdfff) {
          chars++;
          char_count--;

          result += 4;
        } else {
          result += 3;
        }
      } else {
        // This implies we found an unpaired trailing surrogate at the end
        // of a string.
        result += 3;
      }
    } else if (ch > 0x7ff) {
      result += 3;
    } else {
      result += 2;
    }
  }
  return result;
}

static void ConvertUtf16ToModifiedUtf8_reference(char* utf8_out, const uint16_t* utf16_in,
                                                 size_t char_count) {
  while (char_count--) {
    const uint16_t ch = *utf16_in++;
    if (ch > 0 && ch <= 0x7f) {
      *utf8_out++ = ch;
    } else {
      // char_count == 0 here implies we've encountered an unpaired
      // surrogate and we have no choice but to encode it as 3-byte UTF
      // sequence. Note that unpaired surrogates can occur as a part of
      // "normal" operation.
      if ((ch >= 0xd800 && ch <= 0xdbff) && (char_count > 0)) {
        const uint16_t ch2 = *utf16_in;

        // Check if the other half of the pair is within the expected
        // range. If it isn't, we will have to emit both "halves" as
        // separate 3 byte sequences.
        if (ch2 >= 0xdc00 && ch2 <= 0xdfff) {
          utf16_in++;
          char_count--;
          const uint32_t code_point = (ch << 10) + ch2 - 0x035fdc00;
          *utf8_out++ = (code_point >> 18) | 0xf0;
          *utf8_out++ = ((code_point >> 12) & 0x3f) | 0x80;
          *utf8_out++ = ((code_point >> 6) & 0x3f) | 0x80;
          *utf8_out++ = (code_point & 0x3f) | 0x80;
          continue;
        }
      }

      if (ch > 0x07ff) {
        // Three byte encoding.
        *utf8_out++ = (ch >> 12) | 0xe0;
        *utf8_out++ = ((ch >> 6) & 0x3f) | 0x80;
        *utf8_out++ = (ch & 0x3f) | 0x80;
      } else /*(ch > 0x7f || ch == 0)*/ {
        // Two byte encoding.
        *utf8_out++ = (ch >> 6) | 0xc0;
        *utf8_out++ = (ch & 0x3f) | 0x80;
      }
    }
  }
}

// Exhaustive test of converting a single code point to UTF-16, then UTF-8, and back again.

static void codePointToSurrogatePair(uint32_t code_point, uint16_t &first, uint16_t &second) {
  first = (code_point >> 10) + 0xd7c0;
  second = (code_point & 0x03ff) + 0xdc00;
}

static void testConversions(uint16_t *buf, int char_count) {
  char bytes_test[5], bytes_reference[5];
  uint16_t out_buf_test[2], out_buf_reference[2];
  int byte_count_test, byte_count_reference;
  int char_count_test, char_count_reference;

  // calculate the number of utf-8 bytes for the utf-16 chars
  byte_count_reference = CountUtf8Bytes_reference(buf, char_count);
  byte_count_test = CountUtf8Bytes(buf, char_count);
  EXPECT_EQ(byte_count_reference, byte_count_test);

  // convert the utf-16 string to utf-8 bytes
  ConvertUtf16ToModifiedUtf8_reference(bytes_reference, buf, char_count);
  ConvertUtf16ToModifiedUtf8(bytes_test, buf, byte_count_test, char_count);
  EXPECT_EQ(0, memcmp(bytes_reference, bytes_test, byte_count_test));

  // calculate the number of utf-16 chars from the utf-8 bytes
  bytes_reference[byte_count_reference] = 0; // reference function needs null termination
  char_count_reference = CountModifiedUtf8Chars(bytes_reference);
  char_count_test = CountModifiedUtf8Chars(bytes_test, byte_count_test);
  EXPECT_EQ(char_count, char_count_reference);
  EXPECT_EQ(char_count, char_count_test);

  // convert the utf-8 bytes back to utf-16 chars
  ConvertModifiedUtf8ToUtf16(out_buf_reference, bytes_reference);
  ConvertModifiedUtf8ToUtf16(out_buf_test, bytes_test, char_count_test, byte_count_test);
  EXPECT_EQ(buf[0], out_buf_reference[0]);
  EXPECT_EQ(buf[0], out_buf_test[0]);
  if (char_count == 2) {
    EXPECT_EQ(buf[1], out_buf_reference[1]);
    EXPECT_EQ(buf[1], out_buf_test[1]);
  }
}

TEST_F(UtfTest, ExhaustiveBidirectionalCodePointCheck) {
  for (int codePoint = 0; codePoint <= 0x10ffff; ++codePoint) {
    uint16_t buf[2];
    if (codePoint <= 0xffff) {
      buf[0] = codePoint;
      testConversions(buf, 1);
    } else {
      codePointToSurrogatePair(codePoint, buf[0], buf[1]);
      testConversions(buf, 2);
    }
  }
}


}  // namespace art
