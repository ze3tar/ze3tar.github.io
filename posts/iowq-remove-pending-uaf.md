## TL;DR

`io_wq_remove_pending` in Linux 6.19.11 forgets to check whether the previous list node is actually a hashed work item. The shift it uses to read the bucket of the predecessor returns 0 for any non-hashed item, which collides with bucket 0. If you cancel a hashed-to-bucket-0 work whose predecessor is a non-hashed work, the kernel rewires `hash_tail[0]` to point at the predecessor. Then the predecessor gets freed and `hash_tail[0]` is dangling into a freed io_kiocb. The next bucket-0 enqueue writes 8 bytes into that freed slot at offset 0xD8.

On a host with the usual desktop defaults (`panic_on_oops=0`, `dmesg_restrict=0`, `kptr_restrict=0`) the resulting oops dumps registers into dmesg, which a non-root reader can pick up. That gives text base, heap linear-map base, vmemmap base and a SLAB_FREELIST_HARDENED observation off a single trigger.

The chain is UAF write plus KASLR break. I have not turned it into an LPE yet. The constraints on the write and the per-ctx free_list both pushed back.

The fix is in 7.1-rc4 (commit `d6a2d7b04b5a`). 6.19.x in Kali rolling at the time of writing has not picked it up.

---

## What I was looking at

I was reading `io_uring/io-wq.c` line by line, trying to understand how the hashed work list works. Bucket-0 special-cases were everywhere and I kept losing track of who owned `hash_tail[]` at any given moment. So I went through every site that writes to it.

`io_wq_remove_pending` is one of those sites. It runs when a hashed work is cancelled from the pending list. The interesting bit is how it picks the new tail:

```c
if (prev) {
    prev_work = container_of(prev, struct io_wq_work, list);
    if (prev_work->flags >> IO_WQ_HASH_SHIFT == hash) {
        wq->hash_tail[hash] = prev_work;
    } else {
        wq->hash_tail[hash] = NULL;
    }
}
```

The intent is "if the previous work in the list is in the same bucket, it becomes the new tail." The implementation reads the bucket out of `prev_work->flags` with `>> IO_WQ_HASH_SHIFT`. That shift only makes sense if `prev_work` is actually a hashed work. The hashed bit is bit 24 and the bucket value sits in the upper bits, so for a non-hashed work `flags >> 24` is just zero. Zero collides with bucket 0.

So if I have a non-hashed work sitting at `prev`, and I cancel a hashed-to-bucket-0 work, the predicate `0 == 0` fires and `hash_tail[0]` is set to point at the non-hashed work. Nothing protects against this. There's no `io_wq_is_hashed(prev_work)` check.

No reference is taken on `prev_work` here either. The moment it is freed, `hash_tail[0]` becomes a dangling pointer into a freed io_kiocb.

The 7.1-rc4 fix is one extra conjunct:

```c
if (prev_work && io_wq_is_hashed(prev_work) &&
    prev_work->flags >> IO_WQ_HASH_SHIFT == hash) {
    wq->hash_tail[hash] = prev_work;
}
```

That is what separates a UAF from a no-op.

---

## Getting the right items in the right order

To make the predicate fire I need:

1. a non-hashed work at `prev`
2. a hashed-to-bucket-0 work being cancelled
3. the non-hashed work freed shortly after

Three SQEs on one ring:

| Req    | Op                                    | Flags          | What it does                                                                                  |
| ------ | ------------------------------------- | -------------- | --------------------------------------------------------------------------------------------- |
| NW     | `IORING_OP_NOP`                       | `IOSQE_ASYNC`  | Runs in iowq because of `IOSQE_ASYNC`. No file, so non-hashed. Completes immediately.         |
| W0     | `IORING_OP_WRITE`                     | (none)         | Buffered write to a regular file. Returns EAGAIN inline, hashed via `hash_ptr(inode, 6)`. Bucket must be 0. |
| CANCEL | `IORING_OP_ASYNC_CANCEL` targeting W0 |                | Calls `io_wq_remove_pending(W0, prev=NW)`.                                                    |

There is a subtle thing on W0. For it to be hashed at the io_wq layer, `req->file` has to be set at the point `io_prep_async_work` runs. If you put `IOSQE_ASYNC` on the write the request skips `io_issue_sqe`, `io_assign_file` never runs, `req->file == NULL`, and hashing is skipped. So `IOSQE_ASYNC` on W0 does not work. You have to let the buffered write go through the natural EAGAIN path: `io_issue_sqe` runs, `io_write` returns `-EAGAIN`, `io_queue_async` is called, `io_prep_async_work` runs with `req->file` set, and `io_wq_hash_work(&req->work, file_inode(req->file))` finally hashes the work.

NW is the opposite. `IOSQE_ASYNC` on a NOP gives me a non-hashed item that runs in iowq.

What CANCEL(W0) does:

1. walk `acct->work_list`, find W0
2. `prev = &NW->list`
3. `prev_work = container_of(prev, io_wq_work, list)` = `&NW->work`
4. `prev_work->flags >> 24 == 0` and `hash(W0) == 0`, predicate matches
5. no `io_wq_is_hashed` check, so `hash_tail[0] = &NW->work`
6. `wq_list_del` removes W0 from the list, CANCEL returns `res = 0`
7. NW is a NOP and completes, io_kiocb goes back to `req_cachep`
8. `hash_tail[0]` is a dangling pointer into a freed io_kiocb

The actual write happens on the next bucket-0 enqueue. I submit a second hashed bucket-0 write `H3` from a second ring set up with `IORING_SETUP_ATTACH_WQ` so it shares the io_wq. `io_wq_insert_work` runs `wq_list_add_after(&H3->list, hash_tail[0], &acct->work_list)`:

```c
struct io_wq_work_node *next = pos->next;
pos->next = node;       /* freed_NW + 0xD8 := &H3->work.list */
node->next = next;
```

`pos->next = node` writes 8 bytes into the freed slot at offset 0xD8 (the offset of `work.list.next` inside io_kiocb). The value written is `&H3->work.list`, another io_kiocb pointer in the same slab.

So the primitive is: 8 byte write at a fixed offset of a recently freed io_kiocb. Value is a heap pointer in the same cache.

---

## Finding a bucket-0 file

For W0 to be hashed to bucket 0 the inode has to hash to 0 under `hash_ptr(inode, IO_WQ_HASH_ORDER=6)`. The hash uses the inode's heap address, which is per-boot.

Roughly one in 64 fresh tmpfiles hits bucket 0. I create about 256 tmpfiles and run the trigger against each. The first hit lands somewhere between index 30 and 120 depending on what else is allocating from the inode cache at the time. Three back-to-back runs on this host hit at 68, 45, 114.

An earlier version of the PoC used a bpftrace probe on `io_uring:io_uring_queue_async_work` to pick out the bucket directly. That works but needs CAP_BPF. The brute force is slower and needs nothing.

---

## Leak ideas that did not work

Before I figured out the dmesg path I tried a few standard primitives. None of them landed here.

### msg_msg cross-cache spray

The classic: free a 216-byte-ish object, spray `msg_msg` into the freed page, read it back via `msgrcv`. On 6.19 `msg_msg` is allocated through `kmem_buckets_alloc(msg_buckets, ...)` in `ipc/msgutil.c:61`, which puts it in an isolated cache rather than general kmalloc. That is the explicit purpose of `kmem_buckets`. The classic msg_msg primitive is closed here.

### simple_xattr cross-cache spray + getxattr leak

`fs/xattr.c` allocates `simple_xattr` with `kvmalloc(..., GFP_KERNEL_ACCOUNT)`. A 216 byte allocation goes to `kmalloc-cg-256`. The plan was to get the freed io_kiocb's page returned to the buddy, re-allocated as kmalloc-cg-256, then read the corrupted layout back through `getxattr`.

Two obstacles. The first one is well known: io_kiocb's slab cache is `SLAB_TYPESAFE_BY_RCU`, so freed slots wait for an RCU grace period before the page can leave. Tolerable on its own.

The second one I did not see coming. Each io_uring ctx has its own `submit_state.free_list` that caches freed io_kiocbs. The cache is only flushed when `io_ring_exit_work` runs. While the ctx is alive, freed io_kiocbs never go back to slab. The slab page is never empty. The page never returns to the buddy. The whole cross-cache scheme is stuck behind a per-context freelist.

The natural workaround is to close the ring. Closing one ring is not enough though. I need ring2 alive for the H3 write, and ring2 holds the shared io_wq via `IORING_SETUP_ATTACH_WQ`, which keeps ring1's ctx alive. To break that you need a multi-process design: pass ring2 to a child with SCM_RIGHTS, then exit the parent so the kernel actually tears the parent's ctx down. I started writing that and stopped. It is a few days of work and I had not confirmed it would land.

### cred_jar overwrite

If the write at `freed + 0xD8` lined up with a `struct cred`, you would get a classic cred overwrite. cred_jar is 192 bytes, io_kiocb is 248 with slab 256. Different caches. The 0xD8 offset of a 256 byte slot has no meaningful relationship to a 192 byte cred even if you somehow got them in the same page.

### Same-cache neighbor with a writable pointer at +0xD8

The cleanest version would be if 0xD8 of an io_kiocb were a function pointer. It is not. The interesting pointer fields are `req->link` around 0x158 and `req->creds` around 0x178. To hit those you would need a second order corruption that moves the destination of the write, by corrupting the list structure so a later `wq_list_del` writes `prev->next = next` at a chosen offset. That is its own engineering job and I have not built it.

### /proc/kallsyms

`kptr_restrict = 0` on this host but `/proc/kallsyms` zeros symbol addresses for processes without CAP_SYSLOG. `sudo -u nobody head -3 /proc/kallsyms` gives lines starting `0000000000000000`. The easy path is closed for non-root.

---

## The dmesg path

When io_wq later walks the corrupted list it dereferences the planted pointer. On this kernel `panic_on_oops = 0`, so the oops does not reboot. `dmesg_restrict = 0`, so anyone can read the kernel ring buffer. An x86 oops dumps `RIP: <symbol+offset>`, all general purpose registers at fault time, and the faulting address itself. The faulting address is exactly the corrupted pointer I planted.

The same bug supplies the write and the leak.

What landed in dmesg after my trigger:

```
WARNING io_uring/io_uring.c:3046 at io_ring_exit_work+0x129/0x46e
RIP: 0010:io_ring_exit_work+0x129/0x46e
RBX: ffff8bb4069dcc48
R12: ffff8bb4069dc800
R15: ffff8bb4069dcc48
R09: ffff8bb4069dcbb8
```

`ffff8bb4...` is the linear map base for this boot. RBX is the slab object address. R12 is the page-aligned slab boundary. `io_ring_exit_work+0x129` is a fixed offset against a public symbol, so subtracting that offset from the runtime IP gives the text base.

A SLUB GPF usually follows on the same trigger:

```
Oops: general protection fault, probably for non-canonical address 0x2b5472d8516d2946
RIP: deactivate_slab+0x297/0x330
RAX: 2b5472d8516d2946
RCX: 2b5472d8516d286e
RBX: fffff2ae07bde800
RDI: ffff8bb40347cf00
R10: 0cb983834b8cde49
```

The non-canonical address is a SLUB freelist hardened encoded pointer: real ptr XOR per-cache canary XOR `swab64(slab_addr)`. RBX is the `struct page *` from the vmemmap, so `fffff2ae...` is the vmemmap base. RDI is a `kmem_cache *`. The two garbage-looking values RAX and R10 are adjacent freeptr slots. With a known slab address and a known free pointer you can solve for the per-cache canary, which is the SLAB_FREELIST_HARDENED secret for that cache.

From a single trigger I get:

* text base, from `RIP: <sym>+<off>`
* heap linear map base, `ffff8bb4...`
* vmemmap base, `fffff2ae...`
* one SLUB freelist hardened observation

All readable as `nobody`.

---

## Harness

Two binaries do the work.

`iowq_uaf.c` is the trigger. It brute forces tmpfiles, runs NW + W0 + CANCEL plus a probe write against each, and detects the fire when the probe write's CQE does not come back (the corrupted list has tangled and the work is stuck).

`iowq_leak_extract.c` is the parser. It reads dmesg, pulls the text symbol and offset from `RIP:` lines, scans for `ffff8...` heap prefixes, `fffff2ae...` vmemmap prefixes, and non-canonical addresses from SLUB GPFs. Prints derived KASLR bases.

```
$ sudo -u nobody ./iowq_leak_extract
=== iowq UAF infoleak summary (from kernel oops dmesg) ===
text symbol: io_ring_exit_work+0x129
heap object: 0xffff8bb4069dcc48
linear-map prefix: 0xffff8bb400000000  (defeats heap KASLR)
vmemmap object: 0xfffff2ae07bde800
vmemmap prefix: 0xfffff2ae00000000  (defeats vmemmap KASLR)
SLUB freelist-hardened values observed: 1
  fl[0] = 0x2b5472d8516d2946
=== end ===
```

There is also `iowq_lpe_chain.c` that forks the trigger and runs the parser in the parent. It is fussier than the two-step approach because the parent sometimes reads dmesg before the deferred kworker has actually faulted. For reliability I prefer running `iowq_uaf` first and then the extractor against current dmesg state.

Reproducibility, three back to back on a quiet host:

```
$ for i in 1 2 3; do ./iowq_uaf | grep -E "BUG TRIGGERED|Stats|Vulnerable"; done
[!!!] BUG TRIGGERED: file /tmp/iowq_poc_0068 (index 68)
[*] Stats: cancel_ok=69 cancel_fail=0 h1_fast=68
[*] Vulnerable kernel confirmed: 6.19.11+kali-amd64

[!!!] BUG TRIGGERED: file /tmp/iowq_poc_0045 (index 45)
[*] Stats: cancel_ok=46 cancel_fail=0 h1_fast=45
[*] Vulnerable kernel confirmed: 6.19.11+kali-amd64

[!!!] BUG TRIGGERED: file /tmp/iowq_poc_0114 (index 114)
[*] Stats: cancel_ok=115 cancel_fail=0 h1_fast=114
[*] Vulnerable kernel confirmed: 6.19.11+kali-amd64
```

Trigger time is dominated by the brute force search.

---

## What is missing for LPE

What I have is a UAF write and a KASLR break. To turn it into an LPE the missing pieces are:

1. A second-stage primitive that turns the 0xD8 write into either an arbitrary-where write or a function-pointer hijack. The write itself is constrained: 8 bytes, fixed offset, value is a same-cache heap pointer. The realistic direction is to engineer a second corruption that uses `wq_list_del`'s `prev->next = next` to land a write at a chosen offset of another io_kiocb, with `req->link` or `req->creds` as the target.

2. Or a cross-cache primitive against io_kiocb that escapes the per-ctx free_list. The SCM_RIGHTS multi-process approach is the obvious shape, and with the leak in hand the geometry is easier to verify by hand than to guess.

3. A way to steer the fault. The leak only happens when the corrupted `next` dereferences into something unmapped. Sometimes the result is a clean recoverable oops, sometimes a harder fault. Pre-shaping the slab around the freed slot so the corrupted `next` reliably points at something safe-to-fault-on would make the leak more deterministic.

---

## Posture

The fix is in 7.1-rc4. Distro kernels that have not picked up `d6a2d7b04b5a`, including 6.19.x in Kali rolling at the time of writing, are still vulnerable.

The dmesg side of the leak is a configuration question. The defaults on Debian and Kali (`panic_on_oops=0`, `dmesg_restrict=0`, `kptr_restrict=0`) all line up to expose kernel pointers from oops dumps to non-root readers. KSPP guidance has been to set `dmesg_restrict=1`. That closes the non-root leak side without fixing the underlying UAF.

---

## Files

* `iowq_uaf.c`, minimal trigger with the brute-force bucket-0 search.
* `iowq_crash_poc.c`, variant that uses bpftrace to identify the bucket-0 inode directly.
* `iowq_leak_extract.c`, dmesg parser, prints the four KASLR bases.
* `iowq_lpe_chain.c`, merged trigger + parser, single binary.

```bash
gcc -O2 -o iowq_uaf iowq_uaf.c
gcc -O2 -o iowq_leak_extract iowq_leak_extract.c
gcc -O2 -o iowq_lpe_chain iowq_lpe_chain.c
```

---

*Linux 6.19.11-1kali1 · x86_64 · Kali rolling · 2026-05-21*
