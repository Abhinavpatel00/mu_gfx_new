#include "external/mu/mu/mu_common.h"
#include "renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "external/mu/mu/mu_array.h"
typedef uint32_t DocOffset;
typedef u64      DocStateId;

typedef struct {
    DocOffset begin;
    DocOffset end;
} DocRange;

typedef struct {
    uint32_t line;
    uint32_t column;
} DocLocation;

/* One cursor endpoint. */
typedef struct {
    DocOffset offset;
    u8        affinity; // 0 = left, 1 = right
} TextPos;

/* Selection is two byte positions. */
typedef struct {
    TextPos anchor;
    TextPos cursor;
} TextSelection;
/* Temporary visual state for vertical/horizontal movement. */
typedef struct {
    TextSelection selection;
    float         goal_column_px;
} TextCursor;
/*  text storage */
typedef struct {
    u8 *bytes;
    u32 gap_start;
    u32 gap_end;
    u32 capacity;
} GapBuffer;

/* Cached line -> byte offset mapping. */
typedef struct {
    u32 *line_start;

    u32 line_count;

    u32 capacity;

    u32 dirty_from;
} LineIndex;
/* One document mutation. */
typedef struct {
    DocOffset begin;
    DocOffset end;
    u32       inserted_size;
    u32       deleted_size;
} DocEdit;

/* Where history bytes live. */
typedef struct {
    u32 offset;
    u32 size;
} HistoryPayload;

/* Undo/redo record. */
typedef struct {
    DocEdit        edit;
    HistoryPayload deleted;
    HistoryPayload inserted;

    TextSelection before;
    TextSelection after;

    DocStateId state_before;
    DocStateId state_after;
} HistoryRecord;

/* UTF-8 decoder result. */
typedef struct {
    u32  codepoint;
    u32  size;
    bool valid;
} Utf8Decode;

/* The document owns storage + line metadata. */
typedef struct {
    GapBuffer gap;
    LineIndex lines;

    u64 state_id; // changes after every edit
    u64 revision; // monotonic document revision
} Document;

Utf8Decode utf8_decode(const uint8_t *p, uint32_t remaining);

typedef struct {
    const u8 *data;
    u32       size;
} Span;
typedef struct Arena Arena;
bool doc_open(Document *doc, Span bytes);
void doc_close(Document *doc);

u32 doc_size(const Document *doc);
u32 doc_line_count(const Document *doc);
u32 doc_line_length(const Document *doc, u32 line);

DocOffset   doc_line_start(const Document *doc, u32 line);
DocLocation doc_location(const Document *doc, DocOffset offset);

Span doc_line(const Document *doc, u32 line, Arena *scratch);

DocOffset doc_next_char(const Document *doc, DocOffset offset);
DocOffset doc_prev_char(const Document *doc, DocOffset offset);

void doc_insert(Document *doc, DocOffset at, Span text);
void doc_delete(Document *doc, DocRange range);
void doc_replace(Document *doc, DocRange range, Span text);

Utf8Decode utf8_decode(const u8 *p, u32 remaining);





// align as sixteen
typedef struct RenderInstance {
    float    x;
    float    y;
    float    w;
    float    h;

    uint16_t id;
    uint16_t color;
    uint16_t type;
    uint16_t flags;
} RenderInstance;
typedef struct  TextRoot {
   // float    viewport[4][4];// i guess global buffer already sends this 

    u32 atlas_id;
    u32 palette_id;

    u64 instance_addr;

    u32 instance_count;
    u32 _pad;
//     uint64_t quad_addr; // derived in shader
} TextRoot;



































int main(int argc, char **argv) {
    bool use_wayland = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "wayland") == 0 || strcmp(argv[i], "--wayland") == 0)
            use_wayland = true;
        else if (strcmp(argv[i], "x11") == 0 || strcmp(argv[i], "--x11") == 0)
            use_wayland = false;
        else {
            fprintf(stderr, "Usage: %s [x11|wayland]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }
    Renderer *renderer = renderer_create(use_wayland);
    if (!renderer)
        return EXIT_FAILURE;

    while (renderer_frame(renderer)) {

    }
    renderer_destroy(renderer);
    return EXIT_SUCCESS;
}
