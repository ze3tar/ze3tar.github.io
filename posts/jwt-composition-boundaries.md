## TL;DR

The signature was valid, but logout could still be bypassed.

The key thumbprint was trusted, but PyJWT could still use a different key.

Those became
[CVE-2026-102269](https://github.com/jpadilla/pyjwt/security/advisories/GHSA-hxm8-2xgr-2p9m)
and
[CVE-2026-102275](https://github.com/jpadilla/pyjwt/security/advisories/GHSA-x33g-cr3x-6449).
They look like unrelated bugs. I found both with the same question:

```text
Does every component mean the same thing when it says "this token"?
```

That question changed how I test JWT implementations. I stopped treating
`decode()` as the end of the security boundary and started following identities
through the complete authentication path.

The method is:

1. map the path around the verifier
2. name the identities that cross each boundary
3. write one relationship that must stay true
4. break only that relationship
5. follow the result until a real security decision changes
6. rerun it with one control that restores the relationship

This post is about that method. The CVEs are proof that it works.

---

## The question that changed the hunt

Most JWT testing stops here:

```python
claims = jwt.decode(token, key, algorithms=["RS256"])
```

The token is accepted or rejected. That catches bugs inside the verifier, but a
real application does much more before and after this call.

It may fetch a key, follow a redirect, consult a cache, convert claim types,
select a tenant, look up revocation state and finally choose a resource.

![JWT composition graph showing the security-relevant boundaries around the verifier](blog-assets/composition-graph.svg)

The verifier is one node in that path. Every arrow can change representation,
type, origin or ownership.

That is where I started looking.

![Meme showing a JWT verifier approving a token before downstream components disagree about its identity](blog-assets/verifier-pipeline-meme.webp)

The joke is also the bug class. `jwt.decode()` can be completely correct while
the final security decision is wrong.

---

## One token, several identities

The word *token* hides too much.

The HTTP layer sees compact bytes. The verifier sees decoded signature bytes
and claims. The key resolver sees a URL and a JWK. The application sees runtime
values. The state store sees a lookup key.

I record those as separate identities:

| Identity | What I record |
|---|---|
| Wire | Exact compact bytes and SHA-256 |
| Parsed | Header, payload and decoded signature |
| Cryptographic | Effective key fingerprint and authenticated claims |
| Key origin | Configured URL, final URL and response origin |
| Claim | Value and runtime type of `sub`, `iss`, `aud`, `jti` |
| Resource | Tenant, account or object selected downstream |
| State | Revocation, replay, rate-limit or cache key |
| Work | Allocation and parsing done before authentication |

For each value I ask:

```text
Where did it come from?
Who transformed it?
Who treats it as authoritative?
What other value is assumed to be equal to it?
```

If two components give different answers, I have something worth testing.

The unit under test is not just a library. It is a small **composition
profile**:

```text
library @ pinned version
+ verification API and algorithm
+ key source and transport behavior
+ claim mapping
+ authorization or state decision
```

This keeps the claim honest. Sometimes the defect belongs to the library.
Sometimes the dangerous behavior only exists when two otherwise reasonable
components are connected.

---

## CVE-2026-102269: one signature, two revocation identities

This finding started with logout, not signature forgery.

The profile was simple:

```text
PyJWT verification
    ↓
revocation set keyed by SHA-256(raw token)
```

I changed only the encoded signature segment. The alternate token had different
compact bytes, but decoded to the same signature bytes and authenticated to the
same claims.

```text
canonical token
    ├── verifies as claims C
    └── logout stores SHA-256(canonical)

alternate spelling
    ├── verifies as claims C
    └── lookup uses SHA-256(alternate)  →  not found
```

The alternate form did not change a claim and did not forge a signature. It
changed the identity seen by the revocation store.

That was enough to replay the logged-out token.

The paired control replaced the raw-token digest with an authenticated `jti`.
Both serializations then reached the same state entry and both were blocked.

That difference is what turned a Base64URL oddity into a security finding:

```text
same authenticated identity
must imply
same security-state decision
```

PyJWT fixed the malformed signature aliases in 2.14.0. The important lesson is
still at the composition layer: if a verifier accepts several representations,
raw token text is not a safe unique identity for revocation or replay state.

---

## CVE-2026-102275: one JWK, two key identities

The second bug came from splitting a private OKP JWK into the identities used by
different stages.

For Ed25519:

```text
x = declared public key
d = private key material

required relationship:
derive_public(d) == x
```

I built one inconsistent JWK with trusted public material in `x` and attacker
private material in `d`.

A JWK thumbprint identifies the public key, so it includes `x` but not the
private value `d`. The thumbprint therefore still matched the trusted `x`.
PyJWT, however, constructed the usable private key from the attacker-controlled
`d` without checking that its derived public key matched `x`.

So the same object answered “which key is this?” in two different ways:

```text
identity / pinning layer  →  trusted x
PyJWT operative key       →  attacker d
```

An attacker-signed token verified with the operative key. The trusted
public-only JWK rejected the same token.

The paired control was one equality check:

```text
reject if derive_public(d) != x
```

That became CVE-2026-102275 and was fixed in PyJWT 2.15.0.

I did not find it by fuzzing Ed25519. I found it by asking whether two fields
that claim to describe one key actually describe one key.

At that point, the two findings stopped looking like a coincidence. Both
appeared when separate components used different identities for one security
object. I turned that observation into a repeatable method.

---

## The method behind both bugs

Before generating a mutation, I write the relationship that must hold. This is
the step that turns random input generation into a directed security test.

The research uses five main invariant families:

| Invariant | Required relationship |
|---|---|
| Origin confinement | Configured key origin equals an authorized effective origin |
| Semantic state | Same authenticated identity reaches the same revocation or replay state |
| Key consistency | Declared public-key identity equals the operative cryptographic key |
| Representation preservation | Identity authorized equals the resource identity selected |
| Bounded work | The size check happens before attacker-sized copy, decode or parse work |

Then I change one edge.

| Edge | Mutation |
|---|---|
| Wire to parser | Equivalent Base64URL or JSON representation |
| Key metadata to importer | Two members that describe different keys |
| HTTP client to resolver | Same configured URL, different final origin |
| Claims to runtime | Number versus canonical string |
| Input to parser | Large segment before a size check |
| Verification to state | Authenticate, revoke, replay alternate |

One mutation per experiment is important. If I change the algorithm, claims,
key source and policy at the same time, I may get an interesting result without
knowing what caused it.

![Composition research loop from path mapping to attribution](blog-assets/method-loop.svg)

The loop is always the same:

```text
map path
→ name identities
→ write invariant
→ mutate one edge
→ capture trace
→ rerun with paired control
→ assign ownership
```

---

## The trace is the result

An `accepted: true` field is not enough.

Each adapter returns the same observation shape:

```json
{
  "subject": "implementation@version/profile",
  "case": "mutation-id",
  "accepted": false,
  "raw_sha256": "...",
  "authenticated_key_sha256": "...",
  "configured_origin": "...",
  "effective_origin": "...",
  "claims": {"sub": {"type": "string", "value": "..."}},
  "authorization": "allow|deny|not-run",
  "resource_identity": "...",
  "state_events": [],
  "peak_rss_delta_bytes": 0
}
```

This is where most gaps become visible.

A token can verify while the key came from the wrong origin. Two requests can
authenticate to the same claims while the revocation store sees different
keys. A parser can raise the expected size error after it already made the copy
the guard was supposed to prevent.

The return value describes the last function call. The trace describes the
system.

Some properties also need more than one request:

```text
revocation:  authenticate → logout → replay alternate
key trust:   request → redirect → fetch → cache → verify
availability: start worker → send load → inspect cgroup → health-check
```

A stateless fuzzer can miss a bug whose security condition only exists after
request three.

---

## Weird behavior is not automatically a vulnerability

This was the biggest change in how I handled candidates.

![Meme showing a researcher passing an evidence gate only after bringing the complete experiment](blog-assets/evidence-gate-meme.webp)

The early version of this project produced many parser differences. Most were
interesting. Very few had a security consequence.

Now every candidate has to pass the same evidence gate:

![Evidence ladder from behavioral differential to owned security failure](blog-assets/evidence-ladder.svg)

The required evidence is:

1. a normal baseline
2. one controlled mutation or state transition
3. an explicit invariant
4. a real security decision that changes
5. a paired control that blocks the same mutation
6. reproduction on a pinned release

If the invariant does not fail, I keep it as a behavioral differential. If the
invariant fails but nothing reaches authorization, state, trust or availability,
I keep it as hardening. If the control does not isolate the cause, it stays a
hypothesis.

Only after that do I ask who owns the missing check.

That last step matters. A library importer contract, a documented composition
and an application policy are different reporting targets. An RFC deviation by
itself is not impact.

---

## The easiest measurement trap

The boundary method also applies to availability bugs, but memory claims are
easy to exaggerate by accident. If a 64 MiB token already exists before
measurement begins, counting the same 64 MiB as verifier amplification is
wrong.

I split that test in two. A resident-input experiment starts measuring only
after the complete token exists, then records the verifier's extra peak RSS. A
fresh-worker experiment puts a new HTTP worker inside a fixed cgroup, keeps the
load generator outside and checks whether the service survives invalid
requests.

```text
same input
same verifier
same memory limit
same concurrency

only variable: guard placement
```

One answers, "what did verification allocate?" The other answers, "did the
service survive?" Combining them can make a dramatic graph, but it produces a
weaker claim.

---

## How I kept the results honest

The final matrix contains 14 pinned implementation profiles across Python,
Node, Go, Java, .NET, Ruby and PHP.

Every runtime receives the same synthetic cases and returns the same trace
format. I keep the version, key material, profile, state-transition order and
environment with each result.

Timing and memory cases run 30 times after warm-up. Continuous measurements use
median, IQR and maximum. Worker survival uses Wilson intervals for the tested
condition.

Those intervals describe repeatability. They do not prove how common a pattern
is in public repositories. Prevalence needs a different research method.

For the deterministic cases, I keep the baseline, mutation and paired control
together. A headline count without those three pieces is not enough.

---

## What I stopped doing

The first version of this work was basically a mutation catalog. I had many
differences and weak answers when someone asked, “so what?”

I stopped:

- treating the library as the complete security boundary
- recording only accept or reject
- changing several properties in one test
- calling an RFC difference a vulnerability
- promoting a result without a negative control
- hiding application preconditions behind a library name

That removed candidates from the final set. It also made the remaining ones
much easier to explain, reproduce and fix.

---

## If you test JWTs, start here

Pick one real authentication path and write down:

```text
raw token identity
authenticated token identity
declared key identity
operative key identity
configured key origin
effective key origin
authorization identity
resource identity
revocation / replay identity
```

Then look for two values the system silently assumes are equal.

Break only that equality.

If the result crosses into authorization, trusted key selection, security state
or process availability, build the paired control and reproduce it on a pinned
release.

That process produced two PyJWT CVEs from bugs that looked unrelated on the
surface. The shared weakness was not the cryptographic primitive. It was the
meaning lost between components.

That gap is the attack surface.

If you apply the model to another authentication stack and find a boundary I
missed, I want to hear about it.

---

*All experiments used generated keys, synthetic tokens, loopback services and
controlled workers. No production systems or third-party data were tested.*
