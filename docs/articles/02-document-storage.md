# Article 2: Document Storage — Gap Buffers and Line Indexes

## The core problem

A code editor must store arbitrary byte sequences and support two operations efficiently:

1. **Read**: retrieve the bytes of any line, quickly.
2. **Mutate**: insert or delete bytes at any position, quickly.

Every other editor feature — cursor movement, selection, scrolling, syntax highlighting, search — depends on these two operations. The document is the heart of the editor, and its data structure determines the editor's worst-case latency.

## Why not a rope, a piece table, or a piece tree

| Structure | Cursor move | Local insert/delete | Read a line | Memory | Complexity |
|---|---|---|---|---|---|
| **Gap buffer** | O(n) memmove worst case, O(1) near cursor | O(1) at cursor | O(1) via line index | 1 byte/char + gap | Trivial |
| **Piece table** | O(log n) | O(1) append | O(pieces) pointer chase | Low | Moderate |
| **Rope** | O(log n) | O(log n) | O(log n) | High (nodes, balance) | High |
| **Piece tree** | O(log n) | O(1) append | O(log n) | Low | High |

The piece table is attractive for its undo semantics — original text is immutable, edits append — but every read is a pointer chase through pieces. The rope has logarithmic bounds but pays for tree balancing and pointer chasing on every operation. The piece tree is the theoretically optimal choice but is the furthest from minimal implementation.

**The gap buffer wins for a single-cursor editor.** The reasoning is not about asymptotics — it's about data locality at editor scale.

A 10 MB source file fits entirely in L3 cache. Moving the gap 1 MB is approximately 100 µs on modern DDR5 (5–20 GB/s memcpy bandwidth). Every alternative pays more *total bytes moved* for the same operations at this scale, because pointer chasing through trees or piece lists causes cache misses that cost far more than a linear memcpy.

## The gap buffer structure

A document owns one growable byte allocation and two gap boundaries:

```
[ before-gap | GAP | after-gap ]
          ^gap_start        ^gap_end
```

Logical reads map to at most two physical spans. The gap position is the cursor position. Insertion fills the gap from the left. Deletion expands the gap over the deleted bytes.

The critical invariant: **navigation does not move the gap**. Moving the cursor left 100 characters does not shift bytes. The gap stays where it is until an edit occurs. When an edit happens, the gap moves to the edit location, expands over deleted bytes, then consumes itself with inserted bytes.

```c
typedef struct {
    uint8_t *bytes;      // contiguous growable buffer
    uint32_t  gap_start; // first byte of the gap
    uint32_t  gap_end;   // first byte after the gap
    uint32_t  capacity;  // allocated capacity
} GapBuffer;
```

Reading line L's bytes produces at most two spans: `[line_start, gap_start)` and `[gap_end, line_end)` if the gap falls inside the line. In the common case (gap is elsewhere), it's one contiguous span.

`doc_line_span` for a gap buffer is contiguous **unless the gap is inside the line**; in that case it copies into a per-frame scratch arena. The fast path is zero copy. Callers must not hold the span across edits.

## The line index: a flat prefix-offset array

The line index answers one query: "what is the byte offset of line i's first byte?"

A naive approach stores a global absolute byte offset for every line. This means typing at the start of the document rewrites offsets for the entire remaining document — an O(n) operation that dominates editing latency in large files.

Instead, we store unsigned byte lengths, including LF terminators, in leaf blocks:

```c
typedef struct {
    uint32_t *line_start; // line_start[i] = byte offset of line i's first byte
    uint32_t  line_count;
    uint32_t  capacity;
    uint32_t  dirty_from; // first line whose offsets are unverified
} LineIndex;
```

**Build**: one pass over the buffer (`memchr('\n')`-style scan), O(n) once at open.

**Incremental update on insert/delete of n bytes at line L**:

1. The edit changes only line L's length → recompute nothing structurally unless the edited bytes contain `\n`.
2. If newlines were added/removed: rescan only the edited region, splice the line-start array (one `memmove` of the tail).
3. If not: shift all offsets after the edit point by ±n — one `memmove` over `(line_count - L) * 4` bytes. For a 5,000-line file, that's ≤ 20 KB moved per edit; approximately 1–2 µs.

This is the key insight: **ordinary character edits change one line length and O(1) offset shifts. No global tail-offset rewrite occurs.** The index update is dominated by the `memmove` of the tail, which is cache-friendly and fast.

For large pastes of thousands of lines, the splice becomes expensive. The solution is a **rebuild threshold**: if more than a measured number of lines are affected, do a full O(n) rebuild via `memchr` scan. Rebuild is simpler and faster above the threshold because it avoids pathological splice overhead.

## Byte offsets are the only canonical positions

All positions are byte offsets into the buffer. Line and column are *derived* from the line index, never stored as primary state.

```c
typedef struct {
    uint32_t byte_offset;
    uint32_t line;     // derived
    uint32_t column;   // derived
} DocPos;
```

This design decision has profound consequences:

- **Storage is swappable.** The document API only needs to answer "give me bytes [a,b)" and "insert/delete at byte X." The storage implementation can be replaced without changing any cursor, selection, or view code.
- **One source of truth.** A byte offset is the single representation every candidate storage serves directly. There are no inconsistencies between line/column and the underlying buffer.
- **Cursor drift is impossible.** Storing line/column as primary state creates three sources of truth that must stay consistent across every edit. Byte offsets eliminate this class of bugs entirely.

Line/column cost O(log n) to derive from the line index and are free in hot paths.

## The replacement rule

All edits replace the half-open byte range `[a, b)` with a byte span. Resolve both offsets against the **old** index. At a line boundary, choose the following line. EOF belongs to the last line, including the final empty line when present.

The replacement rule handles the non-trivial cases:

- **Same-line edit**: one length adjustment. Prefix bytes + inserted bytes + suffix bytes.
- **Newline insertion**: splices affected leaf ranges. Split full leaves. Merge under-filled neighbors.
- **Multi-line paste**: one rebuild if the affected range exceeds the threshold.
- **CRLF**: remains two stored bytes; display and caret logic treat it as one boundary.

This rule has been validated exhaustively: 142,225 random replacements passed under AddressSanitizer and UndefinedBehaviorSanitizer, with every byte-to-line position checked.

## Capacity and growth

The gap buffer grows by amortized doubling — `mu_array`-style realloc. But the doctrine warns: **choose spare capacity from measured workloads. Do not automatically double a large allocation without measuring peak memory.**

A 100 MB file's gap buffer is 100 MB of committed capacity. Doubling it to 200 MB for a 1 MB insertion is wasteful. Reserve once for a batch before applying it. Growth is an explicit slow path, not a hidden cost of every edit.

## The API contract

```c
typedef struct Document Document; // opaque; storage kind is internal

bool     doc_open(Document *doc, Span bytes);
bool     doc_open_file(Document *doc, const char *path);
void     doc_close(Document *doc);

uint32_t doc_line_count(const Document *doc);
uint32_t doc_line_byte_length(const Document *doc, uint32_t line);
Span     doc_line_span(const Document *doc, uint32_t line, Arena scratch);

void     doc_insert(Document *doc, DocPos pos, Span bytes);
void     doc_delete(Document *doc, DocPos a, DocPos b);

DocPos   doc_next_char(const Document *doc, DocPos pos);
DocPos   doc_prev_char(const Document *doc, DocPos pos);
DocPos   doc_pos_from_byte(const Document *doc, uint32_t byte_offset);
```

No allocation functions are exposed. The storage owns its memory. Growth happens inside `doc_insert`. Callers receive borrowed spans that expire on the next mutation.

## What's next

Article 3 covers how all edits flow through a single transaction layer, how undo/redo is built into the foundation rather than bolted on later, and how text semantics (UTF-8 decoding, newline handling, cursor affinity) are defined.

The document is the editor. Get its storage right, and everything else follows predictably.
