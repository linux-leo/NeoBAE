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
 *   midi.timestamp                    // current position in ms
 *   midi.length                       // total song length in ms
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
