//===---- MachineOutliner.h - Outline instructions -------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Replaces repeated sequences of instructions with function calls.
///
/// This works by placing every instruction from every basic block in a
/// suffix tree, and repeatedly querying that tree for repeated sequences of
/// instructions. If a sequence of instructions appears often, then it ought
/// to be beneficial to pull out into a function.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "machine-outliner"

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/Twine.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/passes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include <map>
#include <sstream>
#include <vector>

using namespace llvm;

STATISTIC(NumOutlinedStat, "Number of candidates outlined");
STATISTIC(FunctionsCreatedStat, "Number of functions created");

// In the outliner, we store a "string" for each MachineBasicBlock in the
// program. Each string consists of integers. Each integer is either the
// unique hash for an instruction, or an unique integer. In the latter case,
// the integer represents something we ought not to outline.

/// Convenience typedef for the "string" type for the outliner.
typedef std::vector<unsigned> String;

/// Convenience typedef for a "two-dimensional string".
// NOTE: This is used as a 2D array *and* as a 2D string.
// For example, say we have these strings: [0,1,2] [3,4] [5,6,7]
// Then a StringCollection SC consisting of those strings would look like this:
//
// [[0,1,2][3,4][5,6,7]]
//
// And we would have SC[0] = 0, SC[2] = 2, SC[7] = 7, etc.
typedef std::vector<String *> StringCollection;

/// Represents a non-existent index into the string.
const size_t EmptyIndex = -1;

/// Returns the index of the string containing the index \p Offset.
///
/// \param StringList The collection of strings to be queried.
/// \param [in, out] Offset The query offset. Filled with the local offset in
/// the returned string on success.
///
/// \returns The index of the string that \p Offset appears in.
size_t indexAndOffsetHelper(const StringCollection &StringList,
                            size_t &Offset) {
  size_t StringIndex = 0;

  while (StringIndex < StringList.size()) {
    size_t CurrSize = StringList[StringIndex]->size();
    if (Offset < CurrSize)
      break;
    Offset -= CurrSize;
    ++StringIndex;
  }

  assert(StringIndex < StringList.size() && "Offset is out of bounds!");

  return StringIndex;
}

/// Returns the element of \p StringList as a 2D string at \p QueryIndex.
unsigned getElementInStringCollection(const StringCollection &StringList,
                                      size_t QueryIndex) {
  size_t StringIndex = indexAndOffsetHelper(StringList, QueryIndex);
  return (*StringList[StringIndex])[QueryIndex];
}

/// Returns the index of the \p String containing the index \p QueryIndex and
/// the offset \p QueryIndex maps into in that string.
std::pair<unsigned, unsigned>
getStringIndexAndOffset(const StringCollection &StringList, size_t QueryIndex) {
  size_t StringIndex = indexAndOffsetHelper(StringList, QueryIndex);
  return std::make_pair(StringIndex, QueryIndex);
}

/// Returns the string that contains the index \p QueryIndex in the 
/// \p StringCollection \p StringList and the offset into that string that
/// \p QueryIndex maps to.
std::pair<std::vector<unsigned> *, size_t>
stringContainingIndex(const StringCollection &StringList, size_t QueryIndex) {
  size_t StringIndex = indexAndOffsetHelper(StringList, QueryIndex);
  return std::make_pair(StringList[StringIndex], QueryIndex);
}

/// A data structure for fast string searching.
///
/// Suffix trees contain the suffixes of their input strings in their leaves.
/// This property makes it possible to quickly determine long repeated
/// substrings of strings.
///
/// In this implementation, a "string" is a vector of unsigned integers.
/// These integers may result from hashing some data type. A suffix tree can
/// contain 1 or many strings, which can then be queried as one large string.
/// 
/// The suffix tree is implemented using Ukkonen's algorithm for linear-time
/// suffix tree construction.
class SuffixTree {

private:

  /// A node in a suffix tree which represents a substring or suffix.
  ///
  /// Each node has either no children or at least two children, with the root
  /// being a exception in the empty tree.
  ///
  /// Children are represented as a map between unsigned integers and nodes. If
  /// a node N has a child M on unsigned integer k, then the string represented
  /// by N is a proper prefix of the string represented by M.
  ///
  /// Each internal node contains a pointer to the internal node representing
  /// the same string, but with the first character chopped off. This is stored
  /// in \p Link. Each leaf node stores the star index of its respective suffix
  /// in \p SuffixIndex.
  struct SuffixTreeNode {
    /// The parent of this node.
    SuffixTreeNode *Parent = nullptr;

    /// The children of this node.
    ///
    /// A child existing on an unsigned integer implies that from the string
    /// represented *by the current node*, there is a way to reach *another*
    /// string by tacking that character on the end of the current string.
    std::map<unsigned, SuffixTreeNode *> Children;

    /// Represents wherther or not the node has been pruned from the tree.
    ///
    /// If a node is invalid, then it is not considered in any future queries.
    bool Valid = true;

    /// If this node is internal, pointer to the internal node representing the
    /// same string missing the first character.
    ///
    /// This is used as a shortcut in the construction algorithm. For more
    /// information, look into Ukkonen's algorithm.
    /// It is also used during the tree pruning process to let us quickly throw
    /// out a bunch of potential overlaps.
    SuffixTreeNode *Link;

    /// Start index of this node's substring in the main string.
    size_t Start;

    /// End index of this node's substring in the main string.
    size_t *End = nullptr;

    /// Start index of this node's associated suffix if it is a leaf.
    size_t SuffixIndex = EmptyIndex;
  };

  /// Create a node starting at \p S, ending at \p E, and with suffix link \p L.
  SuffixTreeNode *createSuffixTreeNode(size_t S, size_t *E, SuffixTreeNode *L) {
    SuffixTreeNode *Node = new SuffixTreeNode;
    Node->Start = S;
    Node->End = E;
    Node->Link = L;

    return Node;
  }

  /// Delete the node \p N.
  void deleteSuffixTreeNode(SuffixTreeNode *N) {
    if (N == nullptr)
      return;

    N->Link = nullptr;
    N->Parent = nullptr;

    if (N->SuffixIndex == EmptyIndex && N->End != nullptr)
      delete N->End;

    for (auto ChildPair : N->Children) {
      if (ChildPair.second != nullptr) {
        deleteSuffixTreeNode(ChildPair.second);
        ChildPair.second = nullptr;
      }
    }

    N = nullptr;
  }

  /// Return the length of the substring defined by \p N.
  inline size_t nodeSize(const SuffixTreeNode &N) {
    size_t SubstringLen = 0;

    if (N.Start != EmptyIndex)
      SubstringLen = *N.End - N.Start + 1;

    return SubstringLen;
  }

  /// The sum of the lengths of the strings that form the input string.
  size_t InputLen;

  /// The amount by which we extend all leaves in the tree during the
  /// construction algorithm
  size_t LeafEnd;

  /// Keeps track of what we're currently working on in the tree during the
  /// construction algorithm.
  struct ActiveState {

    /// The current node in the tree.
    SuffixTreeNode *Node = nullptr;

    /// Index of the active character in the current substring.
    size_t Idx = EmptyIndex;

    /// Length of the current substring.
    size_t Len = 0;
  };

  /// The active state for Ukkonen's algorithm.
  ActiveState Active;

  /// Set the end of each leaf in the tree after constructing it.
  ///
  /// Each leaf will store the start index of its respective suffix after
  /// setting the leaf ends in its \p SuffixIndex.
  void setLeafEnds(SuffixTreeNode *CurrentNode, size_t LabelHeight) {
    if (CurrentNode == nullptr)
      return;

    bool IsLeaf = true;

    for (auto ChildPair : CurrentNode->Children) {
      if (ChildPair.second != nullptr) {
        IsLeaf = false;
        setLeafEnds(ChildPair.second,
                    LabelHeight + nodeSize(*ChildPair.second));
      }
    }

    if (IsLeaf)
      CurrentNode->SuffixIndex = size() - LabelHeight;
  }

  /// Construct the suffix tree for the prefix of the input string ending at
  /// \p EndIdx.
  /// 
  /// Used to construct the full suffix tree iteratively. For more detail,
  /// see Ukkonen's algorithm.
  ///
  /// \param EndIdx The end index of the current prefix in the main string.
  /// \param NeedsLink The internal \p SuffixTreeNode that needs a suffix link.
  /// \param [in, out] SuffixesToAdd The number of suffixes that must be added
  /// to complete the suffix tree at the current phase. 
  void extend(size_t EndIdx, SuffixTreeNode *NeedsLink,
                     size_t &SuffixesToAdd) {
    while (SuffixesToAdd > 0) {

      // The length of the current string is 0, so we look at the last added
      // character to our substring.
      if (Active.Len == 0)
        Active.Idx = EndIdx;

      // The first and last character in the current substring we're looking at.
      unsigned FirstChar =
          getElementInStringCollection(InputString, Active.Idx);

      unsigned LastChar = getElementInStringCollection(InputString, EndIdx);

      // During the previous step, we stopped on a node *and* it has no
      // transition to another node on the next character in our current
      // suffix.
      if (Active.Node->Children[FirstChar] == nullptr) {
        SuffixTreeNode *Child = createSuffixTreeNode(EndIdx, &LeafEnd, Root);
        Child->Parent = Active.Node;
        Active.Node->Children[FirstChar] = Child;

        // The active node is an internal node, and we visited it, so it must
        // need a link if it doesn't have one.
        if (NeedsLink != nullptr) {
          NeedsLink->Link = Active.Node;
          NeedsLink = nullptr;
        }
      }

      // There *is* a match, so we have to traverse the tree and find out where
      // to put the node.
      else {
        SuffixTreeNode *NextNode = Active.Node->Children[FirstChar];

        // The child that we want to move to already contains our current string
        // up to some point.Move to the index in that node where we'd have a
        // mismatch and try again.
        size_t SubstringLen = nodeSize(*NextNode);
        if (Active.Len >= SubstringLen) {
          Active.Idx += SubstringLen;
          Active.Len -= SubstringLen;
          Active.Node = NextNode;
          continue;
        }

        // The string is already in the tree, so we're done.
        if (getElementInStringCollection(
                InputString, NextNode->Start + Active.Len) == LastChar) {
          if (NeedsLink != nullptr && !(Active.Node->Start == EmptyIndex)) {
            NeedsLink->Link = Active.Node;
            NeedsLink = nullptr;
          }

          Active.Len++;
          break;
        }

        // If the other two cases don't hold, then we must have found a
        // mismatch. In this case, we split the edge to represent the two
        // choices: the old string we found, or the string on the mismatch.
        size_t *SplitEnd = new size_t(NextNode->Start + Active.Len - 1);
        SuffixTreeNode *SplitNode =
            createSuffixTreeNode(NextNode->Start, SplitEnd, Root);
        SplitNode->Parent = Active.Node;
        Active.Node->Children[FirstChar] = SplitNode;

        // Create the new node...
        SuffixTreeNode *Child = createSuffixTreeNode(EndIdx, &LeafEnd, Root);

        // The child of the split node on our mismatch = the new node.
        Child->Parent = SplitNode;
        SplitNode->Children[LastChar] = Child;

        // The old node's parent becomes the split node.
        NextNode->Start += Active.Len;
        NextNode->Parent = SplitNode;
        SplitNode->Children[getElementInStringCollection(
            InputString, NextNode->Start)] = NextNode;

        // We visited an internal node, so we have to update the suffix link.
        if (NeedsLink != nullptr)
          NeedsLink->Link = SplitNode;

        NeedsLink = SplitNode;
      }

      // We've added something new to the tree. Now we can move to the next
      // suffix.
      SuffixesToAdd--;
      if (Active.Node->Start == EmptyIndex) {
        if (Active.Len > 0) {
          Active.Len--;

          // Move one index over in the string for the next step
          Active.Idx = EndIdx - SuffixesToAdd + 1;
        }
      }

      // Start the next phase at the next smallest suffix.
      else {
        Active.Node = Active.Node->Link;
      }
    }
  }

public:
  /// The string the suffix tree was constructed for.
  StringCollection InputString; 

  /// The root of the suffix tree.
  /// The root represents the empty string.
  SuffixTreeNode *Root = nullptr;

  /// The sum of the lengths of the strings contained in \p InputString.
  size_t size() {
    return InputLen; 
  }

  /// Append a new string to \p InputString and update the suffix tree.
  ///
  /// /param NewStr The string to append to the tree.
  void append(String *NewStr) {
    InputString.push_back(NewStr);

    // Save the old size so we can start at the end of the old string
    size_t OldSize = InputLen;
    InputLen = OldSize + NewStr->size();

    // Keep track of the number of suffixes we have to add of the current
    // prefix.
    size_t SuffixesToAdd = 0;
    SuffixTreeNode *NeedsLink = nullptr; // The last internal node added

    // OldSize is initially 0 on the insertion of the first string. At the
    // insertion of the next string, OldSize is the index of the end of the
    // previous string.
    for (size_t EndIdx = OldSize; EndIdx < InputLen; EndIdx++) {
      SuffixesToAdd++;
      NeedsLink = nullptr;
      LeafEnd = (size_t)EndIdx;
      extend((size_t)EndIdx, NeedsLink, SuffixesToAdd);
    }

    // Set the leaf ends so we can query the tree.
    size_t LabelHeight = 0;
    setLeafEnds(Root, LabelHeight);
  }

  /// Traverse the tree depth-first and return the node whose substring is
  /// longest and appears at least twice.
  /// 
  /// \param Node The current node being visited in the traversal.
  /// \param LabelHeight The length of the node currently being visited.
  /// \param MaxLength [in, out] The length of the longest repeated substring.
  /// \param SubstringStartIdx [in, out] The start index of the first
  /// occurrence of the longest repeated substring found during the query.
  /// \param NumOccurrences [in, out] The number of times the longest repeated
  /// substring appears.
  void longestRepeatedNode(SuffixTreeNode &N, size_t LabelHeight,
                           size_t &MaxLength, size_t &SubstringStartIdx,
                           size_t &NumOccurrences) {

    // We hit an internal node, so we can traverse further down the tree.
    // For each child, traverse down as far as possible and set MaxHeight
    if (N.SuffixIndex == EmptyIndex) {
      for (auto ChildPair : N.Children) {
        if (ChildPair.second && ChildPair.second->Valid)
          longestRepeatedNode(*ChildPair.second,
                              LabelHeight + nodeSize(*ChildPair.second),
                              MaxLength, SubstringStartIdx, NumOccurrences);
      }
    }

    // We hit a leaf, so update MaxHeight if we've gone further down the
    // tree
    else if (N.SuffixIndex != EmptyIndex &&
             MaxLength < (LabelHeight - nodeSize(N))) {
      MaxLength = LabelHeight - nodeSize(N);
      SubstringStartIdx = N.SuffixIndex;
      NumOccurrences = (size_t)N.Parent->Children.size();
    }
  }

  /// Return a new \p String representing the longest substring of \p
  /// InputString which is repeated at least one time.
  /// 
  /// \returns The longest repeated substring in the suffix tree if it exists,
  /// and nullptr otherwise.
  String *longestRepeatedSubstring() {
    size_t MaxHeight = 0;
    size_t FirstChar = 0;
    SuffixTreeNode *N = Root;
    size_t NumOccurrences = 0;

    longestRepeatedNode(*N, 0, MaxHeight, FirstChar, NumOccurrences);
    String *Longest = nullptr;

    // We found something in the tree, so we know the string must appear
    // at least once
    if (MaxHeight > 0) {
      Longest = new String();

      for (size_t Idx = 0; Idx < MaxHeight; Idx++)
        Longest->push_back(
            getElementInStringCollection(InputString, Idx + FirstChar));
    }

    return Longest;
  }

  /// Perform a depth-first search for \p QueryString on the suffix tree.
  ///
  /// \param QueryString The string to search for.
  /// \param CurrIdx The current index in the query string that is being
  /// matched against.
  /// \param CurrSuffixTreeNode The suffix tree node being searched in.
  ///
  /// \returns A \p SuffixTreeNode that \p QueryString appears in if such a
  /// node exists, and nullptr otherwise.
  SuffixTreeNode *findString(const String &QueryString, size_t &CurrIdx,
                             SuffixTreeNode *CurrSuffixTreeNode) {
    SuffixTreeNode *RetSuffixTreeNode;
    SuffixTreeNode *NextNode;

    if (CurrSuffixTreeNode == nullptr || CurrSuffixTreeNode->Valid == false) {
      RetSuffixTreeNode = nullptr;
    }

    // If we're at the root we have to check if there's a child, and move to
    // that child. We don't consume the character since Root represents the
    // empty string
    else if (CurrSuffixTreeNode->Start == EmptyIndex) {
      if (CurrSuffixTreeNode->Children[QueryString[CurrIdx]] != nullptr &&
          CurrSuffixTreeNode->Children[QueryString[CurrIdx]]->Valid) {
        NextNode = CurrSuffixTreeNode->Children[QueryString[CurrIdx]];
        RetSuffixTreeNode = findString(QueryString, CurrIdx, NextNode);
      }

      else {
        RetSuffixTreeNode = nullptr;
      }
    }

    // The node represents a non-empty string, so we should match against it and
    // check its children if necessary
    else {
      size_t StrIdx = CurrSuffixTreeNode->Start;
      enum FoundState { ExactMatch, SubMatch, Mismatch };
      FoundState Found = ExactMatch;

      // Increment CurrIdx while checking the string for equivalence. Set
      // Found and possibly break based off of the case we find.
      while (CurrIdx < QueryString.size() - 1) {

        // Failure case 1: We moved outside the string, BUT we matched
        // perfectly up to that point.
        if (StrIdx > *(CurrSuffixTreeNode->End)) {
          Found = SubMatch;
          break;
        }

        // Failure case 2: We have a true mismatch.
        if (QueryString[CurrIdx] !=
            getElementInStringCollection(InputString, StrIdx)) {
          Found = Mismatch;
          break;
        }

        StrIdx++;
        CurrIdx++;
      }

      // Decide whether or not we should keep searching.
      switch (Found) {
      case (ExactMatch):
        RetSuffixTreeNode = CurrSuffixTreeNode;
        break;
      case (SubMatch):
        NextNode = CurrSuffixTreeNode->Children[QueryString[CurrIdx]];
        RetSuffixTreeNode = findString(QueryString, CurrIdx, NextNode);
        break;
      case (Mismatch):
        RetSuffixTreeNode = nullptr;
        break;
      }
    }

    return RetSuffixTreeNode;
  }

  /// Find each occurrence of of a string in \p InputString. Prunes each
  /// occurrence found from the tree by setting their nodes to invalid.
  ///
  /// \param QueryString The string to search for.
  ///
  /// \returns A pointer to a list of pairs of \p Strings and offsets into
  /// \p InputString representing each occurrence if \p QueryString is present,
  /// and nullptr otherwise.
  std::vector<std::pair<String *, size_t>> *
  findOccurrencesAndPrune(const String &QueryString) {
    size_t Len = 0;
    std::vector<std::pair<String *, size_t>> *Occurrences = nullptr;
    SuffixTreeNode *N = findString(QueryString, Len, Root);

    // FIXME: Pruning should happen in a separate function.
    if (N != nullptr && N->Valid) {
      N->Valid = false;
      Occurrences = new std::vector<std::pair<String *, size_t>>();

      // We matched exactly, so we're in a suffix. There's then exactly one
      // occurrence.
      if (N->SuffixIndex != EmptyIndex) {
        size_t StartIdx = N->SuffixIndex;
        auto StrPair = stringContainingIndex(InputString, StartIdx);
        Occurrences->push_back(make_pair(StrPair.first, StartIdx));
      }

      // There are no occurrences, so return null.
      else if (N == nullptr) {
        delete Occurrences;
        Occurrences = nullptr;
      }

      // There are occurrences, so find them and invalidate paths along the way.
      else {
        SuffixTreeNode *M;

        for (auto ChildPair : N->Children) {
          M = ChildPair.second;

          if ((M != nullptr) && (M->SuffixIndex != EmptyIndex)) {
            M->Valid = false;
            size_t StartIdx = M->SuffixIndex;
            auto StrPair = stringContainingIndex(InputString, StartIdx);
            Occurrences->push_back(make_pair(StrPair.first, StartIdx));
          }
        }
      }

      // Now invalidate the original node.
      N = N->Link;
      while (N && N != Root) {
        N->Valid = false;
        N = N->Link;
      }
    }

    return Occurrences;
  }

  /// Return the number of times the string \p QueryString appears in \p
  /// InputString.
  size_t numOccurrences(const String &QueryString) {
    size_t Dummy = 0;
    size_t NumOccurrences = 0;
    SuffixTreeNode *N = findString(QueryString, Dummy, Root);

    if (N != nullptr) {
      if (N->SuffixIndex != EmptyIndex)
        NumOccurrences = N->Parent->Children.size();

      else
        NumOccurrences = N->Children.size();
    }

    return NumOccurrences;
  }

  /// Create a suffix tree from a list of strings \p Strings, treating that list
  /// as a flat string.
  SuffixTree(StringCollection Strings) : InputLen(0), LeafEnd(EmptyIndex) {
    size_t *RootEnd = new size_t(EmptyIndex);
    Root = createSuffixTreeNode(EmptyIndex, RootEnd, Root);
    Active.Node = Root;

    for (auto *Str : Strings)
      append(Str);
  }

  /// Create an empty suffix tree.
  SuffixTree() : InputLen(0), LeafEnd(EmptyIndex) {
    size_t *RootEnd = new size_t(EmptyIndex);
    Root = createSuffixTreeNode(EmptyIndex, RootEnd, Root);
    Active.Node = Root;
  }

  /// Delete the suffix tree by calling \p deleteSuffixTreeNode recursively.
  ~SuffixTree() {
    Active.Node = nullptr;
    if (Root != nullptr)
      deleteSuffixTreeNode(Root);
  }
};

/// \brief An individual string of instructions to be replaced with a call
/// to an outlined function.
struct Candidate {
  /// \brief The index of the \p MachineBasicBlock in the worklist containing
  /// the first occurrence of this \p Candidate.
  size_t BBIndex;

  /// \brief The start index of this candidate in its containing \p String and
  /// \p MachineBasicBlock.
  size_t BBOffset;

  /// The number of instructions in this \p Candidate.
  size_t Length;

  /// The start index of \p Str in the full 2D string.
  size_t StartIdxIn2DString;

  /// The index of this \p Candidate's \p OutlinedFunction in the list of
  /// \p OutlinedFunctions.
  size_t FunctionIdx;

  /// The \p String that will be outlined.
  /// Stored to ensure that we don't have any overlaps.
  String *Str; // FIXME: This doesn't have to be stored

  Candidate(size_t BBIndex_, size_t BBOffset_, size_t Length_,
            size_t StartIdxIn2DString_, size_t FunctionIdx_, String *Str_)
      : BBIndex(BBIndex_), BBOffset(BBOffset_), Length(Length_),
        StartIdxIn2DString(StartIdxIn2DString_), FunctionIdx(FunctionIdx_),
        Str(Str_) {}

  /// Allows us to ensure that \p Candidates that appear later in the program
  /// are outlined first.
  bool operator<(const Candidate &rhs) const {
    return StartIdxIn2DString > rhs.StartIdxIn2DString;
  }
};

/// Stores information about an actual outlined function.
struct OutlinedFunction {
  /// The actual outlined function created.
  /// This is initialized after we go through and create the actual function.
  llvm::MachineFunction *MF;

  /// \brief The MachineBasicBlock containing the first occurrence of the
  /// string associated with this function.
  llvm::MachineBasicBlock *OccBB;

  /// The start index of the instructions to outline in \p OccBB.
  size_t StartIdxInBB;

  /// The end index of the instructions to outline in \p OccBB.
  size_t EndIdxInBB;

  /// The number this function will be assigned in the program.
  size_t Name;

  /// The number this function will be given in the full 2D string.
  size_t Id;

  /// The number of times that this function has appeared.
  size_t OccurrenceCount;

  OutlinedFunction(llvm::MachineBasicBlock *OccBB_, const size_t &StartIdxInBB_,
                   const size_t &EndIdxInBB_, const size_t &Name_,
                   const size_t &Id_, const size_t &OccurrenceCount_)
      : OccBB(OccBB_), StartIdxInBB(StartIdxInBB_), EndIdxInBB(EndIdxInBB_),
        Name(Name_), Id(Id_), OccurrenceCount(OccurrenceCount_) {}
};

namespace llvm {

/// \brief An interprocedural pass which finds repeated sequences of
/// instructions and replaces them with calls to functions.
///
/// Each instruction is mapped to an unsigned integer and placed in a string.
/// The resulting string is then placed in a \p SuffixTree. The \p SuffixTree
/// is then repeatedly queried for repeated sequences of instructions. Each
/// non-overlapping repeated sequence is then placed in its own
/// \p MachineFunctionand each instance is then replaced with a call to that
/// function.
struct MachineOutliner : public ModulePass {
  static char ID;

  /// \brief Used to either hash functions or mark them as illegal to outline
  /// depending on the instruction.
  DenseMap<MachineInstr *, unsigned, MachineInstrExpressionTrait>
      InstructionIntegerMap;

  /// The last value assigned to an instruction we ought not to outline.
  unsigned CurrIllegalInstrMapping;

  /// The last value assigned to an instruction we ought to outline.
  unsigned CurrLegalInstrMapping;

  /// The ID of the last function created.
  size_t CurrentFunctionID;

  /// The names of each function created. 
  std::vector<std::string *> *FunctionNames;

  StringRef getPassName() const override { return "Outliner"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineModuleInfo>();
    AU.addPreserved<MachineModuleInfo>();
    AU.setPreservesAll();
    ModulePass::getAnalysisUsage(AU);
  }

  MachineOutliner() : ModulePass(ID) {
    // FIXME: Release function names.
    initializeMachineOutlinerPass(*PassRegistry::getPassRegistry());
    FunctionNames = new std::vector<std::string *>;
  }

  /// Construct a proxy string for a MachineBasicBlock.
  ///
  /// This function translates each instruction into an unsigned integer. Two
  /// instructions are assigned the same integer if they are identical. If an
  /// instruction is deemed unsafe to outline, then it will be assigned an
  /// unique integer. The resultant string is placed into a suffix tree and
  /// queried for candidates.
  ///
  /// \param [out] Container Filled with the instruction-integer mappings for
  /// the program.
  /// \param BB The \p MachineBasicBlock to be translated into integers.
  void buildProxyString(std::vector<unsigned> &Container,
                        llvm::MachineBasicBlock *BB,
                        const TargetRegisterInfo *TRI,
                        const TargetInstrInfo *TII);

  /// \brief Replace the sequences of instructions represented by the
  /// \p Candidates in \p CandidateList with calls to \p MachineFunctions
  /// described in \p FunctionList.
  ///
  /// \param Worklist The basic blocks in the program in order of appearance.
  /// \param CandidateList A list of candidates to be outlined from the program.
  /// \param FunctionList A list of functions to be inserted into the program.
  bool outline(Module &M, std::vector<llvm::MachineBasicBlock *> &Worklist,
               std::vector<Candidate> &CandidateList,
               std::vector<OutlinedFunction> &FunctionList,
               StringCollection &SC);

  /// Creates a function for \p OF and inserts it into the program.
  llvm::MachineFunction *createOutlinedFunction(Module &M,
                                                const OutlinedFunction &OF);

  /// Find potential outlining candidates and store them in \p CandidateList.
  ///
  /// For each type of potential candidate, also build an \p OutlinedFunction
  /// struct containing the information to build the function for that
  /// candidate.
  ///
  /// \param [out] CandidateList Filled with outlining candidates for the
  /// module.
  /// \param [out] FunctionList Filled with functions corresponding to each
  /// type of \p Candidate.
  /// \param WorkList The basic blocks in the program in order of appearance.

  void buildCandidateList(std::vector<Candidate> &CandidateList,
                          std::vector<OutlinedFunction> &FunctionList,
                          std::vector<llvm::MachineBasicBlock *> Worklist,
                          SuffixTree &ST);

  /// Construct a suffix tree on the instructions in \p M and outline repeated
  /// strings from that tree.
  bool runOnModule(Module &M) override;
};

// FIXME: Free after printing.

// Keep the function names from the outliner around to keep the ASMPrinter
// happy...
std::vector<std::string *> *OutlinerFunctionNames;
ModulePass *createOutlinerPass() {
  MachineOutliner *OL = new MachineOutliner();
  OutlinerFunctionNames = OL->FunctionNames;
  return OL;
}
}

INITIALIZE_PASS(MachineOutliner, "outliner", "MIR Function Outlining", false, false)
char MachineOutliner::ID = 0;

void MachineOutliner::buildProxyString(std::vector<unsigned> &Container,
                                       MachineBasicBlock *BB,
                                       const TargetRegisterInfo *TRI,
                                       const TargetInstrInfo *TII) {
  for (MachineInstr &MI : *BB) {

    // First, check if the current instruction is legal to outline at all.
    bool IsSafeToOutline = TII->isLegalToOutline(MI);

    // If it's not, give it a bad number.
    if (!IsSafeToOutline) {
      Container.push_back(CurrIllegalInstrMapping);
      CurrIllegalInstrMapping--;
    }

    // It's safe to outline, so we should give it a legal integer. If it's in
    // the map, then give it the previously assigned integer. Otherwise, give
    // it the next available one.
    else {
      auto Mapping = InstructionIntegerMap.find(&MI);

      if (Mapping != InstructionIntegerMap.end()) {
        Container.push_back(Mapping->second);
      }

      else {
        InstructionIntegerMap.insert(
          std::make_pair(&MI, CurrLegalInstrMapping));
        Container.push_back(CurrLegalInstrMapping);
        CurrLegalInstrMapping++;
        CurrentFunctionID++;
      }
    }
  }
}

void MachineOutliner::buildCandidateList(
    std::vector<Candidate> &CandidateList,
    std::vector<OutlinedFunction> &FunctionList,
    std::vector<MachineBasicBlock *> Worklist,
    SuffixTree &ST) {

  // TODO: It would be better to use a "most beneficial substring" query if we
  // decide to be a bit smarter and use a dynamic programming approximation
  // scheme. For a naive greedy choice, LRS and MBS appear to be about as
  // effective as each other. This is because both can knock out a candidate
  // that would be better, or would lead to a better combination of candidates
  // being chosen.
  String *CandidateString = ST.longestRepeatedSubstring();

  // FIXME: Use the following cost model.
  // Weight = Occurrences * length
  // Benefit = Weight - [Len(outline prologue) + Len(outline epilogue) +
  // Len(functon call)]
  //
  // TODO: Experiment with dynamic programming-based approximation scheme. If it
  // isn't too memory intensive, we really ought to switch to it.
  if (CandidateString != nullptr && CandidateString->size() >= 2) {
    StringCollection SC = ST.InputString;
    std::vector<std::pair<String *, size_t>> *Occurrences =
        ST.findOccurrencesAndPrune(*CandidateString);

    // Query the tree for candidates until we run out of candidates to outline.
    do {
      assert(Occurrences != nullptr &&
             "Null occurrences for longestRepeatedSubstring!");

      // If there are at least two occurrences of this candidate, then we should
      // make it a function and keep track of it.
      if (Occurrences->size() >= 2) {
        std::pair<String *, size_t> FirstOcc = (*Occurrences)[0];

        // The index of the first character of the candidate in the 2D string.
        size_t IndexIn2DString = FirstOcc.second;

        // Use that to find the index of the string/MachineBasicBlock it appears
        // in and the point that it begins in in that string/MBB.
        std::pair<size_t, size_t> FirstIndexAndOffset =
            getStringIndexAndOffset(SC, IndexIn2DString);

        // From there, we can tell where the string starts and ends in the first
        // occurrence so that we can copy it over.
        size_t StartIdxInBB = FirstIndexAndOffset.second;
        size_t EndIdxInBB = StartIdxInBB + CandidateString->size() - 1;

        // Keep track of the MachineBasicBlock and its parent so that we can
        // copy from it later.
        MachineBasicBlock *OccBB = Worklist[FirstIndexAndOffset.first];
        FunctionList.push_back(
            OutlinedFunction(OccBB, StartIdxInBB, EndIdxInBB,
                             FunctionList.size(), CurrentFunctionID,
                             Occurrences->size()));

        // Save each of the occurrences for the outlining process.
        for (auto &Occ : *Occurrences) {
          std::pair<size_t, size_t> IndexAndOffset =
              getStringIndexAndOffset(SC, Occ.second);

          CandidateList.push_back(Candidate(
              IndexAndOffset.first, // Idx of MBB containing candidate.
              IndexAndOffset.second,   // Starting idx in that MBB.
              CandidateString->size(), // Length of the candidate.
              Occ.second,              // Start index in the full string.
              FunctionList.size()-1, // Index of the corresponding function.
              CandidateString // The actual string.
              )
          );
        }

        CurrentFunctionID++;
        FunctionsCreatedStat++;
      }

      // Find the next candidate and continue the process.
      CandidateString = ST.longestRepeatedSubstring();
    } while (CandidateString && CandidateString->size() >= 2 &&
             (Occurrences = ST.findOccurrencesAndPrune(*CandidateString)));

    // Sort the candidates in decending order. This will simplify the outlining
    // process when we have to remove the candidates from the string by
    // allowing us to cut them out without keeping track of an offset.
    std::stable_sort(CandidateList.begin(), CandidateList.end());
  }
}

MachineFunction *
MachineOutliner::createOutlinedFunction(Module &M, const OutlinedFunction &OF) {

  // Create the function name and store it in the list of function names.
  // This has to be done because the char* for the name has to be around
  // after the pass is done for the ASMPrinter to print out.
  std::ostringstream NameStream;
  NameStream << "OUTLINED_FUNCTION" << OF.Name;
  std::string *Name = new std::string(NameStream.str());
  FunctionNames->push_back(Name);

  // Create the function using an IR-level function.
  LLVMContext &C = M.getContext();
  Function *F = dyn_cast<Function>(
      M.getOrInsertFunction(Name->c_str(), Type::getVoidTy(C), NULL));
  assert(F != nullptr);

  F->setLinkage(GlobalValue::PrivateLinkage);

  BasicBlock *EntryBB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> Builder(EntryBB);
  Builder.CreateRetVoid();

  MachineModuleInfo &MMI = getAnalysis<MachineModuleInfo>();
  MachineFunction &MF = MMI.getMachineFunction(*F);
  MachineBasicBlock *MBB = MF.CreateMachineBasicBlock();
  const TargetSubtargetInfo *STI = &(MF.getSubtarget());
  const TargetInstrInfo *TII = STI->getInstrInfo();

  /// Find where the occurrence we want to copy starts and ends.
  DEBUG(dbgs() << "OF.StartIdxInBB = " << OF.StartIdxInBB << "\n";
        dbgs() << "OF.EndIdxInBB = " << OF.EndIdxInBB << "\n";);

  // Insert instructions into the function and a custom outlined
  // prologue/epilogue.
  MF.insert(MF.begin(), MBB);
  TII->insertOutlinerEpilog(*MBB, MF);

  auto It = OF.OccBB->instr_begin();
  std::advance(It, OF.EndIdxInBB);

  for (size_t i = 0, e = OF.EndIdxInBB - OF.StartIdxInBB + 1; i != e; i++) {
    MachineInstr* MI = MF.CloneMachineInstr(&*It);
    MI->dropMemRefs();
    MBB->insert(MBB->instr_begin(), MI);
    It--;
  }

  TII->insertOutlinerProlog(*MBB, MF);

  DEBUG(dbgs() << "New function: \n"; dbgs() << *Name << ":\n";
    for (MachineBasicBlock &MBB : MF)
      MBB.dump(); 
  );

  return &MF;
}

bool MachineOutliner::outline(Module &M,
                              std::vector<MachineBasicBlock *> &Worklist,
                              std::vector<Candidate> &CandidateList,
                              std::vector<OutlinedFunction> &FunctionList,
                              StringCollection &SC) {
  bool OutlinedSomething = false;

  // Create an outlined function for each candidate.
  for (size_t i = 0, e = FunctionList.size(); i < e; i++) {
    OutlinedFunction OF = FunctionList[i];
    FunctionList[i].MF = createOutlinedFunction(M, OF);
  }

  // Replace the candidates with calls to their respective outlined functions.
  //
  // FIXME: Change the suffix tree pruning technique so that it follows the
  // *longest* path on each internal node which *contains the node* that we're
  // invalidating stuff *for*. This will allow us to catch cases like this:
  // Outline "123", Outline "112". This method would make this unnecessary.
  //
  // FIXME: Currently, this method can allow us to unnecessarily outline stuff.
  // This should be done *before* we create the outlined functions.
  for (const Candidate &C : CandidateList) {

    size_t StartIndex = C.BBOffset;
    size_t EndIndex = StartIndex + C.Length;

    // If the index is below 0, then we must have already outlined from it.
    bool AlreadyOutlinedFrom = EndIndex - StartIndex > C.Length;

    // Check if we have any different characters in the string collection versus
    // the string we want to outline. If so, then we must have already outlined
    // from the spot this candidate appeared at.
    if (!AlreadyOutlinedFrom) {
      for (size_t i = StartIndex; i < EndIndex; i++) {
        size_t j = i - StartIndex;
        if ((*SC[C.BBIndex])[i] != (*(C.Str))[j]) {
          FunctionList[C.FunctionIdx].OccurrenceCount--;
          AlreadyOutlinedFrom = true;
          break;
        }
      }
    }

    // If we've outlined from this spot, or we don't have enough occurrences to
    // justify outlining stuff, then skip this candidate.
    if (AlreadyOutlinedFrom || FunctionList[C.FunctionIdx].OccurrenceCount < 2)
      continue;

    // We have a candidate which doesn't conflict with any other candidates, so
    // we can go ahead and outline it.
    OutlinedSomething = true;
    NumOutlinedStat++;

    // Remove the candidate from the string in the suffix tree first, and
    // replace it with the associated function's id.
    auto Begin = SC[C.BBIndex]->begin() + C.BBOffset;
    auto End = Begin + C.Length;

    SC[C.BBIndex]->erase(Begin, End);
    SC[C.BBIndex]->insert(Begin, FunctionList[C.FunctionIdx].Id);

    // Now outline the function in the module using the same idea.
    MachineFunction *MF = FunctionList[C.FunctionIdx].MF;
    MachineBasicBlock *MBB = Worklist[C.BBIndex];
    const TargetSubtargetInfo *STI = &(MF->getSubtarget());
    const TargetInstrInfo *TII = STI->getInstrInfo();
    MCContext &Ctx = MF->getContext();

    // We need the function name to match up with the internal symbol
    // build for it. There's no nice way to do this, so we'll just stick
    // an l_ in front of it manually.
    Twine InternalName = Twine("l_", MF->getName());
    MCSymbol *Name = Ctx.getOrCreateSymbol(InternalName);

    // Now, insert the function name and delete the instructions we don't need.
    MachineBasicBlock::iterator StartIt = MBB->begin();
    MachineBasicBlock::iterator EndIt = StartIt;

    std::advance(StartIt, StartIndex);
    std::advance(EndIt, EndIndex);
    StartIt = TII->insertOutlinedCall(*MBB, StartIt, *MF, Name);
    ++StartIt;
    MBB->erase(StartIt, EndIt);
  }

  return OutlinedSomething;
}

bool MachineOutliner::runOnModule(Module &M) {
  MachineModuleInfo &MMI = getAnalysis<MachineModuleInfo>();

  std::vector<MachineBasicBlock *> Worklist;

  // The current number we'll assign to instructions we ought not to outline.
  CurrIllegalInstrMapping = -1;

  // The current number we'll assign to instructions we want to outline.
  CurrLegalInstrMapping = 0;
  std::vector<std::vector<unsigned> *> Collection;

  // Set up the suffix tree by creating strings for each basic block.
  // Note: This means that the i-th string and the i-th MachineBasicBlock
  // in the work list correspond to each other. It also means that the
  // j-th character in that string and the j-th instruction in that
  // MBB correspond with each other.
  for (Function &F : M) {
    MachineFunction &MF = MMI.getMachineFunction(F);
    const TargetSubtargetInfo *STI = &(MF.getSubtarget());
    const TargetRegisterInfo *TRI = STI->getRegisterInfo();
    const TargetInstrInfo *TII = STI->getInstrInfo();

    if (F.empty() || !TII->functionIsSafeToOutlineFrom(F))
      continue;

    for (MachineBasicBlock &MBB : MF) {
      Worklist.push_back(&MBB);
      std::vector<unsigned> Container;
      buildProxyString(Container, &MBB, TRI, TII);
      String *BBString = new String(Container);
      Collection.push_back(BBString);
    }
  }

  SuffixTree ST = SuffixTree(Collection);

  // Find all of the candidates for outlining.
  bool OutlinedSomething = false;
  std::vector<Candidate> CandidateList;
  std::vector<OutlinedFunction> FunctionList;

  CurrentFunctionID = InstructionIntegerMap.size();
  buildCandidateList(CandidateList, FunctionList, Worklist, ST);
  OutlinedSomething = outline(M, Worklist, CandidateList, FunctionList, Collection);

  for (size_t i = 0, e = Collection.size(); i != e; i++)
    delete Collection[i];

  return OutlinedSomething;
}
