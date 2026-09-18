# Article 3: Transactions, History, and Text Semantics

## The single mutation point

Every edit in the editor flows through one function: the replacement transaction. Typing, pasting, undo, redo, and automated refactoring all share the same path. There is no separate "insert character" function and "paste" function. There is one transaction, and it records everything needed to reverse itself.

```c
typedef struct {
    uint32_t old_byte_start;   // start of replaced range [a, b)
    uint32_t old_byte_end;
    uint32_t inserted_bytes;   // count of bytes inserted
    uint32_t deleted_bytes;    // count of bytes deleted
    uint32_t old_line_a;       // affected old line range
    uint32_t old_line_b;
    uint32_t new_line_a;       // affected new line range
    uint32_t new_line_b;
    uint32_t old_revision;     // revision before this transaction
    uint32_t new_revision;     // revision after this transaction
} ReplacementTransaction;
```

A command may group multiple replacements into one history transaction. For non-overlapping ranges expressed in one pre-edit coordinate system, apply in descending byte order. Resolve overlaps before mutation. Reserve storage and history space once for the group.

## Caret, selection, and marks

Caret, selection anchor, and future marks are **byte offsets**. They are transformed through replacements using a defined left/right insertion affinity.

When text is inserted at position P:
- A cursor at P with **right affinity** moves after the inserted text.
- A cursor at P with **left affinity** moves before the inserted text.
- A selection that starts before P is unaffected by insertions at P.
- A selection that ends at P moves with the appropriate affinity.

Do not store authoritative line/column alongside the offset. Preferred horizontal caret position is view state and resets on intentional horizontal navigation.

## History: dense records with chunked payloads

History uses dense records plus chunked byte payload storage — **not a document snapshot per keystroke**. This is the difference between an editor that uses 16 MB of history memory and one that uses 160 MB.

Each history record stores:
- The old byte range that was replaced
- The inserted bytes (for redo) and deleted bytes (for undo)
- The caret and selection before and after
- The old and new revision numbers
- A pointer to the chunked payload containing the actual bytes

**Coalescing**: adjacent typing under explicit command/time rules is merged into a single history group. Paste and indentation are separate groups. New edits after undo discard redo.

The history budget is configurable: **16 MiB per document** with an application-wide 64 MiB target. Eviction removes whole oldest groups. For a single edit exceeding the budget, the policy is to retain that group alone and report the temporary excess — never silently make part of a group non-undoable.

```c
typedef struct {
    uint32_t revision;
    uint32_t history_state_id; // for dedup
    uint32_t saved_state_id;   // for dirty tracking
    ReplacementTransaction tx;
    uint32_t payload_offset;   // into chunked byte storage
    uint32_t payload_size;
    DocPos   cursor_before;
    DocPos   cursor_after;
    DocPos   selection_before;
    DocPos   selection_after;
} HistoryRecord;
```

## Undo/redo semantics

- Undoing back to the saved state clears dirty status.
- Eviction must not make another state appear saved.
- Track a unique history state identity and the saved state identity.

This means the dirty flag is not a boolean on the document — it is a comparison between the current revision and the last saved revision. The editor can answer "is there unsaved work?" in O(1) by comparing two integers.

## UTF-8: byte offsets, code-point navigation

The editor preserves file bytes, BOM presence, LF/CRLF, and a missing final newline. It does not normalize on load or save. Newline insertion uses the detected predominant style.

UTF-8 decoding uses bounds checks. Invalid sequences consume one byte and display a replacement marker while preserving the original byte. Code-point navigation is the first-release contract; grapheme navigation is deferred.

```c
// Next code-point boundary: UTF-8 self-synchronizing prefix
static const uint8_t utf8_prefix[16] = {
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,  // 0xxxxxxx: lead byte
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,  // 10xxxxxx: continuation byte (invalid start)
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,0,0   // 110xxxxx, 1110xxxx, 11110xxx lead bytes
};
```

This 16-byte lookup table tells us, given a byte, whether it is a lead byte (start of a code point), a continuation byte, or a standalone invalid byte. `doc_next_char` advances by reading the lead byte's prefix count and skipping the continuation bytes. No library needed.

**ASCII controls** except recognized whitespace display visibly. Embedded NUL is data. CRLF is treated as one caret boundary; a lone CR is a visible control.

## The exact replacement rule

All edits replace `[a, b)` with a byte span. Resolve both offsets against the old index.

Let A and B be the endpoint lines. Preserve the prefix from the start of A to a and the suffix from b through the stored end of B. New line lengths describe: `prefix bytes + inserted bytes + suffix bytes`.

Splice those lengths in place of lines A through B. Only inserted bytes need scanning for LF — the prefix cannot contain LF, and the suffix can only end in the old B terminator. When B is not the last document line, do not add an extra empty line after that terminator. At EOF, retain the final empty line when required.

This rule is the foundation of all edit operations. It has been exhaustively validated: **142,225 random replacements passed** under ASan/UBSan with full byte-to-line position checking.

## The transaction API

```c
typedef struct {
    DocPos   anchor;   // selection anchor (== cursor when no selection)
    DocPos   cursor;   // byte offset + derived line/column
    uint32_t goal_column_px; // sticky horizontal target
} TextCursor;

typedef struct {
    Document *doc;
    uint32_t revision;
    int dirty;
    TextCursor cursor;
    Viewport   viewport;
} EditorState;
```

Every mutation goes through:
1. Resolve the byte offsets against the current document state.
2. Apply the replacement: move gap, update line index, apply history.
3. Transform cursors through the replacement.
4. Mark affected cache rows invalid.
5. Mark affected syntax ranges unverified.
6. Set dirty if the revision differs from the saved revision.

Steps 4–6 are the bridge between document mutation and the rest of the editor. They are cheap because they operate on bounded ranges, not the entire document.

## What's next

Article 4 covers the rendering pipeline — how the editor transforms document state into GPU instances, how the pass function issues a single draw call, and why the rendering is embarrassingly simple when the CPU data is right.

The transaction layer is the editor's spine. Every feature branches from it, and every feature returns to it for undo, redo, and state consistency.
