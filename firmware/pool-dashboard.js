(() => {
  const appStyles = `
    :host {
      --c-bg: #071d2b;
      --c-text: #edf8fc;
      --c-primary: #36c9da;
      --c-pri-rgb: 54, 201, 218;
      color: #edf8fc;
    }
    :host([data-pool-theme="light"]) {
      --c-bg: #eaf6f8;
      --c-text: #123342;
      --c-primary: #087d99;
      --c-pri-rgb: 8, 125, 153;
      color: #123342;
    }
    header {
      align-items: center;
      background: linear-gradient(120deg, #06384e, #087b9a 55%, #1eb6c8) !important;
      border-bottom: 1px solid rgba(164, 239, 244, 0.36);
      box-shadow: 0 12px 32px rgba(0, 12, 20, 0.32);
      min-height: 76px;
      padding: 0.55rem 1.25rem;
    }
    header #logo { display: none; }
    header h1 {
      color: #fff;
      font-family: Inter, ui-sans-serif, system-ui, sans-serif;
      font-size: clamp(1.3rem, 4vw, 1.8rem);
      font-weight: 750;
      letter-spacing: 0.035em;
      margin: 0;
      text-transform: uppercase;
    }
    header h1::before {
      color: #9ff5fb;
      content: "POOL COMMAND  /  ";
      font-size: 0.56em;
      font-weight: 700;
      letter-spacing: 0.16em;
      vertical-align: middle;
    }
    header div {
      color: #c7f8fb;
      font-family: Inter, ui-sans-serif, system-ui, sans-serif;
      font-size: 0.78rem;
      letter-spacing: 0.06em;
      text-transform: uppercase;
    }
    #pool-theme-toggle {
      background: rgba(2, 34, 48, 0.25);
      border: 1px solid rgba(235, 254, 255, 0.55);
      border-radius: 999px;
      color: #fff;
      cursor: pointer;
      font: 700 0.72rem/1 Inter, ui-sans-serif, system-ui, sans-serif;
      letter-spacing: 0.07em;
      margin-left: auto;
      padding: 0.6rem 0.75rem;
      text-transform: uppercase;
    }
    #pool-theme-toggle:hover { background: rgba(2, 34, 48, 0.42); }
    :host([data-pool-theme="light"]) header {
      background: linear-gradient(120deg, #e7fbfd, #a5eaf0 55%, #6ccbd8) !important;
      border-bottom-color: rgba(8, 91, 111, 0.25);
    }
    :host([data-pool-theme="light"]) header h1 { color: #123342; }
    :host([data-pool-theme="light"]) header h1::before,
    :host([data-pool-theme="light"]) header div { color: #14657a; }
    :host([data-pool-theme="light"]) #pool-theme-toggle {
      background: rgba(255, 255, 255, 0.58);
      border-color: rgba(8, 91, 111, 0.28);
      color: #164e61;
    }
    main {
      gap: 1.25rem;
      margin: 0 auto;
      max-width: 1180px;
      padding: 1.25rem;
    }
  `;

  const entityStyles = `
    :host {
      color: #e7f5fa;
      font-family: Inter, ui-sans-serif, system-ui, sans-serif;
    }
    :host-context(esp-app[data-pool-theme="light"]) { color: #153b4a; }
    .tab-header {
      background: linear-gradient(90deg, #0b3b51, #0e5269);
      border: 1px solid rgba(105, 220, 232, 0.24);
      border-bottom: 0;
      border-radius: 14px 14px 0 0;
      color: #a9f2f7;
      font-size: 0.76rem;
      font-weight: 750;
      letter-spacing: 0.12em;
      margin-top: 1rem;
      padding: 0.8rem 1rem;
      text-transform: uppercase;
    }
    .tab-container {
      background: rgba(7, 31, 44, 0.92);
      border: 1px solid rgba(105, 220, 232, 0.24);
      border-radius: 0 0 14px 14px;
      box-shadow: 0 10px 28px rgba(0, 8, 14, 0.2);
      overflow: hidden;
    }
    .entity-row {
      border-bottom: 1px solid rgba(135, 224, 233, 0.08);
      min-height: 52px;
      padding: 0.15rem 0.35rem;
    }
    .entity-row:nth-child(2n) { background: rgba(21, 87, 108, 0.18); }
    .entity-row > :nth-child(1) { color: #55dbe7; }
    .entity-row > :nth-child(2) {
      color: #edf9fb;
      font-weight: 600;
    }
    .entity-row > :nth-child(3) { color: #9deaf0; font-weight: 700; }
    button, .btn {
      background: linear-gradient(180deg, #1595ac, #087087) !important;
      border: 1px solid rgba(168, 246, 250, 0.55) !important;
      border-radius: 9px !important;
      box-shadow: 0 4px 12px rgba(0, 0, 0, 0.22);
      color: #fff !important;
      font-weight: 700;
      min-height: 32px !important;
    }
    button:hover, .btn:hover { filter: brightness(1.16); }
    input[type="range"] { accent-color: #41d7e3; }
    :host-context(esp-app[data-pool-theme="light"]) .tab-header {
      background: linear-gradient(90deg, #cbeff3, #e5f8f9);
      border-color: rgba(8, 104, 128, 0.2);
      color: #156176;
    }
    :host-context(esp-app[data-pool-theme="light"]) .tab-container {
      background: rgba(255, 255, 255, 0.94);
      border-color: rgba(8, 104, 128, 0.2);
      box-shadow: 0 10px 28px rgba(20, 75, 92, 0.12);
    }
    :host-context(esp-app[data-pool-theme="light"]) .entity-row {
      border-bottom-color: rgba(8, 104, 128, 0.1);
    }
    :host-context(esp-app[data-pool-theme="light"]) .entity-row:nth-child(2n) {
      background: rgba(118, 220, 230, 0.13);
    }
    :host-context(esp-app[data-pool-theme="light"]) .entity-row > :nth-child(1) { color: #087d99; }
    :host-context(esp-app[data-pool-theme="light"]) .entity-row > :nth-child(2) { color: #173f4e; }
    :host-context(esp-app[data-pool-theme="light"]) .entity-row > :nth-child(3) { color: #0a6980; }
  `;

  const switchStyles = `
    :host {
      display: inline-flex;
      min-height: 40px;
      min-width: 64px;
      align-items: center;
      justify-content: center;
    }
    .sw label { display: inline-flex; padding: 5px; }
    .lever {
      background: #203e4c !important;
      border: 1px solid rgba(154, 220, 229, 0.2);
      box-shadow: inset 0 1px 3px rgba(0, 0, 0, 0.45);
      height: 30px !important;
      overflow: hidden;
      width: 58px !important;
    }
    .lever:before {
      background: transparent !important;
      color: #9fbac4;
      content: "OFF" !important;
      font-family: Inter, ui-sans-serif, system-ui, sans-serif;
      font-size: 9px;
      font-weight: 800;
      height: 30px !important;
      left: 28px !important;
      letter-spacing: 0.07em;
      line-height: 30px;
      text-align: center;
      top: 0 !important;
      transform: none !important;
      width: 30px !important;
    }
    .lever:after {
      background: linear-gradient(145deg, #faffff, #b8d5dc) !important;
      box-shadow: 0 2px 7px rgba(0, 0, 0, 0.45) !important;
      height: 24px !important;
      left: 3px !important;
      top: 2px !important;
      width: 24px !important;
    }
    input[type="checkbox"]:checked + .lever {
      background: linear-gradient(90deg, #087e8e, #20bdc8) !important;
      border-color: rgba(174, 250, 252, 0.76);
      box-shadow: 0 0 14px rgba(54, 224, 232, 0.45), inset 0 1px 3px rgba(0, 0, 0, 0.22);
    }
    input[type="checkbox"]:checked + .lever:before {
      color: #f5ffff;
      content: "ON" !important;
      left: 1px !important;
    }
    input[type="checkbox"]:checked + .lever:after { left: 31px !important; }
    input[type="checkbox"]:focus-visible + .lever {
      outline: 2px solid #b9fbff;
      outline-offset: 3px;
    }
    input[type="checkbox"][disabled] + .lever {
      filter: grayscale(1);
      opacity: 0.5;
    }
    :host-context(esp-app[data-pool-theme="light"]) .lever {
      background: #b9d5dc !important;
      border-color: rgba(6, 98, 121, 0.22);
    }
    :host-context(esp-app[data-pool-theme="light"]) .lever:before { color: #315d6b; }
    :host-context(esp-app[data-pool-theme="light"]) .lever:after {
      background: linear-gradient(145deg, #fff, #d7edf1) !important;
    }
  `;

  const installStyles = (element, id, styles) => {
    const root = element.shadowRoot;
    if (!root || root.querySelector(`#${id}`)) return;
    const style = document.createElement("style");
    style.id = id;
    style.textContent = styles;
    root.append(style);
  };

  const observedRoots = new WeakSet();
  const themeStorageKey = "pool-dashboard-theme";
  const observe = (root) => {
    if (observedRoots.has(root)) return;
    new MutationObserver(decorate).observe(root, { childList: true, subtree: true });
    observedRoots.add(root);
  };

  const decorate = () => {
    const app = document.querySelector("esp-app");
    if (!app) return;
    const savedTheme = localStorage.getItem(themeStorageKey);
    const theme = savedTheme === "light" ? "light" : "dark";
    app.dataset.poolTheme = theme;
    document.documentElement.dataset.poolTheme = theme;
    installStyles(app, "pool-dashboard-app-theme", appStyles);
    const appRoot = app.shadowRoot;
    if (!appRoot) return;
    observe(appRoot);
    const header = appRoot.querySelector("header");
    if (header && !header.querySelector("#pool-theme-toggle")) {
      const toggle = document.createElement("button");
      toggle.id = "pool-theme-toggle";
      toggle.type = "button";
      toggle.addEventListener("click", () => {
        const nextTheme = app.dataset.poolTheme === "light" ? "dark" : "light";
        app.dataset.poolTheme = nextTheme;
        document.documentElement.dataset.poolTheme = nextTheme;
        localStorage.setItem(themeStorageKey, nextTheme);
        toggle.textContent = nextTheme === "light" ? "☾ Dark" : "☀ Light";
        toggle.setAttribute("aria-label", `Switch to ${nextTheme} mode`);
      });
      header.append(toggle);
    }
    const themeToggle = header?.querySelector("#pool-theme-toggle");
    if (themeToggle) {
      themeToggle.textContent = theme === "light" ? "☾ Dark" : "☀ Light";
      themeToggle.setAttribute(
        "aria-label",
        `Switch to ${theme === "light" ? "dark" : "light"} mode`
      );
    }
    appRoot.querySelectorAll("esp-entity-table").forEach((table) => {
      installStyles(table, "pool-dashboard-entities-theme", entityStyles);
      const tableRoot = table.shadowRoot;
      if (!tableRoot) return;
      observe(tableRoot);
      tableRoot.querySelectorAll("esp-switch").forEach((toggle) => {
        installStyles(toggle, "pool-dashboard-switch-theme", switchStyles);
      });
    });
  };

  Promise.all([
    customElements.whenDefined("esp-app"),
    customElements.whenDefined("esp-entity-table"),
    customElements.whenDefined("esp-switch"),
  ]).then(() => {
    decorate();
    new MutationObserver(decorate).observe(document.body, { childList: true, subtree: true });
  });
})();
