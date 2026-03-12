/****************************************************************************
 * baescript_vm.c — Tree-walking interpreter for BAEScript
 *
 * Evaluates the AST produced by the parser.  Channel property writes
 * call through to the BAE API immediately.
 ****************************************************************************/

#include "baescript_internal.h"

/* ── output callback ────────────────────────────────────────────────── */

void BAEScript_SetOutputCallback(BAEScript_Context *ctx,
                                 BAEScript_OutputFn fn,
                                 void *userdata)
{
    if (!ctx) return;
    ctx->output_fn = fn;
    ctx->output_ud = userdata;
}

static void ctx_output(BAEScript_Context *ctx, const char *text)
{
    if (ctx->output_fn)
        ctx->output_fn(text, ctx->output_ud);
    else
        fprintf(stderr, "%s", text);
}

/* ── stop callback ──────────────────────────────────────────────────── */

void BAEScript_SetStopCallback(BAEScript_Context *ctx,
                               BAEScript_StopFn fn,
                               void *userdata)
{
    if (!ctx) return;
    ctx->stop_fn = fn;
    ctx->stop_ud = userdata;
}

/* ── help text ──────────────────────────────────────────────────────── */

static const char *HELP_TEXT =
    "BAEScript Language Reference\n"
    "============================\n"
    "\n"
    "  Variables:\n"
    "    var x = 42;            Declare a variable\n"
    "    x = x + 1;             Assignment\n"
    "\n"
    "  Control Flow:\n"
    "    if (cond) { }          Conditional\n"
    "    if (cond) { } else { } Conditional with else\n"
    "    while (cond) { }       Loop (max 10000 iterations/tick)\n"
    "\n"
    "  Operators:\n"
    "    +  -  *  /  %          Arithmetic\n"
    "    ==  !=  <  >  <=  >=   Comparison\n"
    "    &&  ||  !              Logical\n"
    "\n"
    "  Channel Properties (ch[1..16]):\n"
    "    ch[N].instrument       Program number (0-127)\n"
    "    ch[N].volume           CC 7 (0-127)\n"
    "    ch[N].pan              CC 10 (0-127)\n"
    "    ch[N].expression       CC 11 (0-127)\n"
    "    ch[N].pitchbend        Pitch bend (0-16383, 8192=center)\n"
    "    ch[N].mute             Mute flag (0 or 1)\n"
    "    Read:  var v = ch[1].volume;\n"
    "    Write: ch[1].volume = 100;\n"
    "\n"
    "  MIDI Functions:\n"
    "    noteOn(ch, note, vel);   Send Note On\n"
    "    noteOff(ch, note, vel);  Send Note Off\n"
    "\n"
    "  Global Objects:\n"
    "    midi.timestamp         Current position in ms (read/write)\n"
    "    midi.position          Alias for midi.timestamp\n"
    "    midi.length            Song length in ms (read-only)\n"
    "    midi.exporting         1 if exporting to file, 0 otherwise\n"
    "    midi.stop()            Stop playback and export\n"
    "\n"
    "  Output:\n"
    "    print(expr, ...);      Print values to console\n"
    "    help();                Show this reference\n"
    "\n"
    "  Data Types:\n"
    "    Numbers: 42, 0xFF     Integer (decimal or hex)\n"
    "    Strings: \"hello\"       Double or single quoted\n"
    "    Booleans: true, false\n"
    "\n"
    "  Comments:\n"
    "    // line comment\n"
    "    /* block comment */\n"
    "\n"
    "  Note: Scripts execute once per tick (~15ms).\n"
    "  Use variables to track state across ticks.\n";

/* ── variable helpers ───────────────────────────────────────────────── */

static int32_t *var_lookup(BAEScript_Context *ctx, const char *name)
{
    for (int i = 0; i < ctx->var_count; i++) {
        if (strcmp(ctx->vars[i].name, name) == 0)
            return &ctx->vars[i].value;
    }
    return NULL;
}

static int32_t *var_create(BAEScript_Context *ctx, const char *name)
{
    int32_t *existing = var_lookup(ctx, name);
    if (existing) return existing;
    if (ctx->var_count >= BAESCRIPT_MAX_VARS) {
        fprintf(stderr, "BAEScript: too many variables (max %d)\n", BAESCRIPT_MAX_VARS);
        return NULL;
    }
    BAEScript_Var *v = &ctx->vars[ctx->var_count++];
    strncpy(v->name, name, sizeof(v->name) - 1);
    v->name[sizeof(v->name) - 1] = '\0';
    v->value = 0;
    return &v->value;
}

/* ── channel property read ──────────────────────────────────────────── */

static int32_t ch_prop_read(BAEScript_Context *ctx, int channel, BAEScript_ChProp prop)
{
    if (!ctx->song) return 0;
    /* BAE channels are 1-16 externally, 0-15 internally; the API expects 1-based */
    unsigned char ch = (unsigned char)(channel & 0xFF);

    switch (prop) {
        case CHPROP_INSTRUMENT: {
            unsigned char prog = 0, bank = 0;
            BAESong_GetProgramBank(ctx->song, ch, &prog, &bank, 0);
            return (int32_t)prog;
        }
        case CHPROP_VOLUME: {
            char val = 0;
            BAESong_GetControlValue(ctx->song, ch, 7 /* VOLUME_MSB */, &val);
            return (int32_t)(unsigned char)val;
        }
        case CHPROP_PAN: {
            char val = 0;
            BAESong_GetControlValue(ctx->song, ch, 10 /* PAN_MSB */, &val);
            return (int32_t)(unsigned char)val;
        }
        case CHPROP_EXPRESSION: {
            char val = 0;
            BAESong_GetControlValue(ctx->song, ch, 11 /* EXPRESSION_MSB */, &val);
            return (int32_t)(unsigned char)val;
        }
        case CHPROP_PITCHBEND: {
            unsigned char lsb = 0, msb = 0;
            BAESong_GetPitchBend(ctx->song, ch, &lsb, &msb);
            return (int32_t)((msb << 7) | lsb);
        }
        case CHPROP_MUTE: {
            BAE_BOOL muted[16];
            memset(muted, 0, sizeof(muted));
            BAESong_GetChannelMuteStatus(ctx->song, muted);
            if (ch >= 1 && ch <= 16) return muted[ch - 1] ? 1 : 0;
            return 0;
        }
    }
    return 0;
}

/* ── channel property write ─────────────────────────────────────────── */

static void ch_prop_write(BAEScript_Context *ctx, int channel, BAEScript_ChProp prop, int32_t value)
{
    if (!ctx->song) return;
    unsigned char ch = (unsigned char)(channel & 0xFF);

    switch (prop) {
        case CHPROP_INSTRUMENT:
            BAESong_LoadInstrument(ctx->song, (BAE_INSTRUMENT)(value & 0x7F));
            BAESong_ProgramChange(ctx->song, ch, (unsigned char)(value & 0x7F), 0);
            break;
        case CHPROP_VOLUME:
            BAESong_ControlChange(ctx->song, ch, 7, (unsigned char)(value & 0x7F), 0);
            break;
        case CHPROP_PAN:
            BAESong_ControlChange(ctx->song, ch, 10, (unsigned char)(value & 0x7F), 0);
            break;
        case CHPROP_EXPRESSION:
            BAESong_ControlChange(ctx->song, ch, 11, (unsigned char)(value & 0x7F), 0);
            break;
        case CHPROP_PITCHBEND: {
            unsigned char lsb = (unsigned char)(value & 0x7F);
            unsigned char msb = (unsigned char)((value >> 7) & 0x7F);
            BAESong_PitchBend(ctx->song, ch, lsb, msb, 0);
            break;
        }
        case CHPROP_MUTE:
            if (value)
                BAESong_MuteChannel(ctx->song, ch);
            else
                BAESong_UnmuteChannel(ctx->song, ch);
            break;
    }
}

/* ── expression evaluation ──────────────────────────────────────────── */

int32_t BAEScript_Eval(BAEScript_Context *ctx, BAEScript_Node *node)
{
    if (!node) return 0;

    switch (node->type) {
        case NODE_NUMBER:
            return node->data.num;

        case NODE_STRING:
            /* Strings evaluate to 0 in numeric context */
            return 0;

        case NODE_BOOL:
            return node->data.boolval;

        case NODE_IDENT: {
            int32_t *v = var_lookup(ctx, node->data.str);
            if (!v) {
                fprintf(stderr, "BAEScript [line %d]: undefined variable '%s'\n",
                        node->line, node->data.str);
                return 0;
            }
            return *v;
        }

        case NODE_BINOP: {
            /* Short-circuit for && and || */
            if (node->data.binop.op == TOK_AND) {
                int32_t left = BAEScript_Eval(ctx, node->data.binop.left);
                if (!left) return 0;
                return BAEScript_Eval(ctx, node->data.binop.right) ? 1 : 0;
            }
            if (node->data.binop.op == TOK_OR) {
                int32_t left = BAEScript_Eval(ctx, node->data.binop.left);
                if (left) return 1;
                return BAEScript_Eval(ctx, node->data.binop.right) ? 1 : 0;
            }

            int32_t left  = BAEScript_Eval(ctx, node->data.binop.left);
            int32_t right = BAEScript_Eval(ctx, node->data.binop.right);

            switch (node->data.binop.op) {
                case TOK_PLUS:    return left + right;
                case TOK_MINUS:   return left - right;
                case TOK_STAR:    return left * right;
                case TOK_SLASH:   return right != 0 ? left / right : 0;
                case TOK_PERCENT: return right != 0 ? left % right : 0;
                case TOK_EQ:      return left == right ? 1 : 0;
                case TOK_NEQ:     return left != right ? 1 : 0;
                case TOK_LT:      return left < right  ? 1 : 0;
                case TOK_GT:      return left > right  ? 1 : 0;
                case TOK_LTE:     return left <= right ? 1 : 0;
                case TOK_GTE:     return left >= right ? 1 : 0;
                default:          return 0;
            }
        }

        case NODE_UNARYOP:
            if (node->data.unaryop.op == TOK_NOT)
                return BAEScript_Eval(ctx, node->data.unaryop.operand) ? 0 : 1;
            if (node->data.unaryop.op == TOK_MINUS)
                return -BAEScript_Eval(ctx, node->data.unaryop.operand);
            return 0;

        case NODE_CH_PROP: {
            int ch = (int)BAEScript_Eval(ctx, node->data.ch_prop.channel);
            return ch_prop_read(ctx, ch, node->data.ch_prop.prop);
        }

        case NODE_MIDI_PROP:
            if (node->data.midi_prop == MIDIPROP_TIMESTAMP)
                return (int32_t)ctx->timestamp_ms;
            if (node->data.midi_prop == MIDIPROP_LENGTH)
                return (int32_t)ctx->length_ms;
            if (node->data.midi_prop == MIDIPROP_EXPORTING)
                return ctx->exporting ? 1 : 0;
            return 0;

        case NODE_NOTE_ON: {
            int ch   = (int)BAEScript_Eval(ctx, node->data.note_cmd.channel);
            int note = (int)BAEScript_Eval(ctx, node->data.note_cmd.note);
            int vel  = (int)BAEScript_Eval(ctx, node->data.note_cmd.velocity);
            if (ctx->song) {
                BAESong_NoteOn(ctx->song,
                               (unsigned char)ch,
                               (unsigned char)(note & 0x7F),
                               (unsigned char)(vel & 0x7F), 0);
            }
            return 0;
        }

        case NODE_NOTE_OFF: {
            int ch   = (int)BAEScript_Eval(ctx, node->data.note_cmd.channel);
            int note = (int)BAEScript_Eval(ctx, node->data.note_cmd.note);
            int vel  = (int)BAEScript_Eval(ctx, node->data.note_cmd.velocity);
            if (ctx->song) {
                BAESong_NoteOff(ctx->song,
                                (unsigned char)ch,
                                (unsigned char)(note & 0x7F),
                                (unsigned char)(vel & 0x7F), 0);
            }
            return 0;
        }

        default:
            return 0;
    }
}

/* ── statement execution ────────────────────────────────────────────── */

void BAEScript_Exec(BAEScript_Context *ctx, BAEScript_Node *node)
{
    if (!node) return;

    switch (node->type) {
        case NODE_BLOCK:
            for (int i = 0; i < node->data.block.count; i++)
                BAEScript_Exec(ctx, node->data.block.stmts[i]);
            break;

        case NODE_VAR_DECL: {
            int32_t *v = var_create(ctx, node->data.var_decl.name);
            if (v && node->data.var_decl.init)
                *v = BAEScript_Eval(ctx, node->data.var_decl.init);
            break;
        }

        case NODE_ASSIGN: {
            int32_t *v = var_lookup(ctx, node->data.assign.name);
            if (!v) {
                /* Auto-create if not found (lenient like JS) */
                v = var_create(ctx, node->data.assign.name);
            }
            if (v)
                *v = BAEScript_Eval(ctx, node->data.assign.value);
            break;
        }

        case NODE_IF:
            if (BAEScript_Eval(ctx, node->data.if_stmt.cond))
                BAEScript_Exec(ctx, node->data.if_stmt.then_b);
            else if (node->data.if_stmt.else_b)
                BAEScript_Exec(ctx, node->data.if_stmt.else_b);
            break;

        case NODE_WHILE: {
            /* Safety: cap iterations to prevent infinite loops during a single tick */
            int safety = 0;
            while (BAEScript_Eval(ctx, node->data.while_stmt.cond) && safety < 10000) {
                BAEScript_Exec(ctx, node->data.while_stmt.body);
                safety++;
            }
            if (safety >= 10000) {
                fprintf(stderr, "BAEScript [line %d]: while loop exceeded 10000 iterations, breaking\n",
                        node->line);
            }
            break;
        }

        case NODE_PRINT: {
            char line_buf[2048];
            int pos = 0;
            for (int i = 0; i < node->data.print_call.count; i++) {
                BAEScript_Node *arg = node->data.print_call.args[i];
                if (arg && arg->type == NODE_STRING) {
                    pos += snprintf(line_buf + pos, sizeof(line_buf) - pos, "%s", arg->data.str);
                } else {
                    pos += snprintf(line_buf + pos, sizeof(line_buf) - pos, "%d", (int)BAEScript_Eval(ctx, arg));
                }
                if (i + 1 < node->data.print_call.count && pos < (int)sizeof(line_buf) - 1)
                    line_buf[pos++] = ' ';
            }
            if (pos < (int)sizeof(line_buf) - 1)
                line_buf[pos++] = '\n';
            line_buf[pos] = '\0';
            ctx_output(ctx, line_buf);
            break;
        }

        case NODE_MIDI_STOP:
            if (ctx->stop_fn)
                ctx->stop_fn(ctx->stop_ud);
            else if (ctx->song)
                BAESong_Stop(ctx->song, FALSE);
            break;

        case NODE_HELP: {
            if (!ctx->help_shown) {
                ctx_output(ctx, HELP_TEXT);
                ctx->help_shown = 1;
            }
            break;
        }

        case NODE_CH_PROP_SET: {
            int ch    = (int)BAEScript_Eval(ctx, node->data.ch_prop_set.channel);
            int32_t v = BAEScript_Eval(ctx, node->data.ch_prop_set.value);
            ch_prop_write(ctx, ch, node->data.ch_prop_set.prop, v);
            break;
        }

        case NODE_MIDI_PROP_SET: {
            int32_t v = BAEScript_Eval(ctx, node->data.midi_prop_set.value);
            if (node->data.midi_prop_set.prop == MIDIPROP_TIMESTAMP) {
                if (ctx->song) {
                    uint32_t us = (uint32_t)v * 1000u;
                    BAESong_SetMicrosecondPosition(ctx->song, us);
                }
                ctx->timestamp_ms = (uint32_t)v;
            }
            /* MIDIPROP_LENGTH, MIDIPROP_EXPORTING are read-only — silently ignore writes */
            break;
        }

        case NODE_EXPR_STMT:
            BAEScript_Eval(ctx, node->data.expr);
            break;

        default:
            /* Expression nodes used as statements — just evaluate */
            BAEScript_Eval(ctx, node);
            break;
    }
}
