/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Experimental persistent frontend IR: fresh native emission on every hit. */
#include "qemu/osdep.h"
#include "translate.h"
#include "ir-cache.h"
#include "tcg/ir-cache-store.h"
#include "tcg/ir-cache-lifecycle.h"
#include "exec/target_page.h"
#include "exec/memopidx.h"

#if defined(__linux__) && TCG_TARGET_REG_BITS == 64 && \
    !defined(CONFIG_USER_ONLY) && !defined(CONFIG_PLUGIN)
#include <dlfcn.h>
#include <sys/sysmacros.h>

#define IR_VERSION 2
#define IR_MAX_OPS 2048
#define IR_MAX_LABELS 256
#define IR_MAX_ARGS 8

typedef struct IRTemp {
    uint64_t kind, base_type, type, subindex, value;
} IRTemp;

typedef struct IROp {
    uint64_t opc, p1, p2, nargs;
    uint64_t args[IR_MAX_ARGS];
} IROp;

typedef struct IRBlock {
    unsigned globals, temps, labels, ops, size, icount, pc_relative;
    IRTemp temp[TCG_MAX_TEMPS];
    IROp op[IR_MAX_OPS];
} IRBlock;

static GMutex cache_lock;
static gsize cache_initialized;
static TCGIRCacheStore *store;
static uint8_t engine_hash[32];
static uint64_t hits, misses, rejected, stored;
static bool supported_op(unsigned opc);

/* Only explicit aggregate-v1 enables clocks/counters. Fixed storage and the
 * existing power-of-two/final report sites bound output; no per-TB logging.
 * Attempts are translations, not dynamic instructions or guest CPU usage.
 */
#define DIAG_METRICS(X) \
    X(arm_attempts) X(a32_attempts) X(thumb_attempts) X(a64_attempts) \
    X(key_eligible) X(key_rejected) X(save_preflight_skips) \
    X(save_key_changed) X(capture_rejected) X(validation_rejected) \
    X(store_failed) X(key_us) X(lookup_us) X(load_decode_us) \
    X(replay_us) X(capture_us) X(encode_us) X(save_validate_us) X(put_us)
#define DIAG_ENUM(name) D_##name,
enum { DIAG_METRICS(DIAG_ENUM) D_COUNT };
#undef DIAG_ENUM
#define DIAG_NAME(name) #name,
static const char *const diagnostic_names[] = { DIAG_METRICS(DIAG_NAME) };
#undef DIAG_NAME
static bool diagnostics_enabled;
static GMutex diagnostics_lock;
static uint64_t diagnostics[D_COUNT];

static void diagnostic_add(unsigned metric, uint64_t amount)
{
    if (diagnostics_enabled) {
        g_mutex_lock(&diagnostics_lock);
        diagnostics[metric] += amount;
        g_mutex_unlock(&diagnostics_lock);
    }
}

static int64_t diagnostic_begin(void)
{
    return diagnostics_enabled ? g_get_monotonic_time() : 0;
}

static void diagnostic_end(unsigned metric, int64_t begin)
{
    if (diagnostics_enabled) {
        diagnostic_add(metric, g_get_monotonic_time() - begin);
    }
}

static void report_diagnostics(void)
{
    TCGIRCacheStoreDiagnostics s;
    uint64_t snapshot[D_COUNT];
    if (!diagnostics_enabled) {
        return;
    }
    /* Caller holds cache_lock at milestone sites; never hold diagnostics_lock
     * while acquiring cache_lock. Exit is after the worker's vCPU has stopped.
     */
    g_mutex_lock(&diagnostics_lock);
    memcpy(snapshot, diagnostics, sizeof(snapshot));
    g_mutex_unlock(&diagnostics_lock);
    tcg_ir_store_get_diagnostics(store, &s);
    fprintf(stderr, "LightTouch TCG IR cache diagnostics: version=1");
    for (unsigned i = 0; i < D_COUNT; i++) {
        fprintf(stderr, " %s=%" PRIu64, diagnostic_names[i], snapshot[i]);
    }
    fprintf(stderr, " records=%" PRIu64 " bytes=%" PRIu64
            " elapsed_us=%" PRIu64 " capacity_observed=%u"
            " first_capacity_us=%" PRIu64 " put_calls=%" PRIu64
            " put_success=%" PRIu64 " file_fsync_calls=%" PRIu64
            " file_fsync_errors=%" PRIu64 " file_fsync_us=%" PRIu64
            " directory_fsync_calls=%" PRIu64
            " directory_fsync_errors=%" PRIu64
            " directory_fsync_us=%" PRIu64 "; aggregate wall time, not guest CPU\n",
            s.records, s.bytes, s.elapsed_us, s.capacity_observed,
            s.first_capacity_us, s.put_calls, s.put_success,
            s.file_fsync_calls, s.file_fsync_errors, s.file_fsync_us,
            s.directory_fsync_calls, s.directory_fsync_errors,
            s.directory_fsync_us);
}

static bool mapped_image_matches(const struct stat *st)
{
    FILE *maps = fopen("/proc/self/maps", "re");
    char line[4096], perms[5];
    unsigned long start, end;
    unsigned long long offset, inode;
    unsigned dev_major, dev_minor, count = 0;
    uintptr_t address = (uintptr_t)arm_tcg_ir_cache_load;
    bool match = false;
    if (!maps) { return false; }
    while (++count < 100000 && fgets(line, sizeof(line), maps)) {
        if (sscanf(line, "%lx-%lx %4s %llx %x:%x %llu", &start, &end,
                   perms, &offset, &dev_major, &dev_minor, &inode) == 7 &&
            address >= start && address < end) {
            match = perms[2] == 'x' && st->st_ino == inode &&
                    st->st_dev == makedev(dev_major, dev_minor);
            break;
        }
    }
    fclose(maps);
    return match;
}

static void report_cache(void)
{
    fprintf(stderr, "LightTouch TCG IR cache: hits=%" PRIu64
            " misses=%" PRIu64 " rejected=%" PRIu64 " stored=%" PRIu64
            "; frontend IR only, fresh native code\n",
            hits, misses, rejected, stored);
    report_diagnostics();
}

static void finalize_cache(const char *reason)
{
    /* The embedded caller has completed qemu_cleanup(), so key generation and
     * vCPU translation have stopped too. cache_lock serializes close/report
     * with milestones and the atexit fallback. Preserve the existing lock
     * order: cache_lock, then diagnostics_lock; never acquire them reversed.
     */
    g_mutex_lock(&cache_lock);
    if (store) {
        report_cache();
        fprintf(stderr, "LightTouch TCG IR cache terminal: version=1 reason=%s; "
                "cache totals only, not guest shutdown confirmation\n", reason);
        /* Android kills even clean terminal workers, without stdio cleanup. */
        fflush(stderr);
        tcg_ir_store_close(store);
        store = NULL;
    }
    g_mutex_unlock(&cache_lock);
}

void arm_tcg_ir_cache_finish(void)
{
    finalize_cache("post-qemu-cleanup");
}

static void finish_cache(void)
{
    /* Retain reporting for ordinary process exit, including failures that did
     * not return through embedded cleanup. This reason is deliberately distinct
     * from a post-cleanup report and cannot establish a clean guest halt.
     */
    finalize_cache("process-exit");
}

static void initialize_cache(void)
{
    const char *directory = getenv("LT_TCG_IR_CACHE");
    const char *enable = getenv("LT_TCG_IR_CACHE_ENABLE");
    Dl_info image;
    g_autofree char *backing_file = NULL;
    const char *member;
    struct stat st, after;
    g_autoptr(GChecksum) checksum = NULL;
    uint8_t buffer[65536];
    size_t digest_len = 32;
    ssize_t n;
    int fd;

    /* Explicit experimental opt-in. Merely selecting a state path is not it. */
    if (!directory || !enable || strcmp(enable, "experimental-v1") ||
        !dladdr(arm_tcg_ir_cache_load, &image) || !image.dli_fname) {
        return;
    }
    /* Android commonly maps uncompressed native libraries directly from its
     * APK. Hashing that exact mapped backing APK plus member name also binds
     * every engine byte, without extracting/copying it or trusting a supplied
     * build string. It conservatively invalidates on any APK content change. */
    member = strstr(image.dli_fname, ".apk!/");
    if (member) {
        backing_file = g_strndup(image.dli_fname,
                                member - image.dli_fname + strlen(".apk"));
        member += strlen(".apk!");
    } else {
        backing_file = g_strdup(image.dli_fname);
    }
    fd = open(backing_file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || !mapped_image_matches(&st) ||
        st.st_size <= 0 ||
        st.st_size > 512 * 1024 * 1024) {
        close(fd);
        return;
    }
    checksum = g_checksum_new(G_CHECKSUM_SHA256);
    if (member) {
        g_checksum_update(checksum, (const uint8_t *)member, strlen(member) + 1);
    }
    while ((n = read(fd, buffer, sizeof(buffer))) != 0) {
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return;
        }
        g_checksum_update(checksum, buffer, n);
    }
    if (fstat(fd, &after) || after.st_size != st.st_size ||
        after.st_mtim.tv_sec != st.st_mtim.tv_sec ||
        after.st_mtim.tv_nsec != st.st_mtim.tv_nsec ||
        after.st_ctim.tv_sec != st.st_ctim.tv_sec ||
        after.st_ctim.tv_nsec != st.st_ctim.tv_nsec) {
        close(fd);
        return;
    }
    close(fd);
    g_checksum_get_digest(checksum, engine_hash, &digest_len);
    store = tcg_ir_store_open(directory, engine_hash);
    if (store) {
        diagnostics_enabled = !g_strcmp0(getenv("LT_TCG_IR_CACHE_DIAGNOSTICS"),
                                        "aggregate-v1");
        if (diagnostics_enabled) {
            tcg_ir_store_enable_diagnostics(store);
        }
        fprintf(stderr, "LightTouch TCG IR cache: experimental enabled, "
                "mapped-image-sha256=%s; 32MiB/4096 records maximum\n",
                g_checksum_get_string(checksum));
        atexit(finish_cache);
    }
}

static void hash_u64(GChecksum *checksum, uint64_t value)
{
    value = GUINT64_TO_LE(value);
    g_checksum_update(checksum, (void *)&value, sizeof(value));
}

static bool make_key(CPUState *cs, TranslationBlock *tb, int max_insns,
                     vaddr pc, void *host_pc, uint8_t key[32])
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    CPUARMTBFlags flags = arm_tbflags_from_tb(tb);
    TCGContext *s = tcg_ctx;
    g_autoptr(GChecksum) checksum = NULL;
    size_t len = 32;

    /* Start with the exact baseline CPU and little-endian A32 only. No MMIO,
     * stepping, breakpoints, plugins, parallel CPUs or multi-page translation.
     * Unsupported code uses QEMU's existing translator unchanged. */
    if (strcmp(object_get_typename(OBJECT(cs)), "arm1176-arm-cpu") ||
        !host_pc || tb_page_addr0(tb) == (tb_page_addr_t)-1 || (pc & 3) ||
        EX_TBFLAG_AM32(flags, THUMB) ||
        EX_TBFLAG_ANY(flags, AARCH64_STATE) ||
        EX_TBFLAG_A32(flags, SCTLR__B) ||
        (tb->cflags & (CF_SINGLE_STEP | CF_BP_PAGE | CF_PARALLEL |
                       CF_USE_ICOUNT | CF_MEMI_ONLY)) ||
        max_insns < 1 || max_insns > TCG_MAX_INSNS) {
        return false;
    }
    checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(checksum, engine_hash, sizeof(engine_hash));
    hash_u64(checksum, IR_VERSION);
    hash_u64(checksum, pc);
    hash_u64(checksum, tb->cs_base);
    hash_u64(checksum, tb->flags);
    hash_u64(checksum, tb->cflags);
    hash_u64(checksum, max_insns);
    hash_u64(checksum, env->features);
    hash_u64(checksum, cpu->idle_loop_pc);
    /* All ISA identification registers have integer representation, no pointers.
     * Include fields explicitly, not structure padding. */
    hash_u64(checksum, cpu->isar.mvfr0);
    hash_u64(checksum, cpu->isar.mvfr1);
    hash_u64(checksum, cpu->isar.mvfr2);
    hash_u64(checksum, cpu->isar.dbgdidr);
    hash_u64(checksum, cpu->isar.dbgdevid);
    hash_u64(checksum, cpu->isar.dbgdevid1);
    hash_u64(checksum, cpu->isar.reset_pmcr_el0);
    for (int i = 0; i < ARRAY_SIZE(cpu->isar.idregs); i++) {
        hash_u64(checksum, cpu->isar.idregs[i]);
    }
    hash_u64(checksum, TARGET_PAGE_SIZE);
    hash_u64(checksum, s->guest_mo);
    hash_u64(checksum, s->addr_type);
    hash_u64(checksum, s->nb_globals);
    for (unsigned i = 0; i < NB_OPS; i++) {
        if (supported_op(i)) {
            hash_u64(checksum, tcg_op_supported(i, TCG_TYPE_I32, 0));
        }
    }
    /* Globals are recreated by this process. Bind their stable layout, never
     * their process addresses, contents or register-allocation state. */
    for (int i = 0; i < s->nb_globals; i++) {
        TCGTemp *t = &s->temps[i];
        hash_u64(checksum, t->kind);
        hash_u64(checksum, t->type);
        hash_u64(checksum, t->base_type);
        hash_u64(checksum, t->mem_offset);
        hash_u64(checksum, t->mem_base ? t->mem_base - s->temps : 0);
        hash_u64(checksum, t->kind == TEMP_FIXED ? t->reg : 0);
    }
    /* Caller already holds QEMU's real code-page lock. Hash every possible
     * source byte in this page, including any decoder look-ahead. The ARM
     * fixed-width bound keeps eligible TBs entirely in this page. */
    g_checksum_update(checksum, host_pc, TARGET_PAGE_SIZE -
                       (pc & (TARGET_PAGE_SIZE - 1)));
    g_checksum_get_digest(checksum, key, &len);
    return true;
}

/* Closed supported-op set: calls, plugins, vectors and host-pointer constants
 * are deliberately absent. Even an unexpected future op falls back normally. */
static bool supported_op(unsigned opc)
{
    switch (opc) {
    case INDEX_op_discard: case INDEX_op_set_label:
    case INDEX_op_br: case INDEX_op_brcond: case INDEX_op_mb:
    case INDEX_op_mov: case INDEX_op_add: case INDEX_op_and:
    case INDEX_op_andc: case INDEX_op_bswap16: case INDEX_op_bswap32:
    case INDEX_op_clz: case INDEX_op_ctpop: case INDEX_op_ctz:
    case INDEX_op_deposit: case INDEX_op_eqv:
    case INDEX_op_ld8u: case INDEX_op_ld8s: case INDEX_op_ld16u:
    case INDEX_op_ld16s: case INDEX_op_ld:
    case INDEX_op_mul: case INDEX_op_nand: case INDEX_op_nor:
    case INDEX_op_or: case INDEX_op_orc: case INDEX_op_rotr:
    case INDEX_op_rotl: case INDEX_op_sar: case INDEX_op_setcond:
    case INDEX_op_shl: case INDEX_op_shr: case INDEX_op_st8:
    case INDEX_op_st16: case INDEX_op_st: case INDEX_op_sub:
    case INDEX_op_xor: case INDEX_op_insn_start:
    case INDEX_op_exit_tb: case INDEX_op_goto_tb:
    case INDEX_op_qemu_ld: case INDEX_op_qemu_st:
        return true;
    default:
        return false;
    }
}

static int label_argument(unsigned opc)
{
    switch (opc) {
    case INDEX_op_set_label: case INDEX_op_br:
        return 0;
    case INDEX_op_brcond:
        return 3;
    default:
        return -1;
    }
}

/* Base ARM integer encodings only. Excludes status registers, coprocessors,
 * VFP/SIMD, exception/privileged instructions and multiply-extension spaces.
 * This makes all retained non-global constants guest integers, not host data.
 * The subsequent closed IR validator also rejects any helper or pointer op. */
static bool supported_instructions(const uint8_t *code, unsigned size)
{
    if (!size || (size & 3)) {
        return false;
    }
    for (unsigned i = 0; i < size; i += 4) {
        uint32_t insn = ldl_le_p(code + i);
        unsigned group = (insn >> 25) & 7;
        unsigned alu = (insn >> 21) & 15;
        if ((insn >> 28) == 15) {
            return false;
        }
        if (group == 5) { /* B / BL */
            continue;
        }
        if ((insn & 0x0c000000) == 0x04000000 &&
            (!(insn & (1u << 25)) || !(insn & (1u << 4)))) {
            continue; /* ordinary LDR/STR, byte or word */
        }
        if (group <= 1 && (alu < 8 || alu > 11 || (insn & (1u << 20))) &&
            ((insn & (1u << 25)) || !(insn & (1u << 4)))) {
            continue; /* ARM data processing, no register-shift specials */
        }
        return false;
    }
    return true;
}

static IRBlock *capture_ir(TranslationBlock *tb, vaddr pc, const void *code)
{
    TCGContext *s = tcg_ctx;
    IRBlock *b;
    TCGOp *op;
    unsigned n = 0;

    if (tb_page_addr1(tb) != (tb_page_addr_t)-1 ||
        tb->size > TARGET_PAGE_SIZE - (pc & (TARGET_PAGE_SIZE - 1)) ||
        !supported_instructions(code, tb->size) ||
        s->nb_temps > TCG_MAX_TEMPS || s->nb_labels > IR_MAX_LABELS) {
        return NULL;
    }
    b = g_new0(IRBlock, 1);
    b->globals = s->nb_globals;
    b->temps = s->nb_temps;
    b->labels = s->nb_labels;
    b->size = tb->size;
    b->icount = tb->icount;
    /* CF_PCREL leaves tb->pc unspecified. The translator's actual pc bounds
     * this page; raw instruction markers remain page offsets for unwind. */
    b->pc_relative = !!(tb->cflags & CF_PCREL);
    for (int i = s->nb_globals; i < s->nb_temps; i++) {
        TCGTemp *t = &s->temps[i];
        if (t->type != TCG_TYPE_I32 || t->base_type != TCG_TYPE_I32 ||
            t->temp_subindex || (t->kind != TEMP_EBB &&
            t->kind != TEMP_TB && t->kind != TEMP_CONST) ||
            (t->kind == TEMP_CONST && t->val != (int64_t)(int32_t)t->val)) {
            goto fail;
        }
        b->temp[i] = (IRTemp){ t->kind, t->base_type, t->type,
                               t->temp_subindex,
                               t->kind == TEMP_CONST ? t->val : 0 };
    }
    QTAILQ_FOREACH(op, &s->ops, link) {
        const TCGOpDef *d;
        IROp *o;
        int label;
        if (n == IR_MAX_OPS || !supported_op(op->opc)) {
            goto fail;
        }
        d = &tcg_op_defs[op->opc];
        o = &b->op[n++];
        o->opc = op->opc;
        o->p1 = op->param1;
        o->p2 = op->param2;
        o->nargs = d->nb_oargs + d->nb_iargs + d->nb_cargs;
        if (o->nargs > IR_MAX_ARGS || o->nargs > op->nargs) {
            goto fail;
        }
        label = label_argument(op->opc);
        for (unsigned j = 0; j < o->nargs; j++) {
            uint64_t arg = op->args[j];
            if (j < d->nb_oargs + d->nb_iargs) {
                TCGTemp *t = arg_temp(arg);
                if (t < s->temps || t >= s->temps + s->nb_temps) {
                    goto fail;
                }
                arg = t - s->temps;
            } else if ((int)j == label) {
                TCGLabel *l = arg_label(arg);
                if (l->id >= b->labels || !l->present) {
                    goto fail;
                }
                arg = l->id;
            } else if (op->opc == INDEX_op_exit_tb) {
                uintptr_t base = (uintptr_t)tcg_splitwx_to_rx(tb);
                if (arg == 0) {
                    arg = 0; /* NULL TB exit */
                } else if (arg >= base && arg <= base + TB_EXIT_REQUESTED) {
                    arg = arg - base + 1; /* symbolic current TB + slot */
                } else {
                    goto fail;
                }
            }
            o->args[j] = arg;
        }
    }
    b->ops = n;
    return b;
fail:
    g_free(b);
    return NULL;
}

static void put_u64(GByteArray *a, uint64_t value)
{
    value = GUINT64_TO_LE(value);
    g_byte_array_append(a, (void *)&value, sizeof(value));
}

static GBytes *encode_ir(const IRBlock *b)
{
    GByteArray *a = g_byte_array_new();
    put_u64(a, IR_VERSION);
    put_u64(a, b->globals); put_u64(a, b->temps); put_u64(a, b->labels);
    put_u64(a, b->ops); put_u64(a, b->size); put_u64(a, b->icount);
    put_u64(a, b->pc_relative);
    for (unsigned i = b->globals; i < b->temps; i++) {
        const IRTemp *t = &b->temp[i];
        put_u64(a, t->kind); put_u64(a, t->base_type);
        put_u64(a, t->type); put_u64(a, t->subindex); put_u64(a, t->value);
    }
    for (unsigned i = 0; i < b->ops; i++) {
        const IROp *o = &b->op[i];
        put_u64(a, o->opc); put_u64(a, o->p1);
        put_u64(a, o->p2); put_u64(a, o->nargs);
        for (unsigned j = 0; j < o->nargs; j++) {
            put_u64(a, o->args[j]);
        }
    }
    return g_byte_array_free_to_bytes(a);
}

static bool valid_condition(uint64_t condition)
{
    switch (condition) {
    case TCG_COND_NEVER: case TCG_COND_ALWAYS:
    case TCG_COND_EQ: case TCG_COND_NE: case TCG_COND_TSTEQ:
    case TCG_COND_TSTNE: case TCG_COND_LT: case TCG_COND_GE:
    case TCG_COND_GT: case TCG_COND_LE: case TCG_COND_LTU:
    case TCG_COND_GEU: case TCG_COND_GTU: case TCG_COND_LEU:
        return true;
    default:
        return false;
    }
}

static bool host_memory_op(unsigned opc)
{
    switch (opc) {
    case INDEX_op_ld8u: case INDEX_op_ld8s:
    case INDEX_op_ld16u: case INDEX_op_ld16s: case INDEX_op_ld:
    case INDEX_op_st8: case INDEX_op_st16: case INDEX_op_st:
        return true;
    default:
        return false;
    }
}

static bool validate_operands(const IRBlock *b, const IROp *o)
{
    const TCGOpDef *d = &tcg_op_defs[o->opc];
    bool host_mem = host_memory_op(o->opc);
    unsigned env_index = temp_idx(tcgv_ptr_temp(tcg_env));
    for (unsigned j = 0; j < d->nb_oargs + d->nb_iargs; j++) {
        uint64_t idx = o->args[j];
        uint64_t kind = idx < b->globals ? tcg_ctx->temps[idx].kind :
                                         b->temp[idx].kind;
        uint64_t type = idx < b->globals ? tcg_ctx->temps[idx].type :
                                         b->temp[idx].type;
        if (host_mem && j == 1) {
            if (idx != env_index) { return false; }
        } else if (type != TCG_TYPE_I32) {
            return false;
        }
        if (j < d->nb_oargs && (kind == TEMP_CONST || kind == TEMP_FIXED)) {
            return false;
        }
    }
    if (host_mem) {
        int64_t off = o->args[2];
        /* Bounds include the negative CPUState header used by the interrupt
         * prologue. In particular no arbitrary host pointer bases/offsets. */
        if (o->p2 || off < -(int64_t)sizeof(CPUState) ||
            off > (int64_t)sizeof(CPUARMState) - 4) { return false; }
    } else if (o->opc == INDEX_op_qemu_ld || o->opc == INDEX_op_qemu_st) {
        uint64_t oi = o->args[2];
        uint64_t allowed = MO_SIZE | MO_SIGN | MO_BSWAP | MO_AMASK |
                           MO_ALIGN_TLB_ONLY | MO_ATOM_MASK;
        if (oi > UINT32_MAX || (get_memop(oi) & ~allowed) ||
            (get_memop(oi) & MO_SIZE) != o->p2 ||
            (get_memop(oi) & MO_ATOM_MASK) > MO_ATOM_NONE ||
            get_mmuidx(oi) >= NB_MMU_MODES) { return false; }
    } else if (o->p2) {
        return false;
    }
    if (o->opc == INDEX_op_deposit &&
        (o->args[3] >= 32 || !o->args[4] ||
         o->args[4] > 32 - o->args[3])) { return false; }
    if ((o->opc == INDEX_op_bswap16 || o->opc == INDEX_op_bswap32) &&
        (o->args[2] & ~(TCG_BSWAP_IZ | TCG_BSWAP_OZ | TCG_BSWAP_OS))) {
        return false;
    }
    if ((o->opc == INDEX_op_brcond && !valid_condition(o->args[2])) ||
        (o->opc == INDEX_op_setcond && !valid_condition(o->args[3]))) {
        return false;
    }
    if (o->opc == INDEX_op_mb && o->args[0] > 255) { return false; }
    return tcg_op_supported(o->opc, TCG_TYPE_I32, o->p2);
}

typedef struct IRReader { const uint8_t *p; size_t len; bool failed; } IRReader;
static uint64_t get_u64(IRReader *r)
{
    uint64_t value;
    if (r->len < 8) {
        r->failed = true;
        return 0;
    }
    memcpy(&value, r->p, 8);
    r->p += 8;
    r->len -= 8;
    return GUINT64_FROM_LE(value);
}

/* Parse and validate completely before changing any live TCG state. */
static IRBlock *decode_ir(GBytes *bytes, unsigned globals, unsigned max_insns,
                          unsigned remaining, vaddr pc, uint32_t cflags)
{
    IRReader r;
    IRBlock *b = g_new0(IRBlock, 1);
    bool labels[IR_MAX_LABELS] = { false };
    unsigned starts = 0, exits = 0, gotos = 0;
    r.p = g_bytes_get_data(bytes, &r.len);
    r.failed = false;
    if (get_u64(&r) != IR_VERSION) { goto fail; }
    /* Read wide, bound first, only then convert to implementation sizes. */
    uint64_t bg = get_u64(&r), bt = get_u64(&r), bl = get_u64(&r);
    uint64_t bo = get_u64(&r), bs = get_u64(&r), bi = get_u64(&r);
    uint64_t bp = get_u64(&r);
    if (bg != globals || bt < bg || bt > TCG_MAX_TEMPS ||
        bl > IR_MAX_LABELS || !bo || bo > IR_MAX_OPS || !bs ||
        bs > remaining || (bs & 3) || !bi || bi > max_insns || bi * 4 != bs ||
        bp > 1 || bp != !!(cflags & CF_PCREL)) {
        goto fail;
    }
    b->globals = bg; b->temps = bt; b->labels = bl;
    b->ops = bo; b->size = bs; b->icount = bi;
    b->pc_relative = bp;
    for (unsigned i = b->globals; i < b->temps; i++) {
        IRTemp *t = &b->temp[i];
        t->kind = get_u64(&r); t->base_type = get_u64(&r);
        t->type = get_u64(&r); t->subindex = get_u64(&r);
        t->value = get_u64(&r);
        if ((t->kind != TEMP_EBB && t->kind != TEMP_TB &&
             t->kind != TEMP_CONST) || t->base_type != TCG_TYPE_I32 ||
            t->type != TCG_TYPE_I32 || t->subindex ||
            (t->kind == TEMP_CONST &&
             (int64_t)t->value != (int64_t)(int32_t)t->value)) {
            goto fail;
        }
    }
    for (unsigned i = 0; i < b->ops; i++) {
        IROp *o = &b->op[i];
        const TCGOpDef *d;
        int label;
        o->opc = get_u64(&r); o->p1 = get_u64(&r);
        o->p2 = get_u64(&r); o->nargs = get_u64(&r);
        if (o->opc > UINT_MAX || !supported_op(o->opc) ||
            o->p1 != TCG_TYPE_I32 || o->p2 > 2) { goto fail; }
        d = &tcg_op_defs[o->opc];
        if (o->nargs != d->nb_oargs + d->nb_iargs + d->nb_cargs ||
            o->nargs > IR_MAX_ARGS) { goto fail; }
        label = label_argument(o->opc);
        for (unsigned j = 0; j < o->nargs; j++) {
            uint64_t arg = o->args[j] = get_u64(&r);
            if (j < d->nb_oargs + d->nb_iargs && arg >= b->temps) {
                goto fail;
            }
            if ((int)j == label && arg >= b->labels) { goto fail; }
        }
        if (!validate_operands(b, o)) { goto fail; }
        if (o->opc == INDEX_op_set_label) {
            if (labels[o->args[0]]) { goto fail; }
            labels[o->args[0]] = true;
        } else if (o->opc == INDEX_op_goto_tb) {
            if (o->args[0] > TB_EXIT_IDXMAX ||
                (gotos & (1u << o->args[0]))) { goto fail; }
            gotos |= 1u << o->args[0];
        } else if (o->opc == INDEX_op_exit_tb) {
            if (o->args[0] > TB_EXIT_REQUESTED + 1 || o->args[0] == 3 ||
                ((o->args[0] == 1 || o->args[0] == 2) &&
                 !(gotos & (1u << (o->args[0] - 1))))) { goto fail; }
            exits++;
        } else if (o->opc == INDEX_op_insn_start) {
            /* arm_tr_insn_start masks the guest PC to its page offset for
             * CF_PCREL. arm_restore_state_to_opc deposits that offset into
             * the current dynamic PC's page after a fault. Do not turn these
             * markers into absolute addresses or derive them from tb->pc. */
            vaddr expected_pc = pc + starts * 4;
            if (b->pc_relative) { expected_pc &= ~TARGET_PAGE_MASK; }
            if (o->args[0] != expected_pc || o->args[1] > 255 ||
                o->args[2] > UINT32_MAX) { goto fail; }
            starts++;
        }
    }
    for (unsigned i = 0; i < b->labels; i++) {
        if (!labels[i]) { goto fail; }
    }
    if (r.failed || r.len || starts != b->icount || !exits) { goto fail; }
    return b;
fail:
    g_free(b);
    return NULL;
}

static void replay_ir(const IRBlock *b, TranslationBlock *tb)
{
    TCGContext *s = tcg_ctx;
    TCGLabel *labels[IR_MAX_LABELS];
    for (unsigned i = b->globals; i < b->temps; i++) {
        const IRTemp *t = &b->temp[i];
        TCGTemp *ts = &s->temps[i];
        memset(ts, 0, sizeof(*ts));
        ts->kind = t->kind; ts->base_type = t->base_type;
        ts->type = t->type; ts->temp_subindex = t->subindex;
        ts->val = t->value;
    }
    s->nb_temps = b->temps;
    for (unsigned i = 0; i < b->labels; i++) { labels[i] = gen_new_label(); }
    for (unsigned i = 0; i < b->ops; i++) {
        const IROp *o = &b->op[i];
        const TCGOpDef *d = &tcg_op_defs[o->opc];
        int label = label_argument(o->opc);
        TCGOp *op = tcg_emit_op(o->opc, o->nargs);
        op->param1 = o->p1; op->param2 = o->p2;
        for (unsigned j = 0; j < o->nargs; j++) {
            uint64_t arg = o->args[j];
            if (j < d->nb_oargs + d->nb_iargs) {
                arg = temp_arg(&s->temps[arg]);
            } else if ((int)j == label) {
                arg = label_arg(labels[arg]);
            } else if (o->opc == INDEX_op_exit_tb && arg) {
                arg = (uintptr_t)tcg_splitwx_to_rx(tb) + arg - 1;
            }
            op->args[j] = arg;
        }
        if (o->opc == INDEX_op_set_label) {
            labels[o->args[0]]->present = true;
        } else if (label >= 0) {
            TCGLabelUse *use = tcg_malloc(sizeof(*use));
            use->op = op;
            QSIMPLEQ_INSERT_TAIL(&labels[o->args[label]]->branches, use, next);
        }
#ifdef CONFIG_DEBUG_TCG
        if (o->opc == INDEX_op_goto_tb) {
            s->goto_tb_issue_mask |= 1 << o->args[0];
        }
#endif
    }
    tb->size = b->size;
    tb->icount = b->icount;
}

bool arm_tcg_ir_cache_load(CPUState *cpu, TranslationBlock *tb, int max_insns,
                           vaddr pc, void *host_pc, ARMTcgIRCacheKey *key)
{
    GBytes *bytes;
    IRBlock *b;
    int64_t begin;
    memset(key, 0, sizeof(*key));
    if (g_once_init_enter(&cache_initialized)) {
        initialize_cache();
        g_once_init_leave(&cache_initialized, 1);
    }
    if (!store) {
        return false;
    }
    if (diagnostics_enabled) {
        CPUARMTBFlags flags = arm_tbflags_from_tb(tb);
        diagnostic_add(D_arm_attempts, 1);
        diagnostic_add(EX_TBFLAG_ANY(flags, AARCH64_STATE) ? D_a64_attempts :
                       EX_TBFLAG_AM32(flags, THUMB) ? D_thumb_attempts :
                       D_a32_attempts, 1);
    }
    begin = diagnostic_begin();
    bool eligible = make_key(cpu, tb, max_insns, pc, host_pc, key->bytes);
    diagnostic_end(D_key_us, begin);
    diagnostic_add(eligible ? D_key_eligible : D_key_rejected, 1);
    if (!eligible) {
        return false;
    }
    key->eligible = true;
    key->max_insns = max_insns;
    g_mutex_lock(&cache_lock);
    begin = diagnostic_begin();
    bytes = tcg_ir_store_lookup(store, key->bytes);
    diagnostic_end(D_lookup_us, begin);
    if (!bytes) {
        misses++;
        g_mutex_unlock(&cache_lock);
        return false;
    }
    begin = diagnostic_begin();
    b = decode_ir(bytes, tcg_ctx->nb_globals, max_insns,
                   TARGET_PAGE_SIZE - (pc & (TARGET_PAGE_SIZE - 1)), pc,
                   tb->cflags);
    diagnostic_end(D_load_decode_us, begin);
    g_bytes_unref(bytes);
    if (!b || !supported_instructions(host_pc, b->size)) {
        rejected++;
        g_free(b);
        g_mutex_unlock(&cache_lock);
        return false;
    }
    begin = diagnostic_begin();
    replay_ir(b, tb);
    diagnostic_end(D_replay_us, begin);
    hits++;
    /* Android intentionally kills terminal workers without running atexit.
     * Log bounded milestones into its existing private native log as well. */
    if ((hits & (hits - 1)) == 0) { report_cache(); }
    g_free(b);
    g_mutex_unlock(&cache_lock);
    return true;
}

void arm_tcg_ir_cache_save(CPUState *cpu, TranslationBlock *tb,
                           vaddr pc, void *host_pc,
                           const ARMTcgIRCacheKey *key)
{
    uint8_t now[32];
    IRBlock *b;
    GBytes *bytes;
    bool can_store;
    int64_t begin;

    if (!key->eligible) {
        return;
    }
    /* The bounded store commonly fills during boot. Keep existing-record
     * lookups enabled, but avoid a second page/context hash and all codec
     * allocation/validation work for a record put() cannot possibly accept.
     * put() independently rechecks its bounds after this advisory preflight.
     */
    g_mutex_lock(&cache_lock);
    can_store = tcg_ir_store_can_put(store, key->bytes);
    g_mutex_unlock(&cache_lock);
    if (!can_store) {
        diagnostic_add(D_save_preflight_skips, 1);
        return;
    }
    begin = diagnostic_begin();
    bool unchanged = make_key(cpu, tb, key->max_insns, pc, host_pc, now);
    diagnostic_end(D_key_us, begin);
    if (!unchanged ||
        memcmp(now, key->bytes, sizeof(now))) {
        diagnostic_add(D_save_key_changed, 1);
        return;
    }
    begin = diagnostic_begin();
    b = capture_ir(tb, pc, host_pc);
    diagnostic_end(D_capture_us, begin);
    if (!b) {
        diagnostic_add(D_capture_rejected, 1);
        return;
    }
    begin = diagnostic_begin();
    bytes = encode_ir(b);
    diagnostic_end(D_encode_us, begin);
    g_free(b);
    /* Do not publish records that our closed replay validator cannot accept. */
    begin = diagnostic_begin();
    b = decode_ir(bytes, tcg_ctx->nb_globals, key->max_insns,
                   TARGET_PAGE_SIZE - (pc & (TARGET_PAGE_SIZE - 1)), pc,
                   tb->cflags);
    diagnostic_end(D_save_validate_us, begin);
    if (!b) {
        diagnostic_add(D_validation_rejected, 1);
        g_bytes_unref(bytes);
        return;
    }
    g_free(b);
    g_mutex_lock(&cache_lock);
    begin = diagnostic_begin();
    bool published = tcg_ir_store_put(store, key->bytes, bytes);
    diagnostic_end(D_put_us, begin);
    if (published) {
        stored++;
        if ((stored & (stored - 1)) == 0) { report_cache(); }
    } else {
        diagnostic_add(D_store_failed, 1);
    }
    g_mutex_unlock(&cache_lock);
    g_bytes_unref(bytes);
}
#else
void arm_tcg_ir_cache_finish(void) { }

bool arm_tcg_ir_cache_load(CPUState *cpu, TranslationBlock *tb, int max_insns,
                           vaddr pc, void *host_pc, ARMTcgIRCacheKey *key)
{
    key->eligible = false;
    return false;
}
void arm_tcg_ir_cache_save(CPUState *cpu, TranslationBlock *tb,
                           vaddr pc, void *host_pc,
                           const ARMTcgIRCacheKey *key) { }
#endif
