# Article 6: Incremental Syntax Highlighting

## The problem with full re-lex

A naive syntax highlighter re-lexes the entire document on every edit. For a 10,000-line C file, this takes 2–5 ms. That is unacceptable for a 60 Hz display: every keystroke must complete in under 16 ms, and 5 ms leaves nothing for layout and rendering.

The solution is incremental lexing: after an edit on line L, only re-lex L and any subsequent lines whose syntax state has changed. In practice, most edits affect 0–3 lines. A `//` comment edit at the top of a file re-lexes nothing below it. A `/*` comment edit re-lexes to the matching `*/` — that is O(n) once, and it is identical in every real editor.

## The resumable lexer

The lexer is a C-family lexical scanner with resumable state. It must handle:

- Single-line comments (`//`)
- Multi-line comments (`/* */`)
- String literals (with escape sequences and multi-line)
- Preprocessor continuation lines (`\` at end of line)
- Raw string delimiters (C11 `R"(...)"`)
- Nested preprocessor conditionals (`#if` / `#ifdef` / `#else` / `#endif`)

A pair of booleans is insufficient for all C++ lexical constructs. The lexer needs an incoming state that carries the full context: whether we are inside a comment, inside a string, inside a preprocessor directive, at what nesting depth.

```c
typedef struct {
    uint8_t paren_depth;      // nesting level of () for preprocessor
    uint8_t bracket_depth;    // nesting level of []
    uint8_t brace_depth;      // nesting level of {}
    bool    in_comment;       // inside /* ... */
    bool    in_string;        // inside "..."
    bool    in_preproc_line;  // line continuation of preprocessor directive
} LexerState;
```

This state is stored per-line at line boundaries. The array of states is SoA parallel to the line index — `mu_bitset` for boolean flags and a `uint8_t` depth array.

## Sparse checkpoint storage

The key design: store incoming-state checkpoints at line boundaries, and detailed style runs only for cached visible text.

```c
typedef struct {
    uint32_t line_number;
    LexerState incoming_state; // state entering this line
    uint32_t outgoing_state_hash; // hash of state leaving this line
} SyntaxCheckpoint;
```

Checkpoints are stored in packed blocks — not a global absolute-offset array. Edits do not rewrite a global array. A new checkpoint block is allocated when the edit's region spans an existing block boundary.

## The relex algorithm

After an edit at line L:

1. Mark all downstream lines unverified from L.
2. Find the nearest valid checkpoint before L (scanning backward through packed blocks).
3. Resume the lexer from that checkpoint's outgoing state.
4. Lex line by line forward. At each line, compare the newly computed outgoing state with the stored incoming state of the next line.
5. **Stop propagation only at an unchanged, correctly remapped checkpoint whose incoming state matches the newly computed state.** Equality at an arbitrary edited line is not enough — the checkpoint must be reachable from the edit point with a remapped state.

This early-exit is what makes incremental lexing fast. An edit to a `//` comment inside a function stops after the next newline. An edit to `{` inside a function stops immediately. A flipped `/*` re-lexes to the closing `*/` — accepted as the worst case.

## Style runs and palette indices

Tokens are not stored. Instead, during glyph generation, the lexer runs inline for each visible line and emits palette indices directly.

```c
typedef struct {
    uint32_t byte_offset;    // start of style run
    uint32_t byte_length;    // length of style run
    uint8_t  palette_index;  // 0=plain, 1=keyword, 2=string, 3=comment, 4=number, ...
} StyleRun;
```

The keyword set is a perfect-hash table of ~40 words — a static, fixed-size open-addressing table. No dynamic allocation. The lexer classifies each token by scanning the identifier through the table.

**Style invalidation is separate from geometry invalidation.** Completing syntax need not repeat layout. A line's style can be recomputed without regenerating glyph instances, and vice versa. This is the critical performance optimization: if a theme changes, only the palette is updated — the style runs and glyph instances are reused.

## The lexer budget

The lexer runs under a per-tick byte/time budget. It pauses within long tokens or lines. Until the state reaching a visible region is known, the editor uses **plain styling** rather than stale colors.

This is a deliberate quality-of-service tradeoff: a jump near EOF after an opening comment change may need substantial catch-up. This is an explicit limitation of sequential lexical dependencies. The user sees plain text while the lexer catches up. This is better than freezing the editor.

## Keyword classification

The keyword table is a fixed-size open-addressing hash table:

```c
// C-family keywords
static const char *keywords[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if",
    "inline", "int", "long", "register", "restrict", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
    "unsigned", "void", "volatile", "while", "_Bool", "_Complex", "_Imaginary",
    // C++ additions
    "class", "const_cast", "delete", "dynamic_cast", "explicit", "export",
    "false", "friend", "inline", "namespace", "new", "operator", "private",
    "protected", "public", "reinterpret_cast", "static_cast", "template",
    "this", "throw", "true", "try", "typeid", "typename", "using", "virtual",
    "while", "alignas", "alignof", "constexpr", "decltype", "nullptr", "noexcept",
    "static_assert", "thread_local"
};
```

The table size is a prime number for open addressing. Lookup is O(1) average, with a small constant. The table is compiled into the binary — no runtime initialization.

## Search: literal, streaming, bounded

Literal search runs incrementally over the span reader, including matches crossing the gap. It uses a linear-time streaming matcher with bounded retained matches. The pattern state is prepared when the query changes.

Searches carry a revision and cancel/restart on mutation. Next/previous navigation does not require retaining every match — only the current match position is needed. Initial search is exact byte matching of the entered UTF-8 pattern, with optional ASCII-only case folding.

## What's next

Article 7 covers layout and views: how visible lines are determined, how the layout cache works, and how long lines are handled without destroying performance.

Incremental syntax highlighting is the last of the major CPU subsystems. Combined with the document storage, line index, and transaction layer, it completes the editor's data preparation pipeline.
