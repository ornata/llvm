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

using namespace llvm;

STATISTIC(NumOutlinedStat, "Number of candidates outlined");
STATISTIC(FunctionsCreatedStat, "Number of functions created");

namespace {

const size_t EmptyIdx = -1; /// Represents an undefined index.

/// Stores instruction-integer mappings for MachineBasicBlocks in the program.
///
/// This is used for compatability with the suffix tree. Mappings will tend to
/// be referred to as strings from the context of the suffix tree.
///
/// In the outliner, each \p MachineBasicBlock in the program is mapped to a
/// \p vector of \p unsigneds. Each \p unsigned is either the hash for an
/// instruction, or unique. Unique unsigneds represent instructions that the
/// target specifies as unsafe to outline. The \p ProgramMapping stores these
/// \p vectors and provides several convenience functions for the \p SuffixTree
/// data structure.
///
/// Specifically, it gives us a way to map the collection of \p vectors into
/// one big "string". Let's say [x,y] represents a \p vector where the first
/// element is x, and the second is y. Our mappings might look like this:
///
/// [[1, 2, 3], [6, 28, 496], [1, 1, 2, 3]]
///
/// The suffix tree is a data structure for searching strings though. What it
/// expects is something that looks more like this:
///
/// [1, 2, 3, 6, 28, 496, 1, 1, 2, 3]
///
/// We'll refer to the above case as the "flattened" vector, and indices into
/// that vector as flattened indices. The purpose of the \p ProgramMapping
/// is to let us pretend a 2D vector is a flattened one. We can then place the
/// \p ProgramMapping in the \p SuffixTree, find outlining candidates, but
/// remember which \p MachineBasicBlock the candidate would be mapped from. We
/// need to remember this because we need to clone the instructions from somewhere.
struct ProgramMapping {

  /// \brief Stores mappings between \p MachineBasicBlocks and \p vectors of
  /// \p unsigneds.
  ///
  /// The i-th vector corresponds to the i-th \p MachineBasicBlock in the
  /// module. Each integer corresponds to an instruction. Instructions that may
  /// be outlined are given a hash. Instructions that may not be outlined are
  /// given an unique integer so that they cannot be found in a repeated
  /// substring.
  std::vector<std::vector<unsigned>> MBBMappings;

  /// \brief Returns the pair of indices that a flattened index corresponds to
  /// in \p MBBMappings.
  /// 
  /// \param Offset The flattened index.
  ///
  /// \returns A \p std::pair whose first element is the index of the vector
  /// in \p MBBMappings containing \p Offset and whose second element is the
  /// index of the element that \p Offset corresponds to in that mapping.
  std::pair<size_t, size_t> locationOf(size_t Offset) {
    size_t MappingIdx;
    size_t NumMappings = MBBMappings.size();
    for (MappingIdx = 0; MappingIdx < NumMappings; MappingIdx++) {

      // First, get the size of the mapping we're currently looking at.
      size_t CurrMappingSize = MBBMappings[MappingIdx].size();

      // Now check if the offset is *less* than CurrMappingSize.
      // If this is true, then the offset lies inside the current mapping.
      if (Offset < CurrMappingSize)
        break;

      // Otherwise, move over to the next string.
      Offset -= CurrMappingSize;
    }

    // We should always stop before we hit MBBMappings.size() since we're
    // always looking for offsets that exist.
    assert(MappingIdx < MBBMappings.size() && "Mapping index out of bounds!");

    return std::make_pair(MappingIdx, Offset);
  }

  /// Returns the element of \p ProgramMapping as a 2D mapping at \p QueryIdx.
  unsigned elementAt(size_t QueryIdx) {
    std::pair<size_t, size_t> IndexAndOffset = locationOf(QueryIdx);
    return MBBMappings[IndexAndOffset.first][IndexAndOffset.second];
  }

  /// Returns the mapping that contains the index \p QueryIdx in the
  /// \p ProgramMapping \p ProgramMapping and the offset into that mapping that
  /// \p QueryIdx maps to.
  std::vector<unsigned> &mappingContaining(size_t QueryIdx) {
    return MBBMappings[locationOf(QueryIdx).first];
  }
};

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

  /// The parent of this node. Every node except for the root has a parent.
  SuffixTreeNode *Parent = nullptr;

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
  /// the same mapping with the first character chopped off.
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
  /// quickly throw out a bunch of potential overlaps. Say we have a mapping
  /// S we want to outline. Then each of its suffixes contribute to at least
  /// one overlapping case. Therefore, we can follow the suffix links
  /// starting at the node associated with S to the root and "delete" those
  /// nodes, save for the root. For each candidate, this removes
  /// O(|candidate|) overlaps from the search space.
  SuffixTreeNode *Link;

  SuffixTreeNode(size_t StartIdx_, size_t *EndIdx_, SuffixTreeNode *Link_)
      : StartIdx(StartIdx_), EndIdx(EndIdx_), Link(Link_) {}

  SuffixTreeNode() {}

  /// The length of the substring associated with this node.
  size_t size() {
    size_t SubstringLen = 0;

    if (StartIdx != EmptyIdx)
      SubstringLen = *EndIdx - StartIdx + 1;

    return SubstringLen;
  }
};


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

/// A data structure for fast substring queries.
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
/// suffix tree construction. Ukkonen's algorithm is explained in more detail
/// in the paper by Esko Ukkonen "On-line construction of suffix trees. The
/// paper is available at
///
/// https://www.cs.helsinki.fi/u/ukkonen/SuffixT1withFigs.pdf
///
/// Note that despite the main structure being a tree, the implementation
/// of the suffix tree really forms a digraph due to the suffix links
/// between internal nodes. Furthermore, if suffix links are present, there is
/// a directed cycle in the digraph due to nodes having suffix links to the
/// root.
class SuffixTree {
private:
  /// Maintains each node in the tree.
  ///
  /// Note that because this is a bump pointer allocator, we don't have to
  /// manually delete the nodes in the tree.
  BumpPtrAllocator NodeAllocator;

  /// Maintains the end indices of the internal nodes in the tree.
  ///
  /// Each internal node is guaranteed to never have its end index change
  /// during the construction algorithm; however, leaves must be updated at
  /// every step. Therefore, we need to store leaf end indices by reference
  /// to avoid updating O(N) leaves at every step of construction. Thus,
  /// every internal node must be allocated its own end index.
  /// 
  /// Note that because this is a bump pointer allocator, we don't have to
  /// manually delete the end indices of the nodes in the tree.
  BumpPtrAllocator InternalEndIdxAllocator;

  /// The root of the suffix tree.
  ///
  /// The root represents the empty string. It is maintained by the
  /// NodeAllocator like every other node in the tree. However, we need access
  /// to it so that we can traverse and query the tree.
  SuffixTreeNode *Root = nullptr;

  /// \brief Contains the instruction-unsigned mappings for the basic blocks of
  /// the program.
  ProgramMapping Mapping;

  /// The end index of each leaf in the tree.
  size_t LeafEndIdx = -1;

  /// The sum of the lengths of the strings that form the input string.
  size_t NumInstructionsInTree = 0;

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

    N = new (NodeAllocator) SuffixTreeNode(StartIdx, E, Root);
    N->Parent = Parent;

    if (Parent)
      Parent->Children[Edge] = N;

    return N;
  }

  /// Assign suffix indices to each leaf in the tree.
  ///
  /// Each leaf will store the start index of its respective suffix after
  /// setting the leaf ends in its \p SuffixIdx.
  void setSuffixIndices(SuffixTreeNode &CurrentNode, size_t LabelHeight) {
    bool IsLeaf = true;

    for (auto &ChildPair : CurrentNode.Children) {
      if (ChildPair.second != nullptr) {
        IsLeaf = false;

        assert(ChildPair.second && "Node has a null child!");

        setSuffixIndices(*ChildPair.second,
                             LabelHeight + ChildPair.second->size());
      }
    }

    if (IsLeaf)
      CurrentNode.SuffixIdx = NumInstructionsInTree - LabelHeight;
  }

  /// \brief Construct the suffix tree for the prefix of the input mapping ending
  /// at \p EndIdx.
  ///
  /// Used to construct the full suffix tree iteratively. For more detail, see
  /// Ukkonen's algorithm.
  ///
  /// \param EndIdx The end index of the current prefix in the main string.
  /// \param NeedsLink The internal \p SuffixTreeNode that needs a suffix link.
  /// \param [in, out] SuffixesToAdd The number of suffixes that must be added
  /// to complete the suffix tree at the current phase.
  void extend(size_t EndIdx, SuffixTreeNode *NeedsLink, size_t &SuffixesToAdd) {
    while (SuffixesToAdd > 0) {

      // The length of the current mapping is 0, so we look at the last added
      // character to our substring.
      if (Active.Len == 0)
        Active.Idx = EndIdx;

      // The first and last character in the current substring we're looking at.
      unsigned FirstChar = Mapping.elementAt(Active.Idx);
      unsigned LastChar = Mapping.elementAt(EndIdx);

      // During the previous step, we stopped on a node *and* it has no
      // transition to another node on the next character in our current
      // suffix.
      if (Active.Node->Children[FirstChar] == nullptr) {
        insertNode(Active.Node, EndIdx, EmptyIdx, FirstChar, true);

        // The active node is an internal node, and we visited it, so it must
        // need a link if it doesn't have one.
        if (NeedsLink) {
          NeedsLink->Link = Active.Node;
          NeedsLink = nullptr;
        }
      } else {
        // There *is* a match, so we have to traverse the tree and find out
        // where to put the node.
        SuffixTreeNode *NextNode = Active.Node->Children[FirstChar];

        // The child that we want to move to already contains our current mapping
        // up to some point.Move to the index in that node where we'd have a
        // mismatch and try again.
        size_t SubstringLen = NextNode->size();
        if (Active.Len >= SubstringLen) {
          Active.Idx += SubstringLen;
          Active.Len -= SubstringLen;
          Active.Node = NextNode;
          continue;
        }

        // The mapping is already in the tree, so we're done.
        if (Mapping.elementAt(NextNode->StartIdx + Active.Len) == LastChar) {
          if (NeedsLink && Active.Node->StartIdx != EmptyIdx) {
            NeedsLink->Link = Active.Node;
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
        SplitNode->Children[Mapping.elementAt(NextNode->StartIdx)] = NextNode;

        // We visited an internal node, so we need to set suffix links
        // accordingly.
        if (NeedsLink != nullptr) 
          NeedsLink->Link = SplitNode;
    
        NeedsLink = SplitNode;
      }

      // We've added something new to the tree. Now we can move to the next
      // suffix.
      SuffixesToAdd--;
      if (Active.Node->StartIdx == EmptyIdx) {
        if (Active.Len > 0) {
          Active.Len--;

          // Move to the next suffix that we have to add.
          Active.Idx = EndIdx - SuffixesToAdd + 1;
        }
      } else {
        // Start the next phase at the next smallest suffix.
        Active.Node = Active.Node->Link;
      }
  }
}

public:
  /// Append a new string to \p Mapping and update the suffix tree.
  ///
  /// \param NewStr The string to append to the tree.
  void append(std::vector<unsigned> NewStr) {
    Mapping.MBBMappings.push_back(NewStr);

    // Save the old size so we can start at the end of the old string
    size_t PrevNumInstructions = NumInstructionsInTree;
    NumInstructionsInTree = PrevNumInstructions + NewStr.size();

    // Keep track of the number of suffixes we have to add of the current
    // prefix.
    size_t SuffixesToAdd = 0;
    SuffixTreeNode *NeedsLink = nullptr; // The last internal node added

    // PrevNumInstructions is initially 0 on the insertion of the first string.
    // At the insertion of the next string, PrevNumInstructions is the index of
    // the end of the previous string.
    for (size_t EndIdx = PrevNumInstructions; EndIdx < NumInstructionsInTree;
         EndIdx++) {
      SuffixesToAdd++;
      NeedsLink = nullptr;
      LeafEndIdx = EndIdx;
      extend(EndIdx, NeedsLink, SuffixesToAdd);
    }

    // Now that we're done constructing the tree, we can set the suffix indices
    // of each leaf.
    size_t LabelHeight = 0;
    assert(Root && "Root node was null!");
    setSuffixIndices(*Root, LabelHeight);
  }

  /// \brief Traverse the tree depth-first and return the node whose substring
  /// is longest and appears at least twice.
  ///
  /// \param Node The current node being visited in the traversal.
  /// \param LabelHeight The length of the node currently being visited.
  /// \param MaxLen [in, out] The length of the longest repeated substring.
  /// \param SubstringStartIdx [in, out] The start index of the first
  /// occurrence of the longest repeated substring found during the query.
  /// \param NumOccurrences [in, out] The number of times the longest repeated
  /// substring appears.
  void longestRepeatedNode(SuffixTreeNode &N, size_t LabelHeight,
                           size_t &MaxLen, size_t &SubstringStartIdx,
                           size_t &NumOccurrences) {

    // We hit an internal node, so we can traverse further down the tree.
    // For each child, traverse down as far as possible and set MaxHeight
    if (N.SuffixIdx == EmptyIdx) {
      for (auto &ChildPair : N.Children) {
        if (ChildPair.second && ChildPair.second->IsInTree)
          longestRepeatedNode(*ChildPair.second,
                              LabelHeight + ChildPair.second->size(), MaxLen,
                              SubstringStartIdx, NumOccurrences);
      }
    }

    // We hit a leaf, so update MaxHeight if we've gone further down the
    // tree
    else if (N.SuffixIdx != EmptyIdx && MaxLen < (LabelHeight - N.size())) {
      MaxLen = LabelHeight - N.size();
      SubstringStartIdx = N.SuffixIdx;
      NumOccurrences = (size_t)N.Parent->Children.size();
    }
  }

  /// \brief Return a \p vector representing the longest substring of \p
  /// Mapping which is repeated at least one time.
  ///
  /// Returns an empty vector if no such mapping exists.
  std::vector<unsigned> longestRepeatedSubstring() {
    size_t MaxHeight = 0;
    size_t FirstChar = 0;
    SuffixTreeNode &N = *Root;
    size_t NumOccurrences = 0;

    longestRepeatedNode(N, 0, MaxHeight, FirstChar, NumOccurrences);
    std::vector<unsigned> Longest;

    for (size_t Idx = 0; Idx < MaxHeight; Idx++)
      Longest.push_back(Mapping.elementAt(Idx + FirstChar));

    return Longest;
  }

  /// Perform a depth-first search for \p QueryString on the suffix tree.
  ///
  /// \param QueryString The mapping to search for.
  /// \param CurrIdx The current index in the query mapping that is being
  /// matched against.
  /// \param CurrSuffixTreeNode The suffix tree node being searched in.
  ///
  /// \returns A \p SuffixTreeNode that \p QueryString appears in if such a
  /// node exists, and nullptr otherwise.
  SuffixTreeNode *findString(const std::vector<unsigned> &QueryString,
                             size_t &CurrIdx,
                             SuffixTreeNode *CurrSuffixTreeNode) {
    SuffixTreeNode *RetSuffixTreeNode;
    SuffixTreeNode *NextNode;

    if (CurrSuffixTreeNode == nullptr ||
        CurrSuffixTreeNode->IsInTree == false) {
      RetSuffixTreeNode = nullptr;
    } else if (CurrSuffixTreeNode->StartIdx == EmptyIdx) {
      // If we're at the root we have to check if there's a child, and move to
      // that child. We don't consume the character since Root represents the
      // empty string.
      if (CurrSuffixTreeNode->Children[QueryString[CurrIdx]] != nullptr &&
          CurrSuffixTreeNode->Children[QueryString[CurrIdx]]->IsInTree) {
        NextNode = CurrSuffixTreeNode->Children[QueryString[CurrIdx]];
        RetSuffixTreeNode = findString(QueryString, CurrIdx, NextNode);
      } else {
        RetSuffixTreeNode = nullptr;
      }
    }

    // The node represents a non-empty string, so we should match against it and
    // check its children if necessary.
    else {
      size_t StrIdx = CurrSuffixTreeNode->StartIdx;
      enum FoundState { ExactMatch, SubMatch, Mismatch };
      FoundState Found = ExactMatch;

      // Increment CurrIdx while checking the mapping for equivalence. Set
      // Found and possibly break based off of the case we find.
      while (CurrIdx < QueryString.size() - 1) {

        // Failure case 1: We moved outside the string, BUT we matched
        // perfectly up to that point.
        if (StrIdx > *(CurrSuffixTreeNode->EndIdx)) {
          Found = SubMatch;
          break;
        }

        // Failure case 2: We have a true mismatch.
        if (QueryString[CurrIdx] != Mapping.elementAt(StrIdx)) {
          Found = Mismatch;
          break;
        }

        StrIdx++;
        CurrIdx++;
      }

      // Decide whether or not we should keep searching.
      switch (Found) {
      case ExactMatch:
        RetSuffixTreeNode = CurrSuffixTreeNode;
        break;
      case SubMatch:
        NextNode = CurrSuffixTreeNode->Children[QueryString[CurrIdx]];
        RetSuffixTreeNode = findString(QueryString, CurrIdx, NextNode);
        break;
      case Mismatch:
        RetSuffixTreeNode = nullptr;
        break;
      }
    }

    return RetSuffixTreeNode;
  }

  /// \brief Remove a node from a tree and all nodes representing proper
  /// suffixes of that node's string.
  ///
  /// This is used in the outlining algorithm to reduce the number of
  /// overlapping candidates.
  void prune(SuffixTreeNode *N) {
    N->IsInTree = false;

    // Remove all proper non-empty suffixes of this node from the tree.
    for (SuffixTreeNode *T = N->Link; T && T != Root; T = T->Link)
      T->IsInTree = false;
  }

  /// Find each occurrence of of a mapping in \p Mapping and prune their nodes.
  ///
  /// \param QueryString The mapping to search for.
  ///
  /// \returns A list of pairs of \p Strings and offsets into \p Mapping
  /// representing each occurrence if \p QueryString is present. Returns
  /// an empty vector if there are no occurrences.
  std::vector<std::pair<std::vector<unsigned>, size_t>>
  findOccurrencesAndPrune(const std::vector<unsigned> &QueryString) {
    size_t Len = 0;
    std::vector<std::pair<std::vector<unsigned>, size_t>> Occurrences;
    SuffixTreeNode *N = findString(QueryString, Len, Root);

    if (!N || !N->IsInTree)
      return Occurrences;

    // We matched exactly, so we're in a suffix. There's then exactly one
    // occurrence.
    if (N->SuffixIdx != EmptyIdx) {
      size_t StartIdx = N->SuffixIdx;
      Occurrences.push_back(
          make_pair(Mapping.mappingContaining(StartIdx), StartIdx));
    } else {
      // There are occurrences. Collect them and then prune them from the tree.
      SuffixTreeNode *M;

      for (auto &ChildPair : N->Children) {
        M = ChildPair.second;

        if (M && M->SuffixIdx != EmptyIdx) {
          size_t StartIdx = M->SuffixIdx;
          Occurrences.push_back(
              make_pair(Mapping.mappingContaining(StartIdx), StartIdx));
        }
      }
    }

    prune(N);

    return Occurrences;
  }

  /// \brief Return the number of times the mapping \p QueryString appears in \p
  /// Mapping.
  size_t numOccurrences(const std::vector<unsigned> &QueryString) {
    size_t Dummy;
    SuffixTreeNode *N = findString(QueryString, Dummy, Root);

    // If it isn't in the tree, then just return 0.
    if (!N)
      return 0;

    // If it's a suffix it only appears once.
    if (N->SuffixIdx != EmptyIdx)
      return 1;

    // Otherwise, it appears in the number of strings that we can move to
    // from this point.
    return N->Children.size();
  }

  /// \brief Create a suffix tree from a list of strings \p Strings, treating
  /// that list as a flat string.
  SuffixTree(const ProgramMapping &Strings) {
    Root = insertNode(nullptr, EmptyIdx, EmptyIdx, 0, false);
    Active.Node = Root;

    for (auto &Str : Strings.MBBMappings)
      append(Str);
  }
};

/// \brief An individual sequence of instructions to be replaced with a call
/// to an outlined function.
struct Candidate {
  /// \brief The index of the \p MachineBasicBlock in the worklist containing
  /// the first occurrence of this \p Candidate.
  size_t IdxOfMBB;

  /// \brief The start index of this candidate in its containing
  /// \p MachineBasicBlock.
  size_t StartIdxInMBB;

  /// The number of instructions in this \p Candidate.
  size_t Len;

  /// \brief The flat start index of this Candidate's sequence of instructions
  /// in the \p ProgramMapping.
  size_t FlatMappingStartIdx;

  /// The index of this \p Candidate's \p OutlinedFunction in the list of
  /// \p OutlinedFunctions.
  size_t FunctionIdx;

  /// Represents the sequence of instructions that will be outlined.
  ///
  /// Stored to ensure that the current candidate isn't being outlined from
  /// somewhere that has already been outlined from.
  std::vector<unsigned> Str;

  Candidate(size_t IdxOfMBB_, size_t StartIdxInMBB_, size_t Len_,
            size_t FlatMappingStartIdx_, size_t FunctionIdx_,
            std::vector<unsigned> Str_)
      : IdxOfMBB(IdxOfMBB_), StartIdxInMBB(StartIdxInMBB_), Len(Len_),
        FlatMappingStartIdx(FlatMappingStartIdx_), FunctionIdx(FunctionIdx_),
        Str(Str_) {}

  /// \brief Used to ensure that \p Candidates are outlined in an order that
  /// preserves the start and end indices of other \p Candidates.
  bool operator<(const Candidate &rhs) const {
    return FlatMappingStartIdx > rhs.FlatMappingStartIdx;
  }
};

/// \brief Stores created outlined functions and the information needed to
/// construct them.
struct OutlinedFunction {
  /// The actual outlined function created.
  /// This is initialized after we go through and create the actual function.
  MachineFunction *MF;

  /// \brief The MachineBasicBlock containing the first occurrence of the
  /// mapping associated with this function.
  MachineBasicBlock *OccBB;

  /// The start index of the instructions to outline in \p OccBB.
  size_t StartIdxInBB;

  /// The end index of the instructions to outline in \p OccBB.
  size_t EndIdxInBB;

  /// A number used to identify this function in the outlined program.
  size_t Name;

  /// The number this function will be given in the \p ProgramMapping.
  size_t Id;

  /// The number of times that this function has appeared.
  size_t OccurrenceCount;

  OutlinedFunction(MachineBasicBlock *OccBB_, size_t StartIdxInBB_,
                   size_t EndIdxInBB_, size_t Name_, size_t Id_,
                   size_t OccurrenceCount_)
      : OccBB(OccBB_), StartIdxInBB(StartIdxInBB_), EndIdxInBB(EndIdxInBB_),
        Name(Name_), Id(Id_), OccurrenceCount(OccurrenceCount_) {}
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

  /// \brief Used to either hash functions or mark them as illegal to outline
  /// depending on the instruction.
  DenseMap<MachineInstr *, unsigned, MachineInstrExpressionTrait>
      InstructionIntegerMap;

  /// The last value assigned to an instruction we ought not to outline.
  /// Set to -3 to avoid attempting to query the \p DenseMap in
  /// \p SuffixTreeNode for the tombstone and empty keys given by the
  /// unsigned \p DenseMap template specialization.
  unsigned CurrIllegalInstrMapping = -3;

  /// The last value assigned to an instruction we can outline.
  unsigned CurrLegalInstrMapping = 0;

  /// The ID of the last function created.
  size_t CurrentFunctionID;

  /// The mapping of the program from MachineInstructions to unsigned integers.
  ProgramMapping Mapping;

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

  /// Construct an instruction-integer mapping for a \p MachineBasicBlock.
  ///
  /// This function translates each instruction into an unsigned integer. Two
  /// instructions are assigned the same integer if they are identical. If an
  /// instruction is deemed unsafe to outline, then it will be assigned an
  /// unique integer. The resultant mapping is placed into a suffix tree and
  /// queried for candidates.
  ///
  /// \param [out] Container Filled with the instruction-integer mappings for
  /// the program.
  /// \param BB The \p MachineBasicBlock to be translated into integers.
  void buildInstructionMapping(std::vector<unsigned> &Container,
                        MachineBasicBlock &BB,
                        const TargetRegisterInfo &TRI,
                        const TargetInstrInfo &TII);

  /// \brief Replace the sequences of instructions represented by the
  /// \p Candidates in \p CandidateList with calls to \p MachineFunctions
  /// described in \p FunctionList.
  ///
  /// \param Worklist The basic blocks in the program in order of appearance.
  /// \param CandidateList A list of candidates to be outlined.
  /// \param FunctionList A list of functions to be inserted into the program.
  bool outline(Module &M, std::vector<MachineBasicBlock *> &Worklist,
               std::vector<Candidate> &CandidateList,
               std::vector<OutlinedFunction> &FunctionList,
               ProgramMapping &Mapping);

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
  /// \param WorkList The basic blocks in the program in order of appearance.
  /// \param ST The suffix tree for the program.
  void buildCandidateList(std::vector<Candidate> &CandidateList,
                          std::vector<OutlinedFunction> &FunctionList,
                          std::vector<MachineBasicBlock *> &Worklist,
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

void MachineOutliner::buildInstructionMapping(std::vector<unsigned> &Container,
                                       MachineBasicBlock &MBB,
                                       const TargetRegisterInfo &TRI,
                                       const TargetInstrInfo &TII) {
  for (MachineInstr &MI : MBB) {
    // First, check if the current instruction is legal to outline at all.
    bool IsSafeToOutline = TII.isLegalToOutline(MI);

    // If it's not, give it a bad number.
    if (!IsSafeToOutline) {
      Container.push_back(CurrIllegalInstrMapping);
      CurrIllegalInstrMapping--;
      assert(CurrLegalInstrMapping < CurrIllegalInstrMapping &&
             "Instruction mapping overflow!");
      assert(CurrIllegalInstrMapping != (unsigned)-1 &&
             CurrIllegalInstrMapping != (unsigned)-2 &&
             "Mapping cannot be DenseMap tombstone or empty key!");
      continue;
    }

    // It's safe to outline, so we should give it a legal integer. If it's in
    // the map, then give it the previously assigned integer. Otherwise, give
    // it the next available one.
    auto I = InstructionIntegerMap.insert(
        std::make_pair(&MI, CurrLegalInstrMapping));

    if (I.second)
      CurrLegalInstrMapping++;

    unsigned MINumber = I.first->second;
    Container.push_back(MINumber);
    CurrentFunctionID++;
    assert(CurrLegalInstrMapping < CurrIllegalInstrMapping &&
           "Instruction mapping overflow!");
    assert(CurrLegalInstrMapping != (unsigned)-1 &&
           CurrLegalInstrMapping != (unsigned)-2 &&
           "Mapping cannot be DenseMap tombstone or empty key!");
  }
}

void MachineOutliner::buildCandidateList(
    std::vector<Candidate> &CandidateList,
    std::vector<OutlinedFunction> &FunctionList,
    std::vector<MachineBasicBlock *> &Worklist, SuffixTree &ST) {

  // TODO: It would be better to use a "most beneficial substring" query if we
  // decide to be a bit smarter and use a dynamic programming approximation
  // scheme. For a naive greedy choice, LRS and MBS appear to be about as
  // effective as each other. This is because both can knock out a candidate
  // that would be better, or would lead to a better combination of candidates
  // being chosen.
  std::vector<unsigned> CandidateSequence = ST.longestRepeatedSubstring();

  // FIXME: Use the following cost model.
  // Weight = Occurrences * length
  // Benefit = Weight - [Len(outline prologue) + Len(outline epilogue) +
  // Len(functon call)]
  if (CandidateSequence.size() >= 2) {

    // Query the tree for candidates until we run out of candidates to outline.
    do {
      std::vector<std::pair<std::vector<unsigned>, size_t>> Occurrences =
          ST.findOccurrencesAndPrune(CandidateSequence);

      assert(Occurrences.size() > 0 &&
             "Longest repeated substring has no occurrences.");

      // If there are at least two occurrences of this candidate, then we should
      // make it a function and keep track of it.
      if (Occurrences.size() >= 2) {
        std::pair<std::vector<unsigned>, size_t> FirstOcc = Occurrences[0];

        // The (flat) start index of the Candidate in the ProgramMapping.
        size_t FlatStartIdx = FirstOcc.second;

        // Use that to find the index of the string/MachineBasicBlock it appears
        // in and the point that it begins in in that string/MBB.
        std::pair<size_t, size_t> FirstIdxAndOffset =
            Mapping.locationOf(FlatStartIdx);

        // From there, we can tell where the mapping starts and ends in the first
        // occurrence so that we can copy it over.
        size_t StartIdxInBB = FirstIdxAndOffset.second;
        size_t EndIdxInBB = StartIdxInBB + CandidateSequence.size() - 1;

        // Keep track of the MachineBasicBlock and its parent so that we can
        // copy from it later.
        MachineBasicBlock *OccBB = Worklist[FirstIdxAndOffset.first];
        FunctionList.push_back(OutlinedFunction(
            OccBB, StartIdxInBB, EndIdxInBB, FunctionList.size(),
            CurrentFunctionID, Occurrences.size()));

        // Save each of the occurrences for the outlining process.
        for (auto &Occ : Occurrences) {
          std::pair<size_t, size_t> IdxAndOffset =
              Mapping.locationOf(Occ.second);

          CandidateList.push_back(Candidate(
              IdxAndOffset.first,      // Idx of MBB containing candidate.
              IdxAndOffset.second,     // Starting idx in that MBB.
              CandidateSequence.size(),  // Candidate length.
              Occ.second,              // Start index in the full string.
              FunctionList.size() - 1, // Idx of the corresponding function.
              CandidateSequence          // The actual string.
              ));
        }

        CurrentFunctionID++;
        FunctionsCreatedStat++;
      }

      // Find the next candidate and continue the process.
      CandidateSequence = ST.longestRepeatedSubstring();
    } while (CandidateSequence.size() >= 2);

    // Sort the candidates in decending order. This will simplify the outlining
    // process when we have to remove the candidates from the mapping by
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

  DEBUG(dbgs() << "OF.StartIdxInBB = " << OF.StartIdxInBB << "\n";
        dbgs() << "OF.EndIdxInBB = " << OF.EndIdxInBB << "\n";);

  // Insert instructions into the function and a custom outlined
  // prologue/epilogue.
  MF.insert(MF.begin(), MBB);
  TII->insertOutlinerEpilogue(*MBB, MF);

  MachineBasicBlock::iterator It = OF.OccBB->begin();
  std::advance(It, OF.EndIdxInBB);

  for (size_t i = 0, e = OF.EndIdxInBB - OF.StartIdxInBB + 1; i != e; i++) {
    MachineInstr *MI = MF.CloneMachineInstr(&*It);

    // Each cloned memory operand references the old function.
    // Drop the references.
    MI->dropMemRefs();

    MBB->insert(MBB->begin(), MI);
    It--;
  }

  TII->insertOutlinerPrologue(*MBB, MF);

  DEBUG(dbgs() << "New function: \n"; dbgs() << *Name << ":\n";
        for (MachineBasicBlock &MBB
             : MF) MBB.dump(););

  return &MF;
}

bool MachineOutliner::outline(Module &M,
                              std::vector<MachineBasicBlock *> &Worklist,
                              std::vector<Candidate> &CandidateList,
                              std::vector<OutlinedFunction> &FunctionList,
                              ProgramMapping &Mapping) {
  bool OutlinedSomething = false;

  // Create an outlined function for each candidate.
  for (OutlinedFunction &OF : FunctionList)
    OF.MF = createOutlinedFunction(M, OF);

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

    size_t StartIdx = C.StartIdxInMBB;
    size_t EndIdx = StartIdx + C.Len;

    // If the index is below 0, then we must have already outlined from it.
    bool AlreadyOutlinedFrom = EndIdx - StartIdx > C.Len;

    // Check if we have any different characters in the mapping collection versus
    // the mapping we want to outline. If so, then we must have already outlined
    // from the spot this candidate appeared at.
    if (!AlreadyOutlinedFrom) {
      for (size_t i = StartIdx; i < EndIdx; i++) {
        size_t j = i - StartIdx;
        if (Mapping.MBBMappings[C.IdxOfMBB][i] != C.Str[j]) {
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

    // Remove the candidate from the mapping in the suffix tree first, and
    // replace it with the associated function's id.
    // auto Begin = Mapping.MBBMappings[C.IdxOfMBB]->begin() + C.StartIdxInMBB;
    auto Begin = Mapping.MBBMappings[C.IdxOfMBB].begin() + C.StartIdxInMBB;
    auto End = Begin + C.Len;

    Mapping.MBBMappings[C.IdxOfMBB].erase(Begin, End);
    Mapping.MBBMappings[C.IdxOfMBB].insert(Begin, FunctionList[C.FunctionIdx].Id);

    // Now outline the function in the module using the same idea.
    MachineFunction *MF = FunctionList[C.FunctionIdx].MF;
    MachineBasicBlock *MBB = Worklist[C.IdxOfMBB];
    const TargetSubtargetInfo *STI = &(MF->getSubtarget());
    const TargetInstrInfo *TII = STI->getInstrInfo();

    // Now, insert the function name and delete the instructions we don't need.
    MachineBasicBlock::iterator StartIt = MBB->begin();
    MachineBasicBlock::iterator EndIt = StartIt;

    std::advance(StartIt, StartIdx);
    std::advance(EndIt, EndIdx);
    StartIt = TII->insertOutlinedCall(M, *MBB, StartIt, *MF);
    ++StartIt;
    MBB->erase(StartIt, EndIt);
  }

  return OutlinedSomething;
}

bool MachineOutliner::runOnModule(Module &M) {
  MachineModuleInfo &MMI = getAnalysis<MachineModuleInfo>();
  std::vector<MachineBasicBlock *> Worklist;

  const TargetSubtargetInfo *STI =
      &(MMI.getMachineFunction(*M.begin()).getSubtarget());
  const TargetRegisterInfo *TRI = STI->getRegisterInfo();
  const TargetInstrInfo *TII = STI->getInstrInfo();

  // Set up the suffix tree by creating strings for each basic block.
  // Note: This means that the i-th mapping and the i-th MachineBasicBlock
  // in the work list correspond to each other. It also means that the
  // j-th unsigned in that mapping and the j-th instruction in that
  // MBB correspond with each other.
  for (Function &F : M) {
    MachineFunction &MF = MMI.getMachineFunction(F);

    if (F.empty() || !TII->functionIsSafeToOutlineFrom(F))
      continue;

    for (MachineBasicBlock &MBB : MF) {
      Worklist.push_back(&MBB);
      std::vector<unsigned> Container;
      buildInstructionMapping(Container, MBB, *TRI, *TII);
      Mapping.MBBMappings.push_back(Container);
    }
  }

  SuffixTree ST(Mapping);

  // Find all of the candidates for outlining and then outline them.
  bool OutlinedSomething = false;
  std::vector<Candidate> CandidateList;
  std::vector<OutlinedFunction> FunctionList;

  CurrentFunctionID = InstructionIntegerMap.size();
  buildCandidateList(CandidateList, FunctionList, Worklist, ST);
  OutlinedSomething =
      outline(M, Worklist, CandidateList, FunctionList, Mapping);

  return OutlinedSomething;
}
