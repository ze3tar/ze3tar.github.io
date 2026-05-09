
## TL;DR

Linux 6.15 shipped a new zero-copy receive subsystem for io_uring called ZCRX.
It manages a pool of network I/O vectors (niovs) using a stack: `freelist[]`
holds available slot indices, `free_count` is the depth. There is no upper
bound check on `free_count`. Two separate kernel teardown paths both return
niovs to the same freelist, and when they overlap, `free_count` exceeds the
allocated array length. The result is a 4-byte out-of-bounds write into
adjacent slab memory.

The OOB value is a niov index  a small integer between 0 and N-1. That sounds
useless. It is not.


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
entry, the write goes to `freelist[num_niovs]` one slot past the end.

---

# this post is under review i'm gonna edit it and post the full version as soon as i have some time in hand 
