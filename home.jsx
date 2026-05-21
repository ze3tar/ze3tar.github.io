/* Home page - sections composed into the index */

const POSTS = [
  {
    id: '05',
    title: 'io_wq remove_pending UAF and dmesg-side KASLR leak',
    sub: 'Missing is_hashed check on the predecessor work plants a dangling pointer in hash_tail[0]. 8 byte write into a freed io_kiocb on the next bucket-0 enqueue.',
    target: 'Linux 6.19.11-1kali1 · io_uring io-wq',
    date: '2026-05-21',
    state: 'learning',
    stateLabel: 'learning :)',
    href: 'post-iowq.html',
    tags: ['linux', 'kernel', 'io_uring', 'uaf', 'kaslr'],
  },
  {
    id: '04',
    title: 'io_uring ZCRX freelist OOB write — container escape via CAP_NET_ADMIN',
    sub: 'OOB heap write in io_uring zero-copy receive. 4 bytes past the freelist, call_usermodehelper in host init namespace.',
    target: 'Linux 6.15 – 6.19 · io_uring ZCRX',
    date: '2026-05-06',
    state: 'disclosed',
    stateLabel: 'disclosed',
    href: 'post-zcrx.html',
    tags: ['linux', 'kernel', 'container-escape', 'heap'],
  },
  {
    id: '03',
    title: 'QuickJS - Heap-UAF in Atomics via ResizableArrayBuffer',
    sub: 'All 8 Atomics ops share a stale-pointer bug. valueOf() resizes the RAB, realloc moves it, write lands on freed memory.',
    target: 'bellard/quickjs master d7ae12a',
    date: '2026-04-25',
    state: 'disclosed',
    stateLabel: 'disclosed',
    href: 'post-quickjs.html',
    tags: ['quickjs', 'use-after-free', 'cve'],
  },
  {
    id: '02',
    title: 'Trying to understand how Sublime Text works under the hood',
    sub: 'Reading the binary, naming the pieces, figuring out how plugins talk to the core.',
    target: 'Sublime Text Build 4200',
    date: '2026-04-01',
    state: 'learning',
    stateLabel: 'learning :)',
    href: 'post-sublime.html',
    tags: ['reversing', 'ida'],
  },
  {
    id: '01',
    title: 'V8 12.4 - OOB Heap Write via Atomics.store on Float16Array',
    sub: 'A bounds check that runs before coercion - notes while the hypothesis is still forming.',
    target: 'V8 12.4.254.21 · Node.js 22 LTS',
    date: '2026-02-09',
    state: 'learning',
    stateLabel: 'learning :)',
    href: 'post-v8.html',
    tags: ['v8', 'browser'],
  },
];

function Hero() {
  return (
    <section className="hero">
      <div className="hero-handle">
        @ze3ter
        <span className="num">§ 00 / identity</span>
      </div>
      <div>
        <h1 className="hero-title">
          Mohamed Salem Eddah<span className="cursor"/>
        </h1>
        <p className="hero-sub">
          Security researcher. Always trying to go deeper, trying to develop my skills on building
          <em> proof-of-concept exploit chains</em>. I focus on analyzing attack surfaces and researching
          vulnerabilities in web applications, native software, and system-level components.
        </p>
        <p className="hero-sub" style={{marginTop: 14, color: 'var(--ink-dim)'}}>
          Trying to learn a new thing everyday.
        </p>
        <div className="hero-meta">
          <a href="https://x.com/ze3ter_">x / @ze3ter_</a>
          <span className="sep">·</span>
          <a href="https://github.com/ze3tar/">github / ze3tar</a>
          <span className="sep">·</span>
          <a href="about.html">about →</a>
        </div>
      </div>
    </section>
  );
}

function Digging() {
  // uptime counter from an arbitrary recent instant - purely visual "I'm currently on this"
  const start = React.useMemo(() => new Date('2026-05-06T00:00:00Z').getTime(), []);
  const [now, setNow] = React.useState(Date.now());
  React.useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(t);
  }, []);
  const elapsed = Math.max(0, Math.floor((now - start) / 1000));
  const d = Math.floor(elapsed / 86400);
  const h = Math.floor((elapsed % 86400) / 3600);
  const m = Math.floor((elapsed % 3600) / 60);
  const s = elapsed % 60;
  const uptime = `${d}d ${String(h).padStart(2,'0')}h ${String(m).padStart(2,'0')}m ${String(s).padStart(2,'0')}s`;

  return (
    <div className="digging">
      <div className="digging-head">
        <span className="title">▸ currently_disclosing</span>
        <span>session uptime {uptime}</span>
        <div className="dots"><span/><span/><span/></div>
      </div>
      <div className="digging-row">
        <span className="k">target</span>
        <span className="v">Linux 6.15–6.19 · io_uring ZCRX freelist OOB → container escape</span>
        <span className="t">CVE pending</span>
      </div>
      <div className="digging-row">
        <span className="k">status</span>
        <span className="v">Reported to kernel security + io_uring maintainer. Race fix (<span className="accent">003049b1c4fb</span>) in stable. Write-site guard (<span className="accent">770594e</span>) not yet backported to all stable branches. PoC published.</span>
        <span className="t">disclosed</span>
      </div>
      <div className="digging-row">
        <span className="k">next</span>
        <span className="v">
          Waiting on stable backport + CVE assignment. <span className="accent">Stable kernels still vulnerable</span> if CONFIG_IO_URING_ZCRX=y.
        </span>
        <span className="t">-</span>
      </div>
      <div className="digging-progress"><span style={{width: '60%'}}/></div>
    </div>
  );
}

function FocusAreas() {
  const items = [
    { n: '01', tag: 'vuln_research', h: 'Vulnerability Research', p: 'Logic flaws, race conditions, authentication bypass, privilege escalation.' },
    { n: '02', tag: 'exploit_chains', h: 'Exploit Chain Development', p: 'Chaining low-severity issues into high-impact end-to-end attack scenarios.' },
    { n: '03', tag: 'linux_internals', h: 'Linux Internals', p: 'Process isolation, IPC mechanisms, privilege boundaries, kernel interfaces.' },
    { n: '04', tag: 'protocol_analysis', h: 'Protocol Analysis', p: 'Binary protocol reversal, IPC wire formats, authentication model analysis.' },
  ];
  return (
    <ul className="focus-list">
      {items.map(it => (
        <li key={it.n} className="focus-item">
          <div className="focus-item-rail">
            <span className="focus-item-num">{it.n}</span>
          </div>
          <div className="focus-item-body">
            <div className="focus-item-head">
              <h3>{it.h}</h3>
              <span className="focus-item-tag">#{it.tag}</span>
            </div>
            <p>{it.p}</p>
          </div>
        </li>
      ))}
    </ul>
  );
}

function ToolStack() {
  const groups = [
    { label: 'languages', tools: ['Python', 'C / C++', 'Assembly', 'Bash'] },
    { label: 'domains', tools: ['Networks', 'Linux internals'] },
    { label: 'tools', tools: ['pwntools', 'strace / ltrace', 'gdb / pwndbg', 'Burp Suite', 'IDA / Ghidra'] },
  ];
  return (
    <div>
      {groups.map(g => (
        <div key={g.label} style={{marginBottom: 18}}>
          <div style={{fontSize: 10, color: 'var(--ink-faint)', letterSpacing: '0.14em', textTransform: 'uppercase', marginBottom: 10}}>
            {g.label}
          </div>
          <div className="chips">
            {g.tools.map(t => <Chip key={t}>{t}</Chip>)}
          </div>
        </div>
      ))}
    </div>
  );
}

function Ledger({ rows, showHead = true }) {
  return (
    <div className="ledger">
      {showHead && (
        <div className="ledger-head">
          <span>id</span>
          <span>research</span>
          <span>target</span>
          <span>state</span>
          <span>date</span>
        </div>
      )}
      {rows.map(r => (
        <a key={r.id} className="ledger-row bare" href={r.href}>
          <span className="id">#{r.id}</span>
          <span className="title">
            {r.title}
            <span className="sub">{r.sub}</span>
          </span>
          <span className="target">{r.target}</span>
          <span className="state"><span className={'state-tag ' + r.state}>{r.stateLabel}</span></span>
          <span className="date">{r.date}</span>
        </a>
      ))}
    </div>
  );
}

function HomePage() {
  return (
    <>
      <TopBar current="home" />
      <main className="page">
        <Hero />

        <section className="section">
          <SectionLabel num="01">currently</SectionLabel>
          <div>
            <Digging />
          </div>
        </section>

        <section className="section">
          <SectionLabel num="02">about</SectionLabel>
          <div>
            <p style={{fontSize: 15, lineHeight: 1.7, color: 'var(--ink)', maxWidth: '64ch', marginBottom: 20}}>
              I work across offensive security and software engineering, focusing on vulnerability analysis,
              adversarial reasoning, and system-level investigation.
            </p>
            <p style={{fontSize: 15, lineHeight: 1.7, color: 'var(--ink-dim)', maxWidth: '64ch', marginBottom: 20}}>
              My work centers on understanding how software and systems behave under stress, how weaknesses emerge
              in real environments, and how they can be systematically identified and analyzed.
            </p>
            <p style={{fontSize: 15, lineHeight: 1.7, color: 'var(--ink-dim)', maxWidth: '64ch'}}>
              My approach is methodical, engineering-driven, and grounded in practical system understanding.
            </p>
          </div>
        </section>

        <section className="section">
          <SectionLabel num="03">focus areas</SectionLabel>
          <FocusAreas />
        </section>

        <section className="section">
          <SectionLabel num="04">tools &amp; languages</SectionLabel>
          <ToolStack />
        </section>

        <section className="section">
          <SectionLabel num="05">notes</SectionLabel>
          <div>
            <Ledger rows={POSTS} />
            <div style={{marginTop: 24, display: 'flex', justifyContent: 'space-between', color: 'var(--ink-faint)', fontSize: 11, letterSpacing: '0.08em'}}>
              <span>{POSTS.length} entries</span>
              <a href="archive.html">full archive →</a>
            </div>
          </div>
        </section>
      </main>
      <Footer />
      <TweaksPanel />
    </>
  );
}

Object.assign(window, { HomePage, POSTS, Ledger });
