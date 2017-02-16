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
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallSet.h"
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
#include <functional>
#include <map>
#include <sstream>
#include <vector>

#define DEBUG_TYPE "machine-outliner"

using namespace llvm;

STATISTIC(NumOutlined, "Number of candidates outlined");
STATISTIC(FunctionsCreated, "Number of functions created");
STATISTIC(NumTailCalls, "Number of tail call cases");

namespace {

const size_t SuffixTreeEmptyIdx = -1;

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
  size_t StartIdx = SuffixTreeEmptyIdx;

  /// The end index of this node's substring in the main string.
  ///
  /// Every leaf node must have its \p EndIdx incremented at the end of every
  /// step in the construction algorithm. To avoid having to update O(N)
  /// nodes individually at the end of every step, the end index is stored
  /// as a pointer.
  size_t *EndIdx = nullptr;

  /// For leaves, the start index of the suffix represented by this node.
  /// For all other nodes, this is ignored.
  size_t SuffixIdx = SuffixTreeEmptyIdx;

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
                 SuffixTreeNode *Parent_)
      : StartIdx(StartIdx_), EndIdx(EndIdx_), Link(Link_), Parent(Parent_) {}

  SuffixTreeNode() {}

  /// The length of the substring associated with this node.
  size_t size() {
    if (StartIdx == SuffixTreeEmptyIdx)
      return 0;

    assert(*EndIdx != SuffixTreeEmptyIdx && "EndIdx is undefined!");

    // Length = the number of elements in the string.
    // For example, [0 1 2 3] has length 4, not 3.
    return *EndIdx - StartIdx + 1;
  }

  /// Returns true if this node is a leaf.
  bool isLeaf() { return SuffixIdx != SuffixTreeEmptyIdx; }
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

  /// Stores each leaf in the tree for better pruning.
  std::vector<SuffixTreeNode *> LeafVector;

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
    size_t Idx = SuffixTreeEmptyIdx;

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
  void setSuffixIndices(SuffixTreeNode *CurrentNode, size_t LabelHeight) {

    if (!CurrentNode)
      return;

    bool IsLeaf = CurrentNode->Children.size() == 0 && CurrentNode != Root;

    for (auto &ChildPair : CurrentNode->Children) {
      assert(ChildPair.second && "Node had a null child!");
      setSuffixIndices(ChildPair.second,
                       LabelHeight + ChildPair.second->size());
    }

    // If the node is a leaf, then we need to assign it a suffix index and also
    // bump its parent's occurrence count.
    if (IsLeaf) {
      CurrentNode->SuffixIdx = Str.size() - LabelHeight;
      assert(CurrentNode->Parent && "CurrentNode had no parent!");
      CurrentNode->Parent->OccurrenceCount++;
      LeafVector[CurrentNode->SuffixIdx] = CurrentNode;
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
        insertNode(Active.Node, EndIdx, SuffixTreeEmptyIdx, FirstChar, true);

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
          if (NeedsLink && Active.Node->StartIdx != SuffixTreeEmptyIdx) {
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
        insertNode(SplitNode, EndIdx, SuffixTreeEmptyIdx, LastChar, true);

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

      if (Active.Node->StartIdx == SuffixTreeEmptyIdx) {
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
                size_t &MaxBenefit, size_t &BestStartIdx,
                const std::function<unsigned(SuffixTreeNode &, size_t CurrLen)>
                    &BenefitFn) {

    if (!CurrNode.IsInTree)
      return;

    // We hit an internal node, so we can traverse further down the tree.
    // For each child, traverse down as far as possible and set MaxHeight
    if (!CurrNode.isLeaf()) {
      for (auto &ChildPair : CurrNode.Children) {
        if (ChildPair.second && ChildPair.second->IsInTree)
          findBest(*ChildPair.second, CurrLen + ChildPair.second->size(),
                   BestLen, MaxBenefit, BestStartIdx, BenefitFn);
      }
    }

    // We hit a leaf. Check if we found a better substring than we had before.
    else if (CurrNode.isLeaf() && CurrNode.Parent != Root &&
             CurrNode.Parent->OccurrenceCount > 1) {

      size_t StringLen = CurrLen - CurrNode.size();
      unsigned Benefit = BenefitFn(CurrNode, StringLen);

      // We didn't save anything or do better, so give up.
      if (Benefit < MaxBenefit + 1)
        return;

      MaxBenefit = Benefit;
      BestStartIdx = CurrNode.SuffixIdx;
      BestLen = StringLen;
    }
  }

public:
  /// Return the element at index i in \p Str.
  unsigned operator[](size_t i) { return Str[i]; }

  /// \brief Return the substring of the tree with maximal benefit if a
  /// beneficial substring exists.
  ///
  /// \param [in,out] Best The most beneficial substring in the tree. Empty
  /// if it does not exist.
  void bestRepeatedSubstring(
      std::vector<unsigned> &Best,
      const std::function<unsigned(SuffixTreeNode &, size_t CurrLen)>
          &BenefitFn) {
    Best.clear();
    size_t Length = 0;   // Becomes the length of the best substring.
    size_t Benefit = 0;  // Becomes the benefit of the best substring.
    size_t StartIdx = 0; // Becomes the start index of the best substring.
    findBest(*Root, 0, Length, Benefit, StartIdx, BenefitFn);

    for (size_t Idx = 0; Idx < Length; Idx++)
      Best.push_back(Str[Idx + StartIdx]);

    DEBUG(if (Best.size() > 0) {
      dbgs() << "Substring which maximizes BenefitFn:\n";
      for (size_t Ch : Best)
        dbgs() << Ch << " ";
      dbgs() << "\n";

      dbgs() << "Length = " << Length << ", Benefit = " << Benefit
             << ", A start index = " << StartIdx << "\n";
    }

          else { errs() << "Didn't find anything.\n"; });
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
                             size_t &CurrIdx, SuffixTreeNode *CurrNode) {

    // The search ended at a nonexistent or pruned node. Quit.
    if (!CurrNode || !CurrNode->IsInTree)
      return nullptr;

    if (CurrNode->StartIdx == SuffixTreeEmptyIdx) {
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
  bool prune(SuffixTreeNode *N, size_t Len) {
    bool NoOverlap = true;
    std::vector<unsigned> IndicesToPrune;
    for (auto &ChildPair : N->Children) {
      SuffixTreeNode *M = ChildPair.second;

      if (M && M->IsInTree && M->isLeaf()) {
        IndicesToPrune.push_back(M->SuffixIdx);
        M->IsInTree = false;
      }
    }

    // Remove each suffix we have to prune from the tree.
    unsigned Offset = 1;
    for (unsigned CurrentSuffix = 1; CurrentSuffix < Len; CurrentSuffix++) {
      for (unsigned I : IndicesToPrune) {
        if (I + Offset < LeafVector.size()) {

          if (LeafVector[I + Offset]->IsInTree) {
            LeafVector[I + Offset]->IsInTree = false;
          } else {
            NoOverlap = false;
          }

          SuffixTreeNode *Parent = LeafVector[I + Offset]->Parent;
          if (Parent->OccurrenceCount > 0) {
            Parent->OccurrenceCount--;
            Parent->IsInTree = (Parent->OccurrenceCount > 1);
          }
        }
      }
      Offset++;
    }

    // We know we can never outline anything which starts one index back from
    // the indices we want to outline. This is because our minimum outlining
    // length is always 2.
    for (unsigned I : IndicesToPrune) {
      if (I > 0) {
        SuffixTreeNode *Parent = LeafVector[I - 1]->Parent;

        if (LeafVector[I - 1]->IsInTree) {
          LeafVector[I - 1]->IsInTree = false;
        } else {
          NoOverlap = false;
        }

        if (Parent->OccurrenceCount > 0) {
          Parent->OccurrenceCount--;
          Parent->IsInTree = (Parent->OccurrenceCount > 1);
        }
      }
    }

    N->IsInTree = false;
    N->OccurrenceCount = 0;

    return NoOverlap;
  }

  /// Find each occurrence of of a string in \p Str and prune their nodes.
  ///
  /// \param QueryString The string to search for.
  /// \param [out] Occurrences The start indices of each occurrence.
  bool findOccurrencesAndPrune(const std::vector<unsigned> &QueryString,
                               std::vector<size_t> &Occurrences) {
    size_t Dummy = 0;
    SuffixTreeNode *N = findString(QueryString, Dummy, Root);

    if (!N || !N->IsInTree)
      return false;

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

    return prune(N, QueryString.size());
  }

  /// Construct a suffix tree from a sequence of unsigned integers.
  SuffixTree(const std::vector<unsigned> &Str_) : Str(Str_) {
    Root =
        insertNode(nullptr, SuffixTreeEmptyIdx, SuffixTreeEmptyIdx, 0, false);
    Root->IsInTree = true;
    Active.Node = Root;
    LeafVector.reserve(Str.size());

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
    setSuffixIndices(Root, LabelHeight);
  }
};

/// \brief An individual sequence of instructions to be replaced with a call to
/// an outlined function.
struct Candidate {

  /// Set to false if the candidate overlapped with another candidate.
  bool InCandidateList = true;

  /// The start index of this \p Candidate.
  size_t StartIdx;

  /// The number of instructions in this \p Candidate.
  size_t Len;

  /// The index of this \p Candidate's \p OutlinedFunction in the list of
  /// \p OutlinedFunctions.
  size_t FunctionIdx;

  Candidate(size_t StartIdx_, size_t Len_, size_t FunctionIdx_)
      : StartIdx(StartIdx_), Len(Len_), FunctionIdx(FunctionIdx_) {}

  Candidate() {}

  /// \brief Used to ensure that \p Candidates are outlined in an order that
  /// preserves the start and end indices of other \p Candidates.
  bool operator<(const Candidate &rhs) const { return StartIdx > rhs.StartIdx; }
};

/// \brief Stores created outlined functions and the information needed to
/// construct them.
struct OutlinedFunction {

  /// The actual outlined function created.
  /// This is initialized after we go through and create the actual function.
  MachineFunction *MF = nullptr;

  /// A number used to identify this function in the outlined program.
  size_t Name;

  /// The number of times that this function has appeared.
  size_t OccurrenceCount = 0;

  /// \brief The sequence of integers corresponding to the instructions in this
  /// function.
  std::vector<unsigned> Sequence;

  // The number of instructions this function would save.
  unsigned Benefit = 0;

  bool IsTailCall = false;

  OutlinedFunction(size_t Name_, size_t OccurrenceCount_,
                   const std::vector<unsigned> &Sequence_,
                   const unsigned &Benefit_)
      : Name(Name_), OccurrenceCount(OccurrenceCount_), Sequence(Sequence_),
        Benefit(Benefit_) {}
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

  unsigned mapToUnsigned(MachineInstr &MI, std::vector<unsigned> &UnsignedVec,
                         std::vector<MachineBasicBlock::iterator> &InstrList);

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
                            SmallSet<unsigned, 10> &ReturnIDs,
                            MachineBasicBlock &BB,
                            const TargetRegisterInfo &TRI,
                            const TargetInstrInfo &TII, bool IsLastBasicBlock);

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
  /// \returns The length of the longest candidate found. 0 if there are none.
  unsigned buildCandidateList(std::vector<Candidate> &CandidateList,
                              std::vector<OutlinedFunction> &FunctionList,
                              const SmallSet<unsigned, 10> &ReturnIDs,
                              SuffixTree &ST, const TargetInstrInfo *TII);

  void pruneOverlaps(std::vector<Candidate> &CandidateList,
                     std::vector<OutlinedFunction> &FunctionList,
                     const unsigned &MaxCandidateLen,
                     const TargetInstrInfo *TII);

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

unsigned MachineOutliner::mapToUnsigned(
    MachineInstr &MI, std::vector<unsigned> &UnsignedVec,
    std::vector<MachineBasicBlock::iterator> &InstrList) {
  // Get the integer for this instruction or give it the current
  // LegalInstrNumber.
  auto I = InstructionIntegerMap.insert(std::make_pair(&MI, LegalInstrNumber));

  // There was an insertion.
  if (I.second) {
    LegalInstrNumber++;
    IntegerInstructionMap.insert(std::make_pair(I.first->second, &MI));
  }

  unsigned MINumber = I.first->second;
  UnsignedVec.push_back(MINumber);
  CurrentFunctionID++;

  // Make sure we don't overflow or use any integers reserved by the DenseMap.
  assert(LegalInstrNumber < IllegalInstrNumber &&
         LegalInstrNumber != (unsigned)-3 && "Instruction mapping overflow.");
  assert(LegalInstrNumber != (unsigned)-1 && LegalInstrNumber != (unsigned)-2 &&
         "Tried to assign DenseMap tombstone or empty key to instruction.");

  return MINumber;
}

void MachineOutliner::convertToUnsignedVec(
    std::vector<unsigned> &UnsignedVec,
    std::vector<MachineBasicBlock::iterator> &InstrList,
    SmallSet<unsigned, 10> &ReturnIDs, MachineBasicBlock &MBB,
    const TargetRegisterInfo &TRI, const TargetInstrInfo &TII,
    bool IsLastBasicBlock) {

  for (MachineBasicBlock::iterator It = MBB.begin(), Et = MBB.end(); It != Et;
       It++) {

    // Keep track of where this instruction is in the program.
    InstrList.push_back(It);
    MachineInstr &MI = *It;

    // If we have a return and it's the last instruction in the function, then
    // we can tail call it. Map it, save the return ID, and move on.
    if (IsLastBasicBlock && MI.isTerminator() && std::next(It) == Et) {
      unsigned MINumber = mapToUnsigned(MI, UnsignedVec, InstrList);
      ReturnIDs.insert(MINumber);
      continue;
    }

    // If it's not legal to outline, then give it an unique integer and move
    // on. This way it will never appear in a repeated substring.
    if (!TII.isLegalToOutline(MI)) {
      UnsignedVec.push_back(IllegalInstrNumber);
      IllegalInstrNumber--;
      assert(LegalInstrNumber < IllegalInstrNumber &&
             "Instruction mapping overflow!");
      assert(IllegalInstrNumber != (unsigned)-1 &&
             IllegalInstrNumber != (unsigned)-2 &&
             "Str cannot be DenseMap tombstone or empty key!");
      continue;
    }

    // It's safe and we're not the last instruction in the last basic block.
    // Just map it and move on.
    mapToUnsigned(MI, UnsignedVec, InstrList);
  }

  // After we're done every insertion, uniquely terminate this part of the
  // "string". This makes sure we won't match across basic block or function
  // boundaries since the "end" is encoded uniquely and thus appears in no
  // repeated substring.
  InstrList.push_back(nullptr);
  UnsignedVec.push_back(IllegalInstrNumber);
  IllegalInstrNumber--;
}

void MachineOutliner::pruneOverlaps(std::vector<Candidate> &CandidateList,
                                    std::vector<OutlinedFunction> &FunctionList,
                                    const unsigned &MaxCandidateLen,
                                    const TargetInstrInfo *TII) {
  // Check for overlaps in the range. This is O(n^2) worst case, but we can
  // alleviate that somewhat by bounding our search space using the start
  // index of our first candidate and the maximum distance an overlapping
  // candidate could have from the first candidate. This makes it unlikely
  // that we'll hit a pathological quadratic case. If we base the number of
  // comparisons between one candidate and another off of the length of the
  // maximum candidate then when that length is small, we have fewer
  // comparisons. If that length is large, then there are fewer candidates in
  // the first place to compare against. For example, if the length of our
  // string is S, then the longest possible candidate is length S/2. Then there
  // can only be two candidates, so we only have one comparison.

  dbgs() << "Checking candidate list for overlaps.\n";

  for (auto It = CandidateList.begin(), Et = CandidateList.end(); It != Et;
       It++) {
    Candidate &C1 = *It;

    // If we removed this candidate, skip it.
    if (!C1.InCandidateList)
      continue;

    // If the candidate's function isn't good to outline anymore, then
    // remove the candidate and skip it.
    if (FunctionList[C1.FunctionIdx].OccurrenceCount < 2 ||
        FunctionList[C1.FunctionIdx].Benefit < 1) {
      C1.InCandidateList = false;
      continue;
    }

    // The minimum start index of any candidate that could overlap with this
    // one.
    unsigned FarthestPossibleIdx = 0;

    // Either it's 0, or at most MaxCandidateLen indices away.
    if (C1.StartIdx > MaxCandidateLen)
      FarthestPossibleIdx = C1.StartIdx - MaxCandidateLen;

    // Worst case: We compare against every candidate other than C1. (O(n^2))
    // Average case: We compare with most MaxCandidateLen/2 other candidates.
    // This is because each candidate has to be at least 2 indices away.
    //
    // If MaxCandidateLen is small, say, O(log(#Instructions)), then we have
    // at worst O(N * log(#Instructions)) candidates to compare against.
    //
    // If it's large, then there are far fewer candidates to deal with. For
    // example, let's say the longest candidate takes up 1/4 of the module.
    // Since it's a candidate, it has to appear at least twice, so 1/2 of the
    // module is taken up by that candidate, and 1/2 is free for other
    // candidates. In the worst case, we have a lot of candidates in that
    // other half, say they all have length 2. Then, if we say the number of
    // instructions in the program is I, we have I/4 length 2 candidates in
    // the program. Now, let's look at the first length 2 candidate.
    //
    // The maximum length of a candidate is I/4. We have I/4 candidates in
    // the section, each with length 2. Because they have length 2, the start
    // index of the first candidate, say C0 and the second candidate, C1, have
    // a difference of 2. Therefore, in order to find the first candidate
    // that's too far away to intersect, we only have to do I/8 comparisons
    // before the next candidate appears rather than I/4 comparisons; half
    // of what we would have had to do if we compared everything in the
    // section. In total, that would be I/8 * I/4 = I^2/32 instructions.
    for (auto Sit = It + 1; Sit != Et; Sit++) {
      Candidate &C2 = *Sit;

      // Once we hit a candidate that is too far away from the current one,
      // if one exists, then quit. This happens in at most
      // O(max(FarthestPossibleIdx/2, #Candidates remaining)) steps for every
      // candidate. For most candidates, this loop will then be
      // O(FarthestPossibleIdx).
      if (C2.StartIdx < FarthestPossibleIdx)
        break;

      // If we removed the candidate, or C2 wouldn't provide enough functions,
      // skip it.
      if (!C2.InCandidateList)
        continue;

      // If the candidate's function isn't good to outline anymore, then
      // remove the candidate and skip it.
      if (FunctionList[C2.FunctionIdx].OccurrenceCount < 2 ||
          FunctionList[C2.FunctionIdx].Benefit < 1) {
        C2.InCandidateList = false;
        continue;
      }

      size_t C2End = C2.StartIdx + C2.Len - 1;

      if (C2End < C1.StartIdx && C2.StartIdx < C1.StartIdx)
        continue;

      DEBUG(size_t C1End = C1.StartIdx + C1.Len - 1;
            dbgs() << "- Found an overlap to purge.\n";
            dbgs() << "--- C1 :[" << C1.StartIdx << ", " << C1End << "]\n";
            dbgs() << "--- C2 :[" << C2.StartIdx << ", " << C2End << "]\n";);

      FunctionList[C2.FunctionIdx].OccurrenceCount--;

      // Update the function's benefit.
      FunctionList[C2.FunctionIdx].Benefit =
          TII->outliningBenefit(FunctionList[C2.FunctionIdx].Sequence.size(),
                                FunctionList[C2.FunctionIdx].OccurrenceCount,
                                FunctionList[C2.FunctionIdx].IsTailCall);

      C2.InCandidateList = false;

      dbgs() << "- Removed C2. Num fns left for C2: "
             << FunctionList[C2.FunctionIdx].OccurrenceCount << "\n";
      dbgs() << "- C2's benefit: " << FunctionList[C2.FunctionIdx].Benefit
             << "\n";
    }
  }
}

unsigned
MachineOutliner::buildCandidateList(std::vector<Candidate> &CandidateList,
                                    std::vector<OutlinedFunction> &FunctionList,
                                    const SmallSet<unsigned, 10> &ReturnIDs,
                                    SuffixTree &ST,
                                    const TargetInstrInfo *TII) {
  dbgs() << "Creating candidate list.\n";

  std::vector<unsigned> CandidateSequence;
  unsigned MaxCandidateLen = 0;

  // Function for maximizing query.
  auto BenefitFn = [&TII, &ReturnIDs, &ST](const SuffixTreeNode &Curr,
                                           size_t StringLen) {

    // Anything with length < 2 will never be beneficial on any target.
    if (StringLen < 2)
      return 0u;

    size_t Occurrences = Curr.Parent->OccurrenceCount;

    // Anything with fewer than 2 occurrences will never be beneficial on any
    // target.
    if (Occurrences < 2)
      return 0u;

    bool CanBeTailCall = ReturnIDs.count(ST[Curr.SuffixIdx + StringLen - 1]);
    return TII->outliningBenefit(StringLen, Occurrences, CanBeTailCall);
  };

  for (ST.bestRepeatedSubstring(CandidateSequence, BenefitFn);
       CandidateSequence.size() > 1;
       ST.bestRepeatedSubstring(CandidateSequence, BenefitFn)) {

    DEBUG(dbgs() << "CandidateSequence: \n";
          for (const unsigned &U
               : CandidateSequence) IntegerInstructionMap.find(U)
              ->second->dump();
          dbgs() << "\n";);

    std::vector<size_t> Occurrences;

    bool GotNonOverlappingCandidate =
        ST.findOccurrencesAndPrune(CandidateSequence, Occurrences);

    // If a candidate doesn't appear at least twice, we won't save anything.
    if (Occurrences.size() < 2)
      break;

    // If the candidate was overlapping, skip it and move to the next one.
    if (!GotNonOverlappingCandidate)
      continue;

    if (CandidateSequence.size() > MaxCandidateLen)
      MaxCandidateLen = CandidateSequence.size() + 1;

    // If the last instruction in the sequence is a terminator, then we can
    // tail call it.
    bool IsTailCall = ReturnIDs.count(CandidateSequence.back());
    unsigned FnBenefit = TII->outliningBenefit(CandidateSequence.size(),
                                               Occurrences.size(), IsTailCall);

    // Make sure that whatever we're putting in the list is beneficial.
    assert(FnBenefit > 0 && "Unbeneficial function candidate!");

    OutlinedFunction OF = OutlinedFunction(
        FunctionList.size(), Occurrences.size(), CandidateSequence, FnBenefit);
    OF.IsTailCall = IsTailCall;
    FunctionList.push_back(OF);

    // Save each of the occurrences for the outlining process.
    for (size_t &Occ : Occurrences) {
      CandidateList.push_back(Candidate(
          Occ,                      // Starting idx in that MBB.
          CandidateSequence.size(), // Candidate length.
          FunctionList.size() - 1   // Idx of the corresponding function.
          ));
    }

    CurrentFunctionID++;
    FunctionsCreated++;
  }

  // Sort the candidates in decending order. This will simplify the outlining
  // process when we have to remove the candidates from the mapping by
  // allowing us to cut them out without keeping track of an offset.
  std::stable_sort(CandidateList.begin(), CandidateList.end());

  return MaxCandidateLen;
}

MachineFunction *
MachineOutliner::createOutlinedFunction(Module &M, const OutlinedFunction &OF) {

  // Create the function name. This should be unique. For now, just hash the
  // module name and include it in the function name plus the number of this
  // function.
  std::ostringstream NameStream;
  size_t HashedModuleName = std::hash<std::string>{}(M.getName().str());
  NameStream << "OUTLINED_FUNCTION" << HashedModuleName << "_" << OF.Name;
  std::string *Name = new std::string(NameStream.str());

  // Create the function using an IR-level function.
  LLVMContext &C = M.getContext();
  Function *F = dyn_cast<Function>(
      M.getOrInsertFunction(Name->c_str(), Type::getVoidTy(C), NULL));
  assert(F && "Function was null!");

  // Allow the linker to merge together identical outlined functions between
  // modules.
  F->setLinkage(GlobalValue::LinkOnceODRLinkage);
  F->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);

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

  TII->insertOutlinerPrologue(*MBB, MF, OF.IsTailCall);

  // Copy over the instructions for the function using the integer mappings in
  // its sequence.
  for (unsigned Str : OF.Sequence) {
    MachineInstr *NewMI =
        MF.CloneMachineInstr(IntegerInstructionMap.find(Str)->second);
    NewMI->dropMemRefs();
    MBB->insert(MBB->end(), NewMI);
  }

  TII->insertOutlinerEpilogue(*MBB, MF, OF.IsTailCall);

  DEBUG(dbgs() << "Created " << *Name << ". Type: ";
        if (OF.IsTailCall) dbgs() << "tail call.\n";
        else dbgs() << "normal function.\n";

        dbgs() << "Function body: \n"; for (MachineBasicBlock &MBB
                                            : MF) MBB.dump(););

  return &MF;
}

bool MachineOutliner::outline(
    Module &M, const std::vector<MachineBasicBlock::iterator> &InstrList,
    const std::vector<Candidate> &CandidateList,
    std::vector<OutlinedFunction> &FunctionList) {
  dbgs() << "Outlining.\n";
  bool OutlinedSomething = false;

  // Replace the candidates with calls to their respective outlined functions.
  for (const Candidate &C : CandidateList) {

    // If the candidate was removed during pruneOverlaps, don't bother.
    if (!C.InCandidateList)
      continue;

    OutlinedFunction &OF = FunctionList[C.FunctionIdx];

    // If it wouldn't be beneficial to outline, don't outline it.
    if (OF.OccurrenceCount < 2 || OF.Benefit < 1)
      continue;

    MachineBasicBlock *MBB = (*InstrList[C.StartIdx]).getParent();
    MachineBasicBlock::iterator StartIt = InstrList[C.StartIdx];

    if (!FunctionList[C.FunctionIdx].MF)
      FunctionList[C.FunctionIdx].MF = createOutlinedFunction(M, OF);

    MachineFunction *MF = FunctionList[C.FunctionIdx].MF;
    const TargetSubtargetInfo *STI = &(MF->getSubtarget());
    const TargetInstrInfo *TII = STI->getInstrInfo();

    // Insert a call to the new function and erase the old sequence.
    MachineBasicBlock::iterator EndIt = StartIt;
    std::advance(EndIt, C.Len);
    StartIt = TII->insertOutlinedCall(M, *MBB, StartIt, *MF, OF.IsTailCall);
    ++StartIt;
    MBB->erase(StartIt, EndIt);

    OutlinedSomething = true;

    // Statistics.
    NumOutlined++;
    if (OF.IsTailCall)
      NumTailCalls++;
  }

  return OutlinedSomething;
}

bool MachineOutliner::runOnModule(Module &M) {
  dbgs() << "Outlining from " << M.getName() << ".\n";

  // Don't outline from a module that doesn't contain any functions.
  if (M.empty())
    return false;

  MachineModuleInfo &MMI = getAnalysis<MachineModuleInfo>();
  const TargetSubtargetInfo *STI =
      &(MMI.getMachineFunction(*M.begin()).getSubtarget());
  const TargetRegisterInfo *TRI = STI->getRegisterInfo();
  const TargetInstrInfo *TII = STI->getInstrInfo();
  SmallSet<unsigned, 10> ReturnIDs;
  std::vector<unsigned> UnsignedVec;
  std::vector<MachineBasicBlock::iterator> InstrList;

  dbgs() << "Building mapping between instructions and integers.\n";
  for (Function &F : M) {
    MachineFunction &MF = MMI.getMachineFunction(F);

    // Don't outline from unsafe or empty functions.
    if (F.empty() || !TII->functionIsSafeToOutlineFrom(MF))
      continue;

    auto LastBB = std::prev(MF.end());

    for (auto It = MF.begin(), Et = MF.end(); It != Et; It++) {
      MachineBasicBlock &MBB = *It;

      // Don't outline from empty MachineBasicBlocks.
      if (MBB.empty())
        continue;

      convertToUnsignedVec(UnsignedVec, InstrList, ReturnIDs, MBB, *TRI, *TII,
                           It == LastBB);
    }
  }

  // Construct a suffix tree, use it to find candidates, and then outline them.
  SuffixTree ST(UnsignedVec);
  bool OutlinedSomething = false;
  std::vector<Candidate> CandidateList;
  std::vector<OutlinedFunction> FunctionList;

  CurrentFunctionID = InstructionIntegerMap.size();
  unsigned MaxCandidateLen =
      buildCandidateList(CandidateList, FunctionList, ReturnIDs, ST, TII);

  // If the maximum candidate length was 0, then there's nothing to outline.
  if (MaxCandidateLen > 0) {
    pruneOverlaps(CandidateList, FunctionList, MaxCandidateLen, TII);
    OutlinedSomething = outline(M, InstrList, CandidateList, FunctionList);
  }

  DEBUG(if (OutlinedSomething) dbgs()
            << "********** Outlined something! **********\n";
        else dbgs() << "********** Didn't outline anything! **********\n";);

  return OutlinedSomething;
}
