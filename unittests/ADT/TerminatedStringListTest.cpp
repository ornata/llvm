/// Unit tests for TerminatedStringLists

#include "llvm/ADT/TerminatedString.h"
#include "gtest/gtest.h"
#include <climits>
#include <cstring>
#include <stdarg.h>

using namespace llvm;

namespace {
typedef TerminatedString<std::vector<int>, int> String;
typedef std::vector<String *> StringContainer;
typedef TerminatedStringList<std::vector<int>, int> SL;

class TerminatedStringListTest : public testing::Test {
protected:
  SL sl;

  void assertEmpty(SL &s) {
    EXPECT_EQ(0u, s.size());
    EXPECT_EQ(0u, s.stringCount());
    EXPECT_EQ(s.begin(), s.end());
  }
};

TEST_F(TerminatedStringListTest, EmptyStringTest) {
  SCOPED_TRACE("EmptyStringTest");
  assertEmpty(sl);
}

TEST_F(TerminatedStringListTest, AppendTest) {
  SCOPED_TRACE("AppendTest");

  std::vector<int> v;
  for (int i = 0; i < 10; i++)
    v.push_back(i);
  unsigned oldStringCount = sl.stringCount();
  unsigned oldSize = sl.size();

  String s = String(v);
  sl.append(s);

  EXPECT_EQ(sl.stringCount(), oldStringCount + 1);
  EXPECT_EQ(sl.size(), oldSize + s.size());

  String sat;
  sat = sl.stringAt(0);

  for (int i = 0; i < 10; i++)
    EXPECT_EQ(sat[i], sl[i]);
}

TEST_F(TerminatedStringListTest, ConstructFromStringTest) {
  SCOPED_TRACE("ConstructFromStringTest");
  std::vector<int> v;
  for (int i = 0; i < 10; i++)
    v.push_back(i);
  String *s = new String(v);

  SL testsl = SL(s);

  EXPECT_EQ(1u, testsl.stringCount());
  EXPECT_EQ(s->size(), testsl.size());

  for (int i = 0; i < 10; i++) {
    EXPECT_EQ(testsl[i], i);
  }
}

TEST_F(TerminatedStringListTest, EraseIdxTestOneString) {
  SCOPED_TRACE("EraseIdxTestOneString");

  std::vector<int> v;
  for (int i = 0; i < 100; i++)
    v.push_back(i);
  String s = String(v);
  SL testsl = SL(s);

  ASSERT_DEATH(testsl.erase(101), "c*");
  ASSERT_DEATH(testsl.erase(10000), "c*");

  // 0, 1, 2, 3, 4, 5, 6, ...
  testsl.erase(0);

  // 1, 2, 3,..., 6,...
  EXPECT_EQ(testsl[0], 1);
  testsl.erase(1);

  // 1, 3, 4,..., 6,...
  EXPECT_EQ(testsl[0], 1);
  testsl.erase(0);

  // 3, 4, 5,..., 6,...
  EXPECT_EQ(testsl[0], 3);
  testsl.erase(1);

  // 3, 5,..., 6,...
  EXPECT_EQ(testsl[1], 5);
}

TEST_F(TerminatedStringListTest, EraseIdxTestTwoStrings) {
  SCOPED_TRACE("EraseIdxTestTwoStrings");

  std::vector<int> v1;
  for (int i = 0; i < 10; i++)
    v1.push_back(i);

  std::vector<int> v2;
  for (int i = 11; i < 20; i++)
    v2.push_back(i);

  String s1 = String(v1);
  SL testsl = SL(s1);

  unsigned oldSize = testsl.size();

  String s2 = String(v2);
  testsl += s2;
  EXPECT_EQ(testsl.size(), oldSize + s2.size());

  // 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, TERMINATOR
  // 11, 12, 13, 14, 15, 16, 17, 18, 19, TERMINATOR
  testsl.erase(11);
  EXPECT_EQ(testsl[9], 9);
  EXPECT_EQ(testsl[11], 12);

  testsl.erase(0);
  EXPECT_EQ(testsl[0], 1);
}

TEST_F(TerminatedStringListTest, EraseRangeTestOneString) {
  SCOPED_TRACE("EraseRangeTestOneString");

  std::vector<int> v;
  for (int i = 0; i < 10; i++)
    v.push_back(i);

  String s = String(v);
  SL testsl = SL(s);

  ASSERT_DEATH(testsl.erase(0, 11), "c*");
  ASSERT_DEATH(testsl.erase(7, 2), "c*");
  ASSERT_DEATH(testsl.erase(99, 100), "c*");

  // 0, 1, 2, 3, 4, ... 10
  testsl.erase(0, 1);
  EXPECT_EQ(testsl[0], 1);
  errs() << testsl[0] << "\n";

  // 1, 2, 3, 4, 5, ... 10
  testsl.erase(2, 4);
  EXPECT_TRUE(testsl[2] == 5 && testsl[3] == 6 && testsl[4] == 7);
}

TEST_F(TerminatedStringListTest, EraseRangeTestTwoStrings) {
  SCOPED_TRACE("EraseRangeTestTwoStrings");

  std::vector<int> v1;
  for (int i = 0; i < 10; i++)
    v1.push_back(i);
  std::vector<int> v2;
  for (int i = 11; i < 20; i++)
    v2.push_back(i);

  String s1 = String(v1);
  String s2 = String(v2);
  SL testsl = SL(s1);
  testsl += s2;

  testsl.erase(0, 1);
  EXPECT_EQ(testsl[0], 1);
  testsl.erase(2, 4);
  EXPECT_TRUE(testsl[2] == 5 && testsl[3] == 6 && testsl[4] == 7);
  testsl.erase(12, 13);
  EXPECT_EQ(testsl[12], 16);
  testsl.erase(12, 15);
  EXPECT_EQ(testsl[12], 19);

  errs() << testsl[12] << "\n";
}

TEST_F(TerminatedStringListTest, StringIdxContainingTest) {
  SCOPED_TRACE("StringIdxContainingTest");

  std::vector<int> v1;
  for (int i = 0; i < 10; i++)
    v1.push_back(i);
  std::vector<int> v2;
  for (int i = 11; i < 20; i++)
    v2.push_back(i);
  std::vector<int> v3;
  for (int i = 21; i < 30; i++)
    v3.push_back(i);

  String s1 = String(v1);
  String s2 = String(v2);
  String s3 = String(v3);
  SL testsl = SL(s1);

  testsl += s2;
  testsl += s3;

  auto r1 = testsl.stringIndexContaining(0u);
  EXPECT_EQ(r1.first, 0u);

  auto r2 = testsl.stringIndexContaining(12u);
  EXPECT_EQ(r2.first, 1u);

  auto r3 = testsl.stringIndexContaining(25u);
  EXPECT_EQ(r3.first, 2u);
}
}