/** The pages' markup. Static copy only (no visitor input goes through innerHTML). */

import { FIRMWARE_PLANS, LINKS, STEPS } from "./content.ts";

const pad = (n: number) => String(n).padStart(2, "0");
const ext = (href: string, text: string, cls = "") =>
  `<a class="${cls}" href="${href}" target="_blank" rel="noopener">${text}<span class="arrow" aria-hidden="true">↗</span></a>`;

export const GATE = `
  <section class="gate" aria-label="OP-FREE">
    <img class="gate__img" src="img/op-free-header.webp" width="1672" height="941" fetchpriority="high"
      alt="An OP-Z the size of a monolith standing in a trench on the moon, two astronauts in front of it, the Earth in the sky and the word OP-FREE above." />
    <div class="gate__shade" aria-hidden="true"></div>
    <h1 class="sr-only">OP-FREE</h1>
    <button class="gate__enter" type="button" data-enter>
      <span class="gate__key" aria-hidden="true"></span>
      <span class="gate__label">Enter</span>
    </button>
    <p class="gate__meta gate__meta--l label">community synths, effects &amp; research for the OP-Z</p>
    <p class="gate__meta gate__meta--r label">independent · not affiliated with teenage engineering</p>
  </section>`;

export const HEADER = (active: string) => `
  <header class="bar">
    <a class="bar__brand" href="#/home" aria-label="OP-FREE home">OP-FREE</a>
    <nav class="bar__nav" aria-label="Sections">
      <a href="#/synths" ${active === "synths" ? 'aria-current="page"' : ""}><i class="dot dot--green"></i>Synths &amp; effects</a>
      <a href="#/firmware" ${active === "firmware" ? 'aria-current="page"' : ""}><i class="dot dot--yellow"></i>Firmware mods</a>
      <a href="#/home#source"><i class="dot dot--blue"></i>Source</a>
    </nav>
  </header>`;

export const FOOTER = `
  <footer class="foot">
    <div class="foot__row">
      <p class="foot__brand">OP-FREE</p>
      <nav class="foot__links" aria-label="Footer">
        ${ext(LINKS.repo, "GitHub")}
        ${ext(LINKS.guides, "Guides")}
        ${ext(LINKS.roadmap, "Roadmap")}
        ${ext(LINKS.issues, "Report a problem")}
        <button type="button" class="foot__report" data-copy-report
          title="Copies this browser's problem log (errors, freezes, what you played) to paste into your report. Nothing is sent.">Copy report</button>
      </nav>
    </div>
    <p class="foot__fine">An independent community project. Not affiliated with, endorsed, sponsored or supported by
      teenage engineering; OP-Z is their product and trademark, named here only to say which hardware this works with.
      We host no firmware, no keys and none of teenage engineering's code or sounds. Provided as is, without warranty.
      Code under the MIT licence.</p>
  </footer>`;

const steps = () => STEPS.map((s, i) => `
  <li class="step" data-reveal>
    <p class="step__n label">${pad(i + 1)}</p>
    <h3 class="step__t">${s.title}</h3>
    <p class="step__w label">${s.where}</p>
    <p class="step__b">${s.body}</p>
  </li>`).join("");

export const HOME = `
  <main id="main" class="page home">
    <section class="intro">
      <div class="intro__strip" aria-hidden="true"><img src="img/op-free-header.webp" alt="" loading="eager" /></div>
      <p class="label" data-reveal>Welcome</p>
      <h1 class="intro__h" data-reveal="words">Welcome to OP-FREE, a first step into liberating your OP-Z.</h1>
      <p class="intro__p" data-reveal>This is initial experiments, research and thinking for opening up the OP-Z firmware.
        New synths and effects come first: they load next to the sounds your OP-Z already has, without changing its
        firmware, and you can take them off again. Firmware mods come later.</p>
    </section>

    <section class="how" aria-labelledby="how-h">
      <div class="section-head">
        <p class="label">How it works</p>
        <h2 id="how-h" class="h2" data-reveal="words">From the browser to your OP-Z in six steps.</h2>
      </div>
      <ol class="steps">${steps()}</ol>
    </section>

    <section class="blocks" aria-label="Sections">
      <a class="block block--green" href="#/synths" data-reveal>
        <p class="block__n label">01 · <i class="dot dot--green"></i> available now</p>
        <h2 class="block__h">Synths &amp; effects</h2>
        <p class="block__lead">Not ready to mod your firmware just yet? Try new synths and effects built by the community.</p>
        <p class="block__p">Play them in your browser, then load the ones you like onto your OP-Z, with their presets.
          No firmware change: the OP-Z's own sounds stay installed, and a restore puts everything back.</p>
        <span class="block__cta">Open the synth lab <span aria-hidden="true">→</span></span>
      </a>

      <a class="block block--yellow" href="#/firmware" data-reveal>
        <p class="block__n label">02 · <i class="dot dot--yellow"></i> research</p>
        <h2 class="block__h">Firmware mods</h2>
        <p class="block__soon">Coming soon</p>
        <p class="block__p">A builder that applies the mods you choose to your own firmware file. A 64-step sequencer,
          button debounce, Bluetooth audio, new arpeggiator and player styles, module updates.</p>
        <span class="block__cta">What's planned <span aria-hidden="true">→</span></span>
      </a>

      <section id="source" class="block block--blue block--static" data-reveal aria-labelledby="source-h">
        <p class="block__n label">03 · <i class="dot dot--blue"></i> open source</p>
        <h2 id="source-h" class="block__h">Source &amp; research</h2>
        <p class="block__p">Everything is public so you can take it further: build your own engine, check our findings,
          or help with the firmware.</p>
        <ul class="links">
          <li>${ext(LINKS.repo, "The repository")}<span class="label">code, issues, pull requests</span></li>
          <li>${ext(LINKS.quickstart, "SDK quickstart")}<span class="label">write, simulate and build an engine</span></li>
          <li>${ext(LINKS.research, "Research")}<span class="label">how the OP-Z firmware works</span></li>
          <li>${ext(LINKS.submitEngine, "Submit a synth or effect")}<span class="label">the pull request guide</span></li>
          <li>${ext(LINKS.extractFirmware, "Extract your own firmware")}<span class="label">for the firmware builder, later</span></li>
          <li>${ext(LINKS.proposeMod, "Propose a firmware mod")}<span class="label">ideas and research</span></li>
        </ul>
      </section>
    </section>
  </main>`;

export const SYNTHS = `
  <main id="main" class="page synths">
    <section class="page-head">
      <p class="label"><i class="dot dot--green"></i> 01 · Synths &amp; effects</p>
      <h1 class="h1" data-reveal="words">Try them here. Keep the ones you like.</h1>
      <p class="page-head__p" data-reveal>Every engine below runs in your browser exactly as it will on the OP-Z, so
        you can hear it before it goes near your device. When you're ready, the panel on the right walks you through
        putting your picks on the OP-Z.</p>
      <ol class="mini-steps" aria-label="The steps">
        ${STEPS.map((s, i) => `<li><span class="label">${pad(i + 1)}</span> ${s.title}</li>`).join("")}
      </ol>
    </section>
    <div class="synths__grid">
      <section class="panel" id="lab" aria-label="Synth lab"></section>
      <aside class="panel" id="loader" aria-label="Your OP-Z"></aside>
    </div>
  </main>`;

export const FIRMWARE = `
  <main id="main" class="page firmware">
    <section class="page-head">
      <p class="label"><i class="dot dot--yellow"></i> 02 · Firmware mods</p>
      <h1 class="h1" data-reveal="words">Coming soon.</h1>
      <p class="page-head__p" data-reveal>Some things a synth or effect can't reach: the sequencer, the buttons, the
        radio, the modules. Those need changes to the firmware itself. We're researching them now, and none of them
        is ready to install.</p>
    </section>
    <ul class="plans">
      ${FIRMWARE_PLANS.map((p, i) => `
        <li class="plan" data-reveal>
          <p class="label">${pad(i + 1)}</p>
          <h2 class="plan__t">${p.title}</h2>
          <p class="plan__b">${p.body}</p>
        </li>`).join("")}
    </ul>
    <section class="byo" data-reveal aria-labelledby="byo-h">
      <h2 id="byo-h" class="h2">Bring your own firmware</h2>
      <p>We will never host the OP-Z firmware or the keys to it. The builder will take the firmware file from your
        own OP-Z or your own download, apply the mods you tick, in your browser, and give you a file to install
        through the OP-Z's update mode. The guide below explains how to get your firmware file ready.</p>
      <p class="byo__links">
        ${ext(LINKS.extractFirmware, "Extract your own firmware", "btn btn--ghost")}
        ${ext(LINKS.firmwareExplained, "How the firmware works", "btn btn--ghost")}
        ${ext(LINKS.proposeMod, "Propose a mod", "btn btn--ghost")}
      </p>
    </section>
  </main>`;
