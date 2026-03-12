/****************************************************************************
 * baescript_vm.c — Tree-walking interpreter for BAEScript
 *
 * Evaluates the AST produced by the parser.  Channel property writes
 * call through to the BAE API immediately.
 ****************************************************************************/

#include "baescript_internal.h"

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
            for (int i = 0; i < node->data.print_call.count; i++) {
                BAEScript_Node *arg = node->data.print_call.args[i];
                if (arg && arg->type == NODE_STRING) {
                    fprintf(stderr, "%s", arg->data.str);
                } else {
                    fprintf(stderr, "%d", (int)BAEScript_Eval(ctx, arg));
                }
                if (i + 1 < node->data.print_call.count)
                    fprintf(stderr, " ");
            }
            fprintf(stderr, "\n");
            break;
        }

        case NODE_CH_PROP_SET: {
            int ch    = (int)BAEScript_Eval(ctx, node->data.ch_prop_set.channel);
            int32_t v = BAEScript_Eval(ctx, node->data.ch_prop_set.value);
            ch_prop_write(ctx, ch, node->data.ch_prop_set.prop, v);
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
