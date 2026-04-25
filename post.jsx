/* Single post view */

function PostProgress() {
  const [pct, setPct] = React.useState(0);
  React.useEffect(() => {
    const onScroll = () => {
      const h = document.documentElement;
      const max = h.scrollHeight - h.clientHeight;
      setPct(max > 0 ? (h.scrollTop / max) : 0);
    };
    window.addEventListener('scroll', onScroll, { passive: true });
    onScroll();
    return () => window.removeEventListener('scroll', onScroll);
  }, []);
  const totalBytes = 0x1f20;
  const cur = Math.floor(pct * totalBytes);
  const hex = (n) => '0x' + n.toString(16).padStart(4, '0');
  return (
    <>
      <div className="post-progress" style={{width: (pct * 100) + '%'}}/>
      <div className="post-progress-label">
        <span className="offset">{hex(cur)}</span> / {hex(totalBytes)} &nbsp;<span style={{color:'var(--ink-ghost)'}}>·</span>&nbsp; {Math.round(pct*100)}%
      </div>
    </>
  );
}

function Code({ lang, file, lines }) {
  const [copied, setCopied] = React.useState(false);
  const raw = lines.map(l => l.text || '').join('\n');
  return (
    <div className="codeblock">
      <div className="codeblock-head">
        <span><span className="lang">{lang}</span> &nbsp; · &nbsp; <span style={{color:'var(--ink-dim)'}}>{file}</span></span>
        <button onClick={() => { navigator.clipboard?.writeText(raw); setCopied(true); setTimeout(()=>setCopied(false),1200); }}>
          {copied ? '✓ copied' : 'copy'}
        </button>
      </div>
      <pre><code>
        {lines.map((l, i) => (
          <div key={i} className={'line ' + (l.hl ? 'hl' : '')}>
            <span className="ln">{i + 1}</span>
            <span dangerouslySetInnerHTML={{__html: l.html || l.text}}/>
          </div>
        ))}
      </code></pre>
    </div>
  );
}

function TOC() {
  const items = [
    { id: 's-context', label: 'Context & target' },
    { id: 's-surface', label: 'Attack surface' },
    { id: 's-primitive', label: 'The primitive' },
    { id: 's-exploit', label: 'Exploitation' },
    { id: 's-patch', label: 'Patch analysis' },
    { id: 's-closing', label: 'Closing thoughts' },
  ];
  const [active, setActive] = React.useState('s-context');
  React.useEffect(() => {
    const onScroll = () => {
      let cur = items[0].id;
      for (const it of items) {
        const el = document.getElementById(it.id);
        if (el && el.getBoundingClientRect().top < 120) cur = it.id;
      }
      setActive(cur);
    };
    window.addEventListener('scroll', onScroll, { passive: true });
    onScroll();
    return () => window.removeEventListener('scroll', onScroll);
  }, []);
  return (
    <div className="post-toc">
      <h4>contents</h4>
      <ol>
        {items.map(it => (
          <li key={it.id} className={active === it.id ? 'active' : ''}>
            <a href={'#' + it.id} className="bare">{it.label}</a>
          </li>
        ))}
      </ol>
    </div>
  );
}

function PostSide() {
  return (
    <aside className="post-side">
      <div className="block">
        <h4>status</h4>
        <div style={{color:'var(--ink-dim)', lineHeight: 1.7}}>
          <div><span className="signal">learning :)</span></div>
          <div style={{fontSize: 10, color: 'var(--ink-faint)', marginTop: 6}}>
            this is a live notebook — expect edits as I dig further.
          </div>
        </div>
      </div>
      <div className="block">
        <h4>reading</h4>
        <div style={{color:'var(--ink-dim)', lineHeight: 1.7}}>
          <div>~ 12 min</div>
          <div>4 code snippets</div>
          <div>1 hexdump</div>
        </div>
      </div>
      <div className="block">
        <h4>questions?</h4>
        <div style={{color:'var(--ink-dim)', lineHeight: 1.7, fontSize: 11}}>
          Ping me on X <a href="#" style={{color:'inherit', border:'none'}} className="bare">@ze3ter</a> — happy to
          be told I'm wrong about any of this.
        </div>
      </div>
    </aside>
  );
}

function PostPage() {
  return (
    <>
      <TopBar current="home" />
      <PostProgress />
      <main className="page">
        <div className="post-hero">
          <div className="post-meta-line">
            <span className="id">#01</span>
            <span className="sep">·</span>
            <span>note</span>
            <span className="sep">·</span>
            <span>v8 / browser</span>
            <span className="sep">·</span>
            <span>2026-02-09</span>
            <span className="sep">·</span>
            <span className="signal">learning :)</span>
          </div>
          <h1 className="post-title">V8 12.4 — OOB heap write via <span style={{color:'var(--accent)'}}>Atomics.store</span> on Float16Array</h1>
          <p className="post-subtitle">
            A bounds-check that seems to run before coercion. I'm walking through what I understand so far,
            what I think is happening, and what I still need to prove.
          </p>
          <div className="post-info">
            <div className="post-info-cell"><div className="k">target</div><div className="v">v8 12.4.254.21</div></div>
            <div className="post-info-cell"><div className="k">component</div><div className="v">runtime / typed arrays</div></div>
            <div className="post-info-cell"><div className="k">state</div><div className="v signal">learning :)</div></div>
            <div className="post-info-cell"><div className="k">updated</div><div className="v">2026-02-09</div></div>
          </div>
        </div>

        <div className="post-body-grid">
          <TOC />

          <article className="prose">
            <p className="lead">
              The Float16Array type landed in V8 12.4 behind a flag. Typed arrays are one of the most exercised
              pieces of the runtime, so a new element kind is the kind of thing you look at before the
              documentation is done. This post walks through a narrow bug that shows up only when you mix the new
              element kind with <code>Atomics.store</code>.
            </p>

            <h2 id="s-context">Context &amp; target</h2>
            <p>
              V8 represents typed arrays as a <code>JSTypedArray</code> wrapping a backing store. Each element
              kind has its own fast path in TurboFan and in the CSA (CodeStubAssembler) builtins. The
              <code>Atomics.store</code> builtin is one of the oldest and most battle-tested entries in that family
              — which is exactly why a new element kind slipping into it quietly is interesting.
            </p>

            <Code
              lang="cpp"
              file="src/builtins/builtins-atomics-synchronized.cc"
              lines={[
                { html: '<span class="tok-com">// FastAtomicsStoreJS — simplified</span>' },
                { html: '<span class="tok-ty">Object</span> <span class="tok-fn">FastAtomicsStoreJS</span>(<span class="tok-ty">Isolate</span>* isolate, <span class="tok-ty">Handle</span>&lt;<span class="tok-ty">JSTypedArray</span>&gt; array,' },
                { html: '&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;<span class="tok-ty">size_t</span> index, <span class="tok-ty">Handle</span>&lt;<span class="tok-ty">Object</span>&gt; value) {' },
                { html: '&nbsp;&nbsp;<span class="tok-kw">if</span> (index >= array->GetLength()) {       <span class="tok-com">// ← typed bounds check</span>' },
                { html: '&nbsp;&nbsp;&nbsp;&nbsp;<span class="tok-kw">return</span> <span class="tok-fn">ThrowRangeError</span>(isolate);' },
                { html: '&nbsp;&nbsp;}' },
                { html: '&nbsp;&nbsp;<span class="tok-ty">ElementsKind</span> kind = array-&gt;<span class="tok-fn">GetElementsKind</span>();' },
                { html: '&nbsp;&nbsp;<span class="tok-kw">auto</span> coerced = <span class="tok-fn">CoerceForElementsKind</span>(isolate, kind, value); <span class="tok-com">// ← may trigger GC</span>', hl: true },
                { html: '&nbsp;&nbsp;<span class="tok-kw">auto</span>* raw = <span class="tok-kw">reinterpret_cast</span>&lt;<span class="tok-ty">uint8_t</span>*&gt;(array-&gt;<span class="tok-fn">DataPtr</span>());', hl: true },
                { html: '&nbsp;&nbsp;<span class="tok-fn">StoreElementAtomic</span>(kind, raw, index, coerced);' },
                { html: '&nbsp;&nbsp;<span class="tok-kw">return</span> coerced;' },
                { html: '}' },
              ]}
            />

            <p>
              The bounds check runs against the typed array's length <em>as it was when the builtin was entered</em>.
              The coercion below it is allowed to run arbitrary user JavaScript via <code>valueOf</code>. If that
              user code detaches the backing store, triggers GC, and reattaches a shorter buffer, the
              <code>raw</code> pointer computed afterwards indexes the new, shorter region — but with the old
              <code>index</code>.
            </p>

            <blockquote>
              The bug is not the bounds check. The bug is the contract that made the bounds check useful — the
              assumption that <code>DataPtr()</code> and the length measured at entry describe the same region of
              memory.
            </blockquote>

            <h2 id="s-surface">Attack surface</h2>
            <p>
              To reach this path we need three things: a Float16Array, a value whose <code>valueOf</code> we
              control, and a way to convince GC to move the backing store between the bounds check and the write.
              All three are reachable from pure JS.
            </p>
            <ul>
              <li>Float16Array accepts plain numbers — but also objects with <code>valueOf</code> returning a number. That gives us the JS callback.</li>
              <li>Detaching and reallocating a SharedArrayBuffer is cheap; Firefox-style spray techniques still work in V8.</li>
              <li><code>Atomics.store</code> is synchronous — our <code>valueOf</code> runs <em>inside</em> the builtin.</li>
            </ul>

            <h3>Minimal trigger</h3>
            <Code
              lang="js"
              file="poc/minimal.js"
              lines={[
                { html: '<span class="tok-kw">const</span> sab = <span class="tok-kw">new</span> <span class="tok-ty">SharedArrayBuffer</span>(<span class="tok-num">0x40</span>);' },
                { html: '<span class="tok-kw">const</span> arr = <span class="tok-kw">new</span> <span class="tok-ty">Float16Array</span>(sab);' },
                { html: '' },
                { html: '<span class="tok-kw">const</span> bait = {' },
                { html: '&nbsp;&nbsp;<span class="tok-fn">valueOf</span>() {' },
                { html: '&nbsp;&nbsp;&nbsp;&nbsp;<span class="tok-com">// shrink the backing store mid-store</span>' },
                { html: '&nbsp;&nbsp;&nbsp;&nbsp;sab.<span class="tok-fn">resize</span>(<span class="tok-num">0x10</span>);', hl: true },
                { html: '&nbsp;&nbsp;&nbsp;&nbsp;<span class="tok-kw">return</span> <span class="tok-num">0x1337</span>;' },
                { html: '&nbsp;&nbsp;}' },
                { html: '};' },
                { html: '' },
                { html: '<span class="tok-ty">Atomics</span>.<span class="tok-fn">store</span>(arr, <span class="tok-num">31</span>, bait);  <span class="tok-com">// ← OOB write</span>', hl: true },
              ]}
            />

            <p>The first time I ran this in d8, the write silently landed past the end of the buffer:</p>

            <Code
              lang="hex"
              file="$ d8 --allow-natives-syntax poc/minimal.js --print-heap-after"
              lines={[
                { html: '<span class="hexdump"><span class="addr">0x2a4f0000</span>  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00</span>' },
                { html: '<span class="hexdump"><span class="addr">0x2a4f0010</span>  <span class="hi">37 13</span> 00 00 00 00 00 00 00 00 00 00 00 00 00 00</span>', hl: true },
                { html: '<span class="hexdump"><span class="addr">0x2a4f0020</span>  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00</span>' },
                { html: '<span class="hexdump"><span class="addr">0x2a4f0030</span>  <span class="addr">── new buffer boundary ──</span></span>' },
                { html: '<span class="hexdump"><span class="addr">0x2a4f0040</span>  <span class="hi">00 00 80 3c</span> 00 00 00 00 00 00 00 00 00 00 00 00  <span class="tok-com">; our stray float16(0x1337) → f16</span></span>', hl: true },
              ]}
            />

            <h2 id="s-primitive">The primitive</h2>
            <p>
              An OOB write with a controlled index and a <em>mostly</em> controlled value is a good starting
              point but not yet exploitable. The interesting step is converting this into a map pointer
              overwrite, because Float16Array storage in V8 lives adjacent to inline properties of small objects
              in the young generation.
            </p>

            <h3>Shaping the heap</h3>
            <p>
              V8's young generation is a copying collector — promote-or-die. If we allocate our typed array,
              trigger a minor GC, and then allocate a sentinel object, the sentinel ends up adjacent to the
              promoted buffer. This is the spray we use.
            </p>

            <h2 id="s-exploit">Exploitation</h2>
            <p>
              From the heap primitive to an addrof/fakeobj pair is mechanical; the interesting engineering step
              is making the exploit survive Oilpan compaction. This section is the shortest because it is the
              part that follows from the previous one — the hard work was already done by the time we have the
              first reliable OOB write.
            </p>

            <h2 id="s-patch">Patch analysis</h2>
            <p>
              The obvious fix is to re-check the length after coercion. The Chromium team will almost certainly
              hoist the <code>DataPtr()</code> computation below the coercion, which is what the corresponding
              path for <code>TypedArray.prototype.fill</code> already does. Worth watching: whether they add a
              generic "detached after coercion" check in <code>CoerceForElementsKind</code> itself, which would
              shut down a whole family of nearby bugs.
            </p>

            <h2 id="s-closing">Closing thoughts</h2>
            <p>
              This one was found in an afternoon because the element kind was new; the builtin was old.
              The two had not met long enough to build up mutual defensive assumptions. Most good bugs live in
              seams like that — the interfaces you didn't notice you created.
            </p>
            <p>
              If you want to reproduce this yourself, the docker image in the sidebar pins the exact V8 commit.
              If you find something adjacent, I'd love to read it — <a href="#">research@ze3tar.dev</a>.
            </p>

            <div className="tag-row">
              <span className="tag">v8</span>
              <span className="tag">browser</span>
              <span className="tag">heap</span>
              <span className="tag">typedarray</span>
              <span className="tag">wip</span>
            </div>

            <div className="post-nav">
              <a href="post.html" className="bare">
                <div className="dir">← previous</div>
                <div className="tt">Defeating seccomp-bpf filters with vfork timing</div>
              </a>
              <a href="post.html" className="bare nxt">
                <div className="dir">next →</div>
                <div className="tt">Trying to understand how Sublime Text works under the hood</div>
              </a>
            </div>
          </article>

          <PostSide />
        </div>
      </main>
      <Footer />
      <TweaksPanel />
    </>
  );
}

Object.assign(window, { PostPage });
