/// Unit tests for TerminatedStringLists

#include "llvm/ADT/SuffixTree.h"
#include "gtest/gtest.h"
#include <climits>
#include <cstring>
#include <stdarg.h>
#include <string>

using namespace llvm;

namespace {
typedef char CharacterType;
typedef std::string ContainerType;
typedef TerminatedString<ContainerType, CharacterType> String;
typedef TerminatedStringList<ContainerType, CharacterType> StringCollection;
typedef SuffixTree<ContainerType, CharacterType> STree;

class SuffixTreeTest : public testing::Test {
protected:
  STree ST;

  void assertEmpty(STree &T) { EXPECT_EQ(0u, T.size()); }

  STree initTree(unsigned &StrSize) {
    std::string S1 = "aaaabbaaaabb";
    std::string S2 = "banana";
    std::string S3 = "dog";

    String TS1 = String(S1);
    String TS2 = String(S2);
    String TS3 = String(S3);

    StringCollection SC;
    SC += TS1;
    SC += TS2;
    SC += TS3;
    StrSize = SC.size();
    return STree(SC);
  }
};

TEST_F(SuffixTreeTest, EmptyTreeTest) {
  SCOPED_TRACE("EmptyTreeTest");
  assertEmpty(ST);
}

TEST_F(SuffixTreeTest, InitTreeTest) {
  SCOPED_TRACE("InitTreeTest");
  unsigned StrSize;
  STree testTree = initTree(StrSize);
  EXPECT_EQ(StrSize, testTree.size());
}

TEST_F(SuffixTreeTest, LongestRepeatedTest) {
  SCOPED_TRACE("LongestRepeatedTest");
  unsigned StrSize;
  STree testTree = initTree(StrSize);

  String repeated = *(testTree.longestRepeatedSubstring());
  std::string expected = "aaaabb";

  for (size_t i = 0; i < repeated.length(); i++)
    EXPECT_EQ(repeated[i], expected[i]);
}

TEST_F(SuffixTreeTest, FindOccurrencesTest) {
  SCOPED_TRACE("FindOccurrencesTest");
  unsigned StrSize;
  STree TestTree = initTree(StrSize);

  std::string S1 = "aaaabbaaaab";
  std::string S2 = "banana";
  std::string S3 = "dog";

  String TS1 = String(S1);
  String TS2 = String(S2);
  String TS3 = String(S3);

  auto OccurrencesPtr = TestTree.findOccurrences(TS1);
  EXPECT_FALSE(OccurrencesPtr == nullptr);
  EXPECT_EQ(OccurrencesPtr->size(), 1u);
  errs() << OccurrencesPtr->size() << "\n";
}
}