/*
 * ldd — print the shared-object dependencies of an ELF32/i386 program.
 *
 * Unlike glibc's ldd this is a pure reader: it never executes the target and
 * never lets ld.so map it.  It parses the ELF program/dynamic headers itself,
 * resolves every DT_NEEDED against $LD_LIBRARY_PATH, /lib and /usr/lib, walks
 * the dependency graph breadth-first and prints the result in the GNU shape.
 * The address column is the object's link-time load address (CactOS maps every
 * image at its link address), not a runtime probe.
 *
 * Options follow GNU ldd: -d/--data-relocs, -r/--function-relocs,
 * -u/--unused, -v/--verbose, --help and --version.  Long options accept
 * unambiguous abbreviations, ambiguous ones are rejected, and -u prints only
 * the unused-dependency block (glibc's LD_DEBUG=unused suppresses the normal
 * listing) and exits non-zero when it is not empty.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "version.h"

#ifndef CACTSOLE_VERSION
#define CACTSOLE_VERSION "unknown"
#endif

/* ── ELF32 structures (no <elf.h> in the freestanding headers) ───────────── */

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

typedef struct {
    int32_t  d_tag;
    union {
        uint32_t d_val;
        uint32_t d_ptr;
    } d_un;
} Elf32_Dyn;

typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
} Elf32_Sym;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
} Elf32_Rel;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
    int32_t  r_addend;
} Elf32_Rela;

typedef struct {
    uint16_t vn_version;
    uint16_t vn_cnt;
    uint32_t vn_file;
    uint32_t vn_aux;
    uint32_t vn_next;
} Elf32_Verneed;

typedef struct {
    uint32_t vna_hash;
    uint16_t vna_flags;
    uint16_t vna_other;
    uint32_t vna_name;
    uint32_t vna_next;
} Elf32_Vernaux;

#define ELFCLASS32    1
#define ELFDATA2LSB   1
#define EM_386        3

#define PT_LOAD       1
#define PT_DYNAMIC    2
#define PT_INTERP     3

#define DT_NULL       0
#define DT_NEEDED     1
#define DT_PLTRELSZ   2
#define DT_HASH       4
#define DT_STRTAB     5
#define DT_SYMTAB     6
#define DT_RELA       7
#define DT_RELASZ     8
#define DT_RELAENT    9
#define DT_STRSZ      10
#define DT_REL        17
#define DT_RELSZ      18
#define DT_RELENT     19
#define DT_PLTREL     20
#define DT_JMPREL     23
#define DT_VERNEED    0x6ffffffe
#define DT_VERNEEDNUM 0x6fffffff
#define DT_GNU_HASH   0x6ffffef5

#define R_386_NONE     0
#define R_386_RELATIVE 8

#define SHN_UNDEF     0
#define STB_GLOBAL    1
#define STB_WEAK      2

#define ELF32_ST_BIND(i)  ((i) >> 4)
#define ELF32_R_SYM(i)    ((i) >> 8)
#define ELF32_R_TYPE(i)   ((uint8_t)(i))

/* ── object table ───────────────────────────────────────────────────────── */

#define LDD_MAX_OBJECTS 64
#define LDD_MAX_NEEDED  32
#define LDD_PATH_MAX    512

typedef struct {
    char     *name;         /* soname as requested (DT_NEEDED) */
    char     *path;         /* resolved filesystem path, NULL if missing */
    uint8_t  *img;          /* raw file image */
    uint32_t  size;

    uint32_t  phoff, phnum, phent;
    uint32_t  base;         /* lowest PT_LOAD vaddr */
    int       found;
    int       is_main;
    int       has_dynamic;
    int       has_interp;

    uint32_t  dyn_vaddr, dyn_size;
    uint32_t  interp_off, interp_size;

    uint32_t  symtab, strtab, strsz, symcount;
    uint32_t  hash, gnu_hash;
    uint32_t  rel, relsz, relent;
    uint32_t  rela, relasz, relaent;
    uint32_t  jmprel, jmprelsz, pltrel;
    uint32_t  verneed, verneednum;

    uint32_t  needed[LDD_MAX_NEEDED];
    int       dep[LDD_MAX_NEEDED];
    int       nneeded;
} ldd_obj;

typedef struct {
    int data_relocs;
    int func_relocs;
    int unused;
    int verbose;
} ldd_opts;

static ldd_obj g_obj[LDD_MAX_OBJECTS];
static int     g_nobj;

/* ── file reading ───────────────────────────────────────────────────────── */

static uint8_t *ldd_read(const char *path, uint32_t *size_out)
{
    int      fd;
    uint8_t *buf;
    uint32_t cap = 1u << 16, len = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    buf = malloc(cap);
    if (!buf) { close(fd); return NULL; }

    for (;;) {
        ssize_t n;
        if (len == cap) {
            uint8_t *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); close(fd); return NULL; }
            buf = nb;
            cap *= 2;
        }
        n = read(fd, buf + len, cap - len);
        if (n < 0) { free(buf); close(fd); return NULL; }
        if (n == 0) break;
        len += (uint32_t)n;
    }
    close(fd);
    if (len == 0) { free(buf); return NULL; }
    *size_out = len;
    return buf;
}

/* ── ELF accessors ──────────────────────────────────────────────────────── */

/* Map a virtual address to the raw file image, requiring `len` bytes inside
 * one PT_LOAD file range.  Returns NULL when out of range. */
static void *obj_vaddr(const ldd_obj *o, uint32_t vaddr, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < o->phnum; i++) {
        const Elf32_Phdr *ph =
            (const Elf32_Phdr *)(o->img + o->phoff + i * o->phent);
        if (ph->p_type != PT_LOAD) continue;
        if (vaddr < ph->p_vaddr) continue;
        if (vaddr - ph->p_vaddr + len > ph->p_filesz) continue;
        return o->img + ph->p_offset + (vaddr - ph->p_vaddr);
    }
    return NULL;
}

static const char *obj_str(const ldd_obj *o, uint32_t off)
{
    if (!o->strtab || off >= o->strsz) return NULL;
    return (const char *)obj_vaddr(o, o->strtab + off, 1);
}

static const Elf32_Sym *obj_sym(const ldd_obj *o, uint32_t idx)
{
    if (!o->symtab) return NULL;
    return (const Elf32_Sym *)obj_vaddr(o,
        o->symtab + idx * (uint32_t)sizeof(Elf32_Sym),
        (uint32_t)sizeof(Elf32_Sym));
}

static int elf_check(const ldd_obj *o)
{
    const Elf32_Ehdr *eh;
    if (!o->img || o->size < sizeof(Elf32_Ehdr)) return 0;
    eh = (const Elf32_Ehdr *)o->img;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') return 0;
    if (eh->e_ident[4] != ELFCLASS32 || eh->e_ident[5] != ELFDATA2LSB) return 0;
    if (eh->e_machine != EM_386) return 0;
    if (eh->e_phnum == 0 || eh->e_phentsize < sizeof(Elf32_Phdr)) return 0;
    if (eh->e_phoff > o->size) return 0;
    if ((uint32_t)eh->e_phnum * (uint32_t)eh->e_phentsize > o->size - eh->e_phoff)
        return 0;
    return 1;
}

static void obj_scan_ph(ldd_obj *o)
{
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)o->img;
    uint32_t i;

    o->phoff = eh->e_phoff;
    o->phnum = eh->e_phnum;
    o->phent = eh->e_phentsize;
    o->base  = 0xFFFFFFFFu;

    for (i = 0; i < o->phnum; i++) {
        const Elf32_Phdr *ph =
            (const Elf32_Phdr *)(o->img + o->phoff + i * o->phent);
        if (ph->p_type == PT_LOAD) {
            if (ph->p_vaddr < o->base) o->base = ph->p_vaddr;
        } else if (ph->p_type == PT_DYNAMIC) {
            o->dyn_vaddr = ph->p_vaddr;
            o->dyn_size  = ph->p_filesz;
            o->has_dynamic = 1;
        } else if (ph->p_type == PT_INTERP) {
            o->interp_off  = ph->p_offset;
            o->interp_size = ph->p_filesz;
            o->has_interp  = 1;
        }
    }
    if (o->base == 0xFFFFFFFFu) o->base = 0;
}

/* Symbol count from SYSV/GNU hash; without either, walk until PT_LOAD ends. */
static uint32_t obj_symcount(const ldd_obj *o)
{
    if (!o->symtab) return 0;

    if (o->hash) {
        const uint32_t *h = (const uint32_t *)obj_vaddr(o, o->hash, 8);
        if (h) return h[1];                       /* nchain */
    }
    if (o->gnu_hash) {
        const uint32_t *gh = (const uint32_t *)obj_vaddr(o, o->gnu_hash, 16);
        if (gh) {
            uint32_t nbuckets  = gh[0];
            uint32_t symoffset = gh[1];
            uint32_t bloom     = gh[2];
            if (bloom > 4096) bloom = 4096;
            if (nbuckets) {
                const uint32_t *buckets = (const uint32_t *)obj_vaddr(o,
                    o->gnu_hash + 16 + bloom * 4, nbuckets * 4);
                if (buckets) {
                    uint32_t maxb = 0, i, ci, guard;
                    for (i = 0; i < nbuckets; i++)
                        if (buckets[i] > maxb) maxb = buckets[i];
                    if (maxb < symoffset) return symoffset;
                    ci = maxb - symoffset;
                    for (guard = 0; guard < 65536; guard++, ci++) {
                        const uint32_t *c = (const uint32_t *)obj_vaddr(o,
                            o->gnu_hash + 16 + bloom * 4 + nbuckets * 4 + ci * 4, 4);
                        if (!c) break;
                        if (*c & 1u) return symoffset + ci + 1;
                    }
                }
            }
        }
    }
    {
        uint32_t n = 0;
        while (n < 65536 && obj_sym(o, n)) n++;
        return n;
    }
}

static void obj_parse_dynamic(ldd_obj *o)
{
    const Elf32_Dyn *dyn;
    uint32_t n, i;

    if (!o->has_dynamic) return;
    dyn = (const Elf32_Dyn *)obj_vaddr(o, o->dyn_vaddr, o->dyn_size);
    if (!dyn) { o->has_dynamic = 0; return; }

    n = o->dyn_size / (uint32_t)sizeof(Elf32_Dyn);
    for (i = 0; i < n; i++) {
        const Elf32_Dyn *d = &dyn[i];
        if (d->d_tag == DT_NULL) break;
        switch (d->d_tag) {
        case DT_NEEDED:
            if (o->nneeded < LDD_MAX_NEEDED)
                o->needed[o->nneeded++] = d->d_un.d_val;
            break;
        case DT_STRTAB:    o->strtab    = d->d_un.d_ptr; break;
        case DT_STRSZ:     o->strsz     = d->d_un.d_val; break;
        case DT_SYMTAB:    o->symtab    = d->d_un.d_ptr; break;
        case DT_HASH:      o->hash      = d->d_un.d_ptr; break;
        case DT_GNU_HASH:  o->gnu_hash  = d->d_un.d_ptr; break;
        case DT_REL:       o->rel       = d->d_un.d_ptr; break;
        case DT_RELSZ:     o->relsz     = d->d_un.d_val; break;
        case DT_RELENT:    o->relent    = d->d_un.d_val; break;
        case DT_RELA:      o->rela      = d->d_un.d_ptr; break;
        case DT_RELASZ:    o->relasz    = d->d_un.d_val; break;
        case DT_RELAENT:   o->relaent   = d->d_un.d_val; break;
        case DT_JMPREL:    o->jmprel    = d->d_un.d_ptr; break;
        case DT_PLTRELSZ:  o->jmprelsz  = d->d_un.d_val; break;
        case DT_PLTREL:    o->pltrel    = d->d_un.d_val; break;
        case DT_VERNEED:   o->verneed   = d->d_un.d_ptr; break;
        case DT_VERNEEDNUM:o->verneednum= d->d_un.d_val; break;
        default: break;
        }
    }
    o->symcount = obj_symcount(o);
}

static void obj_parse(ldd_obj *o)
{
    obj_scan_ph(o);
    obj_parse_dynamic(o);
}

static int obj_peek_base(const char *path, uint32_t *base_out)
{
    uint32_t size;
    uint8_t *img = ldd_read(path, &size);
    ldd_obj tmp;
    int ok = 0;

    if (!img) return 0;
    memset(&tmp, 0, sizeof(tmp));
    tmp.img  = img;
    tmp.size = size;
    if (elf_check(&tmp)) {
        obj_scan_ph(&tmp);
        *base_out = tmp.base & ~0xFFFu;
        ok = 1;
    }
    free(img);
    return ok;
}

/* ── dependency resolution ──────────────────────────────────────────────── */

static int path_readable(const char *p)
{
    int fd = open(p, O_RDONLY);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static int try_dir(const char *dir, const char *name, char *out, uint32_t outsz)
{
    uint32_t n = (uint32_t)strlen(dir);
    if (n == 0) return 0;
    if (dir[n - 1] == '/') snprintf(out, outsz, "%s%s", dir, name);
    else                   snprintf(out, outsz, "%s/%s", dir, name);
    return path_readable(out);
}

static int ldd_find_library(const char *name, char *out, uint32_t outsz)
{
    static const char *const dirs[] = { "/lib", "/usr/lib" };
    const char *env, *p, *q;
    int i;

    if (strchr(name, '/')) {
        snprintf(out, outsz, "%s", name);
        return path_readable(out);
    }

    env = getenv("LD_LIBRARY_PATH");
    for (p = env; p && *p; p = q) {
        q = strchr(p, ':');
        if (!q) q = p + strlen(p);
        if (q > p) {
            char dir[LDD_PATH_MAX];
            uint32_t n = (uint32_t)(q - p);
            if (n >= sizeof(dir)) n = sizeof(dir) - 1;
            memcpy(dir, p, n);
            dir[n] = '\0';
            if (try_dir(dir, name, out, outsz)) return 1;
        }
        if (*q == '\0') break;
        q++;
    }
    for (i = 0; i < 2; i++)
        if (try_dir(dirs[i], name, out, outsz)) return 1;
    return 0;
}

static int obj_lookup(const char *name, const char *path)
{
    int i;
    for (i = 0; i < g_nobj; i++) {
        ldd_obj *o = &g_obj[i];
        if (o->is_main) continue;
        if (o->found && path && o->path && strcmp(o->path, path) == 0) return i;
        if (o->name && strcmp(o->name, name) == 0) return i;
    }
    return -1;
}

static int obj_new(void)
{
    ldd_obj *o;
    if (g_nobj >= LDD_MAX_OBJECTS) return -1;
    o = &g_obj[g_nobj];
    memset(o, 0, sizeof(*o));
    return g_nobj++;
}

static int obj_load_dep(const char *name)
{
    char path[LDD_PATH_MAX];
    int  found = ldd_find_library(name, path, sizeof(path));
    int  idx = obj_lookup(name, found ? path : NULL);
    ldd_obj *o;

    if (idx >= 0) return idx;
    idx = obj_new();
    if (idx < 0) return -1;
    o = &g_obj[idx];
    o->name = strdup(name);
    if (!found) return idx;

    o->path = strdup(path);
    o->img  = ldd_read(path, &o->size);
    if (!o->img || !elf_check(o)) {
        if (o->img) { free(o->img); o->img = NULL; }
        return idx;                                  /* present but unusable */
    }
    obj_parse(o);
    o->found = 1;
    return idx;
}

/* Breadth-first over DT_NEEDED, starting from the main object (index 0). */
static void load_graph(void)
{
    int head;
    for (head = 0; head < g_nobj; head++) {
        ldd_obj *o = &g_obj[head];
        int j;
        if (!o->found || !o->has_dynamic) continue;
        for (j = 0; j < o->nneeded; j++) {
            const char *name = obj_str(o, o->needed[j]);
            o->dep[j] = name ? obj_load_dep(name) : -1;
        }
    }
}

/* ── symbol lookup across the loaded graph ──────────────────────────────── */

static int ldd_lookup_symbol(const char *name)
{
    int i;
    for (i = 0; i < g_nobj; i++) {
        ldd_obj *o = &g_obj[i];
        uint32_t j;
        if (!o->found || !o->symtab || !o->strtab) continue;
        for (j = 1; j < o->symcount; j++) {
            const Elf32_Sym *s = obj_sym(o, j);
            const char *sn;
            int bind;
            if (!s || s->st_shndx == SHN_UNDEF || s->st_value == 0) continue;
            bind = ELF32_ST_BIND(s->st_info);
            if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
            sn = obj_str(o, s->st_name);
            if (sn && strcmp(sn, name) == 0) return i;
        }
    }
    return -1;
}

/* ── relocations (-d / -r) and unused deps (-u) ─────────────────────────── */

static void reloc_scan(const ldd_obj *o, uint32_t addr, uint32_t size,
                       uint32_t ent, int rela, int *used)
{
    uint32_t n, i;

    if (!addr || !size) return;
    if (ent != sizeof(Elf32_Rel) && ent != sizeof(Elf32_Rela))
        ent = rela ? (uint32_t)sizeof(Elf32_Rela) : (uint32_t)sizeof(Elf32_Rel);
    n = size / ent;

    for (i = 0; i < n; i++) {
        const void *r = obj_vaddr(o, addr + i * ent, ent);
        uint32_t info, symi;
        uint8_t  type;
        const Elf32_Sym *s;
        const char *name;

        if (!r) break;
        info = rela ? ((const Elf32_Rela *)r)->r_info
                    : ((const Elf32_Rel *)r)->r_info;
        type = ELF32_R_TYPE(info);
        symi = ELF32_R_SYM(info);
        if (type == R_386_NONE || type == R_386_RELATIVE || symi == 0) continue;

        s = obj_sym(o, symi);
        if (!s || s->st_shndx != SHN_UNDEF) continue;
        name = obj_str(o, s->st_name);
        if (!name || !*name) continue;

        {
            int def = ldd_lookup_symbol(name);
            if (used) {
                if (def >= 0) used[def] = 1;
            } else if (def < 0) {
                printf("undefined symbol: %s\t(%s)\n",
                       name, o->path ? o->path : o->name);
            }
        }
    }
}

static void reloc_scan_object(const ldd_obj *o, int func, int *used)
{
    if (!o->found || !o->symtab) return;
    reloc_scan(o, o->rel, o->relsz,
               o->relent ? o->relent : (uint32_t)sizeof(Elf32_Rel), 0, used);
    reloc_scan(o, o->rela, o->relasz,
               o->relaent ? o->relaent : (uint32_t)sizeof(Elf32_Rela), 1, used);
    if (func && o->jmprel && o->jmprelsz) {
        int is_rela = (o->pltrel == DT_RELA);
        reloc_scan(o, o->jmprel, o->jmprelsz,
                   is_rela ? (uint32_t)sizeof(Elf32_Rela) : (uint32_t)sizeof(Elf32_Rel),
                   is_rela, used);
    }
}

static void check_relocations(const ldd_opts *op)
{
    int i;
    for (i = 0; i < g_nobj; i++)
        reloc_scan_object(&g_obj[i], op->func_relocs, NULL);
}

/* glibc's -u prints only this block (LD_DEBUG=unused suppresses the normal
 * listing) and exits non-zero when it is not empty. */
static int print_unused(void)
{
    int used[LDD_MAX_OBJECTS];
    int i, j, n = 0;
    ldd_obj *main_o = &g_obj[0];

    memset(used, 0, sizeof(used));
    for (i = 0; i < g_nobj; i++)
        reloc_scan_object(&g_obj[i], 1, used);

    for (j = 0; j < main_o->nneeded; j++) {
        int idx = main_o->dep[j];
        if (idx <= 0 || used[idx]) continue;
        if (n == 0) fputs("Unused direct dependencies:\n", stdout);
        printf("\t%s\n", g_obj[idx].path ? g_obj[idx].path : g_obj[idx].name);
        n++;
    }
    return n ? 1 : 0;
}

/* ── verbose symbol-versioning block ────────────────────────────────────── */

static void print_version_info(void)
{
    int i, header = 0;

    for (i = 0; i < g_nobj; i++) {
        ldd_obj *o = &g_obj[i];
        uint8_t *vn;
        uint32_t off, cnt, limit;

        if (!o->found || !o->verneed) continue;
        vn = (uint8_t *)obj_vaddr(o, o->verneed, (uint32_t)sizeof(Elf32_Verneed));
        if (!vn) continue;

        if (!header) {
            fputs("\n\tVersion information:\n", stdout);
            header = 1;
        }
        printf("\t%s:\n", o->path ? o->path : o->name);

        limit = o->verneednum ? o->verneednum : 1;
        off = 0;
        for (cnt = 0; cnt < limit && cnt < 64; cnt++) {
            Elf32_Verneed *need = (Elf32_Verneed *)(vn + off);
            const char *lib = obj_str(o, need->vn_file);
            uint32_t aoff = need->vn_aux, k;

            for (k = 0; k < need->vn_cnt && k < 64 && aoff; k++) {
                Elf32_Vernaux *aux;
                const char *ver;
                char rpath[LDD_PATH_MAX];

                if (!obj_vaddr(o, o->verneed + off + aoff,
                               (uint32_t)sizeof(Elf32_Vernaux))) break;
                aux = (Elf32_Vernaux *)((uint8_t *)need + aoff);
                ver = obj_str(o, aux->vna_name);
                if (lib && ver) {
                    if (ldd_find_library(lib, rpath, sizeof(rpath)))
                        printf("\t\t%s (%s) => %s\n", lib, ver, rpath);
                    else
                        printf("\t\t%s (%s) => not found\n", lib, ver);
                }
                if (!aux->vna_next) break;
                aoff += aux->vna_next;
            }
            if (!need->vn_next) break;
            off += need->vn_next;
            if (!obj_vaddr(o, o->verneed + off, (uint32_t)sizeof(Elf32_Verneed)))
                break;
        }
    }
}

static void print_interp(const ldd_obj *o)
{
    char path[LDD_PATH_MAX];
    uint32_t n = o->interp_size, base;

    if (o->interp_off >= o->size) return;
    if (n > sizeof(path) - 1) n = sizeof(path) - 1;
    if (o->interp_off + n > o->size) n = o->size - o->interp_off;
    memcpy(path, o->img + o->interp_off, n);
    path[n] = '\0';

    printf("\t%s", path);
    if (obj_peek_base(path, &base)) printf(" (0x%08x)", (unsigned)base);
    printf("\n");
}

/* ── per-file analysis ──────────────────────────────────────────────────── */

static int ldd_analyze(const char *path, const ldd_opts *op)
{
    ldd_obj *m;
    int i, rc = 0;

    g_nobj = 0;
    m = &g_obj[g_nobj++];
    memset(m, 0, sizeof(*m));
    m->is_main = 1;
    m->name = strdup(path);
    m->path = strdup(path);

    m->img = ldd_read(path, &m->size);
    if (!m->img || !elf_check(m)) {
        fprintf(stderr, "\tnot a dynamic executable\n");
        return 1;
    }
    m->found = 1;
    obj_parse(m);
    if (!m->has_dynamic) {
        fprintf(stderr, "\tnot a dynamic executable\n");
        return 1;
    }
    load_graph();

    if (op->unused) return print_unused();

    if (m->nneeded == 0) {
        fputs("\tstatically linked\n", stdout);
    } else {
        for (i = 1; i < g_nobj; i++) {
            ldd_obj *o = &g_obj[i];
            if (o->found) {
                printf("\t%s => %s (0x%08x)\n", o->name, o->path,
                       (unsigned)(o->base & ~0xFFFu));
            } else {
                printf("\t%s => not found\n", o->name);
                rc = 1;
            }
        }
        if (m->has_interp) print_interp(m);
        if (op->verbose) print_version_info();
    }
    if (op->data_relocs || op->func_relocs) check_relocations(op);
    return rc;
}

/* ── command line ───────────────────────────────────────────────────────── */

static const char *const ldd_long_opts[] = {
    "help", "version", "data-relocs", "function-relocs", "unused", "verbose"
};
#define LDD_LONG_OPTS ((int)(sizeof(ldd_long_opts) / sizeof(ldd_long_opts[0])))

static void ldd_usage(FILE *out)
{
    fputs("Usage: ldd [OPTION]... FILE...\n"
          "      --help              print this help and exit\n"
          "      --version           print version information and exit\n"
          "  -d, --data-relocs       process data relocations\n"
          "  -r, --function-relocs   process data and function relocations\n"
          "  -u, --unused            print unused direct dependencies\n"
          "  -v, --verbose           print all information\n", out);
}

static int ldd_opt_error(const char *arg, int ambiguous)
{
    if (ambiguous)
        fprintf(stderr, "ldd: option `%s' is ambiguous\n", arg);
    else
        fprintf(stderr, "ldd: unrecognized option `%s'\n", arg);
    fputs("Try `ldd --help' for more information.\n", stderr);
    return 1;
}

/* Returns an option index, -1 for no match or -2 for an ambiguous prefix. */
static int ldd_long_match(const char *name)
{
    int i, found = -1;
    size_t n = strlen(name);
    for (i = 0; i < LDD_LONG_OPTS; i++) {
        if (strncmp(ldd_long_opts[i], name, n) == 0) {
            if (found >= 0) return -2;
            found = i;
        }
    }
    return found;
}

static int process_file(const char *arg, const ldd_opts *op)
{
    char path[LDD_PATH_MAX];
    struct stat st;
    const char *p = arg;

    if (!strchr(arg, '/')) {
        snprintf(path, sizeof(path), "./%s", arg);
        p = path;
    }

    if (stat(p, &st) != 0) {
        fprintf(stderr, "ldd: %s: No such file or directory\n", p);
        return 1;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "ldd: %s: not regular file\n", p);
        return 1;
    }
    {
        int fd = open(p, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr,
                    "ldd: error: you do not have read permission for `%s'\n", p);
            return 1;
        }
        close(fd);
    }
    if (access(p, X_OK) != 0)
        fprintf(stderr,
                "ldd: warning: you do not have execution permission for `%s'\n", p);

    return ldd_analyze(p, op);
}

int cact_ub_ldd(char **argv, int argc)
{
    ldd_opts op;
    int i, rc = 0;

    memset(&op, 0, sizeof(op));

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-') break;

        if (a[1] == '\0') return ldd_opt_error(a, 0);
        if (a[1] == '-') {
            int idx;
            if (a[2] == '\0') { i++; break; }        /* "--" ends options */
            idx = ldd_long_match(a + 2);
            if (idx == -2) return ldd_opt_error(a, 1);
            if (idx < 0)   return ldd_opt_error(a, 0);
            switch (idx) {
            case 0: ldd_usage(stdout); return 0;     /* --help */
            case 1:                                  /* --version */
                printf("ldd (CactOS) " CACTSOLE_VERSION "\n");
                return 0;
            case 2: op.data_relocs = 1; break;
            case 3: op.func_relocs = 1; break;
            case 4: op.unused = 1; break;
            case 5: op.verbose = 1; break;
            default: break;
            }
        } else {
            const char *p;
            for (p = a + 1; *p; p++) {
                switch (*p) {
                case 'd': op.data_relocs = 1; break;
                case 'r': op.func_relocs = 1; break;
                case 'u': op.unused = 1; break;
                case 'v': op.verbose = 1; break;
                case 'h': ldd_usage(stdout); return 0;
                case 'V': printf("ldd (CactOS) " CACTSOLE_VERSION "\n"); return 0;
                default: {
                    char bad[3];
                    bad[0] = '-';
                    bad[1] = *p;
                    bad[2] = '\0';
                    return ldd_opt_error(bad, 0);
                }
                }
            }
        }
    }

    if (i >= argc) {
        fputs("ldd: missing file arguments\n", stderr);
        fputs("Try `ldd --help' for more information.\n", stderr);
        return 1;
    }

    for (; i < argc; i++) {
        if (argc - i > 1) printf("%s:\n", argv[i]);
        if (process_file(argv[i], &op) != 0) rc = 1;
    }
    return rc;
}
