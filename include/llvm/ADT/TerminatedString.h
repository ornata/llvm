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
/// Terminators are unique to some string and have an unique ID.
/// This allows us to have multiple copies of a string with the same characters,
/// without having them compare as equal.

template <typename C> struct Character {
  bool is_terminator; /// True if the current character is a terminator.
  C symbol;           /// The actual character, eg 'a', 1, etc.
  size_t id;             /// If this is a terminator, then this is the id.

  ///----------------  Comparisons between character types ------------------///

  /// ==: Equality between character types can only occur when they are of the
  /// same character class.
  bool operator==(const Character &rhs) const {
    bool IsEqual = false;

    if (is_terminator && rhs.is_terminator)
      IsEqual = (id == rhs.id);

    else if (!is_terminator && !rhs.is_terminator)
      IsEqual = (symbol == rhs.symbol);

    return IsEqual;
  }

  /// ==: Equality between a symbol and a Character
  bool operator==(const C &rhs) const {
    if (is_terminator)
      return false;

    return (symbol == rhs);
  }

  /// <: Less-than operator for Characters
  bool operator<(const Character &rhs) const {
    bool IsLessThan;

    /// Terminators can only match against other terminators for string
    /// comparisons.
    if (is_terminator) {
      if (rhs.is_terminator)
        IsLessThan = (id < rhs.id);
      else
        IsLessThan = true;
    }

    /// Terminators are always less than every non-terminator value.
    else {
      if (rhs.is_terminator)
        IsLessThan = false;
      else
        IsLessThan = (symbol < rhs.symbol);
    }

    return IsLessThan;
  }

  /// <: Less-than operator for Characters and symbols
  bool operator<(const C &rhs) const {
    if (is_terminator)
      return true;

    return (symbol < rhs);
  }

  /// >: Greater-than operator for Characters.
  bool operator>(const Character &rhs) const {
    bool IsGreaterThan;

    /// Terminators can only match against other terminators for string
    /// comparisons.
    if (is_terminator) {
      if (rhs.is_terminator)
        IsGreaterThan = (id > rhs.id);
      else
        IsGreaterThan = false;
    }

    /// Terminators are always greater than every non-terminator.
    else {
      if (rhs.is_terminator)
        IsGreaterThan = true;
      else
        IsGreaterThan = (symbol > rhs.symbol);
    }

    return IsGreaterThan;
  }

  /// <: Greater-than operator for Characters and symbols
  bool operator>(const C &rhs) const {
    if (is_terminator)
      return false;

    return (symbol > rhs);
  }

  // Other operators for Characters.
  bool operator!=(const Character &rhs) const { return !(*this == rhs); }
  bool operator<=(const Character &rhs) const { return !(*this > rhs); }
  bool operator>=(const Character &rhs) const { return !(*this < rhs); }
  bool operator!=(const C &rhs) const { return !(*this == rhs); }
  bool operator<=(const C &rhs) const { return !(*this > rhs); }
  bool operator>=(const C &rhs) const { return !(*this < rhs); }
};

/// Output for Characters.
template <typename C>
raw_ostream &operator<<(raw_ostream &OS, const Character<C> &Ch) {
  if (Ch.is_terminator == false)
    OS << Ch.symbol << " ";
  else
    OS << "#" << Ch.id << "# ";
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
  static size_t str_id;
  typedef Character<CharacterType> CharLike;
  typedef std::vector<CharLike> CharList;
  CharList str_container; /// The container for the string's characters.

public:
  ///-------------------  Properties of TerminatedStrings  ------------------///

  /// Note that size and length have different meanings. Size() includes the
  /// terminator in its result, while length() does not.
  size_t size() const { return str_container.size(); }
  size_t size() { return str_container.size(); }
  size_t length() { return str_container.size() - 1; }
  size_t length() const { return str_container.size() - 1; }
  CharLike getTerminator() const { return str_container.back(); }

  ///-------------------  Iterators for TerminatedStrings -------------------///
  typedef typename CharList::iterator iterator;
  typedef typename CharList::const_iterator const_iterator;

  // Iterators which include the terminator.
  iterator begin() { return str_container.begin(); }
  iterator end() { return str_container.end(); }
  const_iterator begin() const { return str_container.begin(); }
  const_iterator end() const { return str_container.end(); }

  /// Iterators where the terminator is one past the end.
  iterator chars_begin() { return str_container.begin(); }
  iterator chars_end() { return str_container.end() - 1; }
  const_iterator chars_begin() const { return str_container.begin(); }
  const_iterator chars_end() const { return str_container.end() - 1; }

  ///-----------------  TerminatedString operator overloads -----------------///

  /// ==: The only TerminatedString a TerminatedString can be equal to is
  /// itself.
  bool operator==(const TerminatedString &rhs) const {
    /// If they don't have the same terminator, then they can't be the same.
    if (getTerminator() != rhs.getTerminator()) {
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
    return str_container[Idx];
  }

  /// +=, CharacterType: Append a character to an existing TerminatedString
  TerminatedString operator+=(const CharacterType &rhs) {
    assert(size() > 0 && "String is non-terminated!");

    CharLike c;
    c.symbol = rhs;
    c.is_terminator = false;

    CharLike Term = str_container.back();
    assert(Term.is_terminator && "Last character wasn't a terminator!");
    str_container.pop_back();
    str_container.push_back(c);
    str_container.push_back(Term);
    return *this;
  }

  /// +=, CharLike: Append a character to an existing TerminatedString
  TerminatedString operator+=(const CharLike &rhs) {
    assert(size() > 0 && "String is non-terminated!");
    CharLike Term = str_container.back();
    assert(Term.is_terminator && "Last character wasn't a terminator!");
    str_container.pop_back();
    str_container.push_back(rhs);
    str_container.push_back(Term);
    return *this;
  }

  /// +=, InputContainer: Append all characters in an InputContainer to a
  /// TerminatedString.
  TerminatedString operator+=(const InputContainer &rhs) {
    assert(size() > 0 && "String is non-terminated!");
    CharLike Term = str_container.back();
    assert(Term.is_terminator && "Last character wasn't a terminator!");
    str_container.pop_back();

    for (size_t i = 0; i < rhs.size(); i++) {
      CharLike c;
      c.symbol = rhs[i];
      c.is_terminator = false;
      str_container.push_back(c);
    }

    str_container.push_back(Term);
    return *this;
  }

  /// +=, TerminatedString: Append another TerminatedString to an existing
  /// TerminatedString, not including the terminator for that string.
  TerminatedString operator+=(const TerminatedString &rhs) {
    assert(size() > 0 && "String is non-terminated!");
    CharLike Term = str_container.back();
    str_container.pop_back();
    size_t max = rhs.length();

    for (size_t i = 0; i < max; i++) {
      CharLike c;
      c.symbol = rhs[i];
      c.is_terminator = false;
      str_container.push_back(c);
    }

    str_container.push_back(Term);
    return *this;
  }

  ///------------------  Operations on TerminatedStrings --------------------///

  /// erase (size_t): Remove the character at index i from the
  /// TerminatedString
  void erase(const size_t &Idx) {
    assert(Idx != length() && "Can't erase terminator! (Idx == length())");
    assert(Idx < size() && "Out of range! (Idx >= size())");

    str_container.erase(str_container.begin() + Idx);
  }

  /// erase(size_t, size_t): Remove the characters in the size_terval
  /// [StartIdx, EndIdx] from the TerminatedString
  void erase(const size_t &StartIdx, const size_t &EndIdx) {
    assert(StartIdx < length() && "Out of range! (StartIdx >= length())");
    assert(EndIdx < length() && "Out of range! (EndIdx >= length())");
    assert(StartIdx <= EndIdx && "StartIdx was greater than EndIdx");
    str_container.erase(str_container.begin() + StartIdx,
                        str_container.begin() + EndIdx);
  }

  /// insertBefore(size_t, CharacterType): Insert c at the index i-1.
  iterator insertBefore(const size_t &Idx, const CharacterType &Symbol) {
    assert(Idx < size() && "Out of range! (Idx >= size())");
    auto ContainerIt = str_container.begin() + Idx;
    CharLike NewChar;
    NewChar.symbol = Symbol;
    NewChar.is_terminator = false;
    str_container.insert(ContainerIt, NewChar);
    return begin() + Idx;
  }

  ///-----------------  Constructors for TerminatedStrings ------------------///

  /// Construct a TerminatedString from an InputContainer.
  TerminatedString(const InputContainer &C) {
    for (auto it = C.begin(); it != C.end(); it++) {
      CharLike c;
      c.symbol = *it;
      c.is_terminator = false;
      str_container.push_back(c);
    }

    /// Add a terminator to the end.
    CharLike Term;
    Term.is_terminator = true;
    Term.id = str_id;
    str_container.push_back(Term);
    str_id++;
  }

  /// Create a copy of another TerminatedString, including the terminator.
  TerminatedString(const TerminatedString &other) {
    for (size_t i = 0; i < other.size(); i++)
      str_container.push_back(other[i]);
  }

  /// Create an empty TerminatedString. That is, all it contains is a
  /// terminator.
  TerminatedString() {
    CharLike Term;
    Term.is_terminator = true;
    Term.id = str_id;
    str_container.push_back(Term);
    str_id++;
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
size_t TerminatedString<InputContainer, CharLike>::str_id = 0;

/// ====================== TerminatedStringList Type ======================= ///

/// TerminatedStringList
/// Represents a compound string.
///
/// This represents a group of strings appended together to form a large
/// compound string. TerminatedStringLists act exactly like
/// normal TerminatedStrings, but allow us to masize_tain the individuality of
/// specific strings.

template <typename InputContainer, typename CharLike>
struct TerminatedStringList {
private:
  size_t size_chars_;   // Sum of size() for each string in list
  size_t size_strings_; // Number of strings in the list

  typedef TerminatedString<InputContainer, CharLike> String;
  typedef std::vector<String *> StringContainer;
  typedef Character<CharLike> CharacterType;

  StringContainer str;

public:
  ///----------------- Iterators for TerminatedStringLists ------------------///

  typedef typename StringContainer::iterator iterator;
  typedef typename StringContainer::const_iterator const_iterator;

  /// Iterators over the strings in the list
  iterator begin() { return str.begin(); }
  iterator end() { return str.end(); }
  const_iterator begin() const { return str.begin(); }
  const_iterator end() const { return str.end(); }

  /// TODO: Iterators over the characters in the string

  ///------------  Operator overloads for TerminatedStringLists -------------///

  /// This returns the character at index i, treating the entire
  /// TerminatedStringList
  /// itself as a string. This does NOT return the string at index i. This is
  /// for compatability with Ukkonen's algorithm.
  CharacterType operator[](size_t Idx) {
    assert(Idx < size_chars_ && "Out of range! (Idx >= size_chars)");
    auto StrIdx = stringIndexContaining(Idx);

    return stringAt(StrIdx.first)[StrIdx.second];
  }

  ///----------------  Operations on TerminatedStringLists ------------------///

  /// pop_back(): Remove the last string from the TerminatedStringList.
  void pop_back() { str.pop_back(); }

  /// clear(): Remove all of the strings from the TerminatedStringLis
  void clear() { str.clear(); }

  /// size(): Return the number of characters in the TerminatedStringList.
  size_t size() { return size_chars_; }

  /// back(): Return the element at the end of the TerminatedStringList.
  String back() { return *(str.back()); }

  /// stringCount(): Return the number of strings in the TerminatedStringList.
  size_t stringCount() { return size_strings_; }

  /// Append(String*): Append a String* to the TerminatedStringList.
  void append(String *str_) {
    str.push_back(str_);
    size_chars_ += str_->size();
    size_strings_++;
  }

  /// append(String &): Append a String& to the TerminatedStringList.
  void append(String &str_) {
    str.push_back(new String(str_));
    size_chars_ += str_.size();
    size_strings_++;
  }

  /// +=, TerminatedString: Append the rhs to the current list
  TerminatedStringList operator+=(String &Other) {
    append(Other);
    return *this;
  }

  // +=, TerminatedStringList: Append the strings in the rhs to this list
  TerminatedStringList operator+=(TerminatedStringList &Other) {
    for (size_t Idx = 0; Idx < Other.stringCount(); Idx++)
      append(Other.stringAt(Idx));
    return *this;
  }

  /// stringAt(): Return the string with index i
  String stringAt(const size_t &Idx) {
    assert(Idx < size_chars_ && "Out of range! (Idx >= size_chars)");
    return *(str[Idx]);
  }

  /// Insert the character c before the index i in the string
  iterator insertBefore(const size_t &Idx, const CharLike &C) {
    assert(Idx < size_chars_ && "Out of range! (Idx >= size_chars)");
    auto string_pair = stringContaining(Idx);
    string_pair.first->insertBefore(string_pair.second, C);
    size_chars_++;

    return begin() + Idx;
  }

  /// stringContaining(size_t): Return the index of the string that index i
  /// maps size_to. If out of bounds, return -1.
  std::pair<size_t, size_t> stringIndexContaining(size_t Idx) {
    assert(Idx < size_chars_ && "Out of range! (Idx >= size_chars)");
    /// Find the string index containing i.
    size_t StrIdx;
    for (StrIdx = 0; StrIdx != size_strings_ - 1; StrIdx++) {

      /// Idx must be inside the string, since it's in the range [0, size-1].
      if (Idx < str[StrIdx]->size())
        break;

      /// Otherwise, we move over to the next string by offsetting Idx by the
      /// size of the current string.
      Idx -= str[StrIdx]->size();
    }

    return std::make_pair(StrIdx, Idx);
  }

  /// stringContaining: Return the string that the index i maps size_to, and the
  /// local index for that string.
  std::pair<String *, size_t> stringContaining(size_t Idx) {
    assert(Idx < size_chars_ && "Out of range! (Idx >= size_chars)");
    auto StrIdx = stringIndexContaining(Idx);

    return make_pair(str[StrIdx.first], StrIdx.second);
  }

  /// erase(size_t): Remove the character at index i in the
  /// TerminatedStringList
  void erase(size_t Idx) {
    assert(Idx < size_chars_ && "Out of range! (Idx >= size_chars)");
    auto string_pair = stringContaining(Idx);
    string_pair.first->erase(string_pair.second);
    size_chars_--;
  }

  /// erase(size_t, size_t): Remove the characters in the range [StartIdx,
  /// EndIdx] from the TerminatedStringList
  void erase(size_t StartIdx, size_t EndIdx) {
    assert(StartIdx < size_chars_ && "Out of range! (StartIdx >= size_chars)");
    assert(EndIdx < size_chars_ && "Out of range! (EndIdx >= size_chars)");

    size_t RangeLen = EndIdx - StartIdx;
    size_t NumRemoved = 0;

    while (NumRemoved < RangeLen) {
      erase(StartIdx);
      NumRemoved++;
    }
  }

  ///---------------  Constructors for TerminatedStringLists ----------------///

  TerminatedStringList() : size_chars_(0), size_strings_(0) {}

  TerminatedStringList(String *Str) : size_chars_(0), size_strings_(0) {
    append(Str);
  }

  TerminatedStringList(String &Str) : size_chars_(0), size_strings_(0) {
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
    OS << "'" << **It << "'" << ", ";

  OS << "]";

  return OS;
}

#endif // TERM_STRING_H
