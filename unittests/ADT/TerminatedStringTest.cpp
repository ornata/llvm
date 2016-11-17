/// Unit tests for TerminatedStrings

#include "llvm/ADT/TerminatedString.h"
#include "gtest/gtest.h"
#include <climits>
#include <cstring>
#include <stdarg.h>

using namespace llvm;

namespace {
typedef int CharType;
typedef TerminatedString<std::vector<CharType>, CharType> StringType;
typedef TerminatedStringList<std::vector<CharType>, CharType> StringList;

class TerminatedStringTest : public testing::Test {

protected:
  StringType str;

  /// Empty terminated string => only have a terminator
  void assertEmpty(StringType &s) {
    EXPECT_EQ(1u, s.size());
    EXPECT_EQ(0u, s.length());
    EXPECT_EQ(s.chars_begin(), s.chars_end());
  }

  /// Add a character to the string
  void addOneCharacter(StringType &s, const CharType &c) {
    unsigned oldLen = str.length();
    unsigned oldSize = str.size();

    str += c;

    EXPECT_EQ(str.length(), oldLen + 1);
    EXPECT_EQ(str.size(), oldSize + 1);
    EXPECT_EQ(*(str.chars_end() - 1), c);
    EXPECT_TRUE((*str.chars_end()).IsTerminator);
  }
};

/// A new string should only contain a terminator.
TEST_F(TerminatedStringTest, EmptyStringTest) {
  SCOPED_TRACE("EmptyStringTest");
  assertEmpty(str);
  EXPECT_TRUE(str.chars_begin() == str.chars_end());
}

/// Append a single character to the string
TEST_F(TerminatedStringTest, AppendOneCharTest) {
  SCOPED_TRACE("AppendOneCharTest");
  unsigned oldLen = str.length();
  unsigned oldSize = str.size();

  addOneCharacter(str, 0);

  EXPECT_EQ(*(str.chars_end() - 1), 0);
  EXPECT_TRUE(str.length() == oldLen + 1);
  EXPECT_TRUE(str.size() == oldSize + 1);
}

/// Append five characters to the string
TEST_F(TerminatedStringTest, AppendFiveCharsTest) {
  SCOPED_TRACE("AppendFiveCharsTest");
  unsigned oldLen = str.length();
  unsigned oldSize = str.size();

  for (int i = 0; i < 5; i++) {
    addOneCharacter(str, i);
    EXPECT_EQ(*(str.chars_end() - 1), i);
  }

  EXPECT_TRUE(str.length() == oldLen + 5);
  EXPECT_TRUE(str.size() == oldSize + 5);
}

/// Append one hundred characters to the string
TEST_F(TerminatedStringTest, AppendHundredCharsTest) {
  SCOPED_TRACE("AppendHundredCharsTest");
  unsigned oldLen = str.length();
  unsigned oldSize = str.size();

  for (int i = 0; i < 100; i++) {
    addOneCharacter(str, i);
    EXPECT_EQ(*(str.chars_end() - 1), i);
  }

  EXPECT_TRUE(str.length() == oldLen + 100);
  EXPECT_TRUE(str.size() == oldSize + 100);
}

/// Strings should be equal to themselves.
TEST_F(TerminatedStringTest, EqualToSelfTest) {
  SCOPED_TRACE("EqualToSelfTest");
  EXPECT_TRUE(str == str);
}

/// Strings shouldn't be equal to empty strings.
TEST_F(TerminatedStringTest, NotEqualEmptyStringTest) {
  SCOPED_TRACE("NotEqualEmptyStringTest");
  StringType other;
  EXPECT_FALSE(str.chars_end() == other.chars_end());

  // Should hold both ways
  EXPECT_FALSE(str == other);
  EXPECT_FALSE(other == str);
}

/// Strings shouldn't be equal to other strings.
TEST_F(TerminatedStringTest, NotEqualNonEmptyStringTest) {
  SCOPED_TRACE("NotEqualNonEmptyStringTest");
  StringType other;
  other += 1;
  other += 1;
  other += 3;
  other += 5;
  EXPECT_FALSE(str.chars_end() == other.chars_end());

  // Should hold both ways
  EXPECT_FALSE(str == other);
  EXPECT_FALSE(other == str);
}

/// Empty strings should be non-equal.
TEST_F(TerminatedStringTest, NotEqualTwoEmptyStringsTest) {
  SCOPED_TRACE("NotEqualTwoEmptyStringsTest");
  StringType a;
  StringType b;
  EXPECT_FALSE(a.chars_end() == b.chars_end());
  EXPECT_FALSE(a == b);
  EXPECT_FALSE(b == a);
}

/// Two strings with the same characters shouldn't be equal.
TEST_F(TerminatedStringTest, NotEqualSameCharsTest) {
  SCOPED_TRACE("NotEqualSameCharsTest");
  StringType a;
  StringType b;

  a += 1;
  a += 2;
  a += 3;

  b += 1;
  b += 2;
  b += 3;

  EXPECT_FALSE(a.chars_end() == b.chars_end());
  EXPECT_FALSE(a == b);
  EXPECT_FALSE(b == a);
}

/// We should be able to index characters, and the last character should always
/// be the terminator
TEST_F(TerminatedStringTest, IndexingTest) {
  SCOPED_TRACE("IndexingTest");
  StringType s;
  Character<int> t = s.getTerminator();

  EXPECT_TRUE(s[0] == t);
  ASSERT_DEATH(s[1], "Out of range!");

  for (int i = 0; i < 100; i++) {
    s += i;
    for (int j = 0; j != i; j++) {
      EXPECT_TRUE(s[j] == j);
    }
    EXPECT_TRUE(s[i + 1] == t);
  }

  EXPECT_TRUE(s[7] == 7);
  EXPECT_TRUE(s[50] == 50);
  EXPECT_TRUE(s[0] == 0);
  EXPECT_TRUE(s[100] == t);
  ASSERT_DEATH(s[101], "Out of range!");
  ASSERT_DEATH(s[1000000], "Out of range!");
}

/// We should be able to erase from strings using indices
TEST_F(TerminatedStringTest, EraseIdxTest) {
  SCOPED_TRACE("EraseIdxTest");
  StringType s;

  // Can't erase the terminator
  ASSERT_DEATH(s.erase(0), "Can't erase terminator!");
  ASSERT_DEATH(s.erase(1), "Out of range!");
  ASSERT_DEATH(s.erase(99999), "Out of range!");

  for (int i = 0; i < 100; i++)
    s += i;

  s.erase(0);
  EXPECT_TRUE(s[0] == 1);
  s.erase(1);
  EXPECT_TRUE(s[0] == 1);
  s.erase(0);
  EXPECT_TRUE(s[0] == 3);
  s += 17;
  s.erase(0);
  EXPECT_TRUE(s[0] == 4);

  ASSERT_DEATH(s.erase(s.length()), "Can't erase terminator!");
  ASSERT_DEATH(s.erase(s.size() + 900), "Out of range!");
}

/// We should be able to erase from strings using indices
TEST_F(TerminatedStringTest, EraseRangeTest) {
  SCOPED_TRACE("EraseRangeTest");
  StringType s;

  ASSERT_DEATH(s.erase(0, 0), "c*");
  ASSERT_DEATH(s.erase(100, 100), "c*");
  ASSERT_DEATH(s.erase(0, 20), "c*");

  for (int i = 0; i < 100; i++)
    s += i;

  ASSERT_DEATH(s.erase(1, 0), "c*");
  ASSERT_DEATH(s.erase(100, 6), "c*");
  ASSERT_DEATH(s.erase(90, 150), "c*");

  // Erase 0.
  s.erase(0, 1);
  EXPECT_TRUE(s[0] == 1);

  // Now s = 1,2,3,4,5...

  // Erase 2, 4
  s.erase(1, 4);
  EXPECT_TRUE(s[1] == 5 && s[2] == 6 && s[3] == 7);
}

/// Test the insertBefore method
TEST_F(TerminatedStringTest, InsertBeforeTest) {
  SCOPED_TRACE("InsertBeforeTest");
  StringType s;
  // Check for bounds.
  ASSERT_DEATH(s.insertBefore(1, 11), "c*");

  // Should be able to insert before terminator.
  ASSERT_TRUE(*(s.insertBefore(0, 0)) == 0);

  // Should be able to insert after the first character
  ASSERT_TRUE(*(s.insertBefore(1, 2)) == 2);

  // The first character shouldn't be impacted
  ASSERT_TRUE(s[0] == 0);

  // Shouldn't impact the characters after
  ASSERT_TRUE(*(s.insertBefore(1, 1)) == 1);
  ASSERT_TRUE(s[2] == 2);
}

/// Test the copy constructor; this is the only time two strings should be equal
/// unless we force their terminators to be the same.
TEST_F(TerminatedStringTest, CopyConstructorTest) {
  SCOPED_TRACE("CopyConstructorTest");
  StringType s = StringType(str);
  EXPECT_EQ(s, str);
}

/// Test making a string from a vector
TEST_F(TerminatedStringTest, ContainerConstructorTest) {
  SCOPED_TRACE("ContainerConstructorTest");
  std::vector<int> v;
  for (int i = 0; i < 100; i++)
    v.push_back(i);

  StringType s = StringType(v);

  for (int i = 0; i < 100; i++)
    EXPECT_EQ(s[i], i);

  EXPECT_TRUE((*s.chars_end()).IsTerminator);
}

} // Anonymous namespace.
