/// SuffixTree
/// Stores all of the suffixes of a string, or of several strings.
/// In this context a "string" refers to an ordered list of some type.
/// Thus, it can be a collection of characters, integers, basic blocks, dogs,
/// or whatever you want.
///
/// This data structure can be used to find information about the structure of
/// strings. For example, we can find out what the most common pattern of some
/// length is in some given string by storing it in a Suffix Tree.
///
/// Suffix trees are constructed online in O(n) using Ukkonen's algorithm, where
/// n = the length of the string.

#ifndef SUFFIXTREE_H
#define SUFFIXTREE_H

#include <map>
#include <vector>

#include "TerminatedString.h"

/// STNode
/// A suffix tree node.
/// Each node contains a map of Children, a Link to the next shortest suffix,
/// and a set of string IDs that the node has been
/// accessed by. Each node keeps track of the substring it represents by storing
/// the start and end indices of that substring.
template <typename CharLike> struct STNode {
  typedef Character<CharLike> CharacterType;
  STNode<CharLike> *Parent = nullptr;
  std::map<CharacterType, STNode<CharLike> *> Children;
  bool Valid = true; // Set to true if we can traverse this node.

  STNode<CharLike> *Link; // Suffix Link.
  int Start;              // Start index in TerminatedStringList.
  int *End = nullptr;     // End index in TerminatedStringList.
  int SuffixIndex = -1;  // Stores index of suffix for path from Root to leaf.
};

/// SuffixTree
/// The suffix tree implementation
template <typename StringContainerType, typename CharLike> class SuffixTree {
private:
  typedef STNode<CharLike> Node;
  typedef TerminatedString<StringContainerType, CharLike> String;
  typedef TerminatedStringList<StringContainerType, CharLike> StringCollection;
  typedef Character<CharLike> CharacterType;

  unsigned size_; // Length of input string
  int LeafEnd;   // The amount by which we extend all leaves in the trees

  /// ActiveState: Keeps track of what we're currently working on in the tree.
  /// That is, the node we start the phase from, the edge we're looking at,
  /// the length of the suffix to add, etc.
  struct ActiveState {
    Node *Node = nullptr;
    int Idx = -1; // Index of Active character in the current string
    int Len = 0;  // Length of the current substring
  };

  ActiveState Active;

  /// Creates a node with given start, end, and Link
  Node *createNode(int S, int *E, Node *L) {
    Node *NewNode = new Node;
    NewNode->Start = S;
    NewNode->End = E;
    NewNode->Link = L;

    return NewNode;
  }

  /// Delete a given node.
  void deleteNode(Node *N) {
    if (!N)
      return;

    N->Link = nullptr;
    N->Parent = nullptr;

    if (N->SuffixIndex == -1 && N->End != nullptr) {
      delete N->End;
    }

    for (auto ChildPair : N->Children) {
      if (ChildPair.second != nullptr) {
        deleteNode(ChildPair.second);
        ChildPair.second = nullptr;
      }
    }

    N = nullptr;
  }

  /// Return the length of the substring defined by this node.
  inline int nodeSize(const Node &N) {
    int SubstringLen = 0;

    // The node isn't Root.
    if (N.Start > -1)
      SubstringLen = *N.End - N.Start + 1;

    return SubstringLen;
  }

  /// Checks if next_node is "contained in" the Active node. That is, the next
  /// node is smaller than, and thus is a valid transition from the Active node.
  /// If it is, it sets the Active node to NextNode and returns true.
  /// Otherwise, it returns false.
  bool hasTransition(Node &NextNode) {
    int SubstringLen = nodeSize(NextNode);

    // The Active string isn't inside the current node, so we should move to
    // NextNode and return.
    if (Active.Len >= SubstringLen) {
      Active.Idx += SubstringLen;
      Active.Len -= SubstringLen;
      Active.Node = &NextNode;
      return true;
    }

    return false;
  }

  /// printSubstring: Print the substring of str defined by the interval
  /// [StartIdx, EndIdx].
  void printSubstring(const int &StartIdx, const int &EndIdx) {
    for (int Idx = StartIdx; Idx <= EndIdx; Idx++)
      errs() << SC[Idx];
  }

  /// Recursively traverse the tree, printing each node as it goes. Called by
  /// print.
  void printHelper(Node *N, int Depth) {
    assert(N != nullptr && "Tried to print a null node!");

    // "Deleted" path
    if (!N->Valid)
      return;

    // Output some lines to show which level we're at in the tree.
    for (size_t Level = 0; Level < Depth; Level++)
      errs() << "-";

    // It isn't the Root, so let's print its substring.
    if (N->Start > -1)
      printSubstring(N->Start, *(N->End));

    // We're at an internal node, so we have to traverse more.
    if (N->SuffixIndex == -1) {
      for (auto ChildPair : N->Children) {
        if (ChildPair.second != nullptr && ChildPair.second->Valid) {
          if (N->Start > -1)
            errs() << " [" << N->SuffixIndex << "]\n";

          printHelper(ChildPair.second, Depth + 1);
        }
      }
    }

    // We're at a leaf, so print the suffix index.
    else {
      errs() << " [" << N->SuffixIndex << "]\n";
    }
  }

  /// Set the end of each leaf in the tree after constructing it
  void setLeafEnds(Node *N, int LabelHeight) {
    if (N == nullptr)
      return;

    bool IsLeaf = true;

    for (auto ChildPair : N->Children) {
      if (ChildPair.second != nullptr) {
        IsLeaf = false;
        setLeafEnds(ChildPair.second,
                    LabelHeight + nodeSize(*ChildPair.second));
      }
    }

    if (IsLeaf) {
      N->SuffixIndex = size() - LabelHeight;
    }
  }

  /// Compute the suffix tree at the "phase" EndIdx from the previous suffix
  /// tree using Ukkonen's algorithm. Update the existing nodes in the tree on
  /// append of additional strings.
  /// That is, suppose we have a suffix tree containing a string S1, and we add
  /// a second string S2. Say that when we add S2 via Ukkonen, we visit some
  /// existing nodes from S1. In this case, we should keep track of where S1
  /// touched the tree as well, so that we can perform operations on both S1 and
  /// S2.
  inline void extend(int EndIdx, Node *NeedsLink, int &SuffixesToAdd) {
    while (SuffixesToAdd > 0) {

      // The length of the current string is 0, so we look at the last added
      // character to our substring.
      if (Active.Len == 0)
        Active.Idx = EndIdx;

      // The first and last character in the current substring we're looking at.
      CharacterType FirstChar = SC[Active.Idx];
      CharacterType LastChar = SC[EndIdx];

      // If there is no child for the Active edge, then we have to add a new
      // node and update the Link.
      if (!Active.Node->Children[FirstChar]) {

        Node *Child = createNode(EndIdx, &LeafEnd, Root);
        Child->Parent = Active.Node;
        Active.Node->Children[FirstChar] = Child;

        if (NeedsLink != nullptr) {
          NeedsLink->Link = Active.Node;
          NeedsLink = nullptr;
        }
      }

      // Otherwise, there's a child, so we have to walk down the tree and find
      // out where to insert the new node if necessary.
      else {
        Node *NextNode = Active.Node->Children[FirstChar];

        // There's a child which we can walk on, so move to it and continue.
        if (hasTransition(*NextNode))
          continue;

        // The string is already in the tree.
        if (SC[NextNode->Start + Active.Len] == LastChar) {

          if (NeedsLink != nullptr && !(Active.Node->Start == -1)) {
            NeedsLink->Link = Active.Node;
            NeedsLink = nullptr;
          }

          Active.Len++;
          break;
        }

        // We ended up in an edge which partially matches our string. Therefore,
        // we need to take that edge and split it into two.
        int *SplitEnd = new int(NextNode->Start + Active.Len - 1);
        Node *SplitNode = createNode(NextNode->Start, SplitEnd, Root);
        SplitNode->Parent = Active.Node;
        Active.Node->Children[FirstChar] = SplitNode;
        Node *Child = createNode(EndIdx, &LeafEnd, Root);

        Child->Parent = SplitNode;
        SplitNode->Children[LastChar] = Child;

        NextNode->Start += Active.Len;
        NextNode->Parent = SplitNode;
        SplitNode->Children[SC[NextNode->Start]] = NextNode;

        // Visited an internal node, so we have to update the suffix link.
        if (NeedsLink != nullptr)
          NeedsLink->Link = SplitNode;

        NeedsLink = SplitNode;
      }

      // If we made it here we must have added a suffix, so we can move to the
      // next one.
      SuffixesToAdd--;
      if (Active.Node->Start == -1) {
        if (Active.Len > 0) {
          Active.Len--;
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
  StringCollection SC;
  Node *Root = nullptr;

  unsigned size() { return size_; }

  /// Print the suffix tree.
  void print() {
    Node *curr = Root;
    printHelper(curr, 0);
  }

  /// Add a string to an existing suffix tree. This continues the Ukkonen
  /// algorithm from where it left off. Thus, we're appending a new string to
  /// the string which the tree already represents.
  void append(String *NewStr) {
    SC.append(NewStr);

    // Save the old size so we can start at the end of the old string
    int OldSize = size_;
    size_ = SC.size();

    int SuffixesToAdd = 0;
    Node *NeedsLink = nullptr; // The last internal node added

    // OldSize is initially 0 on the insertion of the first string. At the
    // insertion of the next string, OldSize is the index of the end of the
    // previous string.
    for (unsigned EndIdx = OldSize; EndIdx < size_; EndIdx++) {
      SuffixesToAdd++;
      NeedsLink = nullptr;
      LeafEnd = (int)EndIdx;
      extend((int)EndIdx, NeedsLink, SuffixesToAdd);
    }

    int LabelHeight = 0;
    setLeafEnds(Root, LabelHeight);
  }

  /// Traverse the tree depth-first and return the longest path in the tree
  void longestPath(Node &N, const int &LabelHeight, int &MaxHeight,
                   int &SubstringStartIdx, int &NumOccurrences) {

    // We hit an internal node, so we can traverse further down the tree.
    // For each child, traverse down as far as possible and set MaxHeight
    if (N.SuffixIndex == -1) {
      for (auto ChildPair : N.Children) {
        if (ChildPair.second && ChildPair.second->Valid)
          longestPath(*ChildPair.second,
                      LabelHeight + nodeSize(*ChildPair.second), MaxHeight,
                      SubstringStartIdx, NumOccurrences);
      }
    }

    // We hit a leaf, so update MaxHeight if we've gone further down the
    // tree
    else if (N.SuffixIndex > -1 && MaxHeight < (LabelHeight - nodeSize(N))) {
      MaxHeight = LabelHeight - nodeSize(N);
      SubstringStartIdx = N.SuffixIndex;
      NumOccurrences = (int)N.Parent->Children.size();
    }
  }

  /// Return the longest substring of str which is repeated at least one time.
  String *longestRepeatedSubstring() {
    int MaxHeight = 0;
    int FirstChar = 0;
    Node *N = Root;
    int NumOccurrences = 0;

    longestPath(*N, 0, MaxHeight, FirstChar, NumOccurrences);
    String *Longest = nullptr;

    // We found something in the tree, so we know the string must appear
    // at least once
    if (MaxHeight > 0) {
      Longest = new String();

      for (int Idx = 0; Idx < MaxHeight; Idx++)
        *Longest += SC[Idx + FirstChar];
    }

    return Longest;
  }

  /// Perform a depth-first search for QueryString on the suffix tree
  Node *DFS(const String &QueryString, int &CurrIdx, Node *CurrNode) {
    Node *RetNode;
    Node *NextNode;

    if (CurrNode == nullptr || CurrNode->Valid == false) {
      RetNode = nullptr;
    }

    // If we're at the Root we have to check if there's a child, and move to
    // that child. We don't consume the character since Root represents the
    // empty string
    else if (CurrNode->Start == -1) {
      if (CurrNode->Children[QueryString[CurrIdx]] != nullptr &&
          CurrNode->Children[QueryString[CurrIdx]]->Valid) {
        NextNode = CurrNode->Children[QueryString[CurrIdx]];
        RetNode = DFS(QueryString, CurrIdx, NextNode);
      }

      else {
        RetNode = nullptr;
      }
    }

    // The node represents a non-empty string, so we should match against it and
    // check its Children if necessary
    else {
      int StrIdx = CurrNode->Start;
      enum FoundState { ExactMatch, SubMatch, Mismatch };
      FoundState Found = ExactMatch;

      // Increment idx while checking the string for equivalence. Set
      // found and possibly break based off of the case we find.
      while ((unsigned)CurrIdx < QueryString.length()) {

        // Failure case 1: We moved outside the string, BUT we matched
        // perfectly up to that point
        if (StrIdx > *(CurrNode->End)) {
          Found = SubMatch;
          break;
        }

        // Failure case 2: We have a true mismatch
        if (QueryString[CurrIdx] != SC[StrIdx]) {
          Found = Mismatch;
          break;
        }

        StrIdx++;
        CurrIdx++;
      }

      // Decide whether or not we should keep searching
      switch (Found) {
      case (ExactMatch):
        RetNode = CurrNode;
        break;
      case (SubMatch):
        NextNode = CurrNode->Children[QueryString[CurrIdx]];
        RetNode = DFS(QueryString, CurrIdx, NextNode);
        break;
      case (Mismatch):
        RetNode = nullptr;
        break;
      }
    }

    return RetNode;
  }

  /// Given a string, traverse the path for that string in the tree
  /// as far as possible. Return the node representing the string, or longest
  /// matching substring in the tree. If there is a mismatch, then return
  /// null.
  Node *find(const String &QueryString, int &Idx) {
    return DFS(QueryString, Idx, Root);
  }

  /// Return true if QueryString is a substring of str and false otherwise
  bool isSubstring(const String &QueryString) {
    int Len = 0;
    Node *N = Root;
    find(QueryString, Len, N);
    return (Len == QueryString.size());
  }

  /// Return true if q is a suffix of str and false otherwise
  bool isSuffix(const String &QueryString) {
    int Dummy = 0;
    Node *N = find(QueryString, Dummy);
    return (N != nullptr && N->SuffixIndex > -1);
  }

  void printNode(Node &N) {
    errs() << "Node: ";
    printSubstring(N.Start, *N.End);
    errs() << "\n";
  }

  /// Given a TerminatedString q, find all NumOccurrences of q in the
  /// TerminatedStringList. Returns the string containing the actual occurrence,
  /// and the index that it starts at.
  std::vector<std::pair<String *, int>> *
  findOccurrences(const String &QueryString) {
    int Len = 0;
    std::vector<std::pair<String *, int>> *Occurrences = nullptr;
    Node *N = find(QueryString, Len);

    if (N != nullptr && N->Valid) {
      N->Valid = false;
      Occurrences = new std::vector<std::pair<String *, int>>();

      // We matched exactly, so we're in a suffix. There's then exactly one
      // occurrence.
      if (N->SuffixIndex > -1) {
        int StartIdx = N->SuffixIndex;
        auto StrPair = SC.stringContaining(StartIdx);
        Occurrences->push_back(make_pair(StrPair.first, StartIdx));
      }

      // There are no occurrences, so return null.
      else if (N == nullptr) {
        delete Occurrences;
        Occurrences = nullptr;
      }

      // There are occurrences, so find them and invalidate paths along the way.
      else {
        Node *M;

        for (auto ChildPair : N->Children) {
          M = ChildPair.second;

          if ((M != nullptr) && (M->SuffixIndex != -1)) {
            M->Valid = false;
            int StartIdx = M->SuffixIndex;
            auto StrPair = SC.stringContaining(StartIdx);
            Occurrences->push_back(make_pair(StrPair.first, StartIdx));
          }
        }
      }

      // Now inValidate the suffix for the original node.
      N = N->Link;
      while (N && N != Root) {
        N->Valid = false;
        N = N->Link;
      }
    }

    return Occurrences;
  }

  /// Return the number of times the string q appears in the tree.
  int numOccurrences(const String &QueryString) {
    int Dummy = 0;
    int NumOccurrences = 0;
    Node *N = find(QueryString, Dummy);

    if (N != nullptr) {
      // We matched exactly, so we need the number of Children from the Parent.
      // FIXME: Is this correct?
      if (N->SuffixIndex > -1)
        NumOccurrences = N->Parent->Children.size();

      else
        NumOccurrences = N->Children.size();
    }

    return NumOccurrences;
  }

  /// Create a suffix tree and initialize it with str.
  SuffixTree(String *str_) : size_(0u), LeafEnd(-1) {
    SC = StringCollection();
    int *RootEnd = new int(-1);
    Root = createNode(-1, RootEnd, Root);
    Active.Node = Root;
    append(str_);
  }

  SuffixTree(StringCollection str_) : size_(0u), LeafEnd(-1) {
    int *RootEnd = new int(-1);
    Root = createNode(-1, RootEnd, Root);
    Active.Node = Root;

    for (auto *s : str_)
      append(s);
  }

  SuffixTree() : size_(0u), LeafEnd(-1) {
    int *RootEnd = new int(-1);
    Root = createNode(-1, RootEnd, Root);
    Active.Node = Root;
  }

  /// Calls freeTree recursively
  ~SuffixTree() {
    Active.Node = nullptr;
    if (Root != nullptr)
      deleteNode(Root);
  }
};

#endif // SUFFIXTREE_H
