"use strict";
// The seven public actions are mode-based. Legacy IDs can still be imported
// and edited without converting or silently changing existing workflows.
const ACTIONS = [
  {
    id: "place_items",
    title: "Place items",
    desc: "Pick a memory node for new items",
    icon: "▣",
    color: "#e8eeff",
    ink: "#5370d6",
    modes: {
      adaptive: "Adaptive (DRAM first)",
      local: "Local node",
      interleave: "Random interleave",
      round_robin: "Round robin",
      weighted: "Weighted random",
      pressure: "Least pressure",
      cxl: "Size-aware CXL",
      weighted_pressure: "Pressure-weighted",
      latency: "Lowest access cost",
    },
    fields: {
      adaptive: [
        [
          "threshold",
          "DRAM pressure limit",
          "0.8",
          "Spill when DRAM pressure exceeds this fraction.",
        ],
      ],
      local: [["node", "Target node", "0", "NUMA node index."]],
      cxl: [
        [
          "min_size",
          "CXL minimum size (bytes)",
          "1024",
          "Larger values go to the CXL tier.",
        ],
      ],
    },
  },
  {
    id: "score_items",
    title: "Score items",
    desc: "Measure heat, frequency or value",
    icon: "◈",
    color: "#fff2e1",
    ink: "#d89238",
    modes: {
      hotness: "Hotness",
      frequency: "Frequency estimate",
      blend: "Blend signals (EWMA)",
      benefit: "Migration benefit",
      decay_hotness: "Decay hotness",
    },
    fields: {
      blend: [
        ["alpha", "Frequency weight", "0.4", "Weight for estimated frequency."],
        ["beta", "Recency weight", "0.4", "Weight for recency."],
        ["gamma", "Hotness weight", "0.2", "Weight for hotness."],
      ],
    },
  },
  {
    id: "filter_items",
    title: "Filter items",
    desc: "Keep only matching candidates",
    icon: "▽",
    color: "#fcebf1",
    ink: "#d26994",
    modes: {
      hot: "Hot items",
      frequent: "Frequent items",
      cold: "Cold items",
      remote: "Remote items",
      local: "Local items",
      size_min: "At least this size",
      size_max: "At most this size",
      benefit: "Positive benefit",
    },
    fields: {
      hot: [
        [
          "threshold",
          "Minimum hotness",
          "5",
          "Keep items at or above this score.",
        ],
      ],
      cold: [
        ["threshold", "Maximum hotness", "2", "Keep items below this score."],
      ],
      frequent: [
        [
          "threshold",
          "Minimum frequency",
          "2",
          "Keep items at or above this estimate.",
        ],
      ],
      remote: [
        ["node", "Reference node", "0", "Keep items outside this node."],
      ],
      local: [["node", "Reference node", "0", "Keep items on this node."]],
      size_min: [
        ["min", "Minimum bytes", "4096", "Small items are discarded."],
      ],
      size_max: [
        ["max", "Maximum bytes", "65536", "Large items are discarded."],
      ],
      benefit: [
        [
          "threshold",
          "Minimum benefit",
          "0",
          "Keep items with benefit above this value.",
        ],
      ],
    },
  },
  {
    id: "rank_items",
    title: "Rank items",
    desc: "Order items by priority",
    icon: "≡",
    color: "#e4f6ef",
    ink: "#39a17c",
    modes: {
      hotness: "Hotness",
      recent: "Most recent",
      frequency: "Frequency",
      benefit: "Migration benefit",
      blend: "Blended score",
      size: "Largest size",
    },
  },
  {
    id: "route_items",
    title: "Choose destination",
    desc: "Pick a target or limit the batch",
    icon: "⑂",
    color: "#eeeaff",
    ink: "#8c6aca",
    modes: { destination: "Best destination", budget: "Limit to a budget" },
    fields: {
      destination: [
        [
          "require_benefit",
          "Only profitable moves",
          "false",
          "Use true to reject unprofitable migrations.",
        ],
      ],
      budget: [
        ["budget", "Max candidates", "256", "Keep this many candidates."],
      ],
    },
  },
  {
    id: "move_items",
    title: "Move items",
    desc: "Migrate, demote or rebalance",
    icon: "↗",
    color: "#ffeae8",
    ink: "#de786a",
    modes: {
      migrate: "Apply migrations",
      demote: "Demote cold items",
      balance: "Rebalance nodes",
    },
    fields: {
      demote: [
        [
          "threshold",
          "Maximum frequency",
          "1",
          "Move lower-frequency DRAM items to CXL.",
        ],
        ["dram_node", "DRAM node", "0", "Source node."],
        ["cxl_node", "CXL node", "1", "Destination node."],
      ],
    },
  },
  {
    id: "track_items",
    title: "Track activity",
    desc: "Record accesses and frequency",
    icon: "◎",
    color: "#e3f4f8",
    ink: "#47a2b9",
    modes: {
      access: "Record accesses",
      observe: "Observe keys (CMS)",
      decay: "Decay frequency counters",
    },
  },
];
function initialLanguage() {
  try {
    const saved = localStorage.getItem("numaflow-language");
    if (saved === "en" || saved === "zh") return saved;
  } catch {
    /* storage may be disabled */
  }
  return navigator.language.toLowerCase().startsWith("zh") ? "zh" : "en";
}
const $ = (s) => document.querySelector(s);
const state = {
  nodes: [],
  edges: [],
  nextId: 1,
  selected: null,
  selectedEdge: null,
  zoom: 1,
  pan: { x: 0, y: 0 },
  gesture: null,
  draft: null,
  ops: [],
  lang: initialLanguage(),
  status: "ready",
  outputState: "idle",
  outputRaw: "",
  templates: [],
  isStarter: false,
  description: "Built in NUMAflow workflow studio",
};
const W = 216,
  H = 72;
const action = (id) => ACTIONS.find((a) => a.id === id);
const node = (id) => state.nodes.find((n) => n.id === id);
const h = (text) =>
  String(text ?? "").replace(
    /[&<>"']/g,
    (c) =>
      ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[
        c
      ],
  );
function tr(key, args = {}) {
  const template = LOCALES[state.lang].ui[key] || LOCALES.en.ui[key] || key;
  return template.replace(/\{(\w+)\}/g, (_, name) => String(args[name] ?? ""));
}
function displayAction(a) {
  const translated = LOCALES[state.lang].guides[a.id];
  return {
    ...a,
    title: translated.title || a.title,
    desc: translated.desc || a.desc,
    modes: { ...a.modes, ...translated.names },
  };
}
function legacyMode(op) {
  return LEGACY_MODES[op];
}
function label(n) {
  const a = action(n.op);
  if (a) return displayAction(a).title;
  const mapping = legacyMode(n.op);
  if (state.lang === "zh" && mapping) {
    const guide = LOCALES.zh.guides[mapping[0]];
    return tr("legacyPrefix") + guide.names[mapping[1]];
  }
  return (
    state.ops.find((o) => o.name === n.op)?.title || n.op.replaceAll("_", " ")
  );
}
function modeLabel(n) {
  const a = action(n.op);
  if (a)
    return (
      displayAction(a).modes[n.params.mode || Object.keys(a.modes)[0]] ||
      n.params.mode
    );
  return state.lang === "zh"
    ? tr("legacyAction")
    : "Legacy · " +
        (state.ops.find((o) => o.name === n.op)?.category || "action");
}
function renderTemplates() {
  const select = $("#templateSelect");
  const chosen = select.value;
  select.replaceChildren();
  const first = document.createElement("option");
  first.value = "";
  first.textContent = tr("chooseTemplate");
  select.append(first);
  const groups = new Map();
  for (const t of state.templates) {
    if (!t.name || !t.category) continue;
    let group = groups.get(t.category);
    if (!group) {
      group = document.createElement("optgroup");
      group.label = LOCALES[state.lang].categories?.[t.category] || t.category;
      groups.set(t.category, group);
      select.append(group);
    }
    const option = document.createElement("option");
    option.value = t.name;
    option.textContent =
      LOCALES[state.lang].templates?.[t.name] || t.description || t.name;
    option.title = `${t.name} — ${option.textContent}`;
    group.append(option);
  }
  select.value = chosen;
  $("#loadTemplateBtn").disabled = !select.value;
}
function renderOutput() {
  let out = tr("noRun");
  if (state.outputState === "running") out = tr("running");
  if (state.outputState === "offline") out = tr("serverOffline");
  if (["done", "failed"].includes(state.outputState)) {
    out = state.outputRaw || tr("engineEmpty");
    if (state.lang === "zh") {
      const match = out.match(
        /^workflow=(.*?) nodes=(\d+) edges=(\d+)\s+execution=OK result_items=(\d+) migrations=(\d+)/,
      );
      out = match
        ? tr("runSummary", {
            name: match[1],
            nodes: match[2],
            edges: match[3],
            items: match[4],
            migrations: match[5],
          }) +
          "\n\n" +
          tr("rawOutput") +
          ":\n" +
          out
        : tr(state.outputState === "done" ? "runDone" : "runFailed") +
          "\n" +
          out;
    }
  }
  $("#outputText").textContent = out;
  $("#toggleOutput").innerHTML =
    h(tr($("#outputPanel").hidden ? "showOutput" : "hideOutput")) +
    " <span>" +
    ($("#outputPanel").hidden ? "⌄" : "⌃") +
    "</span>";
}
function setLanguage(lang) {
  const formerName = LOCALES[state.lang].ui.starterName;
  state.lang = lang === "zh" ? "zh" : "en";
  try {
    localStorage.setItem("numaflow-language", state.lang);
  } catch {
    /* optional */
  }
  $("#languageSelect").value = state.lang;
  document.documentElement.lang = state.lang === "zh" ? "zh-CN" : "en";
  document.title = tr("pageTitle");
  if (state.isStarter && $("#workflowName").value === formerName)
    $("#workflowName").value = tr("starterName");
  document.querySelectorAll("[data-i18n]").forEach((el) => {
    el.textContent = tr(el.dataset.i18n);
  });
  for (const [attr, property] of [
    ["data-i18n-title", "title"],
    ["data-i18n-aria", "aria-label"],
    ["data-i18n-placeholder", "placeholder"],
  ]) {
    document
      .querySelectorAll(`[${attr}]`)
      .forEach((el) => el.setAttribute(property, tr(el.getAttribute(attr))));
  }
  renderTemplates();
  refreshPalette();
  render();
  renderInspector();
  renderOutput();
  setStatus(state.status);
}
let toastTimer;
function toast(message, error = false) {
  const el = $("#toast");
  el.textContent = message;
  el.classList.toggle("error", error);
  el.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove("show"), 3500);
}
function setStatus(key) {
  state.status = key;
  $("#statusText").innerHTML = '<span class="status-dot"></span> ' + h(tr(key));
}
function refreshPalette() {
  const q = $("#actionSearch").value.trim().toLowerCase();
  const list = $("#actionList");
  list.replaceChildren();
  for (const source of ACTIONS.filter((a) => {
    const translated = displayAction(a);
    return [
      a.title,
      a.desc,
      ...Object.values(a.modes),
      translated.title,
      translated.desc,
      ...Object.values(translated.modes),
    ]
      .join(" ")
      .toLowerCase()
      .includes(q);
  })) {
    const a = displayAction(source);
    const b = document.createElement("button");
    b.type = "button";
    b.className = "action-card";
    b.setAttribute("aria-label", tr("addAction", { name: a.title }));
    b.title = LOCALES[state.lang].guides[a.id].purpose;
    b.innerHTML = `<span class="action-icon" style="background:${a.color};color:${a.ink}">${a.icon}</span><span class="action-copy"><strong>${a.title}</strong><small>${a.desc}</small></span><span class="action-plus">+</span>`;
    b.addEventListener("click", () => addNode(source.id));
    list.append(b);
  }
  if (!list.children.length) {
    const p = document.createElement("p");
    p.className = "panel-intro";
    p.textContent = tr("noMatches");
    list.append(p);
  }
}
function worldPoint(e) {
  const r = $("#canvas").getBoundingClientRect();
  return {
    x: (e.clientX - r.left - state.pan.x) / state.zoom,
    y: (e.clientY - r.top - state.pan.y) / state.zoom,
  };
}
function screenCenter() {
  const r = $("#canvas").getBoundingClientRect();
  return {
    x: (r.width / 2 - state.pan.x) / state.zoom,
    y: (r.height / 2 - state.pan.y) / state.zoom,
  };
}
function addNode(op) {
  const a = action(op);
  const prev = node(state.selected);
  const center = screenCenter();
  const n = {
    id: "n" + state.nextId++,
    op,
    params: { mode: Object.keys(a.modes)[0] },
    x: prev ? prev.x + W + 80 : center.x - W / 2,
    y: prev ? prev.y : center.y - H / 2,
  };
  if (!prev) {
    const last = state.nodes.at(-1);
    if (last) {
      n.x = last.x + W + 80;
      n.y = last.y;
    }
  }
  state.nodes.push(n);
  state.isStarter = false;
  if (prev && !state.edges.some((e) => e.from === prev.id && e.to === n.id))
    state.edges.push({ from: prev.id, to: n.id });
  state.selected = n.id;
  state.selectedEdge = null;
  reveal(n);
  render();
  renderInspector();
  toast(
    tr(prev ? "addedConnected" : "added", { name: displayAction(a).title }),
  );
}
function reveal(n) {
  const r = $("#canvas").getBoundingClientRect();
  const left = n.x * state.zoom + state.pan.x,
    right = (n.x + W) * state.zoom + state.pan.x,
    top = n.y * state.zoom + state.pan.y,
    bottom = (n.y + H) * state.zoom + state.pan.y;
  if (right > r.width - 25) state.pan.x -= right - (r.width - 25);
  if (left < 25) state.pan.x += 25 - left;
  if (bottom > r.height - 25) state.pan.y -= bottom - (r.height - 25);
  if (top < 25) state.pan.y += 25 - top;
}
function path(a, b) {
  const x1 = a.x + W,
    y1 = a.y + H / 2,
    x2 = b.x,
    y2 = b.y + H / 2;
  const curve = Math.max(48, Math.abs(x2 - x1) * 0.45);
  return `M ${x1} ${y1} C ${x1 + curve} ${y1}, ${x2 - curve} ${y2}, ${x2} ${y2}`;
}
function render() {
  const stage = $("#stage");
  stage.setAttribute(
    "transform",
    `translate(${state.pan.x} ${state.pan.y}) scale(${state.zoom})`,
  );
  let html = "";
  state.edges.forEach((e, i) => {
    const a = node(e.from),
      b = node(e.to);
    if (!a || !b) return;
    const d = path(a, b);
    html += `<path class="edge${state.selectedEdge === i ? " selected" : ""}" d="${d}"/><path class="edge-hit" data-edge="${i}" d="${d}"/>`;
  });
  if (state.draft) {
    const a = node(state.draft.from);
    if (a) {
      const x = a.x + W,
        y = a.y + H / 2,
        pt = state.draft.to;
      html += `<path class="draft" d="M ${x} ${y} C ${x + 70} ${y}, ${pt.x - 70} ${pt.y}, ${pt.x} ${pt.y}"/>`;
    }
  }
  state.nodes.forEach((n) => {
    const legacy = state.ops.find((o) => o.name === n.op);
    const category = legacy?.category;
    const a = action(n.op) ||
      {
        alloc: ACTIONS[0],
        score: ACTIONS[1],
        filter: ACTIONS[2],
        rank: ACTIONS[3],
        decide: ACTIONS[4],
        emit: ACTIONS[5],
        track: ACTIONS[6],
      }[category] || { icon: "◇", color: "#edf1f6", ink: "#728099" };
    html += `<g class="node${n.id === state.selected ? " selected" : ""}" data-id="${h(n.id)}"><rect class="node-shadow" x="${n.x}" y="${n.y + 5}" width="${W}" height="${H}" rx="12"/><rect class="node-card" x="${n.x}" y="${n.y}" width="${W}" height="${H}" rx="12"/><rect x="${n.x + 13}" y="${n.y + 16}" width="38" height="38" rx="10" fill="${a.color}"/><text x="${n.x + 32}" y="${n.y + 42}" text-anchor="middle" font-size="20" fill="${a.ink}" pointer-events="none">${h(a.icon)}</text><text class="node-title" x="${n.x + 62}" y="${n.y + 29}">${h(label(n).slice(0, 20))}</text><text class="node-subtitle" x="${n.x + 62}" y="${n.y + 49}">${h(modeLabel(n).slice(0, 24))}</text><circle class="port input" data-port="in" data-id="${h(n.id)}" cx="${n.x}" cy="${n.y + H / 2}" r="7"/><circle class="port output" data-port="out" data-id="${h(n.id)}" cx="${n.x + W}" cy="${n.y + H / 2}" r="8"/></g>`;
  });
  stage.innerHTML = html;
  $("#flowCount").textContent = tr("actionCount", {
    nodes: state.nodes.length,
    edges: state.edges.length,
    pluralNodes: state.nodes.length === 1 ? "" : "s",
    pluralEdges: state.edges.length === 1 ? "" : "s",
  });
  $("#emptyState").hidden = state.nodes.length > 0;
  $("#zoomLabel").textContent = Math.round(state.zoom * 100) + "%";
  const details = $(".json-details");
  if (details?.open)
    details.querySelector("pre").textContent = JSON.stringify(
      toWorkflow(),
      null,
      2,
    );
}
function wouldCycle(from, to) {
  const seen = new Set();
  const visit = (id) => {
    if (id === from) return true;
    if (seen.has(id)) return false;
    seen.add(id);
    return state.edges.some((e) => e.from === id && visit(e.to));
  };
  return visit(to);
}
function connect(from, to) {
  if (from === to) {
    toast(tr("selfLink"), true);
    return;
  }
  if (state.edges.some((e) => e.from === from && e.to === to)) {
    toast(tr("duplicateLink"), true);
    return;
  }
  if (wouldCycle(from, to)) {
    toast(tr("cycleLink"), true);
    return;
  }
  state.edges.push({ from, to });
  state.isStarter = false;
  state.selected = to;
  state.selectedEdge = null;
  render();
  renderInspector();
  toast(tr("linked"));
}
function pointerDown(e) {
  if (e.button !== 0 && e.button !== 1) return;
  const target = e.target;
  const port = target.closest("[data-port]");
  const graphNode = target.closest(".node");
  const edge = target.closest("[data-edge]");
  const p = worldPoint(e);
  if (port) {
    e.preventDefault();
    e.stopPropagation();
    if (port.dataset.port === "out") {
      state.gesture = { kind: "connect", from: port.dataset.id };
      state.draft = { from: port.dataset.id, to: p };
      $("#canvas").setPointerCapture(e.pointerId);
    }
    return;
  }
  if (graphNode) {
    state.selected = graphNode.dataset.id;
    state.selectedEdge = null;
    state.gesture = {
      kind: "node",
      id: state.selected,
      delta: {
        x: p.x - node(state.selected).x,
        y: p.y - node(state.selected).y,
      },
    };
    $("#canvas").setPointerCapture(e.pointerId);
    render();
    renderInspector();
    return;
  }
  if (edge) {
    state.selected = null;
    state.selectedEdge = Number(edge.dataset.edge);
    render();
    renderInspector();
    return;
  }
  state.selected = null;
  state.selectedEdge = null;
  state.gesture = {
    kind: "pan",
    clientX: e.clientX,
    clientY: e.clientY,
    panX: state.pan.x,
    panY: state.pan.y,
  };
  $("#canvas").setPointerCapture(e.pointerId);
  $("#canvas").classList.add("panning");
  render();
  renderInspector();
}
function pointerMove(e) {
  const g = state.gesture;
  if (!g) return;
  if (g.kind === "node") {
    const n = node(g.id),
      p = worldPoint(e);
    if (n) {
      n.x = Math.round(p.x - g.delta.x);
      n.y = Math.round(p.y - g.delta.y);
      render();
    }
  } else if (g.kind === "connect") {
    state.draft.to = worldPoint(e);
    render();
  } else if (g.kind === "pan") {
    state.pan.x = g.panX + e.clientX - g.clientX;
    state.pan.y = g.panY + e.clientY - g.clientY;
    render();
  }
}
function pointerUp(e) {
  const g = state.gesture;
  if (!g) return;
  state.gesture = null;
  $("#canvas").classList.remove("panning");
  if (g.kind === "connect") {
    const target = document.elementFromPoint(e.clientX, e.clientY);
    const port = target?.closest?.('[data-port="in"]');
    state.draft = null;
    if (port) connect(g.from, port.dataset.id);
    else {
      render();
      toast(tr("dropPort"));
    }
  } else render();
}
function fitView(all = false) {
  const r = $("#canvas").getBoundingClientRect();
  if (!r.width || !r.height) {
    render(); // imported graphs must still replace the old DOM while hidden
    return;
  }
  if (!state.nodes.length) {
    state.zoom = 1;
    state.pan = { x: 0, y: 0 };
    render();
    return;
  }
  const minX = Math.min(...state.nodes.map((n) => n.x)),
    maxX = Math.max(...state.nodes.map((n) => n.x + W)),
    minY = Math.min(...state.nodes.map((n) => n.y)),
    maxY = Math.max(...state.nodes.map((n) => n.y + H));
  const fit = Math.min(
    1.2,
    Math.min((r.width - 100) / (maxX - minX), (r.height - 120) / (maxY - minY)),
  );
  if (fit < 0.7 && !all) {
    state.zoom = 0.8;
    state.pan = {
      x: 60 - minX * state.zoom,
      y: r.height / 2 - (minY + H / 2) * state.zoom,
    };
  } else {
    state.zoom = Math.max(0.16, fit);
    state.pan = {
      x: (r.width - (maxX - minX) * state.zoom) / 2 - minX * state.zoom,
      y: (r.height - (maxY - minY) * state.zoom) / 2 - minY * state.zoom,
    };
  }
  render();
}
function changeZoom(mult) {
  const r = $("#canvas").getBoundingClientRect(),
    cx = r.width / 2,
    cy = r.height / 2,
    old = state.zoom;
  state.zoom = Math.min(2, Math.max(0.35, Math.round(old * mult * 100) / 100));
  state.pan.x = cx - ((cx - state.pan.x) * state.zoom) / old;
  state.pan.y = cy - ((cy - state.pan.y) * state.zoom) / old;
  render();
}
function toWorkflow() {
  return {
    name: $("#workflowName").value.trim() || "workflow",
    description: state.description,
    nodes: state.nodes.map((n) => ({
      id: n.id,
      op: n.op,
      ...(Object.keys(n.params).length ? { params: n.params } : {}),
    })),
    edges: state.edges.map((e) => ({ from: e.from, to: e.to })),
  };
}
function field(key, title, value, help, onChange, type = "text") {
  const div = document.createElement("div");
  div.className = "field";
  const id = "field-" + key;
  const label = document.createElement("label");
  label.htmlFor = id;
  label.textContent = title;
  const input = document.createElement(type === "boolean" ? "select" : "input");
  input.id = id;
  if (type === "boolean") {
    for (const [value, caption] of [
      ["false", "No"],
      ["true", "Yes"],
    ]) {
      const option = document.createElement("option");
      option.value = value;
      option.textContent = tr(caption === "Yes" ? "yes" : "no");
      input.append(option);
    }
  } else {
    input.type = type;
    if (type === "number") input.step = "any";
  }
  input.value = String(value);
  input.addEventListener(type === "boolean" ? "change" : "input", () =>
    onChange(input.value),
  );
  div.append(label, input);
  if (help) {
    const small = document.createElement("small");
    small.textContent = help;
    div.append(small);
  }
  return div;
}
function renderInspector() {
  const root = $("#inspectorBody");
  root.replaceChildren();
  const n = node(state.selected);
  if (state.selectedEdge !== null) {
    const e = state.edges[state.selectedEdge];
    if (!e) {
      state.selectedEdge = null;
      return renderInspector();
    }
    const h3 = document.createElement("h3");
    h3.className = "inspector-type";
    h3.textContent = tr("connection");
    const p = document.createElement("p");
    p.className = "inspector-desc";
    p.textContent = `${label(node(e.from))} → ${label(node(e.to))}`;
    const btn = document.createElement("button");
    btn.className = "danger";
    btn.textContent = tr("removeConnection");
    btn.onclick = () => {
      state.edges.splice(state.selectedEdge, 1);
      state.isStarter = false;
      state.selectedEdge = null;
      render();
      renderInspector();
    };
    root.append(h3, p, btn);
    return;
  }
  if (!n) {
    root.innerHTML = `<div class="inspector-blank"><div class="blank-symbol">⚙</div><h3>${h(tr("nothingTitle"))}</h3><p>${h(tr("nothingHint"))}</p></div>`;
    return;
  }
  const a = action(n.op),
    legacy = state.ops.find((o) => o.name === n.op);
  const mapping = legacyMode(n.op);
  const guideId = a?.id || mapping?.[0];
  const guide = guideId ? LOCALES[state.lang].guides[guideId] : null;
  const legacyMarkOnly = n.op === "demote_cold" || n.op === "balance_nodes";
  const desc = a
    ? displayAction(a).desc
    : legacyMarkOnly
      ? tr("legacyPurpose")
      : state.lang === "zh" && guide
        ? guide.modes[mapping[1]]
        : legacy?.description || tr("importedAction");
  root.innerHTML = `<div class="inspector-type">${h(tr(a ? "actionSettings" : "legacyAction"))}</div><div class="inspector-heading"><span class="action-icon" style="background:${a?.color || "#edf1f6"};color:${a?.ink || "#728099"}">${a?.icon || "◇"}</span><div><h3>${h(label(n))}</h3><small>${h(n.id)}${a ? "" : " · " + h(n.op)}</small></div></div><p class="inspector-desc">${h(desc)}</p><div class="field-group-title">${h(tr("behavior"))}</div>`;
  if (a) {
    const d = document.createElement("div");
    d.className = "field";
    const lbl = document.createElement("label");
    lbl.htmlFor = "modeSelect";
    lbl.textContent = tr("operation");
    const select = document.createElement("select");
    select.id = "modeSelect";
    Object.entries(displayAction(a).modes).forEach(([id, title]) => {
      const opt = document.createElement("option");
      opt.value = id;
      opt.textContent = title;
      select.append(opt);
    });
    select.value = n.params.mode || Object.keys(a.modes)[0];
    select.onchange = () => {
      n.params = { mode: select.value };
      state.isStarter = false;
      render();
      renderInspector();
    };
    d.append(lbl, select);
    root.append(d);
  }
  if (guide) {
    const mode = a ? n.params.mode || Object.keys(a.modes)[0] : mapping[1];
    const note = legacyMarkOnly ? tr("legacyMark") : guide.modes[mode];
    const panel = document.createElement("section");
    panel.className = "node-guide";
    panel.innerHTML = `<div class="guide-caption">${h(tr("purpose"))}</div><p>${h(legacyMarkOnly ? tr("legacyPurpose") : guide.purpose)}</p>
      <div class="guide-mode"><strong>${h(tr("modeDetail"))} · ${h(a ? displayAction(a).modes[mode] || mode : guide.names?.[mode] || action(guideId).modes[mode])}</strong><p>${h(note || "")}</p></div>
      <div class="guide-caption">${h(tr("input"))}</div><p>${h(guide.input)}</p>
      <div class="guide-caption">${h(tr("result"))}</div><p>${h(legacyMarkOnly ? tr("legacyOutput") : guide.output)}</p>
      <div class="guide-tip"><strong>${h(tr("tip"))}</strong><br>${h(guide.tip)}</div>`;
    root.append(panel);
  }
  if (a) {
    for (const [key, title, def, help] of a.fields?.[
      n.params.mode || Object.keys(a.modes)[0]
    ] || [])
      root.append(
        field(
          key,
          LOCALES[state.lang].fields?.[
            `${a.id}.${n.params.mode || Object.keys(a.modes)[0]}.${key}`
          ]?.[0] || title,
          n.params[key] ?? def,
          LOCALES[state.lang].fields?.[
            `${a.id}.${n.params.mode || Object.keys(a.modes)[0]}.${key}`
          ]?.[1] || help,
          (v) => {
            if (v === "")
              delete n.params[key]; // empty fields use the engine default
            else n.params[key] = v;
            state.isStarter = false;
            render();
          },
          key === "require_benefit" ? "boolean" : "number",
        ),
      );
  }
  const extra = document.createElement("div");
  extra.className = "field-group-title";
  extra.textContent = tr(a ? "advanced" : "parameters");
  root.append(extra);
  const custom = document.createElement("div");
  root.append(custom);
  function drawCustom() {
    custom.replaceChildren();
    const standard = new Set([
      "mode",
      ...(a?.fields?.[n.params.mode || Object.keys(a.modes)[0]] || []).map(
        (f) => f[0],
      ),
    ]);
    for (const key of Object.keys(n.params).filter((k) => !standard.has(k))) {
      const row = document.createElement("div");
      row.className = "kv-row";
      const k = document.createElement("input");
      k.value = key;
      k.setAttribute("aria-label", tr("paramName"));
      const v = document.createElement("input");
      v.value = n.params[key];
      v.setAttribute("aria-label", tr("paramValue"));
      v.oninput = () => {
        n.params[key] = v.value;
        state.isStarter = false;
        render();
      };
      k.onchange = () => {
        const newKey = k.value.trim();
        if (!newKey || newKey === key || newKey in n.params) {
          k.value = key;
          return;
        }
        n.params[newKey] = n.params[key];
        state.isStarter = false;
        delete n.params[key];
        state.isStarter = false;
        drawCustom();
        render();
      };
      const del = document.createElement("button");
      del.textContent = "×";
      del.setAttribute("aria-label", tr("removeParam", { name: key }));
      del.onclick = () => {
        delete n.params[key];
        state.isStarter = false;
        drawCustom();
        render();
      };
      row.append(k, v, del);
      custom.append(row);
    }
  }
  drawCustom();
  const add = document.createElement("button");
  add.className = "secondary-action";
  add.textContent = tr("addParam");
  add.onclick = () => {
    let k = "parameter",
      i = 1;
    while (k in n.params) k = "parameter_" + i++;
    n.params[k] = "";
    state.isStarter = false;
    drawCustom();
    custom.lastElementChild?.querySelector("input")?.focus();
  };
  root.append(add);
  const footer = document.createElement("div");
  footer.className = "inspector-footer";
  const remove = document.createElement("button");
  remove.className = "danger";
  remove.textContent = tr("removeAction");
  remove.onclick = deleteSelection;
  footer.append(remove);
  root.append(footer);
  const details = document.createElement("details");
  details.className = "json-details";
  details.innerHTML = `<summary>${h(tr("viewJson"))}</summary><pre></pre>`;
  details.addEventListener("toggle", () => {
    if (details.open)
      details.querySelector("pre").textContent = JSON.stringify(
        toWorkflow(),
        null,
        2,
      );
  });
  root.append(details);
}
function deleteSelection() {
  if (state.selectedEdge !== null) {
    state.edges.splice(state.selectedEdge, 1);
    state.isStarter = false;
    state.selectedEdge = null;
  } else if (state.selected) {
    state.isStarter = false;
    state.nodes = state.nodes.filter((n) => n.id !== state.selected);
    state.edges = state.edges.filter(
      (e) => e.from !== state.selected && e.to !== state.selected,
    );
    state.selected = null;
  } else return;
  render();
  renderInspector();
  setStatus("updated");
}
function layout() {
  const levels = new Map();
  let remaining = new Set(state.nodes.map((n) => n.id));
  for (let i = 0; i < state.nodes.length; i++) {
    let changed = false;
    for (const n of state.nodes) {
      if (!remaining.has(n.id)) continue;
      const deps = state.edges.filter((e) => e.to === n.id).map((e) => e.from);
      if (deps.every((id) => !remaining.has(id))) {
        levels.set(
          n.id,
          deps.length
            ? Math.max(...deps.map((id) => levels.get(id) || 0)) + 1
            : 0,
        );
        remaining.delete(n.id);
        changed = true;
      }
    }
    if (!changed) break;
  }
  const cols = new Map();
  for (const n of state.nodes) {
    const level = levels.get(n.id) || 0;
    if (!cols.has(level)) cols.set(level, []);
    cols.get(level).push(n);
  }
  for (const [col, ns] of cols)
    ns.forEach((n, i) => {
      n.x = 90 + col * 306;
      n.y = 95 + i * 118;
    });
}
function loadWorkflow(w) {
  if (!w || !Array.isArray(w.nodes) || !Array.isArray(w.edges))
    throw Error(tr("invalidGraph"));
  const ids = new Set();
  const nodes = w.nodes.map((n) => {
    if (
      typeof n.id !== "string" ||
      !n.id ||
      ids.has(n.id) ||
      typeof n.op !== "string" ||
      !n.op
    )
      throw Error(tr("invalidNode"));
    ids.add(n.id);
    if (
      n.params !== undefined &&
      (n.params === null ||
        typeof n.params !== "object" ||
        Array.isArray(n.params))
    )
      throw Error(tr("invalidParams"));
    return {
      id: n.id,
      op: n.op,
      params: Object.fromEntries(
        Object.entries(n.params || {}).map(([k, v]) => [k, String(v)]),
      ),
      x: 0,
      y: 0,
    };
  });
  const edges = w.edges.map((e) => {
    if (!ids.has(e.from) || !ids.has(e.to) || e.from === e.to)
      throw Error(tr("invalidEdge"));
    return { from: e.from, to: e.to };
  });
  const pairs = new Set();
  for (const e of edges) {
    const k = JSON.stringify([e.from, e.to]);
    if (pairs.has(k)) throw Error(tr("duplicateEdge"));
    pairs.add(k);
  }
  const remaining = new Set(ids);
  for (let i = 0; i < nodes.length; i++) {
    const ready = [...remaining].filter((id) =>
      edges.filter((e) => e.to === id).every((e) => !remaining.has(e.from)),
    );
    if (!ready.length) break;
    ready.forEach((id) => remaining.delete(id));
  }
  if (remaining.size) throw Error(tr("invalidCycle"));
  const next =
    1 +
    Math.max(0, ...nodes.map((n) => Number(n.id.match(/^n(\d+)$/)?.[1] || 0)));
  state.nodes = nodes;
  state.isStarter = false;
  state.edges = edges;
  state.nextId = next;
  state.selected = null;
  state.selectedEdge = null;
  state.description = typeof w.description === "string" ? w.description : "";
  $("#workflowName").value = typeof w.name === "string" ? w.name : "workflow";
  layout();
  fitView();
  renderInspector();
  setStatus("loaded");
}
function starter() {
  loadWorkflow({
    name: tr("starterName"),
    description: tr("starterDescription"),
    nodes: [
      { id: "n1", op: "score_items", params: { mode: "hotness" } },
      { id: "n2", op: "route_items", params: { mode: "destination" } },
      { id: "n3", op: "move_items", params: { mode: "migrate" } },
    ],
    edges: [
      { from: "n1", to: "n2" },
      { from: "n2", to: "n3" },
    ],
  });
  state.isStarter = true;
  setStatus("ready");
}
async function api(url, options) {
  const r = await fetch(url, options);
  if (!r.ok) throw Error(tr("requestFailed", { status: r.status }));
  return r;
}
async function run() {
  if (!state.nodes.length) {
    toast(tr("emptyRun"), true);
    return;
  }
  const btn = $("#runBtn");
  btn.disabled = true;
  $("#outputPanel").hidden = false;
  $("#toggleOutput").setAttribute("aria-expanded", "true");
  state.outputState = "running";
  renderOutput();
  setStatus("running");
  try {
    const r = await fetch("/api/run", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(toWorkflow()),
    });
    const text = await r.text();
    state.outputRaw = text;
    state.outputState = r.ok ? "done" : "failed";
    renderOutput();
    setStatus(r.ok ? "runDone" : "runFailed");
    if (!r.ok) toast(tr("runError"), true);
  } catch (err) {
    state.outputState = "offline";
    renderOutput();
    setStatus("offline");
    toast(err.message, true);
  } finally {
    btn.disabled = false;
  }
}
async function fetchTemplates() {
  try {
    state.templates = await (await api("/api/templates")).json();
    renderTemplates();
  } catch {
    toast(tr("templatesOffline"), true);
  }
}
function init() {
  setLanguage(state.lang);
  $("#languageSelect").onchange = (event) => setLanguage(event.target.value);
  starter();
  requestAnimationFrame(fitView);
  $("#canvas").addEventListener("pointerdown", pointerDown);
  $("#canvas").addEventListener("pointermove", pointerMove);
  $("#canvas").addEventListener("pointerup", pointerUp);
  $("#canvas").addEventListener("pointercancel", () => {
    state.gesture = null;
    state.draft = null;
    render();
  });
  $("#canvas").addEventListener(
    "wheel",
    (e) => {
      e.preventDefault();
      changeZoom(e.deltaY < 0 ? 1.1 : 1 / 1.1);
    },
    { passive: false },
  );
  $("#fitBtn").onclick = () => fitView(true);
  $("#zoomIn").onclick = () => changeZoom(1.2);
  $("#zoomOut").onclick = () => changeZoom(1 / 1.2);
  $("#actionSearch").oninput = refreshPalette;
  $("#emptyAdd").onclick = () => addNode("place_items");
  $("#newBtn").onclick = () => {
    if (state.nodes.length && !confirm(tr("confirmNew"))) return;
    loadWorkflow({
      name: tr("newName"),
      description: tr("newDescription"),
      nodes: [],
      edges: [],
    });
    toast(tr("newReady"));
  };
  $("#exportBtn").onclick = () => {
    const blob = new Blob([JSON.stringify(toWorkflow(), null, 2) + "\n"], {
      type: "application/json",
    });
    const url = URL.createObjectURL(blob),
      a = document.createElement("a");
    a.href = url;
    a.download =
      ($("#workflowName")
        .value.trim()
        .replace(/[^a-z0-9_-]+/gi, "-") || "workflow") + ".json";
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
    toast(tr("exported"));
  };
  $("#importBtn").onclick = () => $("#fileInput").click();
  $("#fileInput").onchange = async (e) => {
    const file = e.target.files?.[0];
    if (!file) return;
    try {
      loadWorkflow(JSON.parse(await file.text()));
      toast(tr("imported"));
    } catch (err) {
      toast(tr("importError", { error: err.message }), true);
    }
    e.target.value = "";
  };
  $("#templateSelect").onchange = (e) =>
    ($("#loadTemplateBtn").disabled = !e.target.value);
  $("#loadTemplateBtn").onclick = async () => {
    const name = $("#templateSelect").value;
    if (!name) return;
    try {
      const w = await (
        await api("/api/template/" + encodeURIComponent(name))
      ).json();
      loadWorkflow(w);
      toast(
        tr("templateLoaded", {
          name: LOCALES[state.lang].templates?.[name] || name,
        }),
      );
    } catch (err) {
      toast(tr("templateError", { error: err.message }), true);
    }
  };
  $("#runBtn").onclick = run;
  $("#toggleOutput").onclick = () => {
    const p = $("#outputPanel");
    p.hidden = !p.hidden;
    $("#toggleOutput").setAttribute("aria-expanded", String(!p.hidden));
    renderOutput();
  };
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") {
      state.gesture = null;
      state.draft = null;
      render();
      return;
    }
    if (
      (e.key === "Delete" || e.key === "Backspace") &&
      !["INPUT", "TEXTAREA", "SELECT"].includes(
        document.activeElement?.tagName,
      ) &&
      !document.activeElement?.isContentEditable
    )
      deleteSelection();
  });
  window.addEventListener("resize", () => {
    if (!state.gesture) fitView();
  });
  fetchTemplates();
  api("/api/ops/legacy")
    .then((r) => r.json())
    .then((ops) => {
      state.ops = Array.isArray(ops) ? ops : [];
      render();
      renderInspector();
    })
    .catch(() => toast(tr("engineOffline"), true));
}
document.addEventListener("DOMContentLoaded", init);
