(() => {
  const S = {};
  const RAW_ID = {};
  const SWITCHES = [
    { key: "pool_filter_pump", name: "Pool Filter Pump", label: "Filter Pump" },
    { key: "pool_spa_mode", name: "Pool Spa Mode", label: "Spa Mode" },
    { key: "pool_heater", name: "Pool Heater", label: "Pool Heater" },
    { key: "spa_heater", name: "Spa Heater", label: "Spa Heater" },
    { key: "spa_booster_pump", name: "Spa Booster Pump", label: "Spa Booster Pump" },
    { key: "pool_air_blower", name: "Pool Air Blower", label: "Air Blower" },
    { key: "pool_light", name: "Pool Light", label: "Pool Light" },
    { key: "pool_color_wheel", name: "Pool Color Wheel", label: "Color Wheel" },
    { key: "pool_spillover_waterfall", name: "Pool Spillover Waterfall", label: "Spillover Waterfall" },
    { key: "pool_acid_dosing", name: "Pool Acid Dosing", label: "Acid Dosing (Manual)" },
  ];

  function el(id) { return document.getElementById(id); }

  function normId(raw) {
    if (!raw) return "";
    return raw.toLowerCase().replace("/", "-").replace(/[^a-z0-9-]+/g, "_").replace(/^_|_$/g, "");
  }

  function postAction(domain, fallbackName, action) {
    const key = normId(`${domain}/${fallbackName}`);
    const raw = RAW_ID[key] || `${domain}/${fallbackName}`;
    const path = "/" + raw.split("/").map(encodeURIComponent).join("/") + "/" + action;
    return fetch(path, { method: "POST" });
  }

  function mount() {
    if (el("aq-pro-root")) return;
    document.body.classList.add("aq-hide-native");
    const root = document.createElement("div");
    root.id = "aq-pro-root";
    root.innerHTML = `
      <div class="aq-topbar">
        <div class="aq-brand">
          <div class="aq-logo">AQ</div>
          <div>
            <div class="aq-title">AquaLink RS-8 Pro</div>
            <div class="aq-sub">ESP32-S3 Direct RS485 Pool Controller</div>
          </div>
        </div>
        <div class="aq-pills">
          <span id="aq-mode-pill" class="aq-pill">POOL MODE</span>
          <span id="aq-bus-pill" class="aq-pill ok">BUS: 0 ERR</span>
          <span id="aq-interlock-pill" class="aq-pill warn">INTERLOCK: --</span>
          <button id="aq-toggle-logs" class="aq-btn-ghost">Show Live Logs</button>
        </div>
      </div>

      <div class="aq-kpi-grid">
        <div class="aq-card">
          <div class="aq-kpi-label">Pool Water</div>
          <div id="aq-pool-temp" class="aq-kpi-val">--°F</div>
          <div class="aq-kpi-meta">Live RS485 Display</div>
        </div>
        <div class="aq-card">
          <div class="aq-kpi-label">Spa Water</div>
          <div id="aq-spa-temp" class="aq-kpi-val">--°F</div>
          <div class="aq-kpi-meta">Active in Spa Mode</div>
        </div>
        <div class="aq-card">
          <div class="aq-kpi-label">Air Temp</div>
          <div id="aq-air-temp" class="aq-kpi-val">--°F</div>
          <div class="aq-kpi-meta">Ambient Pad Sensor</div>
        </div>
        <div class="aq-card">
          <div class="aq-kpi-label">Pump Hydraulics</div>
          <div id="aq-rpm" class="aq-kpi-val">-- RPM</div>
          <div id="aq-watts" class="aq-kpi-meta">-- W</div>
          <div class="aq-bar"><span id="aq-rpm-bar" style="width:0%"></span></div>
        </div>
      </div>

      <div class="aq-two-col">
        <div class="aq-card">
          <div class="aq-section-title">
            <span>Water Chemistry & Acid Dosing</span>
            <span id="aq-swg-gen" class="aq-pill">SWG: --</span>
          </div>
          <div class="aq-chem-row">
            <div class="aq-metric">
              <div class="aq-kpi-label">pH (Target 7.2–7.6)</div>
              <div id="aq-ph" class="m-val">--</div>
              <div class="aq-bar"><span id="aq-ph-bar" style="width:0%"></span></div>
            </div>
            <div class="aq-metric">
              <div class="aq-kpi-label">ORP</div>
              <div id="aq-orp" class="m-val">-- mV</div>
              <div class="aq-bar"><span id="aq-orp-bar" style="width:0%"></span></div>
            </div>
            <div class="aq-metric">
              <div class="aq-kpi-label">Salt / SWG Output</div>
              <div id="aq-salt" class="m-val">-- ppm</div>
              <div id="aq-swg-pct" class="aq-kpi-meta">Output: --%</div>
            </div>
          </div>
          <div class="aq-acid-box">
            <div>
              <div class="aq-kpi-label" style="margin-bottom:2px">Acid Dosing Controller</div>
              <div id="aq-acid-state" style="font-weight:700;font-size:14px">--</div>
            </div>
            <div style="display:flex;gap:8px;align-items:center">
              <button id="aq-acid-auto-btn" class="aq-btn-ghost">Auto Dosing: --</button>
              <button id="aq-acid-estop" class="aq-btn-estop">E-STOP</button>
            </div>
          </div>
        </div>

        <div class="aq-card">
          <div class="aq-section-title">
            <span>Equipment Control Deck</span>
            <span id="aq-polls" class="aq-kpi-meta" style="margin:0">Polls: 0</span>
          </div>
          <div id="aq-equip-grid" class="aq-equip-grid">
            ${SWITCHES.map(
              (sw) => `<button class="aq-tile" data-key="${sw.key}" data-name="${sw.name}">
                <span class="aq-tile-name">${sw.label}</span>
                <span class="aq-tile-state" id="st-${sw.key}">OFF</span>
              </button>`
            ).join("")}
          </div>
        </div>
      </div>

      <div id="aq-logs-card" class="aq-card" style="display:none;margin-top:16px">
        <div class="aq-section-title">
          <span>Live RS485 / ESPHome Console</span>
          <button id="aq-clear-logs" class="aq-btn-ghost">Clear</button>
        </div>
        <pre id="aq-logs-pre" style="margin:0;max-height:300px;overflow:auto;font-size:12px;line-height:1.4;color:#93c5fd;background:#070d19;padding:12px;border-radius:10px;border:1px solid rgba(148,163,184,.12)"></pre>
      </div>
    `;
    document.body.prepend(root);

    el("aq-toggle-logs").onclick = () => {
      const card = el("aq-logs-card");
      const open = card.style.display === "none";
      card.style.display = open ? "block" : "none";
      el("aq-toggle-logs").textContent = open ? "Hide Live Logs" : "Show Live Logs";
    };
    el("aq-clear-logs").onclick = () => { el("aq-logs-pre").textContent = ""; };

    el("aq-acid-auto-btn").onclick = () => postAction("switch", "Pool Acid Auto Dosing", "toggle");
    el("aq-acid-estop").onclick = () => postAction("button", "Pool Acid Emergency Stop", "press");

    root.querySelectorAll(".aq-tile").forEach((btn) => {
      btn.onclick = () => postAction("switch", btn.dataset.name, "toggle");
    });
  }

  function render() {
    const val = (k) => (S[k] && S[k].state !== undefined ? S[k].state : null);
    const num = (k) => (S[k] && typeof S[k].value === "number" ? S[k].value : parseFloat(val(k)));

    if (val("sensor-pool_water_temp")) el("aq-pool-temp").textContent = val("sensor-pool_water_temp");
    if (val("sensor-spa_water_temp")) el("aq-spa-temp").textContent = val("sensor-spa_water_temp");
    if (val("sensor-pool_air_temp")) el("aq-air-temp").textContent = val("sensor-pool_air_temp");

    const rpm = num("sensor-pool_pump_speed");
    if (!isNaN(rpm)) {
      el("aq-rpm").textContent = `${Math.round(rpm)} RPM`;
      el("aq-rpm-bar").style.width = `${Math.min(100, Math.max(0, (rpm / 3450) * 100))}%`;
    }
    if (val("sensor-pool_pump_watts")) el("aq-watts").textContent = val("sensor-pool_pump_watts");

    const ph = num("sensor-pool_ph");
    if (!isNaN(ph)) {
      el("aq-ph").textContent = ph.toFixed(1);
      el("aq-ph-bar").style.width = `${Math.min(100, Math.max(0, ((ph - 6.8) / 1.2) * 100))}%`;
    }
    const orp = num("sensor-pool_orp");
    if (!isNaN(orp)) {
      el("aq-orp").textContent = `${Math.round(orp)} mV`;
      el("aq-orp-bar").style.width = `${Math.min(100, Math.max(0, ((orp - 400) / 450) * 100))}%`;
    }
    if (val("sensor-pool_salt_level")) el("aq-salt").textContent = val("sensor-pool_salt_level");
    if (val("sensor-pool_salt_chlorinator_output")) {
      el("aq-swg-pct").textContent = `Output: ${val("sensor-pool_salt_chlorinator_output")}`;
    }

    const gen = val("binary_sensor-pool_salt_chlorinator_generating");
    if (gen) {
      const p = el("aq-swg-gen");
      p.textContent = gen === "ON" ? "SWG: GENERATING" : "SWG: IDLE";
      p.className = `aq-pill ${gen === "ON" ? "ok" : ""}`;
    }

    if (val("text_sensor-pool_acid_controller_state")) {
      el("aq-acid-state").textContent = val("text_sensor-pool_acid_controller_state");
    }
    const auto = val("switch-pool_acid_auto_dosing");
    if (auto) el("aq-acid-auto-btn").textContent = `Auto Dosing: ${auto}`;

    const spa = val("binary_sensor-pool_spa_mode_status");
    if (spa) {
      const mp = el("aq-mode-pill");
      mp.textContent = spa === "ON" ? "SPA MODE" : "POOL MODE";
      mp.className = `aq-pill ${spa === "ON" ? "spa" : "ok"}`;
    }

    const err = val("sensor-pool_bus_checksum_errors");
    if (err !== null) {
      const bp = el("aq-bus-pill");
      bp.textContent = `BUS: ${err} ERR`;
      bp.className = `aq-pill ${err === "0" ? "ok" : "warn"}`;
    }
    if (val("sensor-pool_keypad_polls_answered")) {
      el("aq-polls").textContent = `Polls: ${val("sensor-pool_keypad_polls_answered")}`;
    }

    const il = val("switch-pool_master_control_interlock");
    if (il) {
      const ip = el("aq-interlock-pill");
      ip.textContent = `INTERLOCK: ${il}`;
      ip.className = `aq-pill ${il === "ON" ? "ok" : "warn"}`;
    }

    SWITCHES.forEach((sw) => {
      const st = val(`switch-${sw.key}`);
      if (!st) return;
      const isOn = st === "ON";
      const badge = el(`st-${sw.key}`);
      if (badge) {
        badge.textContent = st;
        badge.closest(".aq-tile").classList.toggle("on", isOn);
      }
    });
  }

  function start() {
    mount();
    const es = new EventSource("/events");
    es.addEventListener("state", (e) => {
      try {
        const d = JSON.parse(e.data);
        if (d && d.id) {
          const k = normId(d.id);
          S[k] = d;
          RAW_ID[k] = d.id;
          render();
        }
      } catch (_) {}
    });
    es.addEventListener("log", (e) => {
      const pre = el("aq-logs-pre");
      if (!pre) return;
      const line = (e.data || "").replace(/\x1b\[[0-9;]*m/g, "");
      pre.textContent += line + "\n";
      if (pre.textContent.length > 25000) {
        pre.textContent = pre.textContent.slice(-20000);
      }
      pre.scrollTop = pre.scrollHeight;
    });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", start);
  } else {
    start();
  }
})();
