#ifndef PB_REGEX_H
#define PB_REGEX_H

// Linear-time byte regex engine for untrusted payloads: a Pike VM (no
// backtracking, no match-time allocation) plus an eager DFA built within the
// PB_REGEX_DFA_* caps, used by test and to reject non-matching text in search.
// All scratch memory comes from the compile arena and lives as long as it.
//
// Syntax (bytes, no Unicode; text may hold NULs):
//   literals, \n \r \t \xHH, any escaped non-alphanumeric byte is literal
//   .  any byte except \n (any byte under the s flag)
//   [abc] [a-z] [^...]  classes; \d \w \s \D \W \S work inside; ']' first and
//      '-' first/last are literal
//   \d \D \w \W \s \S  ASCII classes;  \b \B  ASCII word boundary
//   ^ $  start/end of the whole text (no multiline mode)
//   * + ? {n} {n,} {n,m}  with lazy variants; bounds <= PB_REGEX_MAX_REPEAT;
//      not on assertions; a literal brace is \{
//   a|b  (...)  (?:...)
//   (?i) (?s) (?is)  only as a prefix; (?i) folds ASCII only
// PB_REGEX_UNSUPPORTED: backreferences, lookaround, named groups, possessive
// forms, POSIX classes, \p{..}, later inline flags, unknown alphanumeric escapes.
//
// Perl-style leftmost-first. Group spans may differ from Perl when a repeated
// group body can match empty, e.g. (a?)+.

#include "pb_sexpr.h"

#include <stdbool.h>
#include <stddef.h>

// Pattern caps; compile fails with a specific error past any of them.
#define PB_REGEX_MAX_PATTERN 1024 // pattern bytes
#define PB_REGEX_MAX_PROGRAM 1024 // compiled instructions, after {n,m} expansion
#define PB_REGEX_MAX_GROUPS 9     // capture groups, excluding the whole match
#define PB_REGEX_MAX_REPEAT 255   // largest n or m in {n,m}
#define PB_REGEX_MAX_DEPTH 32     // group nesting depth
// DFA caps. Past either, the pattern silently uses the Pike VM alone.
#define PB_REGEX_DFA_MAX_STATES 512
#define PB_REGEX_DFA_MAX_TABLE_BYTES (128 * 1024) // transition table, states * byte classes * 2

// Pike VM work budget: one step per live thread per position, so worst case is
// about program_len * text_len (roughly 60 to 200 ms for one form call at the
// default). A DFA-backed test is never charged; unlucky patterns without a DFA
// and span-finding searches (replace-all) are the exposed ones.
#define PB_REGEX_DEFAULT_BUDGET (16u * 1024 * 1024)

typedef enum pb_regex_error {
    PB_REGEX_OK,
    PB_REGEX_OOM,             // arena allocation failed
    PB_REGEX_TOO_LONG,        // pattern longer than PB_REGEX_MAX_PATTERN
    PB_REGEX_TOO_LARGE,       // program or repetition bound over its cap
    PB_REGEX_TOO_MANY_GROUPS, // more than PB_REGEX_MAX_GROUPS capture groups
    PB_REGEX_TOO_DEEP,        // groups nested deeper than PB_REGEX_MAX_DEPTH
    PB_REGEX_SYNTAX,          // malformed pattern
    PB_REGEX_UNSUPPORTED,     // valid elsewhere but deliberately not supported
} pb_regex_error;

// Compiled regex, arena-owned. Matching mutates its scratch: not reentrant, one loop thread.
typedef struct pb_regex pb_regex;

// Byte span of a match or group within the searched text. `matched` is false
// for a group that did not participate (start and end are then 0).
typedef struct pb_regex_span {
    size_t start;
    size_t end;
    bool matched;
} pb_regex_span;

// Compile `pattern` into `arena` (bytes are copied). On failure *out is NULL,
// *err_offset (if non-NULL) is the pattern offset, and err_msg (if non-NULL) gets
// a NUL-terminated message.
pb_regex_error pb_regex_compile(pb_arena *arena, pb_slice pattern, pb_regex **out, size_t *err_offset, char *err_msg, size_t err_msg_cap);
const char *pb_regex_error_name(pb_regex_error err);

// True when the pattern has a DFA; behaviour is identical either way. Diagnostic.
bool pb_regex_has_dfa(const pb_regex *re);
// DFA state count including the two terminal states, 0 without a DFA. Diagnostic.
size_t pb_regex_dfa_states(const pb_regex *re);

// Compiled instruction count (what the VM budget scales with). Diagnostic.
size_t pb_regex_program_len(const pb_regex *re);

// Original pattern text (arena copy, including any flag prefix).
pb_slice pb_regex_source(const pb_regex *re);
// Number of capturing groups, 0..PB_REGEX_MAX_GROUPS.
size_t pb_regex_group_count(const pb_regex *re);

// GAVE_UP means the budget ran out before the answer was known: not "no match".
typedef enum pb_regex_result {
    PB_REGEX_NO_MATCH,
    PB_REGEX_MATCH,
    PB_REGEX_GAVE_UP,
} pb_regex_result;

// *budget is the remaining Pike VM allowance in thread-steps, decremented in
// place so an iterating caller shares one budget. NULL means unlimited. DFA
// lookups are not charged.

// Does the pattern match anywhere in text? Tracks no captures.
pb_regex_result pb_regex_test(pb_regex *re, pb_slice text, size_t *budget);

// Leftmost match starting at or after `from` (NO_MATCH if from > text.len).
// Fills spans[0] (whole match) and spans[1..] (groups) up to spans_cap; spans
// may be NULL. ^, $ and \b see the whole text, so iterate by passing the previous
// match end as `from` (plus one after an empty match).
pb_regex_result pb_regex_search(pb_regex *re, pb_slice text, size_t from, pb_regex_span *spans, size_t spans_cap, size_t *budget);

#endif
