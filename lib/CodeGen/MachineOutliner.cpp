//===---- MachineOutliner.cpp - Outline instructions -----------*- C++ -*-===//
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
/// This was originally presented at the 2016 LLVM Developers' Meeting in the
/// talk "Reducing Code Size Using Outlining". For a high-level overview of
/// how this pass works, the talk is available on YouTube at
///
/// https://www.youtube.com/watch?v=yorld-WSOeU
///
/// The slides for the talk are available at
///
/// http://www.llvm.org/devmtg/2016-11/Slides/Paquette-Outliner.pdf
///
/// The talk provides an overview of how the outliner finds candidates and
/// ultimately outlines them. It describes how the main data structure for this
/// pass, the suffix tree, is queried and purged for candidates. It also gives
/// a simplified suffix tree construction algorithm for suffix trees based off
/// of the algorithm actually used here, Ukkonen's algorithm.
///
/// For the original RFC for this pass, please see
///
/// http://lists.llvm.org/pipermail/llvm-dev/2016-August/104170.html
///
/// For more information on the suffix tree data structure, please see
/// https://www.cs.helsinki.fi/u/ukkonen/SuffixT1withFigs.pdf
///
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/Twine.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include <map>
#include <sstream>
#include <vector>

#define DEBUG_TYPE "machine-outliner"

using namespace llvm;

STATISTIC(NumOutlined, "Number of candidates outlined");
STATISTIC(FunctionsCreated, "Number of functions created");

namespace {

const size_t EmptyIdx = -1; /// Represents an undefined index.

/// A node in a suffix tree which represents a substring or suffix.
///
/// Each node has either no children or at least two children, with the root
/// being a exception in the empty tree.
///
/// Children are represented as a map between unsigned integers and nodes. If
/// a node N has a child M on unsigned integer k, then the mapping represented
/// by N is a proper prefix of the mapping represented by M. Note that this,
/// although similar to a trie is somewhat different: each node stores a full
/// substring of the full mapping rather than a single character state.
///
/// Each internal node contains a pointer to the internal node representing
/// the same string, but with the first character chopped off. This is stored
/// in \p Link. Each leaf node stores the start index of its respective
/// suffix in \p SuffixIdx.
struct SuffixTreeNode {

  /// The children of this node.
  ///
  /// A child existing on an unsigned integer implies that from the mapping
  /// represented by the current node, there is a way to reach another
  /// mapping by tacking that character on the end of the current string.
  DenseMap<unsigned, SuffixTreeNode *> Children;

  /// A flag set to false if the node has been pruned from the tree.
  bool IsInTree = true;

  /// The start index of this node's substring in the main string.
  size_t StartIdx = EmptyIdx;

  /// The end index of this node's substring in the main string.
  ///
  /// Every leaf node must have its \p EndIdx incremented at the end of every
  /// step in the construction algorithm. To avoid having to update O(N)
  /// nodes individually at the end of every step, the end index is stored
  /// as a pointer.
  size_t *EndIdx = nullptr;

  /// For leaves, the start index of the suffix represented by this node.
  /// For all other nodes, this is ignored.
  size_t SuffixIdx = EmptyIdx;

  /// \brief For internal nodes, a pointer to the internal node representing
  /// the same sequence with the first character chopped off.
  ///
  /// This has two major purposes in the suffix tree. The first is as a
  /// shortcut in Ukkonen's construction algorithm. One of the things that
  /// Ukkonen's algorithm does to achieve linear-time construction is
  /// keep track of which node the next insert should be at. This makes each
  /// insert O(1), and there are a total of O(N) inserts. The suffix link
  /// helps with inserting children of internal nodes.
  ///
  /// Say we add a child to an internal node with associated mapping S. The 
  /// next insertion must be at the node representing S - its first character.
  /// This is given by the way that we iteratively build the tree in Ukkonen's
  /// algorithm. The main idea is to look at the suffixes of each prefix in the
  /// string, starting with the longest suffix of the prefix, and ending with
  /// the shortest. Therefore, if we keep pointers between such nodes, we can
  /// move to the next insertion point in O(1) time. If we don't, then we'd
  /// have to query from the root, which takes O(N) time. This would make the
  /// construction algorithm O(N^2) rather than O(N).
  ///
  /// The suffix link is also used during the tree pruning process to let us
  /// quickly throw out a bunch of potential overlaps. Say we have a sequence
  /// S we want to outline. Then each of its suffixes contribute to at least
  /// one overlapping case. Therefore, we can follow the suffix links
  /// starting at the node associated with S to the root and "delete" those
  /// nodes, save for the root. For each candidate, this removes
  /// O(|candidate|) overlaps from the search space. We don't actually
  /// completely invalidate these nodes though; doing that is far too
  /// aggressive. Consider the following pathological string:
  ///
  /// 1 2 3 1 2 3 2 3 2 3 2 3 2 3 2 3 2 3
  ///
  /// If we, for the sake of example, outlined 1 2 3, then we would throw
  /// out all instances of 2 3. This isn't desirable. To get around this,
  /// when we visit a link node, we decrement its occurrence count by the
  /// number of sequences we outlined in the current step. In the pathological
  /// example, the 2 3 node would have an occurrence count of 8, while the
  /// 1 2 3 node would have an occurrence count of 2. Thus, the 2 3 node
  /// would survive to the next round allowing us to outline the extra
  /// instances of 2 3.
  SuffixTreeNode *Link = nullptr;

  /// \brief For internal nodes, a pointer from the internal node representing
  /// the same sequence with the first character chopped off.
  ///
  /// This is used during the pruning process. If a node N has a backlink
  /// from a node M, then N is a proper suffix of M. Thus, if we outline all
  /// instances of N, then it can never be the case that we can outline a
  /// non-overlapping instance of M. Therefore, we can remove all such nodes
  /// from the tree at the end of the round of outlining.
  SuffixTreeNode *BackLink = nullptr;

  /// The parent of this node. Every node except for the root has a parent.
  SuffixTreeNode *Parent = nullptr;

  /// The number of times this node's string appears in the tree.
  ///
  /// This is equal to the number of leaf children of the string. It represents
  /// the number of suffixes that the node's string is a prefix of.
  size_t OccurrenceCount = 0;

  SuffixTreeNode(size_t StartIdx_, size_t *EndIdx_, SuffixTreeNode *Link_,
                 SuffixTreeNode *Parent_) :
                 StartIdx(StartIdx_), EndIdx(EndIdx_), Link(Link_),
                 Parent(Parent_) 
                 {}

  SuffixTreeNode() {}

  /// The length of the substring associated with this node.
  size_t size() {
    if (StartIdx == EmptyIdx)
      return 0;

    assert(*EndIdx != EmptyIdx && "EndIdx is undefined!");

    // Length = the number of elements in the string.
    // For example, [0 1 2 3] has length 4, not 3.
    return *EndIdx - StartIdx + 1;
  }

  /// Returns true if this node is a leaf.
  bool isLeaf() {
    return SuffixIdx != EmptyIdx;
  }
};


/// A data structure for fast substring queries.
///
/// Suffix trees represent the suffixes of their input strings in their leaves.
/// A suffix tree is a type of compressed trie structure where each node
/// represents an entire substring rather than a single character. Each leaf
/// of the tree is a suffix.
///
/// A suffix tree can be seen as a type of state machine where each state is a
/// substring of the full string. The tree is structured so that, for a string
/// of length N, there are exactly N leaves in the tree. This structure allows
/// us to quickly find repeated substrings of the input string.
///
/// In this implementation, a "string" is a vector of unsigned integers.
/// These integers may result from hashing some data type. A suffix tree can
/// contain 1 or many strings, which can then be queried as one large string.
///
/// The suffix tree is implemented using Ukkonen's algorithm for linear-time
/// suffix tree construction. Ukkonen's algorithm is explained in more detail
/// in the paper by Esko Ukkonen "On-line construction of suffix trees. The
/// paper is available at
///
/// https://www.cs.helsinki.fi/u/ukkonen/SuffixT1withFigs.pdf
class SuffixTree {

private:

  /// Each element is an integer representing an instruction in the module.
  std::vector<unsigned> Str;

  /// Maintains each node in the tree.
  BumpPtrAllocator NodeAllocator;

  /// The root of the suffix tree.
  ///
  /// The root represents the empty string. It is maintained by the
  /// \p NodeAllocator like every other node in the tree.
  SuffixTreeNode *Root = nullptr;

  /// Maintains the end indices of the internal nodes in the tree.
  ///
  /// Each internal node is guaranteed to never have its end index change
  /// during the construction algorithm; however, leaves must be updated at
  /// every step. Therefore, we need to store leaf end indices by reference
  /// to avoid updating O(N) leaves at every step of construction. Thus,
  /// every internal node must be allocated its own end index.
  BumpPtrAllocator InternalEndIdxAllocator;

  /// The end index of each leaf in the tree.
  size_t LeafEndIdx = -1;

  /// \brief Helper struct which keeps track of the next insertion point in
  /// Ukkonen's algorithm.
  struct ActiveState {
    /// The next node to insert at.
    SuffixTreeNode *Node;

    /// The index of the first character in the substring currently being added.
    size_t Idx = EmptyIdx;

    /// The length of the substring we have to add at the current step.
    size_t Len = 0;
  };

  /// \brief The point the next insertion will take place at in the
  /// construction algorithm.
  ActiveState Active;

  /// Allocate a node and add it to the tree.
  ///
  /// The created node is managed by a \p BumpPtrAllocator. If it is not a leaf
  /// then its \p EndIdx is also managed by a \p BumpPtrAllocator.
  ///
  /// \param Parent The parent of this node if it has one.
  /// \param StartIdx The start index of the new node's associated string.
  /// \param EndIdx The end index of the new node's associated string.
  ///  Ignored if the new node is a leaf.
  /// \param Edge The label on the edge leaving \p Parent to this node.
  /// \param IsLeaf True if the new node is a leaf node.
  ///
  /// \returns The node inserted into the tree.
  SuffixTreeNode *insertNode(SuffixTreeNode *Parent, size_t StartIdx,
                             size_t EndIdx, unsigned Edge, bool IsLeaf) {
    SuffixTreeNode *N;
    size_t *E = &LeafEndIdx;

    if (!IsLeaf)
      E = new (InternalEndIdxAllocator) size_t(EndIdx);

    N = new (NodeAllocator) SuffixTreeNode(StartIdx, E, Root, Parent);

    if (Parent)
      Parent->Children[Edge] = N;

    return N;
  }

  /// \brief Set the suffix indices of the leaves to the start indices of their
  /// respective suffices.
  void setSuffixIndices(SuffixTreeNode &CurrentNode, size_t LabelHeight) {
    bool IsLeaf = true;

    for (auto &ChildPair : CurrentNode.Children) {
      assert(ChildPair.second && "Node had a null child!");
      IsLeaf = false;
      setSuffixIndices(*ChildPair.second,
                             LabelHeight + ChildPair.second->size());
    }

    if (IsLeaf) {
      CurrentNode.SuffixIdx = Str.size() - LabelHeight;
      CurrentNode.Parent->OccurrenceCount++;
    }
  }

  /// \brief Construct the suffix tree for the prefix of the input ending at
  /// \p EndIdx.
  ///
  /// Used to construct the full suffix tree iteratively.
  ///
  /// \param EndIdx The end index of the current prefix in the main string.
  /// \param NeedsLink The internal \p SuffixTreeNode that needs a suffix link.
  /// \param [in, out] SuffixesToAdd The number of suffixes that must be added
  /// to complete the suffix tree at the current phase.
  void extend(size_t EndIdx, SuffixTreeNode *NeedsLink, size_t &SuffixesToAdd) {

    while (SuffixesToAdd > 0) {
      // The length of the current string is 0, so we look at the last added
      // character to our substring.
      if (Active.Len == 0)
        Active.Idx = EndIdx;

      assert(Active.Idx <= EndIdx && "Start index can't be after end index!");

      // The first and last character in the current substring we're looking at.
      unsigned FirstChar = Str[Active.Idx];
      unsigned LastChar = Str[EndIdx];

      // During the previous step, we stopped on a node *and* it has no
      // transition to another node on the next character in our current
      // suffix.
      if (Active.Node->Children.count(FirstChar) == 0) {
        insertNode(Active.Node, EndIdx, EmptyIdx, FirstChar, true);

        // The active node is an internal node, and we visited it, so it must
        // need a link if it doesn't have one.
        if (NeedsLink) {
          NeedsLink->Link = Active.Node;
          Active.Node->BackLink = NeedsLink;
          NeedsLink = nullptr;
        }
      } else {
        // There *is* a match, so we have to traverse the tree and find out
        // where to put the node.
        SuffixTreeNode *NextNode = Active.Node->Children[FirstChar];

        // The child that we want to move to already contains our current
        // string up to some point. Move to the index in that node where we'd
        // have a mismatch and try again.
        size_t SubstringLen = NextNode->size();
        if (Active.Len >= SubstringLen) {
          Active.Idx += SubstringLen;
          Active.Len -= SubstringLen;
          Active.Node = NextNode;
          continue;
        }

        // The string is already in the tree, so we're done.
        if (Str[NextNode->StartIdx + Active.Len] == LastChar) {
          if (NeedsLink && Active.Node->StartIdx != EmptyIdx) {
            NeedsLink->Link = Active.Node;
            Active.Node->BackLink = NeedsLink;
            NeedsLink = nullptr;
          }

          Active.Len++;
          break;
        }

        // If the other two cases don't hold, then we must have found a
        // mismatch. Then there are two choices on the old edge: either we go
        // to the substring that was there before, or we go to the new
        // substring. To handle this, we introduce a "split node", which has
        // the old node and the new node as children. The split node's start
        // and end indices are those of the mapping we matched up to.
        SuffixTreeNode *SplitNode =
            insertNode(Active.Node, NextNode->StartIdx,
                       NextNode->StartIdx + Active.Len - 1, FirstChar, false);

        // Insert the new node representing the new substring into the tree as
        // a child of the split node.
        insertNode(SplitNode, EndIdx, EmptyIdx, LastChar, true);

        // Make the old node a child of the split node and update its start
        // index. When we created the split node, the part of this node's old
        // mapping that matched it was absorbed into the split node. Therefore,
        // this node should only contain the part that differs from the new
        // node we inserted.
        NextNode->StartIdx += Active.Len;
        NextNode->Parent = SplitNode;
        SplitNode->Children[Str[NextNode->StartIdx]] = NextNode;

        // We visited an internal node, so we need to set suffix links
        // accordingly.
        if (NeedsLink != nullptr) {
          NeedsLink->Link = SplitNode;
          SplitNode->BackLink = NeedsLink;
        }
    
        NeedsLink = SplitNode;
      }

      // We've added something new to the tree. Now we can move to the next
      // suffix.
      SuffixesToAdd--;

      if (Active.Node->StartIdx == EmptyIdx) {
        if (Active.Len > 0) {
          Active.Len--;
          Active.Idx = EndIdx - SuffixesToAdd + 1;
        }
      } else {
          // Start the next phase at the next smallest suffix.
          Active.Node = Active.Node->Link;
      }
  }
}

public:

  /// \brief Traverses the tree depth-first for the string with the highest
  /// benefit.
  ///
  /// Helper function for \p bestRepeatedSubstring.
  ///
  /// \param N The node currently being visited.
  /// \param CurrLen Length of the current string.
  /// \param [out] BestLen Length of the most beneficial substring.
  /// \param [out] MaxBenefit Benefit of the most beneficial substring.
  /// \param [out] BestStartIdx Start index of the most beneficial substring.
  void findBest(SuffixTreeNode &CurrNode, size_t CurrLen, size_t &BestLen,
                           size_t &MaxBenefit, size_t &BestStartIdx) {
    if (!CurrNode.IsInTree)
      return;

    // We hit an internal node, so we can traverse further down the tree.
    // For each child, traverse down as far as possible and set MaxHeight
    if (!CurrNode.isLeaf()) {
      for (auto &ChildPair : CurrNode.Children) {
        if (ChildPair.second && ChildPair.second->IsInTree)
          findBest(*ChildPair.second,
                              CurrLen + ChildPair.second->size(),
                              BestLen,
                              MaxBenefit,
                              BestStartIdx);
      }
    }

    // We hit a leaf. Check if we found a better substring than we had before.
    else if (CurrNode.isLeaf() &&
             CurrNode.Parent != Root &&
             CurrNode.Parent->OccurrenceCount >= 2) {

      size_t StringLen = CurrLen - CurrNode.size(); // Length of parent.

      if (StringLen < 2)
        return;

      size_t Occurrences = CurrNode.Parent->OccurrenceCount;

      size_t Benefit = StringLen * (Occurrences - 1);

      // We didn't save anything or do better. Quit.
      // FIXME: 2 should be target defined. It is the number of instructions
      // necessary for the function call, epilogue, prologue, etc.
      if (Benefit <= 2 || Benefit <= MaxBenefit)
        return;

      MaxBenefit = Benefit;
      BestStartIdx = CurrNode.SuffixIdx;
      BestLen = StringLen;
    }
  }

  /// \brief Return the substring of the tree with maximal benefit if a
  /// beneficial substring exists.
  ///
  /// Let
  ///    SL = string length
  ///    OC = # occurrences of string
  ///    IA = # instructions added to create the outlined function
  ///
  /// Then the most beneficial substring is the one which maximizes
  ///
  /// Benefit = SL * (OC - 1) - IA
  ///
  /// Where Benefit >= 1.
  ///
  /// \param [in,out] Best The most beneficial substring in the tree. Empty
  /// if it does not exist.
  void bestRepeatedSubstring(std::vector<unsigned> &Best) {
    Best.clear();
    size_t Length = 0; // Becomes the length of the best substring.
    size_t Benefit = 0; // Becomes the benefit of the best substring.
    size_t StartIdx = 0; // Becomes the start index of the best substring.
    findBest(*Root, 0, Length, Benefit, StartIdx);
    
    for (size_t Idx = 0; Idx < Length; Idx++)
      Best.push_back(Str[Idx + StartIdx]);
  }

  /// Perform a depth-first search for \p QueryString on the suffix tree.
  ///
  /// \param QueryString The string to search for.
  /// \param CurrIdx The current index in \p QueryString that is being matched
  /// against.
  /// \param CurrSuffixTreeNode The suffix tree node being searched in.
  ///
  /// \returns A \p SuffixTreeNode that \p QueryString appears in if such a
  /// node exists, and \p nullptr otherwise.
  SuffixTreeNode *findString(const std::vector<unsigned> &QueryString,
                             size_t &CurrIdx,
                             SuffixTreeNode *CurrNode) {
    
    // The search ended at a nonexistent or pruned node. Quit.
    if (!CurrNode || !CurrNode->IsInTree)
      return nullptr;

    if (CurrNode->StartIdx == EmptyIdx) {
      // If we're at the root we have to check if there's a child, and move to
      // that child. Don't consume the character since \p Root represents the
      // empty string.
      if (CurrNode->Children[QueryString[CurrIdx]] != nullptr &&
          CurrNode->Children[QueryString[CurrIdx]]->IsInTree)
        return findString(QueryString, CurrIdx,
                          CurrNode->Children[QueryString[CurrIdx]]);
      return nullptr;
    }

    size_t StrIdx = CurrNode->StartIdx;
    size_t MaxIdx = QueryString.size();
    bool ContinueSearching = false;

    // Match as far as possible into the string. If there's a mismatch, quit.
    for (; CurrIdx < MaxIdx; CurrIdx++, StrIdx++) {

      // We matched perfectly, but still have a remainder to search.
      if (StrIdx > *(CurrNode->EndIdx)) {
        ContinueSearching = true;
        break;
      }

      if (QueryString[CurrIdx] != Str[StrIdx])
        return nullptr;
    }

    // Move to the node which matches what we're looking for and continue
    // searching.
    if (ContinueSearching)
      return findString(QueryString, CurrIdx,
                        CurrNode->Children[QueryString[CurrIdx]]);

    // We matched perfectly so we're done.
    return CurrNode;
  }

  /// \brief Remove a node from a tree and all nodes representing proper
  /// suffixes of that node's string.
  ///
  /// This is used in the outlining algorithm to reduce the number of
  /// overlapping candidates
  void prune(SuffixTreeNode *N) {

    // For each suffix of N's string, reduce the number of occurrences of that
    // node by the number of occurrences of N. If there are no occurrences,
    // remove that node from the tree.
    for (SuffixTreeNode *T = N->Link; T && T != Root; T = T->Link) {
      T->OccurrenceCount = (T->OccurrenceCount > N->OccurrenceCount) ?
                            T->OccurrenceCount -= N->OccurrenceCount :
                            T->OccurrenceCount = 0; 
      T->IsInTree = T->OccurrenceCount > 1;
    }

    // We removed every occurrence of the string associated with N, so every
    // string it is a suffix of is no longer a candidate. Also throw N out.
    for (SuffixTreeNode *T = N; T && T != Root; T = T->BackLink) {
      T->OccurrenceCount = 0;
      T->IsInTree = false;
    }
  }

  /// Find each occurrence of of a string in \p Str and prune their nodes.
  ///
  /// \param QueryString The string to search for.
  /// \param [out] Occurrences The start indices of each occurrence.
  void findOccurrencesAndPrune(const std::vector<unsigned> &QueryString,
                               std::vector<size_t> &Occurrences) {
    size_t Dummy = 0;
    SuffixTreeNode *N = findString(QueryString, Dummy, Root);

    if (!N || !N->IsInTree)
      return;

    // If this is an internal node, occurrences are the number of leaf children
    // of the node.
    for (auto &ChildPair : N->Children) {
      SuffixTreeNode *M = ChildPair.second;

      if (M && M->IsInTree && M->isLeaf())
        Occurrences.push_back(M->SuffixIdx);
    }

    // If we're in a leaf, then this node is the only occurrence.
    if (N->isLeaf())
      Occurrences.push_back(N->SuffixIdx);

    prune(N);
  }

  /// Construct a suffix tree from a sequence of unsigned integers.
  SuffixTree(const std::vector<unsigned> &Str_) :
  Str(Str_)
  {
    Root = insertNode(nullptr, EmptyIdx, EmptyIdx, 0, false);
    Root->IsInTree = true;
    Active.Node = Root;

    // Keep track of the number of suffixes we have to add of the current
    // prefix.
    size_t SuffixesToAdd = 0;
    SuffixTreeNode *NeedsLink = nullptr; // The last internal node added
    Active.Node = Root;

    // Construct the suffix tree iteratively on each prefix of the string.
    // PfxEndIdx is the end index of the current prefix.
    // End is one past the last element in the string.
    for (size_t PfxEndIdx = 0, End = Str.size(); PfxEndIdx < End; PfxEndIdx++) {
      SuffixesToAdd++;
      NeedsLink = nullptr;
      LeafEndIdx = PfxEndIdx;
      extend(PfxEndIdx, NeedsLink, SuffixesToAdd);
    }

    // Set the suffix indices of each leaf.
    size_t LabelHeight = 0;
    assert(Root && "Root node can't be nullptr!");
    setSuffixIndices(*Root, LabelHeight);
  }
};


/// \brief An individual sequence of instructions to be replaced with a call to
/// an outlined function.
struct Candidate {

  /// \brief The start index of this \p Candidate.
  size_t StartIdx;

  /// The number of instructions in this \p Candidate.
  size_t Len;

  /// The index of this \p Candidate's \p OutlinedFunction in the list of
  /// \p OutlinedFunctions.
  size_t FunctionIdx;

  Candidate(size_t StartIdx_, size_t Len_, size_t FunctionIdx_)
      : StartIdx(StartIdx_), Len(Len_), FunctionIdx(FunctionIdx_)
      {}

  /// \brief Used to ensure that \p Candidates are outlined in an order that
  /// preserves the start and end indices of other \p Candidates.
  bool operator<(const Candidate &rhs) const {
    return StartIdx > rhs.StartIdx;
  }
};

/// \brief Stores created outlined functions and the information needed to
/// construct them.
struct OutlinedFunction {

  /// The actual outlined function created.
  /// This is initialized after we go through and create the actual function.
  MachineFunction *MF;

  /// A number used to identify this function in the outlined program.
  size_t Name;

  /// The number of times that this function has appeared.
  size_t OccurrenceCount;

  /// \brief The sequence of integers corresponding to the instructions in this
  /// function.
  std::vector<unsigned> Sequence;

  OutlinedFunction(size_t Name_, size_t OccurrenceCount_,
                   const std::vector<unsigned> &Sequence_)
      : Name(Name_), OccurrenceCount(OccurrenceCount_), Sequence(Sequence_)
      {}
};
} // Anonymous namespace.

/// \brief An interprocedural pass which finds repeated sequences of
/// instructions and replaces them with calls to functions.
///
/// Each instruction is mapped to an unsigned integer and placed in a string.
/// The resulting mapping is then placed in a \p SuffixTree. The \p SuffixTree
/// is then repeatedly queried for repeated sequences of instructions. Each
/// non-overlapping repeated sequence is then placed in its own
/// \p MachineFunction and each instance is then replaced with a call to that
/// function.
struct MachineOutliner : public ModulePass {
  static char ID;

  /// \brief Maps instructions to integers. Two instructions have the same
  /// integer if and only if they are identical. Instructions that are
  /// unsafe to outline are assigned unique integers.
  DenseMap<MachineInstr *, unsigned, MachineInstrExpressionTrait>
      InstructionIntegerMap;

  /// Maps the integers corresponding to instructions back to instructions.
  DenseMap<unsigned, MachineInstr *> IntegerInstructionMap;

  /// The last value assigned to an instruction we ought not to outline.
  /// Set to -3 to avoid attempting to query the \p DenseMap in
  /// \p SuffixTreeNode for the tombstone and empty keys given by the
  /// unsigned \p DenseMap template specialization.
  unsigned IllegalInstrNumber = -3;

  /// The last value assigned to an instruction we can outline.
  unsigned LegalInstrNumber = 0;

  /// The ID of the last function created.
  size_t CurrentFunctionID;

  StringRef getPassName() const override { return "MIR Function Outlining"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineModuleInfo>();
    AU.addPreserved<MachineModuleInfo>();
    AU.setPreservesAll();
    ModulePass::getAnalysisUsage(AU);
  }

  MachineOutliner() : ModulePass(ID) {
    // FIXME: Release function names.
    initializeMachineOutlinerPass(*PassRegistry::getPassRegistry());
  }

  /// \brief Transforms a \p MachineBasicBlock into a \p vector of \p unsigneds
  /// and appends it to \p UnsignedVec and \p InstrList.
  ///
  /// Two instructions are assigned the same integer if they are identical.
  /// If an instruction is deemed unsafe to outline, then it will be assigned an
  /// unique integer. The resulting mapping is placed into a suffix tree and
  /// queried for candidates.
  ///
  /// \param [out] UnsignedVec Filled with the instruction-integer mappings for
  /// the module.
  /// \param [out] InstrList Filled with iterators so that the iterator at
  /// index i points to the instruction at index i in UnsignedVec.
  /// \param BB The \p MachineBasicBlock to be translated into integers.
  /// \param TRI TargetRegisterInfo for the module.
  /// \param TII TargetInstrInfo for the module.
  void convertToUnsignedVec(std::vector<unsigned> &UnsignedVec,
                        std::vector<MachineBasicBlock::iterator> &InstrList,
                        MachineBasicBlock &BB,
                        const TargetRegisterInfo &TRI,
                        const TargetInstrInfo &TII);

  /// \brief Replace the sequences of instructions represented by the
  /// \p Candidates in \p CandidateList with calls to \p MachineFunctions
  /// described in \p FunctionList.
  ///
  /// \param M The module we are outlining from.
  /// \param InstrList Contains a list of iterators so that the i-th index from
  /// the suffix tree corresponds to the i-th instruction.
  /// \param CandidateList A list of candidates to be outlined.
  /// \param FunctionList A list of functions to be inserted into the program.
  bool outline(Module &M,
               const std::vector<MachineBasicBlock::iterator> &InstrList,
               const std::vector<Candidate> &CandidateList,
               std::vector<OutlinedFunction> &FunctionList);

  /// Creates a function for \p OF and inserts it into the program.
  MachineFunction *createOutlinedFunction(Module &M,
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
  /// \param ST The suffix tree for the program.
  void buildCandidateList(std::vector<Candidate> &CandidateList,
                          std::vector<OutlinedFunction> &FunctionList,
                          SuffixTree &ST);

  /// Construct a suffix tree on the instructions in \p M and outline repeated
  /// strings from that tree.
  bool runOnModule(Module &M) override;
};

char MachineOutliner::ID = 0;

namespace llvm {
ModulePass *createOutlinerPass() { return new MachineOutliner(); }
}

INITIALIZE_PASS(MachineOutliner, "machine-outliner",
                "Machine Function Outliner", false, false)

void MachineOutliner::convertToUnsignedVec(std::vector<unsigned> &UnsignedVec,
                            std::vector<MachineBasicBlock::iterator> &InstrList,
                            MachineBasicBlock &MBB,
                            const TargetRegisterInfo &TRI,
                            const TargetInstrInfo &TII) {
  for (MachineBasicBlock::iterator It = MBB.begin(), Et = MBB.end();
       It != Et; It++) {
    InstrList.push_back(It);
    MachineInstr& MI = *It;
    bool IsSafeToOutline = TII.isLegalToOutline(MI);
    
    // If it's not, give it a bad number.
    if (!IsSafeToOutline) {
      UnsignedVec.push_back(IllegalInstrNumber);
      IllegalInstrNumber--;
      assert(LegalInstrNumber < IllegalInstrNumber &&
             "Instruction mapping overflow!");
      assert(IllegalInstrNumber != (unsigned)-1 &&
             IllegalInstrNumber != (unsigned)-2 &&
             "Str cannot be DenseMap tombstone or empty key!");
      continue;
    }

    // It's safe to outline, so we should give it a legal integer. If it's in
    // the map, then give it the previously assigned integer. Otherwise, give
    // it the next available one.
    auto I = InstructionIntegerMap.insert(
        std::make_pair(&MI, LegalInstrNumber));

    // There was an insertion.
    if (I.second) {
      LegalInstrNumber++;
      IntegerInstructionMap.insert(std::make_pair(I.first->second, &MI)); 
    }

    unsigned MINumber = I.first->second;
    UnsignedVec.push_back(MINumber);
    CurrentFunctionID++;
    assert(LegalInstrNumber < IllegalInstrNumber &&
           LegalInstrNumber != (unsigned)-3 &&
           "Instruction mapping overflow!");
    assert(LegalInstrNumber != (unsigned)-1 &&
           LegalInstrNumber != (unsigned)-2 &&
           "Str cannot be DenseMap tombstone or empty key!");
  }

  InstrList.push_back(nullptr);
  UnsignedVec.push_back(IllegalInstrNumber);
  IllegalInstrNumber--;
}

void MachineOutliner::buildCandidateList(
    std::vector<Candidate> &CandidateList,
    std::vector<OutlinedFunction> &FunctionList,
    SuffixTree &ST) {

  std::vector<unsigned> CandidateSequence;

  for (ST.bestRepeatedSubstring(CandidateSequence);
       CandidateSequence.size() > 0;
       ST.bestRepeatedSubstring(CandidateSequence)) {

    std::vector<size_t> Occurrences;
    ST.findOccurrencesAndPrune(CandidateSequence, Occurrences); 

    // If a candidate doesn't appear at least twice, we won't save anything.
    if (Occurrences.size() < 2)
      break;

    FunctionList.push_back(OutlinedFunction(FunctionList.size(),
                                            Occurrences.size(),
                                            CandidateSequence));

    // Save each of the occurrences for the outlining process.
    for (size_t &Occ : Occurrences) {
      CandidateList.push_back(Candidate(
          Occ,                       // Starting idx in that MBB.
          CandidateSequence.size(),  // Candidate length.
          FunctionList.size() - 1    // Idx of the corresponding function.
          ));
    }

    CurrentFunctionID++;
    FunctionsCreated++;
  }

    // Sort the candidates in decending order. This will simplify the outlining
    // process when we have to remove the candidates from the mapping by
    // allowing us to cut them out without keeping track of an offset.
    std::stable_sort(CandidateList.begin(), CandidateList.end());
}

MachineFunction *
MachineOutliner::createOutlinedFunction(Module &M, const OutlinedFunction &OF) {

  // Create the function name and store it in the list of function names.
  // This has to be done because the char* for the name has to be around
  // after the pass is done for the ASMPrinter to print out.
  std::ostringstream NameStream;
  NameStream << "OUTLINED_FUNCTION" << OF.Name;
  std::string *Name = new std::string(NameStream.str());

  // Create the function using an IR-level function.
  LLVMContext &C = M.getContext();
  Function *F = dyn_cast<Function>(
      M.getOrInsertFunction(Name->c_str(), Type::getVoidTy(C), NULL));
  assert(F && "Function was null!");

  F->setLinkage(GlobalValue::PrivateLinkage);

  BasicBlock *EntryBB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> Builder(EntryBB);
  Builder.CreateRetVoid();

  MachineModuleInfo &MMI = getAnalysis<MachineModuleInfo>();
  MachineFunction &MF = MMI.getMachineFunction(*F);
  MachineBasicBlock *MBB = MF.CreateMachineBasicBlock();
  const TargetSubtargetInfo *STI = &(MF.getSubtarget());
  const TargetInstrInfo *TII = STI->getInstrInfo();

  // Insert the new function into the program.
  MF.insert(MF.begin(), MBB);

  TII->insertOutlinerPrologue(*MBB, MF);

  // Copy over the instructions for the function using the integer mappings in
  // its sequence.
  for (unsigned Str : OF.Sequence) {
    MachineInstr *NewMI = MF.CloneMachineInstr(
                              IntegerInstructionMap.find(Str)->second
                          );
    NewMI->dropMemRefs();
    MBB->insert(MBB->end(), NewMI);
  }

  TII->insertOutlinerEpilogue(*MBB, MF);

  DEBUG(
        dbgs() << "New function: \n"; dbgs() << *Name << ":\n";
        for (MachineBasicBlock &MBB : MF)
          MBB.dump();
       );

  return &MF;
}

bool MachineOutliner::outline(Module &M,
                      const std::vector<MachineBasicBlock::iterator> &InstrList,
                      const std::vector<Candidate> &CandidateList,
                      std::vector<OutlinedFunction> &FunctionList) {
  bool OutlinedSomething = false;

  // Create an outlined function for each candidate.
  // FIXME: Do this after determining if a candidate is non-overlapping.
  for (OutlinedFunction &OF : FunctionList)
    OF.MF = createOutlinedFunction(M, OF);

  // Replace the candidates with calls to their respective outlined functions.
  for (const Candidate &C : CandidateList) {

    MachineBasicBlock *MBB = (*InstrList[C.StartIdx]).getParent();
    MachineBasicBlock::iterator StartIt = InstrList[C.StartIdx];

    // If we're trying to outline from an overlap, we might be out of bounds.
    // If we are, quit.
    size_t OnePastEndIdx = C.StartIdx + C.Len;
    size_t Dist = OnePastEndIdx - C.StartIdx;
    size_t ItEndDist = std::distance(StartIt, MBB->end());
    bool AlreadyOutlinedFrom = (Dist >= ItEndDist);

    if (AlreadyOutlinedFrom)
      continue;

    OutlinedFunction OF = FunctionList[C.FunctionIdx];
    std::vector<unsigned> &Sequence = OF.Sequence;

    auto It = StartIt;
    for (unsigned U : Sequence) {
      MachineInstr* MI = IntegerInstructionMap.find(U)->second;
      if (!MI->isIdenticalTo(*It)) {
        AlreadyOutlinedFrom = true;
        break;
      }
      It++;
    }

    if (AlreadyOutlinedFrom)
      continue;

    MachineFunction *MF = OF.MF;
    const TargetSubtargetInfo *STI = &(MF->getSubtarget());
    const TargetInstrInfo *TII = STI->getInstrInfo();
    MachineBasicBlock::iterator EndIt = StartIt;
    std::advance(EndIt, C.Len);
    StartIt = TII->insertOutlinedCall(M, *MBB, StartIt, *MF);
    ++StartIt;
    MBB->erase(StartIt, EndIt);

    OutlinedSomething = true;
    NumOutlined++;
  }

  return OutlinedSomething;
}

bool MachineOutliner::runOnModule(Module &M) {

  // Don't outline from a module that doesn't contain any functions.
  if (M.empty())
    return false;

  MachineModuleInfo &MMI = getAnalysis<MachineModuleInfo>();
  const TargetSubtargetInfo *STI =
      &(MMI.getMachineFunction(*M.begin()).getSubtarget());
  const TargetRegisterInfo *TRI = STI->getRegisterInfo();
  const TargetInstrInfo *TII = STI->getInstrInfo();

  std::vector<unsigned> UnsignedVec;
  std::vector<MachineBasicBlock::iterator> InstrList;

  for (Function &F : M) {
    MachineFunction &MF = MMI.getMachineFunction(F);

    // Don't outline from unsafe or empty functions.
    if (F.empty() || !TII->functionIsSafeToOutlineFrom(F))
      continue;

    for (MachineBasicBlock &MBB : MF) {

      // Don't outline from empty MachineBasicBlocks.
      if (MBB.empty())
        continue;

      convertToUnsignedVec(UnsignedVec, InstrList, MBB, *TRI, *TII);
    }
  }

  // Construct a suffix tree, use it to find candidates, and then outline them.
  SuffixTree ST(UnsignedVec);
  bool OutlinedSomething = false;
  std::vector<Candidate> CandidateList;
  std::vector<OutlinedFunction> FunctionList;

  CurrentFunctionID = InstructionIntegerMap.size();
  buildCandidateList(CandidateList, FunctionList, ST);
  OutlinedSomething =
      outline(M, InstrList, CandidateList, FunctionList);

  if (OutlinedSomething) errs() << "********** Outlined something!\n";
  return OutlinedSomething;
}
