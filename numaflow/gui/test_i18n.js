"use strict";
// Run with: node numaflow/gui/test_i18n.js (no npm packages required).
const fs = require("node:fs");
const vm = require("node:vm");
const path = require("node:path");
const dir = __dirname;
const context = {};
vm.runInNewContext(
  fs.readFileSync(path.join(dir, "i18n.js"), "utf8") +
    "\nglobalThis.catalogue = { LOCALES, LEGACY_MODES };",
  context,
);
vm.runInNewContext(
  fs
    .readFileSync(path.join(dir, "app.js"), "utf8")
    .split("function initialLanguage()")[0] + "\nglobalThis.actions = ACTIONS;",
  context,
);
const { LOCALES, LEGACY_MODES } = context.catalogue;
const failures = [];
const appSource = fs.readFileSync(path.join(dir, "app.js"), "utf8");
for (const [, key] of appSource.matchAll(
  /\b(?:tr|setStatus)\("([A-Za-z][\w]*)"/g,
))
  if (!LOCALES.en.ui[key] || !LOCALES.zh.ui[key])
    failures.push(`dynamic UI: ${key}`);
for (const action of context.actions) {
  const en = LOCALES.en.guides[action.id];
  const zh = LOCALES.zh.guides[action.id];
  if (!en || !zh) {
    failures.push(`guide: ${action.id}`);
    continue;
  }
  for (const property of ["purpose", "input", "output", "tip"])
    if (!en[property] || !zh[property])
      failures.push(`${action.id}.${property}`);
  for (const mode of Object.keys(action.modes)) {
    if (!en.modes[mode] || !zh.modes[mode] || !zh.names[mode])
      failures.push(`${action.id}.${mode}`);
    for (const [key] of action.fields?.[mode] || [])
      if (!LOCALES.zh.fields[`${action.id}.${mode}.${key}`])
        failures.push(`${action.id}.${mode}.${key}`);
  }
}
const equivalence = fs.readFileSync(
  path.join(dir, "..", "tests", "test_actions.c"),
  "utf8",
);
const proven = new Set(
  [...equivalence.matchAll(/\{"([^"]+)","([^"]+)","([^"]+)"\}/g)].map(
    ([, action, mode, legacy]) => JSON.stringify([action, mode, legacy]),
  ),
);
for (const [op, [action, mode]] of Object.entries(LEGACY_MODES)) {
  if (
    !LOCALES.zh.guides[action]?.modes[mode] ||
    !context.actions.find((a) => a.id === action)?.modes[mode] ||
    !proven.has(JSON.stringify([action, mode, op]))
  )
    failures.push(`unproven legacy mapping: ${op}`);
}
if (
  LEGACY_MODES.demote_cold?.[1] !== "demote_mark" ||
  LEGACY_MODES.balance_nodes?.[1] !== "balance_mark"
)
  failures.push("mark-only legacy semantics lost");
const html = fs.readFileSync(path.join(dir, "index.html"), "utf8");
for (const [, key] of html.matchAll(
  /data-i18n(?:-title|-aria|-placeholder)?="([^"]+)"/g,
))
  if (!LOCALES.zh.ui[key]) failures.push(`HTML: ${key}`);
for (const key of Object.keys(LOCALES.en.ui))
  if (!LOCALES.zh.ui[key]) failures.push(`UI: ${key}`);
if (
  context.actions.length !== 7 ||
  Object.keys(LEGACY_MODES).length !== 36 ||
  Object.keys(LOCALES.zh.templates).length !== 23
)
  failures.push("action, legacy or template catalogue count changed");
if (failures.length) {
  console.error("Missing Chinese/English copy:", failures.join(", "));
  process.exit(1);
}
console.log(
  "EN / 中文 copy complete for 7 actions, 38 modes, 36 fixed legacy presets and 23 templates",
);
