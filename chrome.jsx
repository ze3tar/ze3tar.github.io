/* Shared chrome: topbar, footer, tweaks, progress, common components */

function TopBar({ current }) {
  const items = [
    { id: 'home', label: 'index', href: 'index.html' },
    { id: 'archive', label: 'archive', href: 'archive.html' },
    { id: 'about', label: 'about', href: 'about.html' },
    { id: 'notes', label: 'notes', href: '#' },
    { id: 'rss', label: 'rss', href: '#' },
  ];
  const [clock, setClock] = React.useState(() => new Date());
  React.useEffect(() => {
    const t = setInterval(() => setClock(new Date()), 1000);
    return () => clearInterval(t);
  }, []);
  const pad = n => String(n).padStart(2, '0');
  const utc = `${clock.getUTCFullYear()}-${pad(clock.getUTCMonth()+1)}-${pad(clock.getUTCDate())} ${pad(clock.getUTCHours())}:${pad(clock.getUTCMinutes())}:${pad(clock.getUTCSeconds())}Z`;

  return (
    <div className="topbar">
      <div className="topbar-inner">
        <a className="brand bare" href="index.html">ze3tar<span style={{color:'var(--ink-faint)', marginLeft:4, fontWeight:400}}>/</span><span style={{color:'var(--ink)', fontWeight:400}}>research</span></a>
        <nav className="nav">
          {items.map(it => (
            <a key={it.id} href={it.href} className={current === it.id ? 'current' : ''}>{it.label}</a>
          ))}
        </nav>
        <div className="topbar-meta">
          <span><span className="status-dot"/>online</span>
          <span>{utc}</span>
        </div>
      </div>
    </div>
  );
}

function Footer() {
  return (
    <footer className="footer">
      <div>
        <h4>ze3tar</h4>
        <p style={{color:'var(--ink-dim)', fontSize: 11, lineHeight: 1.6, marginBottom: 12}}>
          Security research. Offensive security. Systems analysis.
        </p>
        <p style={{fontSize:10, color: 'var(--ink-ghost)', letterSpacing: '0.08em'}}>
          Built by hand.
        </p>
      </div>
      <div>
        <h4>elsewhere</h4>
        <ul>
          <li><a href="https://x.com/ze3ter_">x / @ze3ter_</a></li>
          <li><a href="https://github.com/ze3tar/">github / ze3tar</a></li>
        </ul>
      </div>
      <div>
        <h4>pages</h4>
        <ul>
          <li><a href="index.html">index</a></li>
          <li><a href="archive.html">archive</a></li>
          <li><a href="about.html">about</a></li>
        </ul>
      </div>
    </footer>
  );
}

function TweaksPanel() {
  const [on, setOn] = React.useState(false);
  const [accent, setAccent] = React.useState('#ff2d6f');
  const [theme, setTheme] = React.useState('dark');
  const [density, setDensity] = React.useState('default');

  React.useEffect(() => {
    const handler = (e) => {
      if (!e.data) return;
      if (e.data.type === '__activate_edit_mode') setOn(true);
      if (e.data.type === '__deactivate_edit_mode') setOn(false);
    };
    window.addEventListener('message', handler);
    try { window.parent.postMessage({ type: '__edit_mode_available' }, '*'); } catch(e){}
    return () => window.removeEventListener('message', handler);
  }, []);

  React.useEffect(() => {
    document.documentElement.style.setProperty('--accent', accent);
    document.documentElement.setAttribute('data-theme', theme);
    document.documentElement.setAttribute('data-density', density);
  }, [accent, theme, density]);

  const accents = [
    { c: '#ff2d6f', name: 'pink' },
    { c: '#ff5533', name: 'orange' },
    { c: '#00e08a', name: 'green' },
    { c: '#e0a346', name: 'amber' },
    { c: '#7aa2ff', name: 'blue' },
    { c: '#e5e5e5', name: 'mono' },
  ];

  return (
    <div className={'tweaks ' + (on ? 'on' : '')}>
      <div className="tweaks-head">
        <span>tweaks</span>
        <span style={{color:'var(--ink-faint)'}}>×</span>
      </div>
      <div className="tweaks-body">
        <div className="tweaks-row">
          <span className="k">accent</span>
          <div className="swatches">
            {accents.map(a => (
              <div key={a.c}
                className={'sw ' + (accent === a.c ? 'active' : '')}
                style={{background: a.c}}
                onClick={() => setAccent(a.c)}
                title={a.name}
              />
            ))}
          </div>
        </div>
        <div className="tweaks-row">
          <span className="k">theme</span>
          <div style={{display:'flex',gap:4}}>
            <button className={theme==='dark'?'active':''} onClick={() => setTheme('dark')}>dark</button>
            <button className={theme==='light'?'active':''} onClick={() => setTheme('light')}>light</button>
          </div>
        </div>
        <div className="tweaks-row">
          <span className="k">density</span>
          <div style={{display:'flex',gap:4}}>
            <button className={density==='compact'?'active':''} onClick={() => setDensity('compact')}>tight</button>
            <button className={density==='default'?'active':''} onClick={() => setDensity('default')}>std</button>
            <button className={density==='comfortable'?'active':''} onClick={() => setDensity('comfortable')}>loose</button>
          </div>
        </div>
      </div>
    </div>
  );
}

function Chip({ children }) {
  return <span className="chip">{children}</span>;
}

function SectionLabel({ num, children }) {
  return (
    <div className="section-label">
      {children}
      <span className="num">§ {num}</span>
    </div>
  );
}

Object.assign(window, { TopBar, Footer, TweaksPanel, Chip, SectionLabel });
