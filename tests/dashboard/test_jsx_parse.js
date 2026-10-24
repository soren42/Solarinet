#!/usr/bin/env node
/* Parse-check every dashboard/public/*.jsx with the dashboard's OWN vendored
   Babel standalone — the same transform the browser runs at load time, so a
   pass here means the page will not blank out on a syntax error.
   Run: node tests/dashboard/test_jsx_parse.js   — exits non-zero on failure. */
"use strict";

var fs = require("fs");
var vm = require("vm");
var path = require("path");

var PUB = path.join(__dirname, "..", "..", "dashboard", "public");

var sandbox = { console: console, process: process, setTimeout: setTimeout, clearTimeout: clearTimeout };
sandbox.window = sandbox;
sandbox.self = sandbox;
sandbox.global = sandbox;
vm.createContext(sandbox);
vm.runInContext(fs.readFileSync(path.join(PUB, "vendor/babel.min.js"), "utf8"), sandbox, { filename: "babel.min.js" });
var Babel = sandbox.Babel;
if (!Babel) { console.log("  FAIL - vendor/babel.min.js did not expose Babel"); process.exit(2); }

var failures = 0;
var indexHtml = fs.readFileSync(path.join(PUB, "index.html"), "utf8");
var bootSource = fs.readFileSync(path.join(PUB, "boot.js"), "utf8");
var workerSource = fs.readFileSync(path.join(PUB, "sw.js"), "utf8");
var bootCache = (bootSource.match(/TRANSFORM_CACHE\s*=\s*["']([^"']+)["']/) || [])[1];
var workerCache = (workerSource.match(/TRANSFORM_CACHE\s*=\s*["']([^"']+)["']/) || [])[1];
if (!bootCache || bootCache !== workerCache) {
  console.log("  FAIL - dashboard loader and service worker must retain the same compiled-transform cache");
  failures++;
} else {
  console.log("  ok   - dashboard loader and service worker share the compiled-transform cache");
}
var bootBabelRevision = (bootSource.match(/BABEL_REVISION\s*=\s*["']([^"']+)["']/) || [])[1];
if (!bootBabelRevision || bootBabelRevision !== Babel.version) {
  console.log("  FAIL - loader cache revision must match the vendored Babel version (loader=" + bootBabelRevision + ", vendor=" + Babel.version + ")");
  failures++;
} else {
  console.log("  ok   - loader cache revision matches vendored Babel " + Babel.version);
}
var babelTags = indexHtml.match(/<script\b[^>]*type\s*=\s*(['"])application\/x-solarinet-jsx\1[^>]*>/gi) || [];
var missingLegacyPreset = babelTags.filter(function (tag) {
  return !/\bdata-presets\s*=\s*(['"])react,env\1/i.test(tag);
});
var missingLegacyPlugins = babelTags.filter(function (tag) {
  return !/\bdata-plugins\s*=\s*(['"])transform-class-properties,transform-object-rest-spread,transform-flow-strip-types\1/i.test(tag);
});
if (babelTags.length === 0 || missingLegacyPreset.length || missingLegacyPlugins.length) {
  console.log("  FAIL - every dashboard JSX script must preserve the prior React/Env presets and Babel plugins");
  failures++;
} else {
  console.log("  ok   - " + babelTags.length + " dashboard scripts preserve the prior React/Env Babel pipeline");
}

var files = fs.readdirSync(PUB).filter(function (f) { return /\.jsx$/.test(f); }).sort();
var taggedFiles = babelTags.map(function (tag) {
  var match = tag.match(/\bsrc\s*=\s*(['"])([^'"]+)\1/i);
  return match ? path.basename(match[2].split("?")[0]) : "";
});
var unlistedFiles = files.filter(function (file) { return taggedFiles.indexOf(file) === -1; });
var duplicateFiles = taggedFiles.filter(function (file, index) { return file && taggedFiles.indexOf(file) !== index; });
if (unlistedFiles.length || duplicateFiles.length || taggedFiles.length !== files.length) {
  console.log("  FAIL - index.html must declare every dashboard JSX module exactly once for boot.js; unlisted: " + (unlistedFiles.join(", ") || "none") + "; duplicated: " + (duplicateFiles.join(", ") || "none"));
  failures++;
} else {
  console.log("  ok   - custom loader lists all " + files.length + " JSX modules exactly once");
}
files.forEach(function (f) {
  try {
    Babel.transform(fs.readFileSync(path.join(PUB, f), "utf8"), {
      presets: ["react", "env"],
      plugins: ["transform-class-properties", "transform-object-rest-spread", "transform-flow-strip-types"],
      filename: f
    });
    console.log("  ok   - " + f);
  } catch (e) {
    failures++;
    console.log("  FAIL - " + f + ": " + String(e.message).split("\n")[0]);
  }
});

console.log("\n" + (files.length - failures) + "/" + files.length + " parsed\n");
process.exit(failures ? 1 : 0);
