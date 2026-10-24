/* Client-side no-build JSX transform worker. Runs Babel off the UI thread. */
"use strict";

self.onmessage = function (event) {
  var modules = event.data && event.data.modules;
  if (!Array.isArray(modules)) {
    self.postMessage({ ok: false, kind: "worker", error: "The JSX transform worker received an invalid request" });
    return;
  }
  try {
    if (!self.Babel) importScripts("vendor/babel.min.js");
    var codes = [];
    modules.forEach(function (module, index) {
      codes.push(self.Babel.transform(module.source, {
        filename: module.filename,
        presets: module.presets,
        plugins: module.plugins,
        sourceMaps: false
      }).code);
      self.postMessage({ progress: true, completed: index + 1, total: modules.length });
    });
    self.postMessage({ ok: true, codes: codes });
  } catch (error) {
    self.postMessage({
      ok: false,
      kind: "transform",
      error: error && error.message ? error.message : String(error)
    });
  }
};
