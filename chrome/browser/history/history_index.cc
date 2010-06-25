// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/history/history_index.h"

#include <algorithm>
#include <sstream>

#include "third_party/icu/public/common/unicode/brkiter.h"
#include "third_party/icu/public/common/unicode/utypes.h"

using std::string;
using icu::Locale;
using icu::BreakIterator;
using icu::RegexMatcher;
using icu::RuleBasedBreakIterator;
using icu::UnicodeString;

// Define the following to '1' if the parameters to the URL should not be
// indexed.
#define TRIM_PARAMETERS 1

// Define the following to specify that only the most recent X history
// entries are to be considered.
#define LIMIT_HISTORY_TO 10000


HistoryIndex::~HistoryIndex() {
  if (history_database_)
    sqlite3_close(history_database_);
  delete question_matcher_;
  delete percent_matcher_;
  delete non_alnum_matcher_;
  delete bar_matcher_;
  delete word_breaker_;
}

// Indexing

bool HistoryIndex::IndexURLHistory(std::string history_path) {
  bool success = false;
  // Reset our indexes.
  char_word_map_.clear();
  word_id_history_map_.clear();
  if (history_database_)
    sqlite3_close(history_database_);

  int result = sqlite3_open(history_path.c_str(), &history_database_);
  if (result == SQLITE_OK) {
    // TODO(mrossetti): Consider filtering only the most recent and the most
    // often referenced history items. Also consider usage count when scoring.
    std::ostringstream query_stream;
    query_stream << "SELECT id, url, title FROM urls";
#if LIMIT_HISTORY_TO
    query_stream << " ORDER BY last_visit_time DESC LIMIT " << LIMIT_HISTORY_TO;
#endif  // LIMIT_HISTORY_TO
    query_stream << ";";
    result = sqlite3_exec(history_database_,
                          query_stream.str().c_str(),
                          &URLHistoryQueryHandler,
                          reinterpret_cast<void*>(this),
                          NULL);
    if (result == SQLITE_OK) {
      success = true;
    }
  }
  return success;
}

// NOTE: Not thread-safe because RuleBasedBreakIterator::getRuleStatus isn't.
bool HistoryIndex::IndexRow(const char* id, const char* url,
                            const char* title) {
  UnicodeString uni_string = UnicodeString::fromUTF8(url);
  UErrorCode icu_status = U_ZERO_ERROR;

#ifdef TRIM_PARAMETERS
  // Trim the URL at the first '?'.
  if (!question_matcher_) {
    question_matcher_ = new RegexMatcher("\\?", 0, icu_status);
    if (U_FAILURE(icu_status)) {
      // There was a syntax error in the regular expression.
      return false;
    }
  }
  question_matcher_->reset(uni_string);
  if (question_matcher_->find()) {
    int32_t start = question_matcher_->start(icu_status);
    // TODO(mrossetti): Check status here.
    uni_string.handleReplaceBetween(start, uni_string.length(), "");
  }
#endif

  // Eliminate any '%xx' encodings in the URL.
  // TODO(mrossetti): We should probably convert the '%xx' encodings.
  if (!percent_matcher_) {
    percent_matcher_ = new RegexMatcher("%[0-9a-fA-F]{2}", 0, icu_status);
    if (U_FAILURE(icu_status)) {
      // There was a syntax error in the regular expression.
      return false;
    }
  }
  percent_matcher_->reset(uni_string);
  uni_string = percent_matcher_->replaceAll(" ", icu_status);
  if (U_FAILURE(icu_status)) {
    // There was a failure during the replace.
    return false;
  }

  // Determine the row ID in the database.
  sqlite_int64 test_id = strtol(id, NULL, 10);
  if (test_id < 0 || test_id > UINT16_MAX) {
    // TODO(mrossetti): Handle error condition here.
  }
  HistoryID history_id = labs(test_id);

  // Add the row for quick lookup in the history info store.
  // NOTE: Update the following when visit count and time becomes important.
  string lower_url(url);
  string lower_title(title);
  string time_string("");
  // Lowercase URL and Title.
  transform(lower_url.begin(), lower_url.end(), lower_url.begin(),
            tolower);
  transform(lower_title.begin(), lower_title.end(), lower_title.begin(),
            tolower);
  HistoryItem hist_item(history_id, lower_url, lower_title, 0, time_string);
  history_info_map_.insert(std::make_pair(history_id, hist_item));

  // Catenate the URL and the title in preparation for coming up with words.
  if (title[0]) {
    uni_string.append(" ");
    uni_string.append(UnicodeString::fromUTF8(title));
  }

  // Break down the URL and title into individual, unique words.
  UnicodeStringSet words = WordsFromUnicodeString(uni_string);

  // For each word, add a new entry into the word index referring to the
  // history item.
  for (UnicodeStringSet::iterator iter = words.begin();
       iter != words.end(); ++iter) {
    UnicodeString uni_word = *iter;
    WordMap::iterator word_pos = word_map_.find(uni_word);
    if (word_pos != word_map_.end()) {
      // Update existing entry in the word/history index.
      WordID word_id = word_pos->second;
      WordIDHistoryMap::iterator history_pos =
          word_id_history_map_.find(word_id);
      if (history_pos != word_id_history_map_.end()) {
        HistoryIDSet& history_id_set(history_pos->second);
        history_id_set.insert(history_id);
      } else {
        // TODO(mrossetti): Handle the unexpected failure here.
      }
    } else {
      // Add a new word to the word list and the word map, and then create a
      // new entry in the word/history map.
      word_list_.push_back(uni_word);
      WordID word_id = word_list_.size() - 1;
      if (word_map_.insert(std::make_pair(uni_word, word_id)).second) {
        HistoryIDSet history_id_set;
        history_id_set.insert(history_id);
        if (word_id_history_map_.insert(
            std::make_pair(word_id, history_id_set)).second) {
          // For each character in the newly added word (i.e. a word that is not
          // already in the word index), add the word to the character index.
          UCharSet characters =
              HistoryIndex::CharactersFromUnicodeString(uni_word);
          for (UCharSet::iterator uni_char_iter = characters.begin();
               uni_char_iter != characters.end(); ++uni_char_iter) {
            UChar uni_string = *uni_char_iter;
            CharWordIDMap::iterator char_iter = char_word_map_.find(uni_string);
            if (char_iter != char_word_map_.end()) {
              // Update existing entry in the char/word index.
              WordIDSet& word_id_set(char_iter->second);
              word_id_set.insert(word_id);
            } else {
              // Create a new entry in the char/word index.
              WordIDSet word_id_set;
              word_id_set.insert(word_id);
              if (!char_word_map_.insert(std::make_pair(uni_string,
                                                        word_id_set)).second) {
                // TODO(mrossetti): Handle the unexpected failure here.
              }
            }
          }
        } else {
          // TODO(mrossetti): Handle the unexpected failure here.
        }
      } else {
        // TODO(mrossetti): Handle the unexpected failure here.
      }
    }
  }
  ++history_item_count_;
  return true;
}

void HistoryIndex::Reset() {
  if (history_database_) {
    sqlite3_close(history_database_);
    history_database_ = NULL;
  }
  word_list_.clear();
  word_map_.clear();
  char_word_map_.clear();
  word_id_history_map_.clear();
  history_info_map_.clear();
  history_item_count_ = 0;
}

int HistoryIndex::URLHistoryQueryHandler(void* context, int columns,
                                         char** column_strings,
                                         char** column_names) {
  int success = SQLITE_ABORT;
  // Verify we got the expected columns.
  // We expect: 1) id, 2) url, 3) title.
  if (columns == 3) {
    HistoryIndex* delegate(static_cast<HistoryIndex*>(context));
    if (delegate->IndexRow(column_strings[0], column_strings[1],
                           column_strings[2]))
      success = SQLITE_OK;
  }
  return success;
}

// Searching

HistoryItemVector HistoryIndex::HistoryItemsForTerms(
    SearchTermsVector const& terms) {
  HistoryItemVector item_vector;
  if (history_database_ && terms.size()) {
    // Reset used_ flags for term_char_word_set_cache_. We use a basic mark-
    // and-sweep approach.
    // TODO(mrossetti): Consider keeping more of the cache around for possible
    // repeat searches, except a 'shortcuts' approach might be better for that.
    // TODO(mrossetti): Change TermCharWordSet to a class and use for_each.
    for (TermCharWordSetVector::iterator iter =
            term_char_word_set_cache_.begin();
         iter != term_char_word_set_cache_.end(); ++iter)
      iter->used_ = false;

    // Lowercase the terms.
    SearchTermsVector lower_terms;
    for (SearchTermsVector::const_iterator term_iter = terms.begin();
         term_iter != terms.end(); ++term_iter) {
      string lower_string(*term_iter);
      transform(lower_string.begin(), lower_string.end(), lower_string.begin(),
                tolower);
      lower_terms.push_back(lower_string);
    }

    // Composite a string by joining all terms, separated by a space.
    // TODO(mrossetti): Use a transform algorithm, eh.
    string all_terms = lower_terms[0];
    for (SearchTermsVector::iterator term_iter = ++lower_terms.begin();
         term_iter != lower_terms.end(); ++term_iter) {
      all_terms += " " + *term_iter;
    }

    UnicodeString uni_string = UnicodeString::fromUTF8(all_terms.c_str());

    // Break the terms down into individual terms (words), get the candidate
    // set for each term, and intersect each to get a final candidate list.
    HistoryIDSet history_id_set;
    UnicodeStringSet words = WordsFromUnicodeString(uni_string);
    bool first_word = true;
    for (UnicodeStringSet::iterator iter = words.begin();
         iter != words.end(); ++iter) {
      UnicodeString uni_word = *iter;
      HistoryIDSet term_history_id_set =
          HistoryIndex::HistoryIDsForTerm(uni_word);
      if (first_word) {
        history_id_set = term_history_id_set;
        first_word = false;
      } else {
        HistoryIDSet old_history_id_set(history_id_set);
        history_id_set.clear();
        std::set_intersection(old_history_id_set.begin(),
                              old_history_id_set.end(),
                              term_history_id_set.begin(),
                              term_history_id_set.end(),
                              std::inserter(history_id_set,
                                            history_id_set.begin()));
      }
    }

    if (history_id_set.size()) {
      // For each item in the final candidate list, perform a substring match
      // for each original individual term and weed out non-matches.
      if (use_SQLite_filter_) {
        std::ostringstream query_stream;
        query_stream << "SELECT id, url, title, visit_count, last_visit_time "
                        "FROM urls WHERE id IN ( ";

        // I'm sure there's an algorithm for this!
        bool first_id = true;
        for (HistoryIDSet::iterator id_iter = history_id_set.begin();
             id_iter != history_id_set.end(); ++id_iter) {
          if (first_id) {
            query_stream << *id_iter;
            first_id = false;
          } else {
            query_stream << "," << *id_iter;
          }
        }
        query_stream << ");";

        HistoryItemVector term_query_results;
        int result = sqlite3_exec(history_database_,
                                  query_stream.str().c_str(),
                                  &URLTermQueryHandler,
                                  reinterpret_cast<void*>(&term_query_results),
                                  NULL);
        if (result == SQLITE_OK) {
          // Reduce the results by filtering out non-substring matches.
#if 0
          // TODO(mrossetti): Regain your C++-fu and get the following to work.
          term_query_results.erase(remove_if(
              term_query_results.begin(),
              term_query_results.end(),
              bind2nd(mem_fun_ref(&HistoryItem::TermIsMissing), &lower_terms)));
          item_vector = term_query_results;
#else
          for (HistoryItemVector::iterator hist_iter =
                   term_query_results.begin();
               hist_iter != term_query_results.end(); ++hist_iter) {
            HistoryItem& hist_item(*hist_iter);
            if (!hist_item.TermIsMissing(&lower_terms))
              item_vector.push_back(hist_item);
          }
#endif
        }
        // TODO(mrossetti): Handle any sqlite3 errors.
      } else {
        for (HistoryIDSet::iterator id_iter = history_id_set.begin();
             id_iter != history_id_set.end(); ++id_iter) {
          HistoryID history_id = *id_iter;
          HistoryInfoMap::iterator hist_pos =
              history_info_map_.find(history_id);
          if (hist_pos != history_info_map_.end()) {
            HistoryItem& hist_item(hist_pos->second);
            if (!hist_item.TermIsMissing(&lower_terms))
              item_vector.push_back(hist_item);
          } else {
            // Handle unexpected missing history item here.
          }
        }
      }

      // Score.
      // TODO(mrossetti): Implement scoring after looking at current impl.
    }
  }

  // Remove any stale TermCharWordSet's.
  TermCharWordSetVector new_vector;
  for (TermCharWordSetVector::iterator iter = term_char_word_set_cache_.begin();
       iter != term_char_word_set_cache_.end(); ++iter) {
    if (iter->used_)
      new_vector.push_back(*iter);
  }
  term_char_word_set_cache_ = new_vector;

  return item_vector;
}

HistoryIDSet HistoryIndex::HistoryIDsForTerm(UnicodeString uni_word) {
  HistoryIDSet history_id_set;

  // For each character in the word, get the char/word_id map entry and
  // intersect with the set in an incremental manner.
  UCharSet uni_chars = CharactersFromUnicodeString(uni_word);
  WordIDSet word_id_set(WordIDSetForTermChars(uni_chars));

  // If any words resulted then we can compose a set of history IDs by unioning
  // the sets from each word.
  if (word_id_set.size()) {
    for (WordIDSet::iterator word_id_iter = word_id_set.begin();
         word_id_iter != word_id_set.end(); ++word_id_iter) {
      WordID word_id = *word_id_iter;
      WordIDHistoryMap::iterator word_iter = word_id_history_map_.find(word_id);
      if (word_iter != word_id_history_map_.end()) {
        HistoryIDSet& word_history_id_set(word_iter->second);
        history_id_set.insert(word_history_id_set.begin(),
                              word_history_id_set.end());
      }
    }
  }

  return history_id_set;
}

int HistoryIndex::URLTermQueryHandler(void* context, int columns,
                                      char** column_strings,
                                      char** column_names) {
  int success = SQLITE_ABORT;
  // Verify we got the expected columns.
  // We expect: 1) id, 2) url, 3) title, 4) visit_count, 5) last_visit_time.
  if (columns == 5) {
    HistoryItemVector* items = static_cast<HistoryItemVector*>(context);
    HistoryID history_id = atoll(column_strings[0]);
    string url(column_strings[1]);
    string title(column_strings[2]);
    int visit_count = atoi(column_strings[3]);
    string last_visit_time(column_strings[4]);
    HistoryItem item(history_id, url, title, visit_count, last_visit_time);
    items->push_back(item);
    success = SQLITE_OK;
  }
  return success;
}

// Utility Functions

UnicodeStringSet HistoryIndex::WordsFromUnicodeString(
    UnicodeString uni_string) {
  UnicodeStringSet words;
  UErrorCode icu_status = U_ZERO_ERROR;
  // TODO(mrossetti): Get locale from browser:
  //      icu::Locale(g_browser_process->GetApplicationLocale().c_str())
  Locale locale(Locale::getDefault());

  uni_string.toLower(locale);  // Convert to lowercase.

  // Replace all | & _'s with a space.
  // TODO(mrossetti): Roll this into the word_breaker_.
  if (!bar_matcher_) {
    bar_matcher_ = new RegexMatcher("[|_]+", 0, icu_status);
    if (U_FAILURE(icu_status)) {
      // There was a syntax error in the regular expression.
      return words;
    }
  }
  bar_matcher_->reset(uni_string);
  uni_string = bar_matcher_->replaceAll(" ", icu_status);
  if (U_FAILURE(icu_status)) {
    // There was a failure during the replace.
    return words;
  }

  // Break up the string into individual words.
  if (!word_breaker_) {
    word_breaker_ = static_cast<RuleBasedBreakIterator*>
    (RuleBasedBreakIterator::createWordInstance(locale, icu_status));
    if (U_FAILURE(icu_status)) {
      delete word_breaker_;
      return words;
    }
  }
  word_breaker_->setText(uni_string);
  int32_t start = 0;
  int32_t previous_boundary = 0;
  while (start != BreakIterator::DONE) {
    int32_t breakType = word_breaker_->getRuleStatus();
    if (breakType != UBRK_WORD_NONE) {
      // TODO(mrossetti): Numbers may have non-numeric characters that
      //    will have to be stripped before term matching. Do this during
      //    term matching.
      // number: UBRK_WORD_NUMBER <= breakType < UBRK_WORD_NUMBER_LIMIT
      // word: UBRK_WORD_LETTER <= breakType < UBRK_WORD_LETTER_LIMIT
      // kana: UBRK_WORD_KANA <= breakType < UBRK_WORD_KANA_LIMIT
      // ideo: UBRK_WORD_IDEO <= breakType < UBRK_WORD_IDEO_LIMIT
      int32_t current_boundary = word_breaker_->current();
      int32_t word_length = current_boundary - previous_boundary;
      UnicodeString uni_word(uni_string, previous_boundary, word_length);

      // Remove non-word characters from the string.
      if (!non_alnum_matcher_) {
        non_alnum_matcher_ = new RegexMatcher("\\W+", 0, icu_status);
        if (U_FAILURE(icu_status)) {
          // There was a syntax errors in the regular expression.
          return words;
        }
      }
      non_alnum_matcher_->reset(uni_word);
      uni_word = non_alnum_matcher_->replaceAll("", icu_status);
      if (U_FAILURE(icu_status)) {
        // There was a failure during the replace.
        break;
      }

      if (uni_word.length())
        words.insert(uni_word);
      previous_boundary = current_boundary;
    }
    start = word_breaker_->next();
  }
  return words;
}

UCharSet HistoryIndex::CharactersFromUnicodeString(
    icu::UnicodeString uni_word) {
  UCharSet characters;
  icu::StringCharacterIterator ci(uni_word);
  for (UChar uni_char = ci.first();
       uni_char != icu::CharacterIterator::DONE; uni_char = ci.next())
    characters.insert(uni_char);
  return characters;
}

WordIDSet HistoryIndex::WordIDSetForTermChars(UCharSet const& uni_chars) {
  TermCharWordSet* best_term_char_word_set = NULL;
  bool set_found = false;
  size_t outstanding_count = 0;
  UCharSet outstanding_chars;
  for (TermCharWordSetVector::iterator iter = term_char_word_set_cache_.begin();
       iter != term_char_word_set_cache_.end(); ++iter) {
    TermCharWordSet& term_char_word_set(*iter);
    UCharSet& char_set(term_char_word_set.char_set_);
    UCharSet exclusions;
    set_difference(char_set.begin(), char_set.end(),
                   uni_chars.begin(), uni_chars.end(),
                   std::inserter(exclusions, exclusions.begin()));
    if (exclusions.size() == 0) {
      // Do a reverse difference so that we know which characters remain
      // to be indexed. Then decide if this is a better match than any
      // previous cached set.
      set_difference(uni_chars.begin(), uni_chars.end(),
                     char_set.begin(), char_set.end(),
                     std::inserter(exclusions, exclusions.begin()));
      if (!set_found || exclusions.size() < outstanding_count) {
        outstanding_chars = exclusions;
        best_term_char_word_set = &*iter;
        outstanding_count = exclusions.size();
        set_found = true;
      }
    }
  }

  WordIDSet word_id_set;
  if (set_found && outstanding_count == 0) {
    // If there were no oustanding characters then we can use the cached one.
    best_term_char_word_set->used_ = true;
    word_id_set = best_term_char_word_set->word_id_set_;
  } else {
    // Some or all of the characters remain to be indexed.
    bool first_character = true;
    if (set_found) {
      first_character = false;
      word_id_set = best_term_char_word_set->word_id_set_;
    } else {
      outstanding_chars = uni_chars;
    }

    for (UCharSet::iterator uni_char_iter = outstanding_chars.begin();
         uni_char_iter != outstanding_chars.end(); ++uni_char_iter) {
      UChar uni_char = *uni_char_iter;
      CharWordIDMap::iterator char_iter = char_word_map_.find(uni_char);
      if (char_iter != char_word_map_.end()) {
        WordIDSet& char_word_id_set(char_iter->second);
        if (first_character) {
          word_id_set = char_word_id_set;
          first_character = false;
        } else {
          WordIDSet old_word_id_set(word_id_set);
          word_id_set.clear();
          std::set_intersection(old_word_id_set.begin(),
                                old_word_id_set.end(),
                                char_word_id_set.begin(),
                                char_word_id_set.end(),
                                std::inserter(word_id_set,
                                word_id_set.begin()));
        }
      } else {
        // The character was not found so bail.
        word_id_set.clear();
        break;
      }
    }
    // Update the cache.
    if (!set_found || outstanding_count) {
      TermCharWordSet new_char_word_set(uni_chars, word_id_set, true);
      term_char_word_set_cache_.push_back(new_char_word_set);
    }
  }
  return word_id_set;
}

// Statistics

#if HISTORY_INDEX_STATISTICS_ENABLED

u_int64_t HistoryIndex::IndexedHistoryCount() {
  return history_item_count_;
}

u_int64_t HistoryIndex::IndexedWordCount() {
  return word_list_.size();
}

// Return a count of the characters which have been indexed.
u_int64_t HistoryIndex::IndexedCharacterCount() {
  return char_word_map_.size();
}

UnicodeStringVector HistoryIndex::IndexedWords() const {
  UnicodeStringVector words(word_list_);
  return words;  // Return a copy.
}

u_int64_t HistoryIndex::NumberOfHistoryItemsForWord(UnicodeString uni_word) {
  u_int64_t history_count = 0;
  WordMap::iterator word_pos = word_map_.find(uni_word);
  if (word_pos != word_map_.end()) {
    WordID word_id = word_pos->second;
    WordIDHistoryMap::iterator history_pos = word_id_history_map_.find(word_id);
    if (history_pos != word_id_history_map_.end())
      history_count = history_pos->second.size();
  }
  return history_count;
}

UnicodeStringVector HistoryIndex::IndexedCharacters() {
  UnicodeStringVector characters;
  for (CharWordIDMap::iterator iter = char_word_map_.begin();
       iter != char_word_map_.end(); ++iter)
    characters.push_back(iter->first);
  return characters;
}

u_int64_t HistoryIndex::NumberOfWordsForChar(UChar uni_char) {
  u_int64_t word_count = 0;
  CharWordIDMap::iterator char_pos = char_word_map_.find(uni_char);
  if (char_pos != char_word_map_.end())
    word_count = char_pos->second.size();
  return word_count;
}

u_int64_t HistoryIndex::WordListSize() {
  u_int64_t listSize = 0;
  // TODO(mrossetti): Use an algorithm.
  for (UnicodeStringVector::iterator iter = word_list_.begin();
       iter != word_list_.end(); ++iter) {
    listSize += iter->length();  // vector overhead is ignored.
  }
  return listSize;
}

u_int64_t HistoryIndex::WordMapSize() {
  u_int64_t mapSize = 0;
  for (WordMap::iterator iter = word_map_.begin(); iter != word_map_.end();
       ++iter) {
    mapSize += sizeof(WordID) + iter->first.length();
  }
  return mapSize;
}

u_int64_t HistoryIndex::WordHistoryMapSize() {
  u_int64_t mapSize = 0;
  for (WordIDHistoryMap::iterator iter = word_id_history_map_.begin();
       iter != word_id_history_map_.end(); ++iter) {
    mapSize += sizeof(WordID) + (iter->second.size() * sizeof(HistoryID));
  }
  return mapSize;
}

u_int64_t HistoryIndex::CharWordMapSize() {
  u_int64_t mapSize = 0;
  for (CharWordIDMap::iterator iter = char_word_map_.begin();
       iter != char_word_map_.end(); ++iter) {
    mapSize += sizeof(UChar) + (iter->second.size() * sizeof(WordID));
  }
  return mapSize;
}

u_int64_t HistoryIndex::HistoryInfoSize() {
  u_int64_t mapSize = 0;
  for (HistoryInfoMap::iterator iter = history_info_map_.begin();
       iter != history_info_map_.end(); ++iter) {
    mapSize += sizeof(iter->first);
    HistoryItem& hist_item(iter->second);
    mapSize += sizeof(hist_item.history_id_);
    mapSize += hist_item.url_.size();
    mapSize += hist_item.title_.size();
    mapSize += sizeof(hist_item.visit_count_);
    mapSize += hist_item.last_visit_time_.size();
  }
  return mapSize;
}

#endif  // HISTORY_INDEX_STATISTICS_ENABLED

// HistoryItem Methods

bool HistoryItem::TermIsMissing(SearchTermsVector* terms_vector) const {
  for (SearchTermsVector::iterator term_iter = terms_vector->begin();
       term_iter != terms_vector->end(); ++term_iter) {
    string term = *term_iter;
    if (term.length() > 1 &&
        url_.find(term) == string::npos &&
        title_.find(term) == string::npos)
      return true;
  }
  return false;
}

