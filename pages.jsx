/* Archive & About pages */

function ArchivePage() {
  const allPosts = POSTS;

  const [filter, setFilter] = React.useState('all');
  const filters = [
    { k: 'all', label: 'all' },
    { k: 'cve', label: 'cve' },
    { k: 'use-after-free', label: 'use-after-free' },
    { k: 'reversing', label: 'reversing' },
    { k: 'browser', label: 'browser' },
    { k: 'v8', label: 'v8' },
    { k: 'quickjs', label: 'quickjs' },
  ];

  const shown = filter === 'all'
    ? allPosts
    : allPosts.filter(p => (p.tags || []).includes(filter));

  // group by year
  const byYear = {};
  shown.forEach(p => {
    const y = p.date.slice(0, 4);
    (byYear[y] = byYear[y] || []).push(p);
  });
  const years = Object.keys(byYear).sort().reverse();

  return (
    <>
      <TopBar current="archive" />
      <main className="page">
        <section className="hero">
          <div className="hero-handle">§ archive<span className="num">{allPosts.length} entries</span></div>
          <div>
            <h1 className="hero-title">Notes &amp; learning log</h1>
            <p className="hero-sub">
              Everything I've written down so far — newest first. Entries marked
              <span className="signal"> learning :)</span> are in progress; expect them to change as I dig further.
            </p>
          </div>
        </section>

        <section className="section">
          <SectionLabel num="01">filter</SectionLabel>
          <div>
            <div className="filterbar">
              <span className="label">tag</span>
              {filters.map(f => (
                <button key={f.k} className={filter === f.k ? 'active' : ''} onClick={() => setFilter(f.k)}>
                  {f.label}
                </button>
              ))}
            </div>

            <div style={{display: 'grid', gap: 8}}>
              {years.map(y => (
                <React.Fragment key={y}>
                  <div className="year-row">
                    <span>{y}</span>
                    <span className="count">{byYear[y].length} {byYear[y].length === 1 ? 'entry' : 'entries'}</span>
                  </div>
                  <Ledger rows={byYear[y]} showHead={false} />
                </React.Fragment>
              ))}
            </div>

            <div style={{marginTop: 32, padding: '16px 0', borderTop: '1px solid var(--rule)', color: 'var(--ink-faint)', fontSize: 11, letterSpacing: '0.08em', display:'flex', justifyContent:'space-between'}}>
              <span>{shown.length} of {allPosts.length} entries shown</span>
              <a href="#" className="bare" style={{color: 'var(--ink-dim)'}}>rss · atom</a>
            </div>
          </div>
        </section>
      </main>
      <Footer />
      <TweaksPanel />
    </>
  );
}

function AboutPage() {
  return (
    <>
      <TopBar current="about" />
      <main className="page">
        <section className="hero">
          <div className="hero-handle">§ about<span className="num">~/id.md</span></div>
          <div>
            <h1 className="hero-title">Mohamed Salem Eddah<br/><span style={{color:'var(--ink-faint)', fontSize:28}}>aka ze3ter</span></h1>
            <p className="hero-sub">
              Security researcher. Always trying to go deeper, trying to develop my skills on building
              <em> proof-of-concept exploit chains</em>.
            </p>
          </div>
        </section>

        <section className="section">
          <SectionLabel num="01">who</SectionLabel>
          <div style={{maxWidth: '62ch'}}>
            <p style={{fontSize: 16, lineHeight: 1.7, marginBottom: 20, color: 'var(--ink)'}}>
              I work across offensive security and software engineering, focusing on vulnerability analysis,
              adversarial reasoning, and system-level investigation.
            </p>
            <p style={{fontSize: 15, lineHeight: 1.7, color: 'var(--ink-dim)', marginBottom: 20}}>
              My work centers on understanding how software and systems behave under stress, how weaknesses
              emerge in real environments, and how they can be systematically identified and analyzed.
            </p>
            <p style={{fontSize: 15, lineHeight: 1.7, color: 'var(--ink-dim)', marginBottom: 24}}>
              My approach is methodical, engineering-driven, and grounded in practical system understanding.
            </p>
            <p style={{fontSize: 18, lineHeight: 1.4, color: 'var(--ink)', fontFamily:'var(--font-serif)', fontStyle:'italic', fontWeight: 300}}>
              Trying to learn a new thing everyday.
            </p>
          </div>
        </section>

        <section className="section">
          <SectionLabel num="02">focus</SectionLabel>
          <FocusAreas />
        </section>

        <section className="section">
          <SectionLabel num="03">tools &amp; languages</SectionLabel>
          <ToolStack />
        </section>

        <section className="section">
          <SectionLabel num="04">contact</SectionLabel>
          <div style={{maxWidth:'64ch'}}>
            <p style={{color:'var(--ink)', fontSize: 14.5, lineHeight: 1.7, marginBottom: 24}}>
              Best place to reach me is X — <a href="#">@ze3ter</a>. I read everything, I reply when I can.
            </p>
            <div className="chips">
              <Chip>x / @ze3ter</Chip>
              <Chip>github / ze3tar</Chip>
            </div>
          </div>
        </section>
      </main>
      <Footer />
      <TweaksPanel />
    </>
  );
}

Object.assign(window, { ArchivePage, AboutPage });
