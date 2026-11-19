<script>
  import { onMount } from 'svelte';
  import { base } from '$app/paths';
  import { initMotion } from '$lib/motion.js';

  let container;
  let copied = $state(false);

  const installCmd = 'curl -fsSL https://raw.githubusercontent.com/LangerSword/kolma/master/scripts/install.sh | sh';
  const releases = 'https://github.com/LangerSword/kolma/releases/latest';
  const repo = 'https://github.com/LangerSword/kolma';

  const bench = [
    { algo: 'bwt-mtf-ari', ratio: 0.0035, out: '6.45 KiB', thr: '13.7 MB/s', note: 'best ratio', best: true },
    { algo: 'lz77', ratio: 0.0122, out: '22.5 KiB', thr: '226.9 MB/s', note: '' },
    { algo: 'lzw', ratio: 0.0613, out: '113 KiB', thr: '40.4 MB/s', note: '' },
    { algo: 'huffman', ratio: 0.5626, out: '1.02 MiB', thr: '180.3 MB/s', note: '' },
    { algo: 'store', ratio: 1.0, out: '1.80 MiB', thr: '1198 MB/s', note: 'fastest' }
  ];

  async function copyInstall() {
    try {
      await navigator.clipboard.writeText(installCmd);
      copied = true;
      setTimeout(() => (copied = false), 1600);
    } catch (e) {
      copied = false;
    }
  }

  onMount(() => initMotion(container));
</script>

<svelte:head>
  <title>Kolma — compression that shows its work</title>
  <meta name="description" content="A desktop app, terminal UI and CLI over one auditable C++ compression engine. Zip, tar and 7z out of the box, perceptual media shrinking, and split archives that extract in place." />
  <meta property="og:title" content="Kolma — compression that shows its work" />
  <meta property="og:description" content="Seven methods, an analyser that decides with evidence, and archives anything can open. Free and open source." />
  <meta property="og:image" content="{base}/screenshots/gui-compress.png" />
</svelte:head>

<div bind:this={container}>
  <nav class="nav">
    <div class="container nav-inner">
      <a class="brand" href="{base}/">
        <img src="{base}/favicon.png" alt="" width="28" height="28" />
        kol<span class="mark">ma</span>
      </a>
      <div class="nav-links">
        <a class="link" href="#features">Features</a>
        <a class="link" href="#app">App</a>
        <a class="link" href="#benchmarks">Benchmarks</a>
        <a class="link" href="#security">Security</a>
        <a class="link" href="#download">Download</a>
      </div>
      <div class="nav-cta">
        <a class="btn btn-ghost" href={repo} target="_blank" rel="noopener">GitHub</a>
        <a class="btn btn-primary" href="#download">Get Kolma</a>
      </div>
    </div>
  </nav>

  <main>
    <!-- Hero -->
    <section class="hero">
      <div class="container hero-grid">
        <div class="hero-copy">
          <h1>Compression that<br /><span class="lit">shows its work.</span></h1>
          <p class="hero-sub">A desktop app, terminal and CLI over one auditable C++ engine. Archives anything can open.</p>
          <div class="hero-cta">
            <a class="btn btn-primary" href="#download">Download the app</a>
            <a class="btn btn-ghost" href={repo} target="_blank" rel="noopener">View on GitHub</a>
          </div>
        </div>
        <div class="hero-visual">
          <div class="hero-glow"></div>
          <div class="hero-frame">
            <img src="{base}/screenshots/tui-browse.png" alt="The Kolma terminal UI browsing a folder with file-type icons and semantic colors" width="1180" height="868" />
          </div>
        </div>
      </div>
    </section>

    <!-- Stat strip -->
    <div class="stats">
      <div class="container stats-grid">
        <div class="stat"><b>7</b><span>compression methods</span></div>
        <div class="stat"><b><em>103</em></b><span>engine + integration tests</span></div>
        <div class="stat"><b>416k</b><span>fuzz runs, zero crashes</span></div>
        <div class="stat"><b>3</b><span>operating systems</span></div>
      </div>
    </div>

    <!-- Features bento -->
    <section id="features">
      <div class="container">
        <div class="section-head reveal">
          <span class="eyebrow">What it does</span>
          <h2>An engine you can audit, an app anyone can use.</h2>
          <p>Kolma pairs a pluggable compression engine with interfaces that hide the machinery. Every number on screen comes from the engine, not a guess.</p>
        </div>
        <div class="bento">
          <div class="cell wide tint reveal">
            <h3>Opens the archives you already have</h3>
            <p>Create <span class="mono accent">.zip</span> by default, and read zip, 7z, tar.gz, tar.xz and tar.bz2. Your files open in Finder, Explorer and file-roller, not just Kolma.</p>
            <span class="tag">libarchive-backed, universal by default</span>
          </div>
          <div class="cell wide reveal">
            <h3>The analyser decides with evidence</h3>
            <p>It samples your file, trials the methods, and picks the best one. You see the ratios before you commit.</p>
            <div class="methods">
              <div><span>bwt-mtf-ari</span><b>0.0035</b></div>
              <div><span>lz77</span><b>0.0122</b></div>
              <div><span>lzw</span><b>0.0613</b></div>
              <div><span>huffman</span><b>0.5626</b></div>
            </div>
          </div>
          <div class="cell reveal">
            <h3>Shrink video and images</h3>
            <div class="big">still playable</div>
            <p>Perceptual re-encoding makes media smaller without turning it into a brick. Quality presets, real output.</p>
          </div>
          <div class="cell reveal">
            <h3>Split big files, extract in place</h3>
            <div class="big">.001 .002</div>
            <p>Cut archives into volumes for a FAT32 drive, then extract while reclaiming space as you go. No 2x free disk needed.</p>
          </div>
          <div class="cell reveal">
            <h3>Parallel by construction</h3>
            <div class="big">16 threads</div>
            <p>Chunks compress across your cores, then merge with a verified checksum.</p>
            <span class="tag">chunked, merged, verified</span>
          </div>
          <div class="cell wide tint reveal">
            <h3>Authenticated encryption</h3>
            <p>Encrypt-then-MAC with ChaCha20 and HMAC-SHA256. The tag is verified before a single byte is written to disk.</p>
            <span class="tag">format v2, constant-time verification</span>
          </div>
          <div class="cell wide reveal">
            <h3>Folders become one archive</h3>
            <p>Multi-select files and whole directories, bundle them into a single archive, and peek inside without extracting. Path-traversal safe.</p>
            <span class="tag">bundles, contents preview, safe extraction</span>
          </div>
        </div>
      </div>
    </section>

    <!-- App + TUI showcase -->
    <section id="app">
      <div class="container">
        <div class="section-head reveal">
          <h2>Pick the surface that fits the job.</h2>
          <p>A guided desktop app for everyday use, and a dense terminal UI for people who live in the shell. Both drive the same engine.</p>
        </div>
        <div class="showcase-grid">
          <div class="show-card reveal">
            <div class="img"><img src="{base}/screenshots/gui-compress.png" alt="Kolma desktop app compress view" loading="lazy" width="1060" height="760" /></div>
            <div class="body">
              <h3>Desktop app</h3>
              <p>Drag files in, choose a format, press Compress. Inspect, shrink, split and test archives from the Tools view. Linux, macOS and Windows.</p>
            </div>
          </div>
          <div class="show-card reveal">
            <div class="img"><img src="{base}/screenshots/tui-bench.png" alt="Kolma terminal benchmark with braille throughput graph" loading="lazy" width="1180" height="868" /></div>
            <div class="body">
              <h3>Terminal UI</h3>
              <p>Browse, mark, bundle and extract without leaving the terminal. Braille throughput graphs and value-ramped meters, checked at three sizes.</p>
              <div class="keys"><b>space</b> mark &nbsp; <b>c</b> compress &nbsp; <b>x</b> extract &nbsp; <b>v</b> test &nbsp; <b>b</b> bench</div>
            </div>
          </div>
        </div>
      </div>
    </section>

    <!-- Benchmarks -->
    <section id="benchmarks">
      <div class="container">
        <div class="section-head reveal">
          <h2>What the trials measured.</h2>
          <p>Every method on the same 1.8 MiB text corpus. Ratio is compressed over original, so lower is better.</p>
        </div>
        <div class="bench-wrap reveal">
          <div class="bench-scroll">
            <table class="bench-table">
              <thead>
                <tr><th>method</th><th class="num">ratio</th><th class="num">output</th><th class="num">throughput</th><th>note</th></tr>
              </thead>
              <tbody>
                {#each bench as row}
                  <tr class:best={row.best}>
                    <td class="algo">{row.algo}</td>
                    <td class="num">{row.ratio.toFixed(4)}{#if row.ratio < 1}<span class="bench-bar" style="width: {Math.max(3, (1 - row.ratio) * 90)}px"></span>{/if}</td>
                    <td class="num">{row.out}</td>
                    <td class="num">{row.thr}</td>
                    <td>{row.note}</td>
                  </tr>
                {/each}
              </tbody>
            </table>
          </div>
          <div class="bench-note">Single-threaded per method, GCC, Ryzen 7. Reproduce with <span class="mono">kolma bench &lt;file&gt;</span>. The full suite runs on every commit.</div>
        </div>
      </div>
    </section>

    <!-- Security -->
    <section id="security">
      <div class="container">
        <div class="section-head reveal">
          <h2>Security, stated plainly.</h2>
          <p>What Kolma protects, and what it does not. No fine print.</p>
        </div>
        <div class="sec-grid">
          <div class="sec-card good reveal">
            <h3>What is protected</h3>
            <ul>
              <li>Encrypted archives use encrypt-then-MAC, verified before output.</li>
              <li>Every payload carries a CRC-32, checked on read and on restore.</li>
              <li>Extraction refuses path traversal and absolute paths.</li>
              <li>Overwrites need an explicit confirm, never silent.</li>
              <li>The parser is fuzzed and bounds-checked, sanitizer-clean.</li>
            </ul>
          </div>
          <div class="sec-card warn reveal">
            <h3>What is not</h3>
            <ul>
              <li>Passwords are only as strong as you make them.</li>
              <li>Legacy v1 archives are not authenticated. Re-encrypt them.</li>
              <li>Standard zip encryption is whatever the format provides.</li>
              <li>Kolma is not a substitute for a reviewed threat model.</li>
            </ul>
          </div>
        </div>
        <div class="mac-line reveal">mac = HMAC-SHA256(mac_key, payload || header_without_tag)</div>
      </div>
    </section>

    <!-- Download -->
    <section id="download">
      <div class="container">
        <div class="section-head reveal">
          <span class="eyebrow">Get Kolma</span>
          <h2>Free, open source, checksummed.</h2>
          <p>Grab the desktop app for your platform, or install the CLI and terminal UI with one command.</p>
        </div>
        <div class="install-card reveal">
          <div class="install-bar">
            <span>sh · CLI + TUI</span>
            <button class="copy-btn" onclick={copyInstall}>{copied ? 'copied' : 'copy'}</button>
          </div>
          <div class="install-cmd"><span class="p">$ </span>{installCmd}</div>
        </div>
        <div class="dl-grid">
          <a class="dl-card reveal" href={releases} target="_blank" rel="noopener">
            <div class="os">Linux <span>AppImage + CLI</span></div>
            <div class="sub">x86_64 · arm64</div>
          </a>
          <a class="dl-card reveal" href={releases} target="_blank" rel="noopener">
            <div class="os">macOS <span>DMG + CLI</span></div>
            <div class="sub">Apple Silicon · Intel</div>
          </a>
          <a class="dl-card reveal" href={releases} target="_blank" rel="noopener">
            <div class="os">Windows <span>zip + CLI</span></div>
            <div class="sub">x64</div>
          </a>
        </div>
      </div>
    </section>
  </main>

  <footer>
    <div class="container foot">
      <div class="foot-links">
        <a href={repo} target="_blank" rel="noopener">GitHub</a>
        <a href={releases} target="_blank" rel="noopener">Releases</a>
        <a href="{repo}/blob/master/SECURITY.md" target="_blank" rel="noopener">Security</a>
        <a href="{repo}/blob/master/CHANGELOG.md" target="_blank" rel="noopener">Changelog</a>
      </div>
      <div class="by">MIT licensed. Built by <b>LangerSword</b>.</div>
    </div>
  </footer>
</div>
