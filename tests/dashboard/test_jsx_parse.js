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

var files = fs.readdirSync(PUB).filter(function (f) { return /\.jsx$/.test(f); }).sort();
var failures = 0;
files.forEach(function (f) {
  try {
    Babel.transform(fs.readFileSync(path.join(PUB, f), "utf8"), { presets: ["react"], filename: f });
    console.log("  ok   - " + f);
  } catch (e) {
    failures++;
    console.log("  FAIL - " + f + ": " + String(e.message).split("\n")[0]);
  }
});

console.log("\n" + (files.length - failures) + "/" + files.length + " parsed\n");
process.exit(failures ? 1 : 0);
