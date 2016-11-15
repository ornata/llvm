/// TerminatedString.h
///----------------------------------------------------------------------------
/// This file contains definitions for a "terminated string" and a "terminated
/// string list." These are the string types used in SuffixTree.h.
/// A TerminatedString is a collection of Characters followed by an unique
/// terminator. TerminatedStringLists are higher-order TerminatedStrings; they
/// are a list of TerminatedStrings which act as a TerminatedString themselves.
/// This definition is required for the suffix tree's construction method.
///----------------------------------------------------------------------------

#ifndef TERM_STRING_H
#define TERM_STRING_H

#include "llvm/Support/raw_ostream.h"
#include <vector>

using namespace llvm;

///============================ Character Type ==============================///

/// Character
/// Represents a character or a terminator.
///
/// A character is some member of some alphabet. For example, 'a' is a character
/// in the alphabet [a...z], and 1 is a character in the alphabet [0...9].
/// A terminator is a special character which represents the end of a string.
/// Terminators are unique to some string and have an unique Id.
/// This allows us to have multiple copies of a string with the same characters,
/// without having them compare as equal.

template <typename C> struct Character {
  bool IsTerminator; /// True if the current character is a terminator.
  C Symbol;          /// The actual character, eg 'a', 1, etc.
  size_t Id;         /// If this is a terminator, then this is the Id.

  ///----------------  Comparisons between character types ------------------///

  /// ==: Equality between character types can only occur when they are of the
  /// same character class.
  bool operator==(const Character &Rhs) const {
    bool IsEqual = false;

    if (IsTerminator && Rhs.IsTerminator)
      IsEqual = (Id == Rhs.Id);

    else if (!IsTerminator && !Rhs.IsTerminator)
      IsEqual = (Symbol == Rhs.Symbol);

    return IsEqual;
  }

  /// ==: Equality between a Symbol and a Character
  bool operator==(const C &Rhs) const {
    if (IsTerminator)
      return false;

    return (Symbol == Rhs);
  }

  /// <: Less-than operator for Characters
  bool operator<(const Character &Rhs) const {
    bool IsLessThan;

    /// Terminators can only match against other terminators for string
    /// comparisons.
    if (IsTerminator) {
      if (Rhs.IsTerminator)
        IsLessThan = (Id < Rhs.Id);
      else
        IsLessThan = true;
    }

    /// Terminators are always less than every non-terminator value.
    else {
      if (Rhs.IsTerminator)
        IsLessThan = false;
      else
        IsLessThan = (Symbol < Rhs.Symbol);
    }

    return IsLessThan;
  }

  /// <: Less-than operator for Characters and Symbols
  bool operator<(const C &Rhs) const {
    if (IsTerminator)
      return true;

    return (Symbol < Rhs);
  }

  /// >: Greater-than operator for Characters.
  bool operator>(const Character &Rhs) const {
    bool IsGreaterThan;

    /// Terminators can only match against other terminators for string
    /// comparisons.
    if (IsTerminator) {
      if (Rhs.IsTerminator)
        IsGreaterThan = (Id > Rhs.Id);
      else
        IsGreaterThan = false;
    }

    /// Terminators are always greater than every non-terminator.
    else {
      if (Rhs.IsTerminator)
        IsGreaterThan = true;
      else
        IsGreaterThan = (Symbol > Rhs.Symbol);
    }

    return IsGreaterThan;
  }

  /// <: Greater-than operator for Characters and Symbols
  bool operator>(const C &Rhs) const {
    if (IsTerminator)
      return false;

    return (Symbol > Rhs);
  }

  // Other operators for Characters.
  bool operator!=(const Character &Rhs) const { return !(*this == Rhs); }
  bool operator<=(const Character &Rhs) const { return !(*this > Rhs); }
  bool operator>=(const Character &Rhs) const { return !(*this < Rhs); }
  bool operator!=(const C &Rhs) const { return !(*this == Rhs); }
  bool operator<=(const C &Rhs) const { return !(*this > Rhs); }
  bool operator>=(const C &Rhs) const { return !(*this < Rhs); }
};

/// Output for Characters.
template <typename C>
raw_ostream &operator<<(raw_ostream &OS, const Character<C> &Ch) {
  if (Ch.IsTerminator == false)
    OS << Ch.Symbol << " ";
  else
    OS << "#" << Ch.Id << "# ";
  return OS;
}

///========================= TerminatedString Type ==========================///

/// TerminatedString
/// Represents a finite sequence of characters, where the last character is an
/// unique terminator.
///
/// A TerminatedString takes in an input container of some sort, and produces a
/// string for that input container. After creating the string, the
/// TerminatedString appends an unique terminator to that string.
/// This means that every TerminatedString is unique.

template <typename InputContainer, typename CharacterType>
struct TerminatedString {
private:
  static size_t StrId;
  typedef Character<CharacterType> CharLike;
  typedef std::vector<CharLike> CharList;
  CharList StrContainer; /// The container for the string's characters.

public:
  ///-------------------  Properties of TerminatedStrings  ------------------///

  /// Note that size and length have different meanings. SizeStrings() includes
  /// the
  /// terminator in its result, while length() does not.
  size_t size() const { return StrContainer.size(); }
  size_t size() { return StrContainer.size(); }
  size_t length() { return StrContainer.size() - 1; }
  size_t length() const { return StrContainer.size() - 1; }
  CharLike getTerminator() const { return StrContainer.back(); }

  ///-------------------  Iterators for TerminatedStrings -------------------///
  typedef typename CharList::iterator iterator;
  typedef typename CharList::const_iterator const_iterator;

  // Iterators which include the terminator.
  iterator begin() { return StrContainer.begin(); }
  iterator end() { return StrContainer.end(); }
  const_iterator begin() const { return StrContainer.begin(); }
  const_iterator end() const { return StrContainer.end(); }

  /// Iterators where the terminator is one past the end.
  iterator chars_begin() { return StrContainer.begin(); }
  iterator chars_end() { return StrContainer.end() - 1; }
  const_iterator chars_begin() const { return StrContainer.begin(); }
  const_iterator chars_end() const { return StrContainer.end() - 1; }

  ///-----------------  TerminatedString operator overloads -----------------///

  /// ==: The only TerminatedString a TerminatedString can be equal to is
  /// itself.
  bool operator==(const TerminatedString &Rhs) const {
    /// If they don't have the same terminator, then they can't be the same.
    if (getTerminator() != Rhs.getTerminator()) {
      return false;
    }

    /// Otherwise, they have the same terminators. Therefore, their characters
    /// should match exactly.
    else {
      /// Check if the characters match
      for (auto It = chars_begin(), Et = chars_end(); It != Et; It++) {
        assert(
            *It == *Et &&
            "Strings had equal terminators, but contained unequal characters!");
      }
    }

    return true;
  }

  /// []: Returns the character at index Idx.
  CharLike operator[](size_t Idx) const {
    assert(Idx < size() && "Out of range! (Idx >= size())");
    return StrContainer[Idx];
  }

  /// +=, CharacterType: Append a character to an existing TerminatedString
  TerminatedString operator+=(const CharacterType &Rhs) {
    assert(size() > 0 && "String is non-terminated!");

    CharLike Ch;
    Ch.Symbol = Rhs;
    Ch.IsTerminator = false;

    CharLike Term = StrContainer.back();
    assert(Term.IsTerminator && "Last character wasn't a terminator!");
    StrContainer.pop_back();
    StrContainer.push_back(Ch);
    StrContainer.push_back(Term);
    return *this;
  }

  /// +=, CharLike: Append a character to an existing TerminatedString
  TerminatedString operator+=(const CharLike &Rhs) {
    assert(size() > 0 && "String is non-terminated!");
    CharLike Term = StrContainer.back();
    assert(Term.IsTerminator && "Last character wasn't a terminator!");
    StrContainer.pop_back();
    StrContainer.push_back(Rhs);
    StrContainer.push_back(Term);
    return *this;
  }

  /// +=, InputContainer: Append all characters in an InputContainer to a
  /// TerminatedString.
  TerminatedString operator+=(const InputContainer &Rhs) {
    assert(size() > 0 && "String is non-terminated!");
    CharLike Term = StrContainer.back();
    assert(Term.IsTerminator && "Last character wasn't a terminator!");
    StrContainer.pop_back();

    for (size_t i = 0, e = Rhs.size(); i < e; i++) {
      CharLike Ch;
      Ch.Symbol = Rhs[i];
      Ch.IsTerminator = false;
      StrContainer.push_back(Ch);
    }

    StrContainer.push_back(Term);
    return *this;
  }

  /// +=, TerminatedString: Append another TerminatedString to an existing
  /// TerminatedString, not including the terminator for that string.
  TerminatedString operator+=(const TerminatedString &Rhs) {
    assert(size() > 0 && "String is non-terminated!");
    CharLike Term = StrContainer.back();
    StrContainer.pop_back();

    for (size_t i = 0, e = Rhs.length(); i < e; i++) {
      CharLike Ch;
      Ch.Symbol = Rhs[i];
      Ch.IsTerminator = false;
      StrContainer.push_back(Ch);
    }

    StrContainer.push_back(Term);
    return *this;
  }

  ///------------------  Operations on TerminatedStrings --------------------///

  /// erase (size_t): Remove the character at index i from the
  /// TerminatedString
  void erase(const size_t &Idx) {
    assert(Idx != length() && "Can't erase terminator! (Idx == length())");
    assert(Idx < size() && "Out of range! (Idx >= size())");

    StrContainer.erase(StrContainer.begin() + Idx);
  }

  /// erase(size_t, size_t): Remove the characters in the size_terval
  /// [StartIdx, EndIdx] from the TerminatedString
  void erase(const size_t &StartIdx, const size_t &EndIdx) {
    assert(StartIdx < length() && "Out of range! (StartIdx >= length())");
    assert(EndIdx < length() && "Out of range! (EndIdx >= length())");
    assert(StartIdx <= EndIdx && "StartIdx was greater than EndIdx");
    StrContainer.erase(StrContainer.begin() + StartIdx,
                       StrContainer.begin() + EndIdx);
  }

  /// insertBefore(size_t, CharacterType): Insert c at the index i-1.
  iterator insertBefore(const size_t &Idx, const CharacterType &Symbol) {
    assert(Idx < size() && "Out of range! (Idx >= size())");
    auto ContainerIt = StrContainer.begin() + Idx;
    CharLike NewChar;
    NewChar.Symbol = Symbol;
    NewChar.IsTerminator = false;
    StrContainer.insert(ContainerIt, NewChar);
    return begin() + Idx;
  }

  ///-----------------  Constructors for TerminatedStrings ------------------///

  /// Construct a TerminatedString from an InputContainer.
  TerminatedString(const InputContainer &C) {
    for (auto it = C.begin(); it != C.end(); it++) {
      CharLike Ch;
      Ch.Symbol = *it;
      Ch.IsTerminator = false;
      StrContainer.push_back(Ch);
    }

    /// Add a terminator to the end.
    CharLike Term;
    Term.IsTerminator = true;
    Term.Id = StrId;
    StrContainer.push_back(Term);
    StrId++;
  }

  /// Create a copy of another TerminatedString, including the terminator.
  TerminatedString(const TerminatedString &other) {
    for (size_t i = 0; i < other.size(); i++)
      StrContainer.push_back(other[i]);
  }

  /// Create an empty TerminatedString. That is, all it contains is a
  /// terminator.
  TerminatedString() {
    CharLike Term;
    Term.IsTerminator = true;
    Term.Id = StrId;
    StrContainer.push_back(Term);
    StrId++;
  }
};

template <typename InputContainer, typename CharLike>
raw_ostream &operator<<(raw_ostream &OS,
                        const TerminatedString<InputContainer, CharLike> &Str) {
  for (auto &Ch : Str)
    OS << Ch;
  return OS;
}

template <typename InputContainer, typename CharLike>
size_t TerminatedString<InputContainer, CharLike>::StrId = 0;

/// ====================== TerminatedStringList Type ======================= ///

/// TerminatedStringList
/// Represents a compound string.
///
/// This represents a group of strings appended together to form a large
/// compound string. TerminatedStringLists act exactly like
/// normal TerminatedStrings, but allow us to masize_tain the indivIduality of
/// specific strings.

template <typename InputContainer, typename CharLike>
struct TerminatedStringList {
private:
  size_t SizeChars;   // Sum of size() for each string in list
  size_t SizeStrings; // Number of strings in the list

  typedef TerminatedString<InputContainer, CharLike> String;
  typedef std::vector<String *> StringContainer;
  typedef Character<CharLike> CharacterType;

  StringContainer Str;

public:
  ///----------------- Iterators for TerminatedStringLists ------------------///

  typedef typename StringContainer::iterator iterator;
  typedef typename StringContainer::const_iterator const_iterator;

  /// Iterators over the strings in the list
  iterator begin() { return Str.begin(); }
  iterator end() { return Str.end(); }
  const_iterator begin() const { return Str.begin(); }
  const_iterator end() const { return Str.end(); }

  /// TODO: Iterators over the characters in the string

  ///------------  Operator overloads for TerminatedStringLists -------------///

  /// This returns the character at index i, treating the entire
  /// TerminatedStringList
  /// itself as a string. This does NOT return the string at index i. This is
  /// for compatability with Ukkonen's algorithm.
  CharacterType operator[](size_t Idx) {
    assert(Idx < SizeChars && "Out of range! (Idx >= size_chars)");
    auto StrIdx = stringIndexContaining(Idx);

    return stringAt(StrIdx.first)[StrIdx.second];
  }

  ///----------------  Operations on TerminatedStringLists ------------------///

  /// pop_back(): Remove the last string from the TerminatedStringList.
  void pop_back() { Str.pop_back(); }

  /// clear(): Remove all of the strings from the TerminatedStringLis
  void clear() { Str.clear(); }

  /// size(): Return the number of characters in the TerminatedStringList.
  size_t size() { return SizeChars; }

  /// back(): Return the element at the end of the TerminatedStringList.
  String back() { return *(Str.back()); }

  /// stringCount(): Return the number of strings in the TerminatedStringList.
  size_t stringCount() { return SizeStrings; }

  /// Append(String*): Append a String* to the TerminatedStringList.
  void append(String *str_) {
    Str.push_back(str_);
    SizeChars += str_->size();
    SizeStrings++;
  }

  /// append(String &): Append a String& to the TerminatedStringList.
  void append(String &str_) {
    Str.push_back(new String(str_));
    SizeChars += str_.size();
    SizeStrings++;
  }

  /// +=, TerminatedString: Append the Rhs to the current list
  TerminatedStringList operator+=(String &Other) {
    append(Other);
    return *this;
  }

  // +=, TerminatedStringList: Append the strings in the Rhs to this list
  TerminatedStringList operator+=(TerminatedStringList &Other) {
    for (size_t Idx = 0; Idx < Other.stringCount(); Idx++)
      append(Other.stringAt(Idx));
    return *this;
  }

  /// stringAt(): Return the string with index i
  String stringAt(const size_t &Idx) {
    assert(Idx < SizeChars && "Out of range! (Idx >= size_chars)");
    return *(Str[Idx]);
  }

  /// Insert the character c before the index i in the string
  iterator insertBefore(const size_t &Idx, const CharLike &C) {
    assert(Idx < SizeChars && "Out of range! (Idx >= size_chars)");
    auto string_pair = stringContaining(Idx);
    string_pair.first->insertBefore(string_pair.second, C);
    SizeChars++;

    return begin() + Idx;
  }

  /// stringContaining(size_t): Return the index of the string that index i
  /// maps size_to. If out of bounds, return -1.
  std::pair<size_t, size_t> stringIndexContaining(size_t Idx) {
    assert(Idx < SizeChars && "Out of range! (Idx >= size_chars)");
    /// Find the string index containing i.
    size_t StrIdx;
    for (StrIdx = 0; StrIdx != size() - 1; StrIdx++) {
      /// Idx must be insIde the string, since it's in the range [0, size-1].
      if (Idx < Str[StrIdx]->size())
        break;

      /// Otherwise, we move over to the next string by offsetting Idx by the
      /// size of the current string.
      Idx -= Str[StrIdx]->size();
    }

    return std::make_pair(StrIdx, Idx);
  }

  /// stringContaining: Return the string that the index i maps size_to, and the
  /// local index for that string.
  std::pair<String *, size_t> stringContaining(size_t Idx) {
    assert(Idx < SizeChars && "Out of range! (Idx >= size_chars)");
    auto StrIdx = stringIndexContaining(Idx);

    return make_pair(Str[StrIdx.first], StrIdx.second);
  }

  /// erase(size_t): Remove the character at index i in the
  /// TerminatedStringList
  void erase(size_t Idx) {
    assert(Idx < SizeChars && "Out of range! (Idx >= size_chars)");
    auto string_pair = stringContaining(Idx);
    string_pair.first->erase(string_pair.second);
    SizeChars--;
  }

  /// erase(size_t, size_t): Remove the characters in the range [StartIdx,
  /// EndIdx] from the TerminatedStringList
  void erase(size_t StartIdx, size_t EndIdx) {
    assert(StartIdx < SizeChars && "Out of range! (StartIdx >= size_chars)");
    assert(EndIdx < SizeChars && "Out of range! (EndIdx >= size_chars)");

    size_t RangeLen = EndIdx - StartIdx;
    size_t NumRemoved = 0;

    while (NumRemoved < RangeLen) {
      erase(StartIdx);
      NumRemoved++;
    }
  }

  ///---------------  Constructors for TerminatedStringLists ----------------///

  TerminatedStringList() : SizeChars(0), SizeStrings(0) {}

  TerminatedStringList(String *Str) : SizeChars(0), SizeStrings(0) {
    append(Str);
  }

  TerminatedStringList(String &Str) : SizeChars(0), SizeStrings(0) {
    append(Str);
  }
};

/// Output for TerminatedStringList
template <typename InputContainer, typename CharLike>
raw_ostream &
operator<<(raw_ostream &OS,
           const TerminatedStringList<InputContainer, CharLike> &TS) {
  OS << "[";

  for (auto It = TS.begin(), Et = TS.end(); It != Et; It++)
    OS << "'" << **It << "'"
       << ", ";

  OS << "]";

  return OS;
}

#endif // TERM_STRING_H
