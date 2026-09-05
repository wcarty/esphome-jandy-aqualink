(() => {
  const appStyles = `
    :host {
      --c-bg: #071d2b;
      --c-text: #edf8fc;
      --c-primary: #36c9da;
      --c-pri-rgb: 54, 201, 218;
      color: #edf8fc;
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
  `;

  const installStyles = (element, id, styles) => {
    const root = element.shadowRoot;
    if (!root || root.querySelector(`#${id}`)) return;
    const style = document.createElement("style");
    style.id = id;
    style.textContent = styles;
    root.append(style);
  };

  const decorate = () => {
    const app = document.querySelector("esp-app");
    if (!app) return;
    installStyles(app, "pool-dashboard-app-theme", appStyles);
    const appRoot = app.shadowRoot;
    if (!appRoot) return;
    appRoot.querySelectorAll("esp-entity-table").forEach((table) => {
      installStyles(table, "pool-dashboard-entities-theme", entityStyles);
    });
  };

  customElements.whenDefined("esp-app").then(() => {
    decorate();
    new MutationObserver(decorate).observe(document.body, { childList: true, subtree: true });
  });
})();
