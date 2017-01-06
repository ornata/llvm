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

// Anonymous namespace for helper functions.
namespace {

/// Represents a non-existent index into the string.

const size_t EmptyIdx = -1; /// Represents an undefined index.

/// Stores instruction-integer mappings for MachineBasicBlocks in the program.
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
/// This is where the \p ProgramMapping comes in. It's equipped with some
/// indexing functions that let us access the vector of vectors as if it was
/// a flat vector. Furthermore, it allows us to keep track of which mapping
/// corresponds to which MachineBasicBlock. This allows us to quickly decide
/// where in the program to outline candidates from.
struct ProgramMapping {
  std::vector<std::vector<unsigned>> MBBMappings;

  /// Returns the index of the mapping containing the index \p Offset.
  ///
  /// \param [in, out] Offset The query offset. Filled with the local offset in
  /// the returned mapping on success.
  ///
  /// \returns The index of the mapping that \p Offset appears in.
  size_t mappingIdx(size_t &Offset) {
    size_t MappingIdx;
    size_t NumMappings = MBBMappings.size();
    for (MappingIdx = 0; MappingIdx < NumMappings; MappingIdx++) {

      // First get the length of the current string...
      size_t CurrSize = MBBMappings[MappingIdx].size();

      // Now check if the offset is *less* than the max index of the string.
      // If this is true, then the offset lies inside this string.
      if (Offset < CurrSize)
        break;

      // Otherwise, move over to the next string.
      Offset -= CurrSize;
    }

    // We should always stop before we hit MBBMappings.size() for the outliner
    // because we're always looking for something inside the string.
    assert(MappingIdx < MBBMappings.size() && "Mapping index out of bounds!");

    return MappingIdx;
  }

  /// Returns the element of \p ProgramMapping as a 2D string at \p QueryIdx.
  unsigned elementAt(size_t QueryIdx) {
    size_t MappingIdx = mappingIdx(QueryIdx);
    return MBBMappings[MappingIdx][QueryIdx];
  }

  /// Returns the index of the \p String containing the index \p QueryIdx and
  /// the offset \p QueryIdx maps into in that string.
  std::pair<unsigned, unsigned> mappingIdxAndOffset(size_t QueryIdx) {
    size_t MappingIdx = mappingIdx(QueryIdx);
    return std::make_pair(MappingIdx, QueryIdx);
  }

  /// Returns the string that contains the index \p QueryIdx in the
  /// \p ProgramMapping \p ProgramMapping and the offset into that string that
  /// \p QueryIdx maps to.
  std::vector<unsigned> &mappingContaining(size_t QueryIdx) {
    size_t MappingIdx = mappingIdx(QueryIdx);
    return MBBMappings[MappingIdx];
  }
};

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
/// in \p Link. Each leaf node stores the start index of its respective
/// suffix in \p SuffixIdx.
struct SuffixTreeNode {

  /// The parent of this node.
  SuffixTreeNode *Parent = nullptr;

  /// The children of this node.
  /// A child existing on an unsigned integer implies that from the string
  /// represented by the current node, there is a way to reach another
  /// string by tacking that character on the end of the current string.
  std::map<unsigned, SuffixTreeNode *> Children;

  /// Represents whether or not the node has been pruned from the tree.
  /// If a node has been pruned, then this is set to false. Any node where
  /// \p IsInTree is false is treated as if it, and all of its children,
  /// don't exist in the tree.
  bool IsInTree = true;

  /// Start index of this node's substring in the main string.
  size_t StartIdx = EmptyIdx;

  /// End index of this node's substring in the main string.
  /// This must be updated for every node at the end of every phase in the
  /// algorithm. Because of this, we store it as a pointer. Most nodes'
  /// \p EndIdx value points to \p LeafEnd in the \p SuffixTree. Thus, when
  /// we update \p LeafEnd, we update all required nodes' end indices in one
  /// step. The other nodes, split nodes, don't require updates per-iteration
  /// and thus maintain their own end indices.
  size_t *EndIdx = nullptr;

  /// Start index of this node's associated suffix if it is a leaf.
  size_t SuffixIdx = EmptyIdx;

  /// \brief A pointer stored in internal nodes which points to the internal
  /// node representing the same string missing the first character.
  ///
  /// This is used as a shortcut in the construction algorithm.
  ///
  /// To see why, first, let's show that for some node with associated string
  /// S', if S is a child of S', then the nodes representing each suffix of
  /// S' must have S as a child as well.
  ///
  /// Say we're adding some new suffix S to the tree as a child of some node
  /// N. Suppose that N has length k. Now, let S' be the string represented
  /// by N. Since we are adding S as a child of N, then the concatenation
  /// S'S must be in the entire string. Since S' has length k, it must have
  /// k suffixes. Since S follows S', it must be included in each of these
  /// suffixes as well. Therefore, for each suffix S'i of S', S'iS is also
  /// in the tree.
  ///
  /// With this in mind, it would make sense to store a link between each
  /// of these nodes as we add them to the tree. Otherwise we'd have to
  /// query the tree for each of these suffixes and insert a child at each
  /// of them. If we keep track of links between these nodes, we can skip
  /// the extra queries and jump right to the spots we need to insert the
  /// new suffix.
  ///
  /// The suffix link is also used during the tree pruning process to let us
  /// quickly throw out a bunch of potential overlaps. Say we have a string
  /// S we want to outline. Then each of its suffixes contribute to at least
  /// one overlapping case. Therefore, we can follow the suffix links
  /// starting at the node associated with S to the root and "delete" those
  /// nodes, save for the root. For each candidate, this removes
  /// O(|candidate|) overlaps from the search space.
  SuffixTreeNode *Link;

  /// Points to the node whose suffix link is this node.
  ///
  /// This is used to prune nodes that overlap with this one, but wouldn't be
  /// visited while walking over suffix links. Suffix links would only let us
  /// prune the nodes which are proper suffixes of this node's string. This
  /// allows us to catch proper prefixes as well.
  SuffixTreeNode *BackLink;

  /// Create a node starting at \p S, ending at \p E, and with suffix link \p L.
  SuffixTreeNode(size_t StartIdx_, size_t *EndIdx_, SuffixTreeNode *Link_)
      : StartIdx(StartIdx_), EndIdx(EndIdx_), Link(Link_) {}

  /// Default constructor for \p SuffixTreeNodes.
  SuffixTreeNode() {}

  /// The length of the substring associated with this node.
  size_t size() {
    size_t SubstringLen = 0;

    if (StartIdx != EmptyIdx)
      SubstringLen = *EndIdx - StartIdx + 1;

    return SubstringLen;
  }
};

/// Keeps track of what we're currently working on in the tree during the
/// construction algorithm.
struct ActiveState {

  /// The current node in the tree.
  SuffixTreeNode *Node;

  /// Idx of the active character in the current substring.
  size_t Idx = EmptyIdx;

  /// Len of the current substring.
  size_t Len = 0;
};

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
/// suffix tree construction. Ukkonen's algorithm is explained in more detail
/// in the paper by Esko Ukkonen "On-line construction of suffix trees. The
/// paper is available at
///
/// https://www.cs.helsinki.fi/u/ukkonen/SuffixT1withFigs.pdf
///
/// Note that despite the main structure being a tree, the implementation
/// of the suffix tree really forms a digraph. This is because of the presence
/// of so-called "suffix links" between internal nodes. Suffix links are used
/// to avoid repeatedly querying the tree during certain steps of Ukkonen's
/// algorithm. (Suffix links are also better explained on the \p Link member
/// variable). A result of this is that suffix trees can contain directed
/// cycles. Therefore, unfortunately, we have to put up with using raw pointers
/// rather than smart pointers; although we're implementing a structure which
/// for all intents and purposes is a tree, the implementation driving that
/// structure is sadly a digraph.
class SuffixTree {
private:
  /// Maintains each node in the tree.
  /// Note that because this is a bump pointer allocator, we don't have to
  /// manually delete the nodes in the tree.
  BumpPtrAllocator NodeAllocator;

  /// Maintains the end indices of the internal nodes in the tree.
  /// Each internal node is guaranteed to never have its end index change.
  /// Note that because this is a bump pointer allocator, we don't have to
  /// manually delete the end indices of the nodes in the tree.
  BumpPtrAllocator InternalEndIdxAllocator;

  /// The root of the suffix tree.
  /// The root represents the empty string. It is maintained by the
  /// NodeAllocator like every other node in the tree. However, we need access
  /// to it so that we can traverse and query the tree.
  SuffixTreeNode *Root = nullptr;

  /// Contains the instruction-unsigned mappings for the basic blocks of the
  /// program.
  ProgramMapping Mapping;

  /// The end index of each leaf in the tree.
  /// For each intermediate suffix tree constructed in the algorithm, we have
  /// to update the end index of every leaf in the tree. By storing this and
  /// updating it, we can avoid traversing the tree and updating each
  /// individual leaf.
  size_t CurrPrefixEndIdx = -1;

  /// The sum of the lengths of the strings that form the input string.
  size_t NumInstructionsInTree = 0;

  /// The active state for Ukkonen's algorithm.
  ActiveState Active;

  /// Allocate a node and add it to the tree.
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
    size_t *E = &CurrPrefixEndIdx;

    if (!IsLeaf)
      E = new (InternalEndIdxAllocator) size_t(EndIdx);

    N = new (NodeAllocator) SuffixTreeNode(StartIdx, E, nullptr);
    N->Parent = Parent;

    if (Parent)
      Parent->Children[Edge] = N;

    return N;
  }

  /// Set the end of each leaf in the tree after constructing it.
  ///
  /// Each leaf will store the start index of its respective suffix after
  /// setting the leaf ends in its \p SuffixIdx.
  void setLeafEnds(SuffixTreeNode &CurrentNode, size_t LabelHeight) {
    bool IsLeaf = true;

    for (auto &ChildPair : CurrentNode.Children) {
      if (ChildPair.second != nullptr) {
        IsLeaf = false;

        assert(ChildPair.second && "Node has a null child!");

        setLeafEnds(*ChildPair.second, LabelHeight + ChildPair.second->size());
      }
    }

    if (IsLeaf)
      CurrentNode.SuffixIdx = NumInstructionsInTree - LabelHeight;
  }

  /// \brief Construct the suffix tree for the prefix of the input string ending
  /// at
  /// \p EndIdx.
  ///
  /// Used to construct the full suffix tree iteratively. For more detail,
  /// see Ukkonen's algorithm.
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
          Active.Node->BackLink = NeedsLink;
          NeedsLink = nullptr;
        }
      } else {
        // There *is* a match, so we have to traverse the tree and find out
        // where to put the node.
        SuffixTreeNode *NextNode = Active.Node->Children[FirstChar];

        // The child that we want to move to already contains our current string
        // up to some point.Move to the index in that node where we'd have a
        // mismatch and try again.
        size_t SubstringLen = NextNode->size();
        if (Active.Len >= SubstringLen) {
          Active.Idx += SubstringLen;
          Active.Len -= SubstringLen;
          Active.Node = NextNode;
          continue;
        }

        // The string is already in the tree, so we're done.
        if (Mapping.elementAt(NextNode->StartIdx + Active.Len) == LastChar) {
          if (NeedsLink && Active.Node->StartIdx != EmptyIdx) {
            NeedsLink->Link = Active.Node;
            Active.Node->BackLink = NeedsLink;
            NeedsLink = nullptr;
          }

          Active.Len++;
          break;
        }

        // If the other two cases don't hold, then we must have found a
        // mismatch. In this case, we split the edge to represent the two
        // choices: the old string we found, or the string on the mismatch.
        SuffixTreeNode *SplitNode =
            insertNode(Active.Node, NextNode->StartIdx,
                       NextNode->StartIdx + Active.Len - 1, FirstChar, false);

        // Create the new node...
        insertNode(SplitNode, EndIdx, EmptyIdx, LastChar, true);

        // The old node's parent becomes the split node.
        NextNode->StartIdx += Active.Len;
        NextNode->Parent = SplitNode;
        SplitNode->Children[Mapping.elementAt(NextNode->StartIdx)] = NextNode;

        // We visited an internal node, so we have to update the suffix link.
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

          // Move one index over in the string for the next step
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
    // At the
    // insertion of the next string, PrevNumInstructions is the index of the end
    // of the
    // previous string.
    for (size_t EndIdx = PrevNumInstructions; EndIdx < NumInstructionsInTree;
         EndIdx++) {
      SuffixesToAdd++;
      NeedsLink = nullptr;
      CurrPrefixEndIdx = EndIdx;
      extend(EndIdx, NeedsLink, SuffixesToAdd);
    }

    // Set the leaf ends so we can query the tree.
    size_t LabelHeight = 0;

    assert(Root && "Root node was null!");
    setLeafEnds(*Root, LabelHeight);
  }

  /// Traverse the tree depth-first and return the node whose substring is
  /// longest and appears at least twice.
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

  /// Return a new \p String representing the longest substring of \p
  /// Mapping which is repeated at least one time.
  ///
  /// \returns The longest repeated substring in the suffix tree if it exists,
  /// and nullptr otherwise.
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
  /// \param QueryString The string to search for.
  /// \param CurrIdx The current index in the query string that is being
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
      // empty string
      if (CurrSuffixTreeNode->Children[QueryString[CurrIdx]] != nullptr &&
          CurrSuffixTreeNode->Children[QueryString[CurrIdx]]->IsInTree) {
        NextNode = CurrSuffixTreeNode->Children[QueryString[CurrIdx]];
        RetSuffixTreeNode = findString(QueryString, CurrIdx, NextNode);
      } else {
        RetSuffixTreeNode = nullptr;
      }
    }

    // The node represents a non-empty string, so we should match against it and
    // check its children if necessary
    else {
      size_t StrIdx = CurrSuffixTreeNode->StartIdx;
      enum FoundState { ExactMatch, SubMatch, Mismatch };
      FoundState Found = ExactMatch;

      // Increment CurrIdx while checking the string for equivalence. Set
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

  /// Remove a node from a tree and shorter nodes that overlap with it.
  /// This allows us to prune a large number of overlaps from the tree without
  /// having to query the tree again.
  void prune(SuffixTreeNode *N) {
    N->IsInTree = false;

    // Remove all proper non-empty suffixes of this node from the tree.
    for (SuffixTreeNode *T = N->Link; T && T != Root; T = T->Link)
      T->IsInTree = false;

    // Remove all proper non-empty prefixes of this node from the tree.
    for (SuffixTreeNode *T = N->BackLink; T && T != Root; T = T->BackLink)
      T->IsInTree = false;
  }

  /// Find each occurrence of of a string in \p Mapping. Prunes each
  /// occurrence found from the tree by setting their nodes to invalid.
  ///
  /// \param QueryString The string to search for.
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

  /// Return the number of times the string \p QueryString appears in \p
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

  /// Create a suffix tree from a list of strings \p Strings, treating that list
  /// as a flat string.
  SuffixTree(const ProgramMapping &Strings) {
    Root = insertNode(nullptr, EmptyIdx, EmptyIdx, 0, false);
    Active.Node = Root;

    for (auto &Str : Strings.MBBMappings)
      append(Str);
  }
};

/// \brief An individual string of instructions to be replaced with a call
/// to an outlined function.
struct Candidate {
  /// \brief The index of the \p MachineBasicBlock in the worklist containing
  /// the first occurrence of this \p Candidate.
  size_t BBIdx;

  /// \brief The start index of this candidate in its containing \p String and
  /// \p MachineBasicBlock.
  size_t BBOffset;

  /// The number of instructions in this \p Candidate.
  size_t Len;

  /// The start index of \p Str in the full 2D string.
  size_t StartIdxIn2DString;

  /// The index of this \p Candidate's \p OutlinedFunction in the list of
  /// \p OutlinedFunctions.
  size_t FunctionIdx;

  /// The \p String that will be outlined.
  /// Stored to ensure that we don't have any overlaps.
  // std::vector<unsigned> *Str; // FIXME: This doesn't have to be stored
  std::vector<unsigned> Str;

  Candidate(size_t BBIdx_, size_t BBOffset_, size_t Len_,
            size_t StartIdxIn2DString_, size_t FunctionIdx_,
            std::vector<unsigned> Str_)
      : BBIdx(BBIdx_), BBOffset(BBOffset_), Len(Len_),
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
  MachineFunction *MF;

  /// \brief The MachineBasicBlock containing the first occurrence of the
  /// string associated with this function.
  MachineBasicBlock *OccBB;

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
  void buildProxyString(std::vector<unsigned> &Container, MachineBasicBlock &BB,
                        const TargetRegisterInfo &TRI,
                        const TargetInstrInfo &TII);

  /// \brief Replace the sequences of instructions represented by the
  /// \p Candidates in \p CandidateList with calls to \p MachineFunctions
  /// described in \p FunctionList.
  ///
  /// \param Worklist The basic blocks in the program in order of appearance.
  /// \param CandidateList A list of candidates to be outlined from the program.
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

void MachineOutliner::buildProxyString(std::vector<unsigned> &Container,
                                       MachineBasicBlock &MBB,
                                       const TargetRegisterInfo &TRI,
                                       const TargetInstrInfo &TII) {
  for (MachineInstr &MI : MBB) {
    errs() << "Mapping: \n";
    MI.dump();

    // First, check if the current instruction is legal to outline at all.
    bool IsSafeToOutline = TII.isLegalToOutline(MI);

    // If it's not, give it a bad number.
    if (!IsSafeToOutline) {
      Container.push_back(CurrIllegalInstrMapping);
      errs() << "(Assigned " << CurrIllegalInstrMapping << ")\n";
      CurrIllegalInstrMapping--;
      assert(CurrLegalInstrMapping < CurrIllegalInstrMapping && "Overflow.");
    } else {
      // It's safe to outline, so we should give it a legal integer. If it's in
      // the map, then give it the previously assigned integer. Otherwise, give
      // it the next available one.
      auto I = InstructionIntegerMap.insert(
          std::make_pair(&MI, CurrLegalInstrMapping));
      if (I.second)
        CurrLegalInstrMapping++;
      unsigned MINumber = I.first->second;
      Container.push_back(MINumber);
      errs() << "(Assigned " << MINumber << ")\n";
      CurrentFunctionID++;
      assert(CurrLegalInstrMapping < CurrIllegalInstrMapping && "Overflow");
    }
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
  // std::vector<unsigned> *CandidateString = ST.longestRepeatedSubstring();
  std::vector<unsigned> CandidateString = ST.longestRepeatedSubstring();

  // FIXME: Use the following cost model.
  // Weight = Occurrences * length
  // Benefit = Weight - [Len(outline prologue) + Len(outline epilogue) +
  // Len(functon call)]
  //
  // TODO: Experiment with dynamic programming-based approximation scheme. If it
  // isn't too memory intensive, we really ought to switch to it.
  if (CandidateString.size() >= 2) {

    // Query the tree for candidates until we run out of candidates to outline.
    do {
      std::vector<std::pair<std::vector<unsigned>, size_t>> Occurrences =
          ST.findOccurrencesAndPrune(CandidateString);

      assert(Occurrences.size() > 0 &&
             "Longest repeated substring has no occurrences.");

      // If there are at least two occurrences of this candidate, then we should
      // make it a function and keep track of it.
      if (Occurrences.size() >= 2) {
        std::pair<std::vector<unsigned>, size_t> FirstOcc = Occurrences[0];

        // The index of the first character of the candidate in the 2D string.
        size_t IdxIn2DString = FirstOcc.second;

        // Use that to find the index of the string/MachineBasicBlock it appears
        // in and the point that it begins in in that string/MBB.
        std::pair<size_t, size_t> FirstIdxAndOffset =
            Mapping.mappingIdxAndOffset(IdxIn2DString);

        // From there, we can tell where the string starts and ends in the first
        // occurrence so that we can copy it over.
        size_t StartIdxInBB = FirstIdxAndOffset.second;
        size_t EndIdxInBB = StartIdxInBB + CandidateString.size() - 1;

        // Keep track of the MachineBasicBlock and its parent so that we can
        // copy from it later.
        MachineBasicBlock *OccBB = Worklist[FirstIdxAndOffset.first];
        FunctionList.push_back(OutlinedFunction(
            OccBB, StartIdxInBB, EndIdxInBB, FunctionList.size(),
            CurrentFunctionID, Occurrences.size()));

        // Save each of the occurrences for the outlining process.
        for (auto &Occ : Occurrences) {
          std::pair<size_t, size_t> IdxAndOffset =
              Mapping.mappingIdxAndOffset(Occ.second);

          CandidateList.push_back(Candidate(
              IdxAndOffset.first,      // Idx of MBB containing candidate.
              IdxAndOffset.second,     // Starting idx in that MBB.
              CandidateString.size(),  // Candidate length.
              Occ.second,              // Start index in the full string.
              FunctionList.size() - 1, // Idx of the corresponding function.
              CandidateString          // The actual string.
              ));
        }

        CurrentFunctionID++;
        FunctionsCreatedStat++;
      }

      // Find the next candidate and continue the process.
      CandidateString = ST.longestRepeatedSubstring();
    } while (CandidateString.size() >= 2);

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

  errs() << "OF.StartIdxInBB = " << OF.StartIdxInBB << "\n";
  errs() << "OF.EndIdxInBB = " << OF.EndIdxInBB << "\n";

  /// Find where the occurrence we want to copy starts and ends.
  DEBUG(dbgs() << "OF.StartIdxInBB = " << OF.StartIdxInBB << "\n";
        dbgs() << "OF.EndIdxInBB = " << OF.EndIdxInBB << "\n";);

  // Insert instructions into the function and a custom outlined
  // prologue/epilogue.
  MF.insert(MF.begin(), MBB);
  TII->insertOutlinerEpilog(*MBB, MF);

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

  TII->insertOutlinerProlog(*MBB, MF);

  errs() << "New function: \n";
  errs() << *Name << ":\n";
  for (MachineBasicBlock &MBB : MF)
    MBB.dump();

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

    size_t StartIdx = C.BBOffset;
    size_t EndIdx = StartIdx + C.Len;

    // If the index is below 0, then we must have already outlined from it.
    bool AlreadyOutlinedFrom = EndIdx - StartIdx > C.Len;

    // Check if we have any different characters in the string collection versus
    // the string we want to outline. If so, then we must have already outlined
    // from the spot this candidate appeared at.
    if (!AlreadyOutlinedFrom) {
      for (size_t i = StartIdx; i < EndIdx; i++) {
        size_t j = i - StartIdx;
        if (Mapping.MBBMappings[C.BBIdx][i] != C.Str[j]) {
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
    // auto Begin = Mapping.MBBMappings[C.BBIdx]->begin() + C.BBOffset;
    auto Begin = Mapping.MBBMappings[C.BBIdx].begin() + C.BBOffset;
    auto End = Begin + C.Len;

    Mapping.MBBMappings[C.BBIdx].erase(Begin, End);
    Mapping.MBBMappings[C.BBIdx].insert(Begin, FunctionList[C.FunctionIdx].Id);

    // Now outline the function in the module using the same idea.
    MachineFunction *MF = FunctionList[C.FunctionIdx].MF;
    MachineBasicBlock *MBB = Worklist[C.BBIdx];
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

    std::advance(StartIt, StartIdx);
    std::advance(EndIt, EndIdx);
    StartIt = TII->insertOutlinedCall(*MBB, StartIt, *MF, Name);
    ++StartIt;
    MBB->erase(StartIt, EndIt);
  }

  return OutlinedSomething;
}

bool MachineOutliner::runOnModule(Module &M) {
  MachineModuleInfo &MMI = getAnalysis<MachineModuleInfo>();
  errs() << "Trying to outline something...\n";
  std::vector<MachineBasicBlock *> Worklist;

  // The current number we'll assign to instructions we ought not to outline.
  CurrIllegalInstrMapping = -1;

  // The current number we'll assign to instructions we want to outline.
  CurrLegalInstrMapping = 0;

  const TargetSubtargetInfo *STI =
      &(MMI.getMachineFunction(*M.begin()).getSubtarget());
  const TargetRegisterInfo *TRI = STI->getRegisterInfo();
  const TargetInstrInfo *TII = STI->getInstrInfo();

  // Set up the suffix tree by creating strings for each basic block.
  // Note: This means that the i-th string and the i-th MachineBasicBlock
  // in the work list correspond to each other. It also means that the
  // j-th character in that string and the j-th instruction in that
  // MBB correspond with each other.
  for (Function &F : M) {
    MachineFunction &MF = MMI.getMachineFunction(F);

    if (F.empty() || !TII->functionIsSafeToOutlineFrom(F))
      continue;

    for (MachineBasicBlock &MBB : MF) {
      Worklist.push_back(&MBB);
      std::vector<unsigned> Container;
      buildProxyString(Container, MBB, *TRI, *TII);
      Mapping.MBBMappings.push_back(Container);
    }
  }

  errs() << "String collection: \n";
  for (auto &S : Mapping.MBBMappings) {
    for (auto c : S) {
      errs() << c << " ";
    }
    errs() << "\n";
  }

  SuffixTree ST(Mapping);

  // Find all of the candidates for outlining.
  bool OutlinedSomething = false;
  std::vector<Candidate> CandidateList;
  std::vector<OutlinedFunction> FunctionList;

  CurrentFunctionID = InstructionIntegerMap.size();
  buildCandidateList(CandidateList, FunctionList, Worklist, ST);
  OutlinedSomething =
      outline(M, Worklist, CandidateList, FunctionList, Mapping);

  return OutlinedSomething;
}
