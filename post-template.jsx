/* Generic markdown post renderer
   Requires window.POST_META = { id, title, sub, target, date, state, stateLabel, mdFile, tags, prev, next }
   Requires marked.js loaded before this script */

function PostProgress() {
  const [pct, setPct] = React.useState(0);
  React.useEffect(() => {
    const fn = () => {
      const h = document.documentElement;
      const max = h.scrollHeight - h.clientHeight;
      setPct(max > 0 ? h.scrollTop / max : 0);
    };
    window.addEventListener('scroll', fn, { passive: true });
    fn();
    return () => window.removeEventListener('scroll', fn);
  }, []);
  const totalBytes = 0x2000;
  const cur = Math.floor(pct * totalBytes);
  const hex = n => '0x' + n.toString(16).padStart(4, '0');
  return (
    <>
      <div className="post-progress" style={{ width: pct * 100 + '%' }} />
      <div className="post-progress-label">
        <span className="offset">{hex(cur)}</span> / {hex(totalBytes)}
        &nbsp;<span style={{ color: 'var(--ink-ghost)' }}>·</span>&nbsp;
        {Math.round(pct * 100)}%
      </div>
    </>
  );
}

function PostTOC({ items, active }) {
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

function PostSide({ meta }) {
  const readingWords = Math.ceil((meta.wordCount || 1200) / 200);
  return (
    <aside className="post-side">
      <div className="block">
        <h4>status</h4>
        <div style={{ color: 'var(--ink-dim)', lineHeight: 1.7 }}>
          <span className={'state-tag ' + meta.state}>{meta.stateLabel}</span>
          {meta.state === 'learning' && (
            <div style={{ fontSize: 10, color: 'var(--ink-faint)', marginTop: 6 }}>
              live notebook — expect edits as I dig further.
            </div>
          )}
        </div>
      </div>
      <div className="block">
        <h4>reading</h4>
        <div style={{ color: 'var(--ink-dim)', lineHeight: 1.7 }}>
          <div>~ {readingWords} min</div>
        </div>
      </div>
      {meta.cwe && (
        <div className="block">
          <h4>bug class</h4>
          <div style={{ color: 'var(--ink-dim)', lineHeight: 1.7, fontSize: 12 }}>
            <div>{meta.cwe}</div>
            {meta.cvss && <div style={{ marginTop: 4 }}>CVSS {meta.cvss}</div>}
          </div>
        </div>
      )}
      <div className="block">
        <h4>target</h4>
        <div style={{ color: 'var(--ink-dim)', fontSize: 11, lineHeight: 1.7 }}>
          {meta.target}
        </div>
      </div>
      <div className="block">
        <h4>tags</h4>
        <div className="chips" style={{ flexWrap: 'wrap' }}>
          {(meta.tags || []).map(t => <Chip key={t}>#{t}</Chip>)}
        </div>
      </div>
      <div className="block">
        <h4>contact</h4>
        <div style={{ color: 'var(--ink-dim)', fontSize: 12, lineHeight: 1.7 }}>
          <a href="#" className="bare" style={{ color: 'var(--accent)' }}>x / @ze3ter</a>
        </div>
      </div>
    </aside>
  );
}

function MarkdownBody({ html, tocItems, setActive }) {
  const ref = React.useRef(null);

  React.useEffect(() => {
    if (!ref.current || tocItems.length === 0) return;
    const fn = () => {
      let cur = tocItems[0].id;
      for (const it of tocItems) {
        const el = document.getElementById(it.id);
        if (el && el.getBoundingClientRect().top < 120) cur = it.id;
      }
      setActive(cur);
    };
    window.addEventListener('scroll', fn, { passive: true });
    fn();
    return () => window.removeEventListener('scroll', fn);
  }, [tocItems]);

  return (
    <div
      ref={ref}
      className="post-md-body"
      dangerouslySetInnerHTML={{ __html: html }}
    />
  );
}

function MarkdownPostPage() {
  const meta = window.POST_META;
  const [html, setHtml] = React.useState('');
  const [toc, setToc] = React.useState([]);
  const [active, setActive] = React.useState('');
  const [loading, setLoading] = React.useState(true);
  const [wordCount, setWordCount] = React.useState(0);

  React.useEffect(() => {
    const renderer = new marked.Renderer();
    const slugUsed = {};
    const headings = [];

    renderer.heading = ({ text, depth, tokens }) => {
      const rawText = tokens.map(t => t.raw || t.text || '').join('').replace(/[`*_]/g, '').trim();
      let slug = rawText.toLowerCase().replace(/[^\w\s-]/g, '').replace(/\s+/g, '-').replace(/-+/g, '-').trim('-');
      if (slugUsed[slug]) { slugUsed[slug]++; slug = slug + '-' + slugUsed[slug]; } else { slugUsed[slug] = 1; }
      if (depth === 2) headings.push({ id: slug, label: rawText });
      return `<h${depth} id="${slug}">${marked.parseInline(tokens.map(t=>t.raw||'').join(''))}</h${depth}>`;
    };

    renderer.code = ({ text, lang }) => {
      const escaped = text.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
      const langLabel = lang ? `<span class="code-lang">${lang}</span>` : '';
      return `<div class="md-codeblock">${langLabel}<pre><code class="language-${lang||''}">${escaped}</code></pre></div>`;
    };

    renderer.table = (token) => {
      let thead = '<thead><tr>';
      token.header.forEach(cell => { thead += `<th>${marked.parseInline(cell.tokens.map(t=>t.raw||'').join(''))}</th>`; });
      thead += '</tr></thead>';
      let tbody = '<tbody>';
      token.rows.forEach(row => {
        tbody += '<tr>';
        row.forEach(cell => { tbody += `<td>${marked.parseInline(cell.tokens.map(t=>t.raw||'').join(''))}</td>`; });
        tbody += '</tr>';
      });
      tbody += '</tbody>';
      return `<div class="md-table-wrap"><table>${thead}${tbody}</table></div>`;
    };

    marked.use({ renderer });

    fetch(meta.mdFile)
      .then(r => r.text())
      .then(md => {
        // strip YAML frontmatter if any
        const stripped = md.replace(/^---[\s\S]*?---\n/, '');
        const words = stripped.split(/\s+/).length;
        setWordCount(words);
        meta.wordCount = words;
        const rendered = marked.parse(stripped);
        setHtml(rendered);
        setToc(headings);
        if (headings.length > 0) setActive(headings[0].id);
        setLoading(false);
      })
      .catch(e => {
        setHtml(`<p style="color:var(--accent)">Failed to load post content: ${e.message}</p>`);
        setLoading(false);
      });
  }, []);

  return (
    <>
      <TopBar current="archive" />
      <PostProgress />
      <main className="page">
        <section className="hero">
          <div className="hero-handle">
            §&nbsp;{String(meta.id).padStart(2, '0')}
            <span className="num">{meta.date}</span>
          </div>
          <div>
            <h1 className="hero-title">{meta.title}</h1>
            <p className="hero-sub">{meta.sub}</p>
          </div>
        </section>

        <div className="post-layout">
          <div className="post-main">
            {loading
              ? <div style={{ color: 'var(--ink-faint)', fontSize: 13, padding: '40px 0' }}>loading...</div>
              : <MarkdownBody html={html} tocItems={toc} setActive={setActive} />
            }
          </div>
          <div className="post-sidebar">
            {toc.length > 0 && <PostTOC items={toc} active={active} />}
            <PostSide meta={meta} />
          </div>
        </div>

        <div className="post-nav">
          {meta.prev
            ? <a href={meta.prev.href} className="bare post-nav-link">← {meta.prev.title}</a>
            : <span />
          }
          <a href="index.html" className="bare" style={{ color: 'var(--ink-faint)' }}>home</a>
          {meta.next
            ? <a href={meta.next.href} className="bare post-nav-link">{meta.next.title} →</a>
            : <span />
          }
        </div>
      </main>
      <Footer />
      <TweaksPanel />
    </>
  );
}

Object.assign(window, { MarkdownPostPage });
