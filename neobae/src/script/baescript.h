/****************************************************************************
 * baescript.h — BAEScript: a lightweight JavaScript-like scripting engine
 *              for real-time MIDI manipulation in NeoBAE.
 *
 * The script runs once per playback tick (~15 ms).  Channel properties
 * (instrument, volume, pan, expression, pitch bend, mute) are readable
 * and writable; writes immediately issue the corresponding BAE API call.
 *
 * Language overview
 * ─────────────────
 *   var x = 42;                       // variable declarations
 *   x = x + 1;                        // assignment & arithmetic
 *   if (condition) { ... }            // if / else if / else
 *   while (condition) { ... }         // while loops (use carefully!)
 *   ch[1].instrument                  // read channel 1 instrument
 *   ch[1].instrument = 100;           // set channel 1 instrument
 *   ch[1].volume                      // channel volume  (CC 7,  0-127)
 *   ch[1].pan                         // channel pan     (CC 10, 0-127)
 *   ch[1].expression                  // expression      (CC 11, 0-127)
 *   ch[1].pitchbend                   // pitch bend      (0-16383, 8192=center)
 *   ch[1].mute                        // 0 or 1
 *   midi.timestamp                    // current position in ms (read/write)
 *   midi.position                     // alias for midi.timestamp
 *   midi.length                       // total song length in ms (read-only)
 *   midi.exporting                    // 1 if exporting to file, 0 otherwise
 *   midi.stop();                      // stop playback and export
 *   print("hello");                   // debug output
 *   print(expression);                // print numeric or string values
 *
 * Operators: + - * / % == != < > <= >= && || !
 * Parens, braces, semicolons work as in JavaScript.
 * Comments: // line  and  /⁎ block ⁎/
 *
 ****************************************************************************/

#ifndef BAESCRIPT_H
#define BAESCRIPT_H

#include <NeoBAE.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle returned by BAEScript_Load*() */
typedef struct BAEScript_Context BAEScript_Context;

/**
 * Callback for print() output. If set, print() sends text here
 * instead of stderr.
 */
typedef void (*BAEScript_OutputFn)(const char *text, void *userdata);

/**
 * Callback for midi.stop(). If set, midi.stop() calls this;
 * otherwise it falls back to BAESong_Stop on the bound song.
 */
typedef void (*BAEScript_StopFn)(void *userdata);

/**
 * Set a callback to receive print() output.
 */
void BAEScript_SetOutputCallback(BAEScript_Context *ctx,
                                 BAEScript_OutputFn fn,
                                 void *userdata);

/**
 * Set a callback for midi.stop().
 */
void BAEScript_SetStopCallback(BAEScript_Context *ctx,
                               BAEScript_StopFn fn,
                               void *userdata);

/**
 * Load a script from a file path.
 * Returns NULL on error (parse failure printed to stderr).
 */
BAEScript_Context *BAEScript_LoadFile(const char *path);

/**
 * Load a script from an in-memory NUL-terminated string.
 * Returns NULL on error.
 */
BAEScript_Context *BAEScript_LoadString(const char *source);

/**
 * Bind the script to a live BAESong so that channel read/write
 * operations go through the engine.  Must be called before Tick().
 */
void BAEScript_SetSong(BAEScript_Context *ctx, BAESong song);

/**
 * Set the exporting flag so scripts can query midi.exporting.
 */
void BAEScript_SetExporting(BAEScript_Context *ctx, int exporting);

/**
 * Execute one tick of the script.  Call this once per playback
 * loop iteration (~15 ms).  `timestamp_ms` is the current playback
 * position in milliseconds; `length_ms` is the total song length.
 */
void BAEScript_Tick(BAEScript_Context *ctx,
                    uint32_t timestamp_ms,
                    uint32_t length_ms);

/**
 * Free all resources associated with a script context.
 */
void BAEScript_Free(BAEScript_Context *ctx);

#ifdef __cplusplus
}
#endif

#endif /* BAESCRIPT_H */
