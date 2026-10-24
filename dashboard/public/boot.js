/* SolariNet dashboard loader.
 * Transforms editable JSX in the browser without a build step, then caches the
 * compiled classic scripts by a SHA-256 of source and pipeline settings. Babel is requested
 * only when a source has no compatible cached transform (or Cache API/WebCrypto
 * is unavailable). The service worker preserves this CacheStorage namespace.
 */
(function () {
  "use strict";

  var SOURCE_SELECTOR = 'script[type="application/x-solarinet-jsx"][src]';
  // Bump TRANSFORM_CACHE with a matching service-worker allow-list entry if
  // the Babel version, preset pipeline, or transform options change.
  var TRANSFORM_CACHE = "solari-jsx-transform-v2";
  var BABEL_REVISION = "7.29.0";
  var TRANSFORM_KEY = "__solari_compiled";
  var WORKER_IDLE_TIMEOUT_MS = 45000;
  var nodes = Array.prototype.slice.call(document.querySelectorAll(SOURCE_SELECTOR));
  var status = document.getElementById("boot-status");
  var root = document.getElementById("root");
  var appRenderTimer = null;

  function observeAppRender() {
    if (!root || !window.MutationObserver) return;
    var markReady = function () {
      if (root.querySelector(".app, form")) {
        window.__solariAppRendered = true;
        if (window.performance && performance.mark) performance.mark("solari-app-ready");
        if (appRenderTimer !== null) window.clearTimeout(appRenderTimer);
        observer.disconnect();
      }
    };
    var observer = new MutationObserver(markReady);
    observer.observe(root, { childList: true, subtree: true });
    markReady();
  }

  function showFailure(error) {
    console.error("[SolariNet] dashboard startup failed", error);
    if (status) {
      status.setAttribute("role", "alert");
      status.textContent = error && error.solariStage === "transform"
        ? "Dashboard code could not be compiled. Check the latest local app edits, then reload."
        : error && error.solariStage === "runtime"
          ? "Dashboard modules loaded, but no view appeared. Check the browser console, then reload."
          : "Dashboard files could not be loaded. Check the intranet connection, then reload.";
    }
  }

  function transformOptions(node) {
    function list(name, fallback) {
      var raw = node.getAttribute(name);
      return (raw ? raw.split(",") : fallback).map(function (value) { return value.trim(); }).filter(Boolean);
    }
    return {
      presets: list("data-presets", ["react", "env"]),
      plugins: list("data-plugins", ["transform-class-properties", "transform-object-rest-spread", "transform-flow-strip-types"])
    };
  }

  function sourceDigest(source, options) {
    if (!window.crypto || !window.crypto.subtle || !window.TextEncoder) return Promise.resolve("");
    var material = JSON.stringify({ babel: BABEL_REVISION, source: source, presets: options.presets, plugins: options.plugins });
    return window.crypto.subtle.digest("SHA-256", new TextEncoder().encode(material)).then(function (bytes) {
      return Array.prototype.map.call(new Uint8Array(bytes), function (byte) {
        return byte.toString(16).padStart(2, "0");
      }).join("");
    }).catch(function () { return ""; });
  }

  function requestFor(node, digest) {
    if (!digest) return null;
    var url = new URL(node.src);
    // The source digest already represents its bytes; deployment cache-busters
    // must not create a second transform entry for unchanged source.
    url.search = "";
    url.searchParams.set(TRANSFORM_KEY, "v2-" + digest);
    return new Request(url.href, { method: "GET", credentials: "same-origin" });
  }

  function fetchModule(node) {
    return fetch(node.src, { credentials: "same-origin", cache: "no-cache" }).then(function (response) {
      if (!response.ok) throw new Error("Could not load " + node.src + " (HTTP " + response.status + ")");
      return response.text();
    }).then(function (source) {
      var options = transformOptions(node);
      return sourceDigest(source, options).then(function (digest) {
        return { node: node, source: source, options: options, digest: digest, key: requestFor(node, digest), cached: null };
      });
    }).catch(function (error) {
      error.solariStage = "source";
      throw error;
    });
  }

  function loadBabel() {
    if (window.Babel) return Promise.resolve();
    return new Promise(function (resolve, reject) {
      var script = document.createElement("script");
      script.src = new URL("vendor/babel.min.js", document.baseURI).href;
      script.onload = resolve;
      script.onerror = function () { reject(new Error("Could not load the local Babel fallback")); };
      document.head.appendChild(script);
    });
  }

  function transformed(module) {
    try {
      return window.Babel.transform(module.source, {
        filename: module.node.src,
        presets: module.options.presets,
        plugins: module.options.plugins,
        sourceMaps: false
      }).code;
    } catch (error) {
      error.solariStage = "transform";
      throw error;
    }
  }

  function transformInWorker(modules) {
    return new Promise(function (resolve, reject) {
      if (typeof window.Worker !== "function") {
        var unsupported = new Error("Web Workers are unavailable");
        unsupported.solariStage = "worker";
        reject(unsupported);
        return;
      }
      var worker;
      try {
        worker = new Worker(new URL("transform-worker.js?v=20261024d", document.baseURI));
      } catch (error) {
        error.solariStage = "worker";
        reject(error);
        return;
      }
      var settled = false;
      var timeout = null;
      function armTimeout() {
        if (timeout !== null) window.clearTimeout(timeout);
        timeout = window.setTimeout(function () {
          var error = new Error("The JSX transform worker stopped reporting progress");
          error.solariStage = "worker";
          finish(error);
        }, WORKER_IDLE_TIMEOUT_MS);
      }
      function finish(error, codes) {
        if (settled) return;
        settled = true;
        if (timeout !== null) window.clearTimeout(timeout);
        worker.terminate();
        if (error) reject(error);
        else resolve(codes);
      }
      worker.onmessage = function (event) {
        var result = event.data || {};
        if (result.progress === true) {
          armTimeout();
          return;
        }
        if (result.ok && Array.isArray(result.codes) && result.codes.length === modules.length) {
          window.__solariTransformWorkerUsed = true;
          finish(null, result.codes);
          return;
        }
        var error = new Error(result.error || "The JSX transform worker returned an invalid response");
        error.solariStage = result.kind === "transform" ? "transform" : "worker";
        finish(error);
      };
      worker.onerror = function (event) {
        var error = new Error(event && event.message ? event.message : "The JSX transform worker failed");
        error.solariStage = "worker";
        finish(error);
      };
      armTimeout();
      try {
        worker.postMessage({ modules: modules.map(function (module) {
          return {
            source: module.source,
            filename: module.node.src,
            presets: module.options.presets,
            plugins: module.options.plugins
          };
        }) });
      } catch (error) {
        error.solariStage = "worker";
        finish(error);
      }
    });
  }

  function execute(code, sourceUrl) {
    // The shipped Apache CSP permits same-origin scripts and unsafe-eval but
    // deliberately forbids unsafe-inline. Babel's previous script-tag runner
    // appended classic scripts to the document; indirect eval keeps that same
    // global scope while honoring the existing policy.
    try {
      (0, window.eval)(code + "\n//# sourceURL=" + sourceUrl);
    } catch (error) {
      error.solariStage = "runtime";
      throw error;
    }
  }

  function removeObsolete(cache, module, keysPromise) {
    if (!module.key || !cache) return Promise.resolve();
    var current = new URL(module.key.url);
    return (keysPromise || cache.keys()).then(function (keys) {
      return Promise.all(keys.map(function (key) {
        var old = new URL(key.url);
        if (old.origin !== current.origin || old.pathname !== current.pathname || !old.searchParams.has(TRANSFORM_KEY)) {
          return Promise.resolve(false);
        }
        if (old.href === current.href) return Promise.resolve(false);
        return cache.delete(key);
      }));
    }).catch(function () {});
  }

  function persist(cache, module, code, keysPromise) {
    if (!cache || !module.key) return Promise.resolve();
    var response = new Response(code, { headers: { "Content-Type": "text/javascript; charset=utf-8" } });
    return removeObsolete(cache, module, keysPromise).then(function () { return cache.put(module.key, response); }).catch(function (error) {
      console.warn("[SolariNet] compiled cache unavailable for " + module.node.src, error && error.message);
    });
  }

  function boot() {
    if (!nodes.length) throw new Error("No dashboard JSX modules were declared");
    var cachePromise = window.caches && window.crypto && window.crypto.subtle
      ? window.caches.open(TRANSFORM_CACHE).catch(function () { return null; })
      : Promise.resolve(null);

    return Promise.all([cachePromise, Promise.all(nodes.map(fetchModule))]).then(function (results) {
      var cache = results[0];
      var modules = results[1];
      return Promise.all(modules.map(function (module) {
        if (!cache || !module.key) return null;
        return cache.match(module.key).catch(function () { return null; });
      })).then(function (hits) {
        modules.forEach(function (module, index) {
          module.cached = hits[index];
        });
        // Resolve every cached body before executing any module so the
        // window-global script order remains exactly as declared in index.
        return Promise.all(modules.map(function (module) {
          return module.cached ? module.cached.text().catch(function () { return null; }) : Promise.resolve(null);
        })).then(function (cachedCode) {
          var missingModules = modules.filter(function (_module, index) {
            return typeof cachedCode[index] !== "string" || !cachedCode[index].length;
          });
          var workerTransforms = missingModules.length ? transformInWorker(missingModules).catch(function (error) {
            if (error && error.solariStage === "transform") throw error;
            return loadBabel().then(function () { return missingModules.map(transformed); });
          }) : Promise.resolve([]);
          return workerTransforms.then(function (transformedCodes) {
            var writes = [];
            var transformIndex = 0;
            var keysPromise = missingModules.length && cache
              ? cache.keys().catch(function () { return []; })
              : null;
            modules.forEach(function (module, index) {
              var code = cachedCode[index];
              if (typeof code !== "string" || !code.length) code = transformedCodes[transformIndex++];
              execute(code, module.node.src);
              if (cachedCode[index] !== code) writes.push(persist(cache, module, code, keysPromise));
            });
            return Promise.all(writes);
          });
        });
      });
    }).then(function () {
      window.__solariBootReady = true;
      window.dispatchEvent(new Event("solari:boot-ready"));
      if (!window.__solariAppRendered) {
        appRenderTimer = window.setTimeout(function () {
          if (window.__solariAppRendered) return;
          var error = new Error("Dashboard modules finished without rendering a view");
          error.solariStage = "runtime";
          showFailure(error);
        }, 15000);
      }
    });
  }

  observeAppRender();
  Promise.resolve().then(boot).catch(showFailure);
})();
