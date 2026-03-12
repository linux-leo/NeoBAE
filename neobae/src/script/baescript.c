/****************************************************************************
 * baescript.c — Public API for BAEScript
 *
 * Provides LoadFile / LoadString / SetSong / Tick / Free.
 ****************************************************************************/

#include "baescript_internal.h"

/* ── Load from file ────────────────────────────────────────────────── */

BAEScript_Context *BAEScript_LoadFile(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "BAEScript: cannot open '%s'\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0 || len > 1024 * 1024) {     /* 1 MB cap */
        fprintf(stderr, "BAEScript: file too large or empty '%s'\n", path);
        fclose(f);
        return NULL;
    }

    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t read_bytes = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[read_bytes] = '\0';

    BAEScript_Context *ctx = BAEScript_LoadString(buf);
    free(buf);
    return ctx;
}

/* ── Load from string ──────────────────────────────────────────────── */

BAEScript_Context *BAEScript_LoadString(const char *source)
{
    BAEScript_Node *program = BAEScript_Parse(source);
    if (!program) return NULL;

    BAEScript_Context *ctx = (BAEScript_Context *)calloc(1, sizeof(BAEScript_Context));
    if (!ctx) {
        BAEScript_FreeNode(program);
        return NULL;
    }
    ctx->program = program;
    return ctx;
}

/* ── Bind to a song ────────────────────────────────────────────────── */

void BAEScript_SetSong(BAEScript_Context *ctx, BAESong song)
{
    if (ctx) ctx->song = song;
}

/* ── Execute one tick ──────────────────────────────────────────────── */

void BAEScript_Tick(BAEScript_Context *ctx,
                    uint32_t timestamp_ms,
                    uint32_t length_ms)
{
    if (!ctx || !ctx->program) return;
    ctx->timestamp_ms = timestamp_ms;
    ctx->length_ms    = length_ms;
    BAEScript_Exec(ctx, ctx->program);
}

/* ── Cleanup ───────────────────────────────────────────────────────── */

void BAEScript_Free(BAEScript_Context *ctx)
{
    if (!ctx) return;
    BAEScript_FreeNode(ctx->program);
    free(ctx);
}
