/* @ds-bundle: {"format":4,"namespace":"SolariNetDesignSystem_95eca0","components":[{"name":"BandwidthGauge","sourcePath":"components/charts/BandwidthGauge.jsx"},{"name":"HealthDonut","sourcePath":"components/charts/HealthDonut.jsx"},{"name":"MetricBar","sourcePath":"components/charts/MetricBar.jsx"},{"name":"RTTBars","sourcePath":"components/charts/RTTBars.jsx"},{"name":"RadialGauge","sourcePath":"components/charts/RadialGauge.jsx"},{"name":"Sparkline","sourcePath":"components/charts/Sparkline.jsx"},{"name":"TimeSeries","sourcePath":"components/charts/TimeSeries.jsx"},{"name":"Button","sourcePath":"components/core/Button.jsx"},{"name":"Chip","sourcePath":"components/core/Chip.jsx"},{"name":"ICON_NAMES","sourcePath":"components/core/Icon.jsx"},{"name":"Icon","sourcePath":"components/core/Icon.jsx"},{"name":"IconButton","sourcePath":"components/core/IconButton.jsx"},{"name":"SearchField","sourcePath":"components/core/SearchField.jsx"},{"name":"SegmentedControl","sourcePath":"components/core/SegmentedControl.jsx"},{"name":"Switch","sourcePath":"components/core/Switch.jsx"},{"name":"Tag","sourcePath":"components/core/Tag.jsx"},{"name":"DataTable","sourcePath":"components/data/DataTable.jsx"},{"name":"MetricCard","sourcePath":"components/data/MetricCard.jsx"},{"name":"MetricRow","sourcePath":"components/data/MetricRow.jsx"},{"name":"NodeTile","sourcePath":"components/data/NodeTile.jsx"},{"name":"Panel","sourcePath":"components/data/Panel.jsx"},{"name":"PoolCard","sourcePath":"components/data/PoolCard.jsx"},{"name":"AlertRow","sourcePath":"components/feedback/AlertRow.jsx"},{"name":"CommandPalette","sourcePath":"components/feedback/CommandPalette.jsx"},{"name":"EmptyState","sourcePath":"components/feedback/EmptyState.jsx"},{"name":"Modal","sourcePath":"components/feedback/Modal.jsx"},{"name":"Toast","sourcePath":"components/feedback/Toast.jsx"},{"name":"ToastStack","sourcePath":"components/feedback/Toast.jsx"},{"name":"BrandMark","sourcePath":"components/navigation/BrandMark.jsx"},{"name":"PageHeader","sourcePath":"components/navigation/PageHeader.jsx"},{"name":"SidebarNav","sourcePath":"components/navigation/SidebarNav.jsx"},{"name":"TopBar","sourcePath":"components/navigation/TopBar.jsx"},{"name":"Heartbeat","sourcePath":"components/status/Heartbeat.jsx"},{"name":"StatusCell","sourcePath":"components/status/StatusCell.jsx"},{"name":"STATUS_COLOR","sourcePath":"components/status/StatusDot.jsx"},{"name":"STATUS_LABEL","sourcePath":"components/status/StatusDot.jsx"},{"name":"StatusDot","sourcePath":"components/status/StatusDot.jsx"},{"name":"StatusPill","sourcePath":"components/status/StatusPill.jsx"}],"sourceHashes":{"components/charts/BandwidthGauge.jsx":"8b9d63babe4b","components/charts/HealthDonut.jsx":"123283e97e7a","components/charts/MetricBar.jsx":"947249ccaa22","components/charts/RTTBars.jsx":"296ae85ce8b9","components/charts/RadialGauge.jsx":"2d60b508e4a7","components/charts/Sparkline.jsx":"7995db90e88f","components/charts/TimeSeries.jsx":"861d4af0c244","components/core/Button.jsx":"4a54434f51ce","components/core/Chip.jsx":"5dc34cf60d40","components/core/Icon.jsx":"dfe6fbb90b06","components/core/IconButton.jsx":"be72f32250bb","components/core/SearchField.jsx":"0ae5849e31f5","components/core/SegmentedControl.jsx":"34b63eb07834","components/core/Switch.jsx":"80354699271e","components/core/Tag.jsx":"65a6da95ce04","components/data/DataTable.jsx":"e76044df35f5","components/data/MetricCard.jsx":"07fb4bda6276","components/data/MetricRow.jsx":"3ca312b05597","components/data/NodeTile.jsx":"da11f19da755","components/data/Panel.jsx":"92ded2a53758","components/data/PoolCard.jsx":"13ba3c78174d","components/feedback/AlertRow.jsx":"6e3f9a1dbcdb","components/feedback/CommandPalette.jsx":"19a17dbdbb62","components/feedback/EmptyState.jsx":"551d54fc7edc","components/feedback/Modal.jsx":"40ab0f1e797b","components/feedback/Toast.jsx":"fb54dfcb791f","components/navigation/BrandMark.jsx":"e780806a8763","components/navigation/PageHeader.jsx":"df740c2ee70d","components/navigation/SidebarNav.jsx":"ca39894bebbd","components/navigation/TopBar.jsx":"f2b82e1bce14","components/status/Heartbeat.jsx":"a21ca21853b2","components/status/StatusCell.jsx":"feb838cfb533","components/status/StatusDot.jsx":"58b62cee8407","components/status/StatusPill.jsx":"eed6a8d01d97","src/js/01-0e23a051.jsx":"b436cf7ad2a0","src/js/02-91f5cdd6.jsx":"920ac9758264","src/js/03-ff8ac535.jsx":"e1e1609f5fa2","src/js/04-b4da7cd4.jsx":"421ef1ab8d29","src/js/05-d9cca021.jsx":"4874ca6c42c7","src/js/06-2381da4a.jsx":"1c712f5155f9","src/js/07-f83da9fd.jsx":"9e75f411e28b","ui_kits/monitoring-dashboard/AlertsScreen.jsx":"7d2a39565630","ui_kits/monitoring-dashboard/App.jsx":"9ca8afb01a64","ui_kits/monitoring-dashboard/DiscoveryScreen.jsx":"3252783c58ff","ui_kits/monitoring-dashboard/FleetOverview.jsx":"b1d50b80e8a0","ui_kits/monitoring-dashboard/ReachabilityScreen.jsx":"abbf42f7d91e","ui_kits/monitoring-dashboard/SystemDetail.jsx":"eed2c9507435","ui_kits/monitoring-dashboard/data.js":"6212ff1d5c14"},"inlinedExternals":[],"unexposedExports":[{"name":"metricColor","sourcePath":"components/charts/MetricBar.jsx"}]} */

(() => {

const __ds_ns = (window.SolariNetDesignSystem_95eca0 = window.SolariNetDesignSystem_95eca0 || {});

const __ds_scope = {};

(__ds_ns.__errors = __ds_ns.__errors || []);

// components/charts/BandwidthGauge.jsx
try { (() => {
function BandwidthGauge({
  label,
  used,
  cap,
  format = v => v,
  color = "var(--sn-accent)",
  style
}) {
  const pct = cap ? Math.min(100, used / cap * 100) : 0;
  const c = pct >= 85 ? "var(--sn-crit)" : pct >= 65 ? "var(--sn-warn)" : color;
  return /*#__PURE__*/React.createElement("div", {
    style: style
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      display: "flex",
      justifyContent: "space-between",
      fontFamily: "var(--sn-font-mono)",
      fontSize: 11,
      marginBottom: 6
    }
  }, /*#__PURE__*/React.createElement("span", {
    style: {
      letterSpacing: ".08em",
      textTransform: "uppercase",
      fontSize: 10,
      color: "var(--sn-text-tertiary)"
    }
  }, label), /*#__PURE__*/React.createElement("span", {
    style: {
      color: c,
      fontWeight: 600
    }
  }, format(used), " ", /*#__PURE__*/React.createElement("span", {
    style: {
      color: "var(--sn-text-tertiary)"
    }
  }, "/ ", format(cap)))), /*#__PURE__*/React.createElement("span", {
    style: {
      display: "block",
      width: "100%",
      height: 9,
      borderRadius: 4,
      background: "var(--sn-border-hairline)",
      overflow: "hidden"
    }
  }, /*#__PURE__*/React.createElement("i", {
    style: {
      display: "block",
      height: "100%",
      borderRadius: 4,
      width: pct + "%",
      background: c
    }
  })));
}
Object.assign(__ds_scope, { BandwidthGauge });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/charts/BandwidthGauge.jsx", error: String((e && e.message) || e) }); }

// components/charts/MetricBar.jsx
try { (() => {
function metricColor(pct) {
  if (pct >= 90) return "var(--sn-crit)";
  if (pct >= 75) return "var(--sn-warn)";
  return "var(--sn-accent)";
}
function MetricBar({
  pct,
  width = 78,
  height = 6,
  showValue = true,
  color,
  style
}) {
  const c = color || metricColor(pct);
  return /*#__PURE__*/React.createElement("span", {
    style: {
      display: "inline-flex",
      alignItems: "center",
      gap: 8,
      justifyContent: "flex-end",
      ...style
    }
  }, showValue ? /*#__PURE__*/React.createElement("span", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 12,
      width: 34,
      textAlign: "right",
      color: c
    }
  }, pct, "%") : null, /*#__PURE__*/React.createElement("span", {
    style: {
      width,
      height,
      borderRadius: 4,
      background: "var(--sn-border-hairline)",
      overflow: "hidden",
      display: "inline-block",
      verticalAlign: "middle"
    }
  }, /*#__PURE__*/React.createElement("i", {
    style: {
      display: "block",
      height: "100%",
      borderRadius: 4,
      width: Math.max(0, Math.min(100, pct)) + "%",
      background: c
    }
  })));
}
Object.assign(__ds_scope, { metricColor, MetricBar });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/charts/MetricBar.jsx", error: String((e && e.message) || e) }); }

// components/charts/RTTBars.jsx
try { (() => {
function RTTBars({
  vantages,
  height = 56,
  style
}) {
  const max = Math.max(1, ...vantages.map(v => v.rttMicros || 0));
  const barH = height - 8;
  return /*#__PURE__*/React.createElement("div", {
    style: {
      display: "flex",
      alignItems: "flex-end",
      gap: 6,
      height,
      ...style
    }
  }, vantages.map((v, i) => {
    const ok = v.outcome === "ok";
    const h = ok ? Math.max(4, v.rttMicros / max * barH) : barH;
    const c = !ok ? "var(--sn-crit)" : v.lossPermille > 0 ? "var(--sn-warn)" : "var(--sn-accent)";
    return /*#__PURE__*/React.createElement("div", {
      key: i,
      style: {
        flex: 1,
        textAlign: "center",
        minWidth: 0
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        height: barH,
        display: "flex",
        alignItems: "flex-end"
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        width: "100%",
        height: h,
        borderRadius: 3,
        background: ok ? c : "var(--sn-crit-tint)",
        border: ok ? "none" : "1px solid var(--sn-crit)"
      }
    })), /*#__PURE__*/React.createElement("div", {
      style: {
        fontFamily: "var(--sn-font-mono)",
        fontSize: 8.5,
        color: "var(--sn-text-tertiary)",
        marginTop: 4,
        whiteSpace: "nowrap",
        overflow: "hidden",
        textOverflow: "ellipsis"
      }
    }, v.monitorName));
  }));
}
Object.assign(__ds_scope, { RTTBars });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/charts/RTTBars.jsx", error: String((e && e.message) || e) }); }

// components/charts/RadialGauge.jsx
try { (() => {
function RadialGauge({
  value,
  max = 100,
  label,
  sub,
  size = 116,
  color,
  style
}) {
  const pct = Math.min(1, value / max);
  const r = size / 2 - 9;
  const circ = 2 * Math.PI * r;
  const c = color || (pct >= .9 ? "var(--sn-crit)" : pct >= .75 ? "var(--sn-warn)" : "var(--sn-accent)");
  return /*#__PURE__*/React.createElement("div", {
    style: {
      width: size,
      textAlign: "center",
      ...style
    }
  }, /*#__PURE__*/React.createElement("svg", {
    width: size,
    height: size,
    viewBox: "0 0 " + size + " " + size
  }, /*#__PURE__*/React.createElement("circle", {
    cx: size / 2,
    cy: size / 2,
    r: r,
    fill: "none",
    stroke: "var(--sn-border-hairline)",
    strokeWidth: "8"
  }), /*#__PURE__*/React.createElement("circle", {
    cx: size / 2,
    cy: size / 2,
    r: r,
    fill: "none",
    stroke: c,
    strokeWidth: "8",
    strokeLinecap: "round",
    strokeDasharray: circ,
    strokeDashoffset: circ * (1 - pct),
    transform: "rotate(-90 " + size / 2 + " " + size / 2 + ")",
    style: {
      transition: "stroke-dashoffset .5s var(--sn-ease)"
    }
  }), /*#__PURE__*/React.createElement("text", {
    x: "50%",
    y: "48%",
    textAnchor: "middle",
    dominantBaseline: "middle",
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 22,
      fontWeight: 600,
      fill: "var(--sn-text-primary)"
    }
  }, Math.round(value)), sub ? /*#__PURE__*/React.createElement("text", {
    x: "50%",
    y: "63%",
    textAnchor: "middle",
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 9,
      fill: "var(--sn-text-tertiary)"
    }
  }, sub) : null), label ? /*#__PURE__*/React.createElement("div", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 10,
      letterSpacing: ".1em",
      textTransform: "uppercase",
      color: "var(--sn-text-tertiary)",
      marginTop: 4
    }
  }, label) : null);
}
Object.assign(__ds_scope, { RadialGauge });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/charts/RadialGauge.jsx", error: String((e && e.message) || e) }); }

// components/charts/Sparkline.jsx
try { (() => {
function Sparkline({
  data,
  color = "var(--sn-accent)",
  width = 120,
  height = 28,
  fill = true,
  strokeWidth = 1.6,
  style
}) {
  if (!data || !data.length) return null;
  const min = Math.min(...data),
    max = Math.max(...data);
  const span = max - min || 1;
  const stepX = width / (data.length - 1);
  const pts = data.map((v, i) => [i * stepX, height - 3 - (v - min) / span * (height - 6)]);
  const line = pts.map((p, i) => (i ? "L" : "M") + p[0].toFixed(1) + " " + p[1].toFixed(1)).join(" ");
  const area = line + " L " + width + " " + height + " L 0 " + height + " Z";
  const gid = React.useMemo(() => "sn-sg-" + Math.random().toString(36).slice(2, 8), []);
  return /*#__PURE__*/React.createElement("svg", {
    width: "100%",
    height: height,
    viewBox: "0 0 " + width + " " + height,
    preserveAspectRatio: "none",
    style: {
      display: "block",
      ...style
    },
    "aria-hidden": "true"
  }, fill ? /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("defs", null, /*#__PURE__*/React.createElement("linearGradient", {
    id: gid,
    x1: "0",
    y1: "0",
    x2: "0",
    y2: "1"
  }, /*#__PURE__*/React.createElement("stop", {
    offset: "0",
    stopColor: color,
    stopOpacity: "0.35"
  }), /*#__PURE__*/React.createElement("stop", {
    offset: "1",
    stopColor: color,
    stopOpacity: "0"
  }))), /*#__PURE__*/React.createElement("path", {
    d: area,
    fill: "url(#" + gid + ")"
  })) : null, /*#__PURE__*/React.createElement("path", {
    d: line,
    fill: "none",
    stroke: color,
    strokeWidth: strokeWidth,
    strokeLinejoin: "round",
    strokeLinecap: "round",
    vectorEffect: "non-scaling-stroke"
  }));
}
Object.assign(__ds_scope, { Sparkline });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/charts/Sparkline.jsx", error: String((e && e.message) || e) }); }

// components/charts/TimeSeries.jsx
try { (() => {
function TimeSeries({
  data,
  color = "var(--sn-accent)",
  height = 150,
  max: forceMax,
  style
}) {
  const width = 600;
  const max = forceMax != null ? forceMax : Math.max(10, Math.ceil(Math.max(...data) / 10) * 10);
  const stepX = width / (data.length - 1);
  const y = v => height - 22 - v / max * (height - 34);
  const pts = data.map((v, i) => [i * stepX, y(v)]);
  const line = pts.map((p, i) => (i ? "L" : "M") + p[0].toFixed(1) + " " + p[1].toFixed(1)).join(" ");
  const area = line + " L " + width + " " + (height - 22) + " L 0 " + (height - 22) + " Z";
  const gid = React.useMemo(() => "sn-ts-" + Math.random().toString(36).slice(2, 8), []);
  const last = data[data.length - 1];
  return /*#__PURE__*/React.createElement("svg", {
    width: "100%",
    height: height,
    viewBox: "0 0 " + width + " " + height,
    preserveAspectRatio: "none",
    style: {
      display: "block",
      ...style
    },
    "aria-hidden": "true"
  }, /*#__PURE__*/React.createElement("defs", null, /*#__PURE__*/React.createElement("linearGradient", {
    id: gid,
    x1: "0",
    y1: "0",
    x2: "0",
    y2: "1"
  }, /*#__PURE__*/React.createElement("stop", {
    offset: "0",
    stopColor: color,
    stopOpacity: "0.30"
  }), /*#__PURE__*/React.createElement("stop", {
    offset: "1",
    stopColor: color,
    stopOpacity: "0"
  }))), [0, .5, 1].map((g, i) => /*#__PURE__*/React.createElement("line", {
    key: i,
    x1: "0",
    x2: width,
    y1: height - 22 - g * (height - 34),
    y2: height - 22 - g * (height - 34),
    stroke: "var(--sn-border-hairline)",
    strokeWidth: "1",
    vectorEffect: "non-scaling-stroke"
  })), /*#__PURE__*/React.createElement("path", {
    d: area,
    fill: "url(#" + gid + ")"
  }), /*#__PURE__*/React.createElement("path", {
    d: line,
    fill: "none",
    stroke: color,
    strokeWidth: "1.8",
    strokeLinejoin: "round",
    strokeLinecap: "round",
    vectorEffect: "non-scaling-stroke"
  }), /*#__PURE__*/React.createElement("circle", {
    cx: width,
    cy: y(last),
    r: "3",
    fill: color
  }));
}
Object.assign(__ds_scope, { TimeSeries });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/charts/TimeSeries.jsx", error: String((e && e.message) || e) }); }

// components/core/Icon.jsx
try { (() => {
/* SolariNet's own glyph set, lifted verbatim from the product build
   (src: dashboard icons module). Solid geometric marks on a 24 grid,
   drawn in currentColor. Knockouts use --sn-on-field. */
const PATHS = {
  // nav / structure
  overview: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
    x: "3",
    y: "3",
    width: "8",
    height: "8",
    rx: "1.5"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "13",
    y: "3",
    width: "8",
    height: "5",
    rx: "1.5"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "13",
    y: "10",
    width: "8",
    height: "11",
    rx: "1.5"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "3",
    y: "13",
    width: "8",
    height: "8",
    rx: "1.5"
  })),
  server: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
    x: "3",
    y: "3",
    width: "18",
    height: "7",
    rx: "2"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "3",
    y: "14",
    width: "18",
    height: "7",
    rx: "2"
  }), /*#__PURE__*/React.createElement("circle", {
    cx: "7",
    cy: "6.5",
    r: "1.2",
    fill: "var(--sn-on-field)"
  }), /*#__PURE__*/React.createElement("circle", {
    cx: "7",
    cy: "17.5",
    r: "1.2",
    fill: "var(--sn-on-field)"
  })),
  host: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
    x: "3",
    y: "4",
    width: "18",
    height: "12",
    rx: "2"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "8",
    y: "18",
    width: "8",
    height: "2.4",
    rx: "1.2"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "6",
    y: "19.6",
    width: "12",
    height: "1.8",
    rx: "0.9"
  })),
  monitor: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("circle", {
    cx: "12",
    cy: "12",
    r: "2.6"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M12 2a10 10 0 0 1 10 10h-3a7 7 0 0 0-7-7z",
    opacity: "0.85"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M12 22A10 10 0 0 1 2 12h3a7 7 0 0 0 7 7z",
    opacity: "0.55"
  })),
  reachability: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("circle", {
    cx: "5",
    cy: "6",
    r: "2.4"
  }), /*#__PURE__*/React.createElement("circle", {
    cx: "5",
    cy: "18",
    r: "2.4"
  }), /*#__PURE__*/React.createElement("circle", {
    cx: "19",
    cy: "12",
    r: "2.4"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M7 7l10 4M7 17l10-4",
    stroke: "currentColor",
    strokeWidth: "1.7",
    fill: "none"
  })),
  topology: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("circle", {
    cx: "12",
    cy: "5",
    r: "2.4"
  }), /*#__PURE__*/React.createElement("circle", {
    cx: "5",
    cy: "18",
    r: "2.4"
  }), /*#__PURE__*/React.createElement("circle", {
    cx: "19",
    cy: "18",
    r: "2.4"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M12 7v4m0 0l-6 6m6-6l6 6",
    stroke: "currentColor",
    strokeWidth: "1.7",
    fill: "none"
  })),
  alerts: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
    d: "M12 3l9 16H3z"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "11",
    y: "9",
    width: "2",
    height: "5",
    rx: "1",
    fill: "var(--sn-on-field)"
  }), /*#__PURE__*/React.createElement("circle", {
    cx: "12",
    cy: "16.5",
    r: "1.1",
    fill: "var(--sn-on-field)"
  })),
  discovery: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("circle", {
    cx: "11",
    cy: "11",
    r: "6.5",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M16 16l5 5",
    stroke: "currentColor",
    strokeWidth: "2.4",
    strokeLinecap: "round"
  }), /*#__PURE__*/React.createElement("circle", {
    cx: "11",
    cy: "11",
    r: "2"
  })),
  provision: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
    x: "4",
    y: "4",
    width: "16",
    height: "16",
    rx: "3",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M12 8v8M8 12h8",
    stroke: "currentColor",
    strokeWidth: "2.2",
    strokeLinecap: "round"
  })),
  settings: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("circle", {
    cx: "12",
    cy: "12",
    r: "3"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M12 2l1.5 3 3.3-.6L17 7.7 20 9l-1.3 3L20 15l-2.2 2.3.2 3.3L15 21l-3 1-3-1-2.9.6.2-3.3L4 15l1.3-3L4 9l3-1.3-.2-3.3L10 5z",
    opacity: "0.35"
  })),
  // metrics
  cpu: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
    x: "6",
    y: "6",
    width: "12",
    height: "12",
    rx: "2"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "9",
    y: "9",
    width: "6",
    height: "6",
    rx: "1",
    fill: "var(--sn-on-field)"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M9 2v3M15 2v3M9 19v3M15 19v3M2 9h3M2 15h3M19 9h3M19 15h3",
    stroke: "currentColor",
    strokeWidth: "1.8",
    strokeLinecap: "round"
  })),
  ram: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
    x: "2",
    y: "7",
    width: "20",
    height: "10",
    rx: "2"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "5",
    y: "10",
    width: "2.5",
    height: "7",
    fill: "var(--sn-on-field)"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "9",
    y: "10",
    width: "2.5",
    height: "7",
    fill: "var(--sn-on-field)"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "13",
    y: "10",
    width: "2.5",
    height: "7",
    fill: "var(--sn-on-field)"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "17",
    y: "10",
    width: "2.5",
    height: "7",
    fill: "var(--sn-on-field)"
  })),
  disk: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("circle", {
    cx: "12",
    cy: "12",
    r: "9"
  }), /*#__PURE__*/React.createElement("circle", {
    cx: "12",
    cy: "12",
    r: "2.4",
    fill: "var(--sn-on-field)"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M12 3v4",
    stroke: "var(--sn-on-field)",
    strokeWidth: "1.6"
  })),
  network: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
    x: "9",
    y: "2",
    width: "6",
    height: "5",
    rx: "1.4"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "2",
    y: "17",
    width: "6",
    height: "5",
    rx: "1.4"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "16",
    y: "17",
    width: "6",
    height: "5",
    rx: "1.4"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M12 7v4m0 0H5v6m7-6h7v6",
    stroke: "currentColor",
    strokeWidth: "1.7",
    fill: "none"
  })),
  bandwidth: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
    d: "M3 17a9 9 0 0 1 18 0",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M12 17l5-4",
    stroke: "currentColor",
    strokeWidth: "2.2",
    strokeLinecap: "round"
  }), /*#__PURE__*/React.createElement("circle", {
    cx: "12",
    cy: "17",
    r: "1.8"
  })),
  activity: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
    d: "M2 12h4l3 8 4-16 3 8h6",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2.2",
    strokeLinecap: "round",
    strokeLinejoin: "round"
  })),
  process: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
    x: "3",
    y: "3",
    width: "18",
    height: "18",
    rx: "3",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M7 9l3 3-3 3M12 15h5",
    stroke: "currentColor",
    strokeWidth: "2",
    strokeLinecap: "round",
    strokeLinejoin: "round"
  })),
  // ui
  search: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("circle", {
    cx: "11",
    cy: "11",
    r: "6.5",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M16 16l5 5",
    stroke: "currentColor",
    strokeWidth: "2.4",
    strokeLinecap: "round"
  })),
  command: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
    d: "M9 3a3 3 0 1 1-3 3v12a3 3 0 1 1 3-3h6a3 3 0 1 1-3 3V6a3 3 0 1 1 3 3H9",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2"
  })),
  grid: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
    x: "3",
    y: "3",
    width: "7",
    height: "7",
    rx: "1.5"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "14",
    y: "3",
    width: "7",
    height: "7",
    rx: "1.5"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "3",
    y: "14",
    width: "7",
    height: "7",
    rx: "1.5"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "14",
    y: "14",
    width: "7",
    height: "7",
    rx: "1.5"
  })),
  table: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
    x: "3",
    y: "4",
    width: "18",
    height: "16",
    rx: "2",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M3 9h18M3 14h18M9 9v11",
    stroke: "currentColor",
    strokeWidth: "1.7"
  })),
  cards: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
    x: "3",
    y: "4",
    width: "18",
    height: "7",
    rx: "2"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "3",
    y: "13",
    width: "18",
    height: "7",
    rx: "2",
    opacity: "0.5"
  })),
  bell: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
    d: "M6 16V11a6 6 0 0 1 12 0v5l2 2H4z"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M10 20a2 2 0 0 0 4 0",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "1.8"
  })),
  sun: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("circle", {
    cx: "12",
    cy: "12",
    r: "4.5"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M12 1v3M12 20v3M1 12h3M20 12h3M4.2 4.2l2.1 2.1M17.7 17.7l2.1 2.1M19.8 4.2l-2.1 2.1M6.3 17.7l-2.1 2.1",
    stroke: "currentColor",
    strokeWidth: "2",
    strokeLinecap: "round"
  })),
  moon: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
    d: "M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z"
  })),
  menu: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
    d: "M4 6h16M4 12h16M4 18h16",
    stroke: "currentColor",
    strokeWidth: "2.2",
    strokeLinecap: "round"
  })),
  close: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
    d: "M6 6l12 12M18 6L6 18",
    stroke: "currentColor",
    strokeWidth: "2.2",
    strokeLinecap: "round"
  })),
  chevronLeft: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
    d: "M15 5l-7 7 7 7",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2.2",
    strokeLinecap: "round",
    strokeLinejoin: "round"
  })),
  chevronRight: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
    d: "M9 5l7 7-7 7",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2.2",
    strokeLinecap: "round",
    strokeLinejoin: "round"
  })),
  check: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
    d: "M4 12l5 5L20 6",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2.4",
    strokeLinecap: "round",
    strokeLinejoin: "round"
  })),
  refresh: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
    d: "M20 11a8 8 0 1 0-1 5",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2.2",
    strokeLinecap: "round"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M20 4v6h-6",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2.2",
    strokeLinecap: "round",
    strokeLinejoin: "round"
  })),
  survey: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("circle", {
    cx: "12",
    cy: "12",
    r: "2.2"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M12 12L4 6M12 12l8-6",
    stroke: "currentColor",
    strokeWidth: "1.8",
    strokeLinecap: "round"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M5 19a10 10 0 0 1 14 0",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2",
    strokeLinecap: "round"
  })),
  filter: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
    d: "M3 5h18l-7 8v6l-4 2v-8z"
  })),
  shield: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
    d: "M12 2l8 3v6c0 5-3.5 8.5-8 11-4.5-2.5-8-6-8-11V5z"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M8.5 12l2.5 2.5 4.5-5",
    fill: "none",
    stroke: "var(--sn-on-field)",
    strokeWidth: "1.8",
    strokeLinecap: "round",
    strokeLinejoin: "round"
  })),
  pulse: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
    d: "M2 12h5l2-5 4 12 2-7h7",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2",
    strokeLinecap: "round",
    strokeLinejoin: "round"
  })),
  clock: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("circle", {
    cx: "12",
    cy: "12",
    r: "9",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M12 7v5l3.5 2",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2",
    strokeLinecap: "round",
    strokeLinejoin: "round"
  })),
  arch: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
    x: "4",
    y: "3",
    width: "16",
    height: "4",
    rx: "1.4"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "4",
    y: "10",
    width: "16",
    height: "4",
    rx: "1.4",
    opacity: "0.7"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "4",
    y: "17",
    width: "16",
    height: "4",
    rx: "1.4",
    opacity: "0.45"
  })),
  chip: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
    x: "6",
    y: "6",
    width: "12",
    height: "12",
    rx: "2"
  })),
  link: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
    d: "M9 15l6-6",
    stroke: "currentColor",
    strokeWidth: "2",
    strokeLinecap: "round"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M8 12l-2 2a3 3 0 0 0 4 4l2-2M16 12l2-2a3 3 0 0 0-4-4l-2 2",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2",
    strokeLinecap: "round"
  })),
  plus: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
    d: "M12 5v14M5 12h14",
    stroke: "currentColor",
    strokeWidth: "2.4",
    strokeLinecap: "round"
  })),
  enter: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
    d: "M9 10l-4 4 4 4",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "1.8",
    strokeLinecap: "round",
    strokeLinejoin: "round"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M5 14h10a4 4 0 0 0 4-4V6",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "1.8",
    strokeLinecap: "round",
    strokeLinejoin: "round"
  })),
  gateway: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
    x: "3",
    y: "9",
    width: "18",
    height: "9",
    rx: "2"
  }), /*#__PURE__*/React.createElement("circle", {
    cx: "7",
    cy: "13.5",
    r: "1.3",
    fill: "var(--sn-on-field)"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "11",
    y: "12.5",
    width: "7",
    height: "2",
    rx: "1",
    fill: "var(--sn-on-field)"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M8 9V6a4 4 0 0 1 8 0v3",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "1.8"
  })),
  netswitch: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
    x: "2",
    y: "8",
    width: "20",
    height: "8",
    rx: "1.6"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "5",
    y: "11",
    width: "2",
    height: "2.5",
    fill: "var(--sn-on-field)"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "8.5",
    y: "11",
    width: "2",
    height: "2.5",
    fill: "var(--sn-on-field)"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "12",
    y: "11",
    width: "2",
    height: "2.5",
    fill: "var(--sn-on-field)"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "15.5",
    y: "11",
    width: "2",
    height: "2.5",
    fill: "var(--sn-on-field)"
  })),
  wifi: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
    d: "M5 12.5a10 10 0 0 1 14 0",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2",
    strokeLinecap: "round"
  }), /*#__PURE__*/React.createElement("path", {
    d: "M8 15.5a6 6 0 0 1 8 0",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2",
    strokeLinecap: "round"
  }), /*#__PURE__*/React.createElement("circle", {
    cx: "12",
    cy: "18.5",
    r: "1.6"
  })),
  close2: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
    d: "M6 6l12 12M18 6L6 18",
    stroke: "currentColor",
    strokeWidth: "2.2",
    strokeLinecap: "round"
  }))
};
const ICON_NAMES = Object.keys(PATHS);
function Icon({
  name,
  size = 18,
  className,
  style,
  title
}) {
  const body = PATHS[name];
  if (!body) return null;
  return /*#__PURE__*/React.createElement("svg", {
    className: className,
    style: style,
    width: size,
    height: size,
    viewBox: "0 0 24 24",
    fill: "currentColor",
    role: title ? "img" : undefined,
    "aria-hidden": title ? undefined : "true",
    "aria-label": title,
    focusable: "false"
  }, title ? /*#__PURE__*/React.createElement("title", null, title) : null, body);
}
Object.assign(__ds_scope, { ICON_NAMES, Icon });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/core/Icon.jsx", error: String((e && e.message) || e) }); }

// components/core/Button.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
const base = {
  display: "inline-flex",
  alignItems: "center",
  gap: 7,
  whiteSpace: "nowrap",
  fontFamily: "var(--sn-font-sans)",
  fontWeight: 600,
  cursor: "pointer",
  borderRadius: "var(--sn-radius-md)",
  border: "1px solid transparent",
  transition: "background var(--sn-dur-fast) var(--sn-ease), color var(--sn-dur-fast) var(--sn-ease), border-color var(--sn-dur-fast) var(--sn-ease)"
};
const sizes = {
  sm: {
    minHeight: 32,
    padding: "0 11px",
    fontSize: 12.5
  },
  md: {
    minHeight: 38,
    padding: "0 14px",
    fontSize: 13
  },
  lg: {
    minHeight: 44,
    padding: "0 18px",
    fontSize: 13.5
  }
};
const variants = {
  primary: {
    background: "var(--sn-surface-control-active)",
    color: "var(--sn-accent)",
    borderColor: "var(--sn-border-interactive)"
  },
  secondary: {
    background: "var(--sn-surface-control)",
    color: "var(--sn-text-secondary)",
    borderColor: "var(--sn-border-hairline)"
  },
  ghost: {
    background: "transparent",
    color: "var(--sn-text-secondary)",
    borderColor: "var(--sn-border-hairline)"
  },
  danger: {
    background: "var(--sn-crit-tint)",
    color: "var(--sn-crit)",
    borderColor: "var(--sn-crit)"
  }
};
function Button({
  variant = "secondary",
  size = "md",
  icon,
  iconRight,
  disabled,
  children,
  style,
  onClick,
  type = "button",
  ...rest
}) {
  return /*#__PURE__*/React.createElement("button", _extends({
    type: type,
    onClick: disabled ? undefined : onClick,
    disabled: disabled,
    "aria-disabled": disabled || undefined,
    style: {
      ...base,
      ...sizes[size],
      ...variants[variant],
      ...(disabled ? {
        opacity: .4,
        pointerEvents: "none"
      } : null),
      ...style
    }
  }, rest), icon ? /*#__PURE__*/React.createElement(__ds_scope.Icon, {
    name: icon,
    size: size === "sm" ? 14 : 16
  }) : null, children, iconRight ? /*#__PURE__*/React.createElement(__ds_scope.Icon, {
    name: iconRight,
    size: size === "sm" ? 14 : 16
  }) : null);
}
Object.assign(__ds_scope, { Button });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/core/Button.jsx", error: String((e && e.message) || e) }); }

// components/core/IconButton.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
function IconButton({
  icon,
  label,
  active,
  size = 44,
  glyph = 20,
  onClick,
  style,
  ...rest
}) {
  return /*#__PURE__*/React.createElement("button", _extends({
    type: "button",
    onClick: onClick,
    "aria-label": label,
    title: label,
    style: {
      width: size,
      height: size,
      flex: "0 0 " + size + "px",
      display: "grid",
      placeItems: "center",
      border: "1px solid " + (active ? "var(--sn-border-interactive)" : "var(--sn-border-hairline)"),
      background: "var(--sn-surface-control)",
      color: active ? "var(--sn-accent)" : "var(--sn-text-secondary)",
      borderRadius: "var(--sn-radius-md)",
      cursor: "pointer",
      transition: "border-color var(--sn-dur-fast) var(--sn-ease), color var(--sn-dur-fast) var(--sn-ease), background var(--sn-dur-fast) var(--sn-ease)",
      ...style
    }
  }, rest), /*#__PURE__*/React.createElement(__ds_scope.Icon, {
    name: icon,
    size: glyph
  }));
}
Object.assign(__ds_scope, { IconButton });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/core/IconButton.jsx", error: String((e && e.message) || e) }); }

// components/core/SearchField.jsx
try { (() => {
function SearchField({
  placeholder = "Search systems, targets, alerts…",
  value,
  onChange,
  onClick,
  shortcut = "⌘K",
  readOnly,
  style
}) {
  return /*#__PURE__*/React.createElement("div", {
    onClick: onClick,
    style: {
      flex: "1 1 120px",
      minWidth: 0,
      maxWidth: 520,
      height: "var(--sn-tap)",
      display: "flex",
      alignItems: "center",
      gap: 10,
      padding: "0 14px",
      border: "1px solid var(--sn-border-hairline)",
      borderRadius: "var(--sn-radius-md)",
      background: "var(--sn-surface-control)",
      color: "var(--sn-text-tertiary)",
      cursor: "text",
      ...style
    }
  }, /*#__PURE__*/React.createElement(__ds_scope.Icon, {
    name: "search",
    size: 17,
    style: {
      flex: "0 0 auto"
    }
  }), /*#__PURE__*/React.createElement("input", {
    value: value,
    onChange: onChange ? e => onChange(e.target.value) : undefined,
    readOnly: readOnly,
    placeholder: placeholder,
    "aria-label": "Search",
    style: {
      flex: "1 1 auto",
      minWidth: 0,
      border: "none",
      background: "none",
      outline: "none",
      color: "var(--sn-text-primary)",
      fontFamily: "var(--sn-font-sans)",
      fontSize: 14
    }
  }), shortcut ? /*#__PURE__*/React.createElement("span", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 10,
      letterSpacing: ".06em",
      border: "1px solid var(--sn-border-strong)",
      borderRadius: 5,
      padding: "2px 6px",
      color: "var(--sn-text-tertiary)",
      flex: "0 0 auto"
    }
  }, shortcut) : null);
}
Object.assign(__ds_scope, { SearchField });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/core/SearchField.jsx", error: String((e && e.message) || e) }); }

// components/core/SegmentedControl.jsx
try { (() => {
function SegmentedControl({
  options,
  value,
  onChange,
  style
}) {
  return /*#__PURE__*/React.createElement("div", {
    role: "tablist",
    style: {
      display: "inline-flex",
      border: "1px solid var(--sn-border-hairline)",
      borderRadius: "var(--sn-radius-md)",
      background: "var(--sn-surface-control)",
      padding: 3,
      gap: 2,
      ...style
    }
  }, options.map(o => {
    const on = o.value === value;
    return /*#__PURE__*/React.createElement("button", {
      key: o.value,
      type: "button",
      role: "tab",
      "aria-selected": on,
      onClick: () => onChange(o.value),
      style: {
        minHeight: 36,
        padding: "0 13px",
        border: "none",
        cursor: "pointer",
        background: on ? "var(--sn-surface-control-active)" : "transparent",
        boxShadow: on ? "inset 0 0 0 1px var(--sn-border-interactive)" : "none",
        color: on ? "var(--sn-accent)" : "var(--sn-text-secondary)",
        fontFamily: "var(--sn-font-sans)",
        fontSize: 12.5,
        fontWeight: 500,
        letterSpacing: ".01em",
        borderRadius: 6,
        display: "inline-flex",
        alignItems: "center",
        gap: 7,
        transition: "background var(--sn-dur-fast) var(--sn-ease), color var(--sn-dur-fast) var(--sn-ease)"
      }
    }, o.icon ? /*#__PURE__*/React.createElement(__ds_scope.Icon, {
      name: o.icon,
      size: 14
    }) : null, o.label);
  }));
}
Object.assign(__ds_scope, { SegmentedControl });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/core/SegmentedControl.jsx", error: String((e && e.message) || e) }); }

// components/core/Switch.jsx
try { (() => {
function Switch({
  checked,
  onChange,
  label,
  disabled,
  style
}) {
  return /*#__PURE__*/React.createElement("button", {
    type: "button",
    role: "switch",
    "aria-checked": !!checked,
    "aria-label": label,
    disabled: disabled,
    onClick: () => !disabled && onChange(!checked),
    style: {
      width: 46,
      height: 27,
      flex: "0 0 46px",
      borderRadius: "var(--sn-radius-pill)",
      border: "none",
      position: "relative",
      cursor: disabled ? "default" : "pointer",
      opacity: disabled ? .4 : 1,
      background: checked ? "var(--sn-ok)" : "var(--sn-border-strong)",
      transition: "background var(--sn-dur-base) var(--sn-ease)",
      ...style
    }
  }, /*#__PURE__*/React.createElement("i", {
    style: {
      position: "absolute",
      top: 3,
      left: checked ? 22 : 3,
      width: 21,
      height: 21,
      borderRadius: "50%",
      background: "#fff",
      transition: "left var(--sn-dur-base) var(--sn-ease)"
    }
  }));
}
Object.assign(__ds_scope, { Switch });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/core/Switch.jsx", error: String((e && e.message) || e) }); }

// components/core/Tag.jsx
try { (() => {
function Tag({
  children,
  tone = "neutral",
  style
}) {
  const tones = {
    neutral: {
      background: "var(--sn-raised)",
      color: "var(--sn-text-secondary)",
      borderColor: "var(--sn-border-hairline)"
    },
    accent: {
      background: "var(--sn-azure-tint)",
      color: "var(--sn-accent)",
      borderColor: "var(--sn-border-interactive)"
    }
  };
  return /*#__PURE__*/React.createElement("span", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 10,
      letterSpacing: ".08em",
      textTransform: "uppercase",
      padding: "2px 8px",
      borderRadius: "var(--sn-radius-sm)",
      border: "1px solid",
      ...tones[tone],
      ...style
    }
  }, children);
}
Object.assign(__ds_scope, { Tag });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/core/Tag.jsx", error: String((e && e.message) || e) }); }

// components/data/DataTable.jsx
try { (() => {
function DataTable({
  columns,
  rows,
  onRowClick,
  sort,
  onSort,
  style
}) {
  return /*#__PURE__*/React.createElement("div", {
    style: {
      border: "1px solid var(--sn-border-hairline)",
      borderRadius: "var(--sn-radius-lg)",
      overflow: "hidden",
      background: "var(--sn-surface-card)",
      ...style
    }
  }, /*#__PURE__*/React.createElement("table", {
    style: {
      width: "100%",
      borderCollapse: "collapse",
      fontSize: 13,
      fontVariantNumeric: "tabular-nums"
    }
  }, /*#__PURE__*/React.createElement("thead", null, /*#__PURE__*/React.createElement("tr", null, columns.map(c => {
    const on = sort && sort.key === c.key;
    return /*#__PURE__*/React.createElement("th", {
      key: c.key,
      onClick: onSort ? () => onSort(c.key) : undefined,
      style: {
        textAlign: c.align === "right" ? "right" : "left",
        fontFamily: "var(--sn-font-mono)",
        fontSize: 10,
        letterSpacing: ".12em",
        textTransform: "uppercase",
        color: "var(--sn-text-tertiary)",
        padding: "11px 14px",
        background: "var(--sn-raised)",
        borderBottom: "1px solid var(--sn-border-hairline)",
        cursor: onSort ? "pointer" : "default",
        whiteSpace: "nowrap"
      }
    }, c.label, on ? /*#__PURE__*/React.createElement("span", {
      style: {
        color: "var(--sn-accent)",
        marginLeft: 4
      }
    }, sort.dir === 1 ? "▲" : "▼") : null);
  }))), /*#__PURE__*/React.createElement("tbody", null, rows.map((r, i) => /*#__PURE__*/React.createElement("tr", {
    key: r.id != null ? r.id : i,
    onClick: onRowClick ? () => onRowClick(r) : undefined,
    style: {
      cursor: onRowClick ? "pointer" : "default"
    }
  }, columns.map(c => /*#__PURE__*/React.createElement("td", {
    key: c.key,
    style: {
      padding: "10px 14px",
      borderBottom: i === rows.length - 1 ? "none" : "1px solid var(--sn-border-hairline)",
      verticalAlign: "middle",
      whiteSpace: "nowrap",
      textAlign: c.align === "right" ? "right" : "left"
    }
  }, c.render ? c.render(r) : r[c.key])))))), rows.length === 0 ? /*#__PURE__*/React.createElement("div", {
    style: {
      textAlign: "center",
      color: "var(--sn-text-tertiary)",
      fontFamily: "var(--sn-font-mono)",
      fontSize: 13,
      padding: "60px 20px"
    }
  }, "No rows match this filter") : null);
}
Object.assign(__ds_scope, { DataTable });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/data/DataTable.jsx", error: String((e && e.message) || e) }); }

// components/data/MetricCard.jsx
try { (() => {
/* Nominal cards are neutral: no coloured underline, no halo. Colour appears only
   when the value itself is a fault, and the caption must agree with the number. */
function MetricCard({
  label,
  value,
  unit,
  caption,
  state = "nominal",
  compact,
  style
}) {
  const fault = state === "degraded" || state === "down";
  const colour = state === "degraded" ? "var(--sn-warn)" : state === "down" ? "var(--sn-crit)" : "var(--sn-text-primary)";
  return /*#__PURE__*/React.createElement("div", {
    style: {
      border: "1px solid " + (fault ? colour : "var(--sn-border-hairline)"),
      borderRadius: "var(--sn-radius-lg)",
      background: "var(--sn-surface-card)",
      padding: compact ? "10px 14px" : "14px 16px",
      position: "relative",
      overflow: "hidden",
      boxShadow: state === "down" ? "var(--sn-glow-crit)" : "none",
      ...style
    }
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 10,
      letterSpacing: "var(--sn-track-label)",
      textTransform: "uppercase",
      color: "var(--sn-text-tertiary)"
    }
  }, label), /*#__PURE__*/React.createElement("div", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: compact ? 20 : "var(--sn-text-metric)",
      fontWeight: 600,
      lineHeight: "var(--sn-lh-metric)",
      marginTop: 6,
      letterSpacing: "var(--sn-track-metric)",
      color: colour,
      fontVariantNumeric: "tabular-nums"
    }
  }, value, unit ? /*#__PURE__*/React.createElement("span", {
    style: {
      fontSize: 16,
      color: "var(--sn-text-tertiary)"
    }
  }, unit) : null), caption ? /*#__PURE__*/React.createElement("div", {
    style: {
      fontSize: 11.5,
      color: "var(--sn-text-secondary)",
      marginTop: 2
    }
  }, caption) : null);
}
Object.assign(__ds_scope, { MetricCard });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/data/MetricCard.jsx", error: String((e && e.message) || e) }); }

// components/data/MetricRow.jsx
try { (() => {
function MetricRow({
  label,
  value,
  pct,
  color,
  last,
  style
}) {
  const c = color || __ds_scope.metricColor(pct);
  return /*#__PURE__*/React.createElement("div", {
    style: {
      display: "flex",
      alignItems: "center",
      gap: 14,
      padding: "11px 0",
      borderBottom: last ? "none" : "1px dashed var(--sn-border-hairline)",
      ...style
    }
  }, /*#__PURE__*/React.createElement("span", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 11,
      letterSpacing: ".06em",
      textTransform: "uppercase",
      color: "var(--sn-text-tertiary)",
      width: 110,
      flex: "0 0 110px"
    }
  }, label), /*#__PURE__*/React.createElement("span", {
    style: {
      flex: "1 1 auto",
      minWidth: 0,
      height: 8,
      borderRadius: 5,
      background: "var(--sn-border-hairline)",
      overflow: "hidden"
    }
  }, /*#__PURE__*/React.createElement("i", {
    style: {
      display: "block",
      height: "100%",
      borderRadius: 5,
      width: Math.max(0, Math.min(100, pct)) + "%",
      background: c
    }
  })), /*#__PURE__*/React.createElement("span", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 15,
      fontWeight: 600,
      width: 120,
      flex: "0 0 120px",
      textAlign: "right",
      fontVariantNumeric: "tabular-nums"
    }
  }, value));
}
Object.assign(__ds_scope, { MetricRow });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/data/MetricRow.jsx", error: String((e && e.message) || e) }); }

// components/data/Panel.jsx
try { (() => {
function Panel({
  title,
  icon,
  right,
  children,
  padded = true,
  style,
  bodyStyle
}) {
  return /*#__PURE__*/React.createElement("section", {
    style: {
      border: "1px solid var(--sn-border-hairline)",
      borderRadius: "var(--sn-radius-lg)",
      background: "var(--sn-surface-card)",
      overflow: "hidden",
      ...style
    }
  }, title ? /*#__PURE__*/React.createElement("header", {
    style: {
      display: "flex",
      alignItems: "center",
      gap: 10,
      padding: "13px 16px",
      borderBottom: "1px solid var(--sn-border-hairline)"
    }
  }, icon ? /*#__PURE__*/React.createElement(__ds_scope.Icon, {
    name: icon,
    size: 16,
    style: {
      color: "var(--sn-accent)"
    }
  }) : null, /*#__PURE__*/React.createElement("h3", {
    style: {
      margin: 0,
      fontFamily: "var(--sn-font-sans)",
      fontSize: "var(--sn-text-h2)",
      fontWeight: 600,
      letterSpacing: "-.005em",
      color: "var(--sn-text-primary)"
    }
  }, title), right ? /*#__PURE__*/React.createElement("div", {
    style: {
      marginLeft: "auto",
      fontFamily: "var(--sn-font-mono)",
      fontSize: 11,
      color: "var(--sn-text-tertiary)"
    }
  }, right) : null) : null, /*#__PURE__*/React.createElement("div", {
    style: {
      padding: padded ? "var(--sn-panel-pad)" : 0,
      ...bodyStyle
    }
  }, children));
}
Object.assign(__ds_scope, { Panel });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/data/Panel.jsx", error: String((e && e.message) || e) }); }

// components/feedback/EmptyState.jsx
try { (() => {
function EmptyState({
  kind = "not-configured",
  icon,
  title,
  description,
  actionLabel,
  onAction,
  style
}) {
  return /*#__PURE__*/React.createElement("div", {
    style: {
      display: "grid",
      placeItems: "center",
      padding: "60px 20px",
      ...style
    }
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      textAlign: "center",
      maxWidth: 420
    }
  }, icon ? /*#__PURE__*/React.createElement(__ds_scope.Icon, {
    name: icon,
    size: 52,
    style: {
      color: "var(--sn-text-tertiary)",
      marginBottom: 14
    }
  }) : null, /*#__PURE__*/React.createElement("h2", {
    style: {
      margin: 0,
      fontFamily: "var(--sn-font-sans)",
      fontSize: "var(--sn-text-h2)",
      fontWeight: 600
    }
  }, title), description ? /*#__PURE__*/React.createElement("p", {
    style: {
      color: "var(--sn-text-secondary)",
      fontSize: "var(--sn-text-body)",
      lineHeight: "var(--sn-lh-body)",
      marginTop: 8
    }
  }, description) : null, actionLabel ? /*#__PURE__*/React.createElement("div", {
    style: {
      marginTop: 16
    }
  }, /*#__PURE__*/React.createElement(__ds_scope.Button, {
    variant: kind === "filtered" ? "ghost" : "primary",
    onClick: onAction
  }, actionLabel)) : null));
}
Object.assign(__ds_scope, { EmptyState });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/feedback/EmptyState.jsx", error: String((e && e.message) || e) }); }

// components/feedback/Modal.jsx
try { (() => {
function Modal({
  open,
  title,
  icon,
  children,
  footer,
  onClose,
  width = 560
}) {
  if (!open) return null;
  return /*#__PURE__*/React.createElement("div", {
    onClick: onClose,
    style: {
      position: "fixed",
      inset: 0,
      background: "var(--sn-scrim)",
      backdropFilter: "var(--sn-blur-overlay)",
      zIndex: 100,
      display: "flex",
      alignItems: "center",
      justifyContent: "center",
      padding: 24
    }
  }, /*#__PURE__*/React.createElement("div", {
    role: "dialog",
    "aria-modal": "true",
    "aria-label": title,
    onClick: e => e.stopPropagation(),
    style: {
      width: "min(" + width + "px, 94vw)",
      maxHeight: "86vh",
      background: "var(--sn-surface-overlay)",
      border: "1px solid var(--sn-border-strong)",
      borderRadius: "var(--sn-radius-lg)",
      boxShadow: "var(--sn-shadow-overlay)",
      display: "flex",
      flexDirection: "column",
      overflow: "hidden"
    }
  }, /*#__PURE__*/React.createElement("header", {
    style: {
      display: "flex",
      alignItems: "center",
      gap: 12,
      padding: "16px 18px",
      borderBottom: "1px solid var(--sn-border-hairline)",
      flex: "0 0 auto"
    }
  }, icon ? /*#__PURE__*/React.createElement(__ds_scope.Icon, {
    name: icon,
    size: 18,
    style: {
      color: "var(--sn-accent)"
    }
  }) : null, /*#__PURE__*/React.createElement("h3", {
    style: {
      margin: 0,
      fontSize: "var(--sn-text-h2)",
      fontWeight: 600
    }
  }, title), /*#__PURE__*/React.createElement("button", {
    type: "button",
    onClick: onClose,
    "aria-label": "Close",
    style: {
      marginLeft: "auto",
      width: 28,
      height: 28,
      border: "1px solid var(--sn-border-hairline)",
      background: "var(--sn-surface-control)",
      color: "var(--sn-text-secondary)",
      borderRadius: 7,
      display: "grid",
      placeItems: "center",
      cursor: "pointer"
    }
  }, /*#__PURE__*/React.createElement(__ds_scope.Icon, {
    name: "close",
    size: 14
  }))), /*#__PURE__*/React.createElement("div", {
    style: {
      padding: "14px 18px",
      overflowY: "auto"
    }
  }, children), footer ? /*#__PURE__*/React.createElement("footer", {
    style: {
      padding: "13px 18px",
      borderTop: "1px solid var(--sn-border-hairline)",
      display: "flex",
      gap: 10,
      justifyContent: "flex-end",
      flex: "0 0 auto"
    }
  }, footer) : null));
}
Object.assign(__ds_scope, { Modal });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/feedback/Modal.jsx", error: String((e && e.message) || e) }); }

// components/feedback/Toast.jsx
try { (() => {
function Toast({
  message,
  icon = "check",
  style
}) {
  return /*#__PURE__*/React.createElement("div", {
    role: "status",
    style: {
      display: "flex",
      alignItems: "center",
      gap: 11,
      padding: "12px 16px",
      border: "1px solid var(--sn-border-strong)",
      borderRadius: "var(--sn-radius-md)",
      background: "var(--sn-surface-overlay)",
      boxShadow: "var(--sn-shadow-panel)",
      fontSize: 13,
      minWidth: 240,
      animation: "sn-toast-in var(--sn-dur-slow) var(--sn-ease)",
      ...style
    }
  }, /*#__PURE__*/React.createElement(__ds_scope.Icon, {
    name: icon,
    size: 18,
    style: {
      color: "var(--sn-accent)"
    }
  }), /*#__PURE__*/React.createElement("span", null, message));
}
function ToastStack({
  items = []
}) {
  return /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("style", null, "@keyframes sn-toast-in{from{transform:translateY(8px);opacity:0}to{transform:none;opacity:1}}"), /*#__PURE__*/React.createElement("div", {
    style: {
      position: "fixed",
      bottom: 22,
      right: 22,
      zIndex: 120,
      display: "flex",
      flexDirection: "column",
      gap: 10
    }
  }, items.map(t => /*#__PURE__*/React.createElement(Toast, {
    key: t.id,
    message: t.message,
    icon: t.icon
  }))));
}
Object.assign(__ds_scope, { Toast, ToastStack });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/feedback/Toast.jsx", error: String((e && e.message) || e) }); }

// components/navigation/BrandMark.jsx
try { (() => {
/* The Rev 2 mark: server hub with three monitor vantages, on a 24-unit grid.
   One accent hue only. Never boxed in a filled tile, never rotated, never
   stretched, never rendered in a status hue — the sole exception is the live
   favicon, where the satellites may report fleet state. */
function BrandMark({
  size = 30,
  wordmark = false,
  satelliteStates,
  style
}) {
  const sat = satelliteStates || [];
  const fill = i => {
    const s = sat[i];
    return s === "down" ? "var(--sn-crit)" : s === "degraded" ? "var(--sn-warn)" : s === "up" ? "var(--sn-ok)" : "var(--sn-text-primary)";
  };
  const mark = /*#__PURE__*/React.createElement("svg", {
    width: size,
    height: size,
    viewBox: "0 0 24 24",
    role: "img",
    "aria-label": "SolariNet",
    style: {
      flex: "0 0 " + size + "px"
    }
  }, /*#__PURE__*/React.createElement("g", {
    fill: "none",
    stroke: "var(--sn-accent)",
    strokeWidth: "1.1",
    opacity: ".55"
  }, /*#__PURE__*/React.createElement("path", {
    d: "M12 12V4M12 12l6.9 4M12 12L5.1 16"
  })), /*#__PURE__*/React.createElement("rect", {
    x: "9",
    y: "9",
    width: "6",
    height: "6",
    rx: "1.6",
    fill: "var(--sn-accent)"
  }), /*#__PURE__*/React.createElement("rect", {
    x: "10.4",
    y: "2.4",
    width: "3.2",
    height: "3.2",
    rx: ".5",
    transform: "rotate(45 12 4)",
    fill: fill(0)
  }), /*#__PURE__*/React.createElement("rect", {
    x: "17.3",
    y: "14.4",
    width: "3.2",
    height: "3.2",
    rx: ".5",
    transform: "rotate(45 18.9 16)",
    fill: fill(1)
  }), /*#__PURE__*/React.createElement("rect", {
    x: "3.5",
    y: "14.4",
    width: "3.2",
    height: "3.2",
    rx: ".5",
    transform: "rotate(45 5.1 16)",
    fill: fill(2)
  }));
  if (!wordmark) return /*#__PURE__*/React.createElement("span", {
    style: {
      display: "inline-flex",
      color: "var(--sn-accent)",
      ...style
    }
  }, mark);
  return /*#__PURE__*/React.createElement("span", {
    style: {
      display: "inline-flex",
      alignItems: "center",
      gap: 11,
      ...style
    }
  }, mark, /*#__PURE__*/React.createElement("span", {
    style: {
      fontFamily: "var(--sn-font-sans)",
      fontWeight: 400,
      fontSize: size * .58,
      letterSpacing: "var(--sn-track-display)",
      whiteSpace: "nowrap",
      color: "var(--sn-text-primary)"
    }
  }, "Solari", /*#__PURE__*/React.createElement("b", {
    style: {
      fontWeight: 700
    }
  }, "Net")));
}
Object.assign(__ds_scope, { BrandMark });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/navigation/BrandMark.jsx", error: String((e && e.message) || e) }); }

// components/navigation/PageHeader.jsx
try { (() => {
function PageHeader({
  title,
  meta,
  actions,
  style
}) {
  return /*#__PURE__*/React.createElement("div", {
    style: {
      display: "flex",
      alignItems: "flex-end",
      gap: 18,
      marginBottom: 20,
      flexWrap: "wrap",
      ...style
    }
  }, /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("h1", {
    style: {
      margin: 0,
      fontFamily: "var(--sn-font-sans)",
      fontSize: "var(--sn-text-h1)",
      fontWeight: 600,
      letterSpacing: "var(--sn-track-h1)",
      lineHeight: "var(--sn-lh-h1)"
    }
  }, title), meta ? /*#__PURE__*/React.createElement("div", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 11,
      letterSpacing: ".08em",
      textTransform: "uppercase",
      color: "var(--sn-text-tertiary)",
      marginTop: 3
    }
  }, meta) : null), actions ? /*#__PURE__*/React.createElement("div", {
    style: {
      marginLeft: "auto",
      display: "flex",
      alignItems: "center",
      gap: 10,
      flexWrap: "wrap"
    }
  }, actions) : null);
}
Object.assign(__ds_scope, { PageHeader });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/navigation/PageHeader.jsx", error: String((e && e.message) || e) }); }

// components/navigation/SidebarNav.jsx
try { (() => {
function SidebarNav({
  items,
  active,
  onNavigate,
  collapsed,
  onToggleCollapse,
  summary,
  style
}) {
  return /*#__PURE__*/React.createElement("aside", {
    style: {
      width: collapsed ? "var(--sn-sidebar-w-collapsed)" : "var(--sn-sidebar-w)",
      flex: "0 0 " + (collapsed ? "var(--sn-sidebar-w-collapsed)" : "var(--sn-sidebar-w)"),
      background: "var(--sn-surface-card)",
      borderRight: "1px solid var(--sn-border-hairline)",
      display: "flex",
      flexDirection: "column",
      overflow: "hidden",
      zIndex: 30,
      transition: "flex-basis var(--sn-dur-slow) var(--sn-ease), width var(--sn-dur-slow) var(--sn-ease)",
      ...style
    }
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      height: "var(--sn-topbar-h)",
      display: "flex",
      alignItems: "center",
      gap: 11,
      padding: "0 16px",
      borderBottom: "1px solid var(--sn-border-hairline)",
      flex: "0 0 auto"
    }
  }, /*#__PURE__*/React.createElement(__ds_scope.BrandMark, {
    size: 26,
    wordmark: !collapsed
  })), /*#__PURE__*/React.createElement("nav", {
    style: {
      flex: "1 1 auto",
      overflowY: "auto",
      padding: "12px 10px 16px"
    }
  }, items.map((item, i) => {
    if (item.group) return /*#__PURE__*/React.createElement("div", {
      key: "g" + i,
      style: {
        fontFamily: "var(--sn-font-mono)",
        fontSize: "var(--sn-text-label)",
        letterSpacing: "var(--sn-track-nav-label)",
        textTransform: "uppercase",
        color: "var(--sn-text-tertiary)",
        padding: "16px 12px 7px",
        display: collapsed ? "none" : "block"
      }
    }, item.group);
    const on = active === item.id;
    return /*#__PURE__*/React.createElement("div", {
      key: item.id,
      onClick: () => onNavigate && onNavigate(item.id),
      role: "button",
      "aria-current": on || undefined,
      title: collapsed ? item.label : undefined,
      style: {
        display: "flex",
        alignItems: "center",
        gap: 12,
        minHeight: "var(--sn-tap)",
        padding: collapsed ? 0 : "0 12px",
        justifyContent: collapsed ? "center" : "flex-start",
        borderRadius: "var(--sn-radius-md)",
        cursor: "pointer",
        position: "relative",
        fontFamily: "var(--sn-font-sans)",
        fontSize: 13.5,
        fontWeight: on ? 600 : 400,
        color: on ? "var(--sn-text-primary)" : "var(--sn-text-secondary)",
        background: on ? "var(--sn-surface-control-active)" : "transparent",
        userSelect: "none",
        transition: "background var(--sn-dur-fast) var(--sn-ease), color var(--sn-dur-fast) var(--sn-ease)"
      }
    }, on ? /*#__PURE__*/React.createElement("span", {
      style: {
        position: "absolute",
        left: -10,
        top: 9,
        bottom: 9,
        width: 3,
        borderRadius: 3,
        background: "var(--sn-accent)"
      }
    }) : null, /*#__PURE__*/React.createElement(__ds_scope.Icon, {
      name: item.icon,
      size: 19,
      style: {
        color: on ? "var(--sn-accent)" : "var(--sn-text-tertiary)",
        flex: "0 0 auto"
      }
    }), collapsed ? null : /*#__PURE__*/React.createElement("span", null, item.label), !collapsed && item.badge != null ? /*#__PURE__*/React.createElement("span", {
      style: {
        marginLeft: "auto",
        fontFamily: "var(--sn-font-mono)",
        fontSize: 11,
        fontWeight: 600,
        minWidth: 22,
        height: 20,
        display: "inline-flex",
        alignItems: "center",
        justifyContent: "center",
        padding: "0 6px",
        borderRadius: "var(--sn-radius-pill)",
        background: item.badgeTone === "crit" ? "var(--sn-crit-tint)" : "var(--sn-unknown-tint)",
        color: item.badgeTone === "crit" ? "var(--sn-crit)" : "var(--sn-text-secondary)"
      }
    }, item.badge) : null);
  })), summary && !collapsed ? /*#__PURE__*/React.createElement("div", {
    style: {
      flex: "0 0 auto",
      margin: "8px 10px 6px"
    }
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      display: "grid",
      gridTemplateColumns: "1fr 1fr",
      gap: 1,
      background: "var(--sn-border-hairline)",
      border: "1px solid var(--sn-border-hairline)",
      borderRadius: "var(--sn-radius-md)",
      overflow: "hidden"
    }
  }, summary.map(s => /*#__PURE__*/React.createElement("div", {
    key: s.label,
    style: {
      background: "var(--sn-raised)",
      padding: "9px 11px"
    }
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 16,
      fontWeight: 600,
      lineHeight: 1.1,
      fontVariantNumeric: "tabular-nums"
    }
  }, s.value), /*#__PURE__*/React.createElement("div", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 9,
      letterSpacing: ".12em",
      textTransform: "uppercase",
      color: "var(--sn-text-tertiary)",
      marginTop: 2
    }
  }, s.label))))) : null, /*#__PURE__*/React.createElement("button", {
    type: "button",
    onClick: onToggleCollapse,
    "aria-label": collapsed ? "Expand sidebar" : "Collapse sidebar",
    style: {
      flex: "0 0 auto",
      margin: "4px 10px 12px",
      minHeight: 40,
      display: "flex",
      alignItems: "center",
      gap: 12,
      padding: collapsed ? 0 : "0 12px",
      justifyContent: collapsed ? "center" : "flex-start",
      border: "1px solid var(--sn-border-hairline)",
      borderRadius: "var(--sn-radius-md)",
      background: "var(--sn-raised)",
      color: "var(--sn-text-secondary)",
      cursor: "pointer",
      fontFamily: "var(--sn-font-sans)",
      fontSize: 12.5,
      fontWeight: 500
    }
  }, /*#__PURE__*/React.createElement(__ds_scope.Icon, {
    name: collapsed ? "chevronRight" : "chevronLeft",
    size: 18
  }), collapsed ? null : /*#__PURE__*/React.createElement("span", null, "Collapse")));
}
Object.assign(__ds_scope, { SidebarNav });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/navigation/SidebarNav.jsx", error: String((e && e.message) || e) }); }

// components/status/Heartbeat.jsx
try { (() => {
/* Sparkbar plus age — an indicator, never a switch. A lost heartbeat renders
   the missing intervals as floor-height crit bars. */
function Heartbeat({
  beats = 8,
  missed = 0,
  age,
  width = 34,
  height = 12,
  style
}) {
  const bars = Array.from({
    length: beats
  }, (_, i) => i);
  const firstMissed = beats - missed;
  return /*#__PURE__*/React.createElement("span", {
    style: {
      display: "inline-flex",
      alignItems: "center",
      gap: 6,
      fontFamily: "var(--sn-font-mono)",
      fontSize: 11.5,
      color: missed ? "var(--sn-crit)" : "var(--sn-text-secondary)",
      ...style
    }
  }, /*#__PURE__*/React.createElement("svg", {
    width: width,
    height: height,
    viewBox: "0 0 " + (beats * 4 + 2) + " 12",
    "aria-hidden": "true"
  }, bars.map(i => {
    const dead = i >= firstMissed;
    const h = dead ? 2 : [8, 10, 7, 9][i % 4];
    return /*#__PURE__*/React.createElement("rect", {
      key: i,
      x: i * 4,
      y: 12 - h,
      width: "2",
      height: h,
      rx: "1",
      fill: dead ? "var(--sn-crit)" : "var(--sn-quiet)",
      opacity: missed && !dead ? .45 : 1
    });
  })), age ? /*#__PURE__*/React.createElement("span", null, age) : null);
}
Object.assign(__ds_scope, { Heartbeat });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/status/Heartbeat.jsx", error: String((e && e.message) || e) }); }

// components/status/StatusDot.jsx
try { (() => {
const STATUS_COLOR = {
  up: "var(--sn-status-operational)",
  operational: "var(--sn-status-operational)",
  degraded: "var(--sn-status-degraded)",
  down: "var(--sn-status-down)",
  maintenance: "var(--sn-status-maintenance)",
  unknown: "var(--sn-status-unknown)",
  stale: "var(--sn-status-stale)"
};
const STATUS_LABEL = {
  up: "up",
  operational: "operational",
  degraded: "degraded",
  down: "down",
  maintenance: "maintenance",
  unknown: "unknown",
  stale: "stale"
};

/* Solid geometric primitives, drawn in-house on the 24 grid. These are not
   icons and must never be swapped for library glyphs. Glow is licensed for
   `down` only, and only for the single most severe item in view. */
function StatusDot({
  state = "up",
  size = 9,
  glow = false,
  style
}) {
  const colour = STATUS_COLOR[state] || STATUS_COLOR.unknown;
  const hollow = state === "unknown" || state === "stale";
  return /*#__PURE__*/React.createElement("span", {
    "aria-hidden": "true",
    style: {
      display: "inline-block",
      width: size,
      height: size,
      flex: "0 0 " + size + "px",
      borderRadius: "50%",
      background: hollow ? "transparent" : colour,
      border: hollow ? "1.5px solid " + colour : "none",
      opacity: state === "stale" ? .55 : 1,
      boxShadow: glow && state === "down" ? "0 0 9px " + colour : "none",
      ...style
    }
  });
}
Object.assign(__ds_scope, { STATUS_COLOR, STATUS_LABEL, StatusDot });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/status/StatusDot.jsx", error: String((e && e.message) || e) }); }

// components/charts/HealthDonut.jsx
try { (() => {
function HealthDonut({
  roll,
  size = 76,
  caption = "SYSTEMS",
  style
}) {
  const r = size / 2 - 7,
    circ = 2 * Math.PI * r;
  const order = ["up", "degraded", "down", "maintenance", "unknown"];
  const total = roll.total || 1;
  let offset = 0;
  return /*#__PURE__*/React.createElement("div", {
    style: {
      position: "relative",
      width: size,
      height: size,
      flex: "0 0 " + size + "px",
      ...style
    }
  }, /*#__PURE__*/React.createElement("svg", {
    width: size,
    height: size,
    viewBox: "0 0 " + size + " " + size,
    style: {
      transform: "rotate(-90deg)"
    },
    "aria-hidden": "true"
  }, /*#__PURE__*/React.createElement("circle", {
    cx: size / 2,
    cy: size / 2,
    r: r,
    fill: "none",
    stroke: "var(--sn-border-hairline)",
    strokeWidth: "7"
  }), order.map(k => {
    const frac = (roll[k] || 0) / total;
    if (!frac) return null;
    const seg = /*#__PURE__*/React.createElement("circle", {
      key: k,
      cx: size / 2,
      cy: size / 2,
      r: r,
      fill: "none",
      stroke: __ds_scope.STATUS_COLOR[k],
      strokeWidth: "7",
      strokeDasharray: frac * circ + " " + circ,
      strokeDashoffset: -offset * circ
    });
    offset += frac;
    return seg;
  })), /*#__PURE__*/React.createElement("div", {
    style: {
      position: "absolute",
      inset: 0,
      display: "grid",
      placeItems: "center"
    }
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      textAlign: "center"
    }
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 18,
      fontWeight: 700,
      lineHeight: 1
    }
  }, roll.total), /*#__PURE__*/React.createElement("div", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 8,
      color: "var(--sn-text-tertiary)",
      letterSpacing: ".1em"
    }
  }, caption))));
}
Object.assign(__ds_scope, { HealthDonut });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/charts/HealthDonut.jsx", error: String((e && e.message) || e) }); }

// components/core/Chip.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
function Chip({
  children,
  active,
  icon,
  state,
  count,
  onClick,
  style,
  ...rest
}) {
  return /*#__PURE__*/React.createElement("button", _extends({
    type: "button",
    onClick: onClick,
    "aria-pressed": active || undefined,
    style: {
      minHeight: 36,
      padding: "0 12px",
      display: "inline-flex",
      alignItems: "center",
      gap: 8,
      border: "1px solid " + (active ? "var(--sn-border-interactive)" : "var(--sn-border-hairline)"),
      borderRadius: "var(--sn-radius-pill)",
      background: active ? "var(--sn-surface-control-active)" : "var(--sn-surface-control)",
      color: active ? "var(--sn-text-primary)" : "var(--sn-text-secondary)",
      fontFamily: "var(--sn-font-sans)",
      fontSize: 12.5,
      fontWeight: 500,
      letterSpacing: ".01em",
      cursor: "pointer",
      userSelect: "none",
      ...style
    }
  }, rest), state ? /*#__PURE__*/React.createElement(__ds_scope.StatusDot, {
    state: state,
    size: 9,
    glow: false
  }) : null, icon ? /*#__PURE__*/React.createElement(__ds_scope.Icon, {
    name: icon,
    size: 14
  }) : null, children, count != null ? /*#__PURE__*/React.createElement("span", {
    style: {
      color: "var(--sn-text-tertiary)",
      fontSize: 11,
      fontFamily: "var(--sn-font-mono)"
    }
  }, count) : null);
}
Object.assign(__ds_scope, { Chip });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/core/Chip.jsx", error: String((e && e.message) || e) }); }

// components/data/NodeTile.jsx
try { (() => {
/* One tile design. Three rows always: name + dot, mono meta, sparkline.
   Minimum width 132px so hostnames are never truncated. */
function NodeTile({
  name,
  meta,
  state = "up",
  history,
  load,
  badge,
  onClick,
  detailed,
  style
}) {
  const fault = state === "degraded" || state === "down";
  const border = state === "down" ? "var(--sn-crit)" : state === "degraded" ? "var(--sn-warn)" : state === "maintenance" ? "var(--sn-maint)" : "var(--sn-border-hairline)";
  const stroke = state === "down" ? "var(--sn-crit)" : state === "degraded" ? "var(--sn-warn)" : "var(--sn-quiet)";
  return /*#__PURE__*/React.createElement("div", {
    onClick: onClick,
    title: name + " — " + state,
    style: {
      minWidth: "var(--sn-tile-min)",
      minHeight: detailed ? 116 : 84,
      borderRadius: "var(--sn-radius-sm)",
      border: "1px solid " + border,
      borderStyle: state === "maintenance" ? "dashed" : "solid",
      background: state === "down" ? "var(--sn-crit-tint)" : state === "degraded" ? "var(--sn-warn-tint)" : "var(--sn-surface-card)",
      boxShadow: state === "down" ? "0 0 9px rgba(255,77,94,.4)" : "none",
      opacity: state === "unknown" || state === "stale" ? .72 : 1,
      position: "relative",
      cursor: onClick ? "pointer" : "default",
      overflow: "hidden",
      display: "flex",
      flexDirection: "column",
      padding: "8px 10px 7px",
      transition: "border-color var(--sn-dur-fast) var(--sn-ease)",
      ...style
    }
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      display: "flex",
      alignItems: "center",
      justifyContent: "space-between",
      gap: 6
    }
  }, /*#__PURE__*/React.createElement("span", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 11.5,
      fontWeight: 600,
      letterSpacing: ".01em"
    }
  }, name), /*#__PURE__*/React.createElement(__ds_scope.StatusDot, {
    state: state,
    size: 10,
    glow: state === "down"
  })), /*#__PURE__*/React.createElement("div", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 9.5,
      color: "var(--sn-text-tertiary)",
      marginTop: 1,
      whiteSpace: "nowrap",
      overflow: "hidden",
      textOverflow: "ellipsis"
    }
  }, meta), badge ? /*#__PURE__*/React.createElement("span", {
    style: {
      position: "absolute",
      top: 4,
      right: 4,
      minWidth: 15,
      height: 15,
      padding: "0 3px",
      borderRadius: 8,
      fontFamily: "var(--sn-font-mono)",
      fontSize: 9,
      fontWeight: 700,
      display: "inline-flex",
      alignItems: "center",
      justifyContent: "center",
      background: "var(--sn-crit)",
      color: "#fff"
    }
  }, badge) : null, /*#__PURE__*/React.createElement("div", {
    style: {
      marginTop: "auto",
      height: detailed ? 26 : 20
    }
  }, history ? /*#__PURE__*/React.createElement(__ds_scope.Sparkline, {
    data: history,
    color: stroke,
    height: detailed ? 26 : 20,
    fill: fault,
    strokeWidth: 1.5
  }) : null), load != null ? /*#__PURE__*/React.createElement("span", {
    style: {
      display: "block",
      height: 3,
      borderRadius: 3,
      background: "var(--sn-border-hairline)",
      marginTop: 4,
      overflow: "hidden"
    }
  }, /*#__PURE__*/React.createElement("i", {
    style: {
      display: "block",
      height: "100%",
      borderRadius: 3,
      width: load + "%",
      background: __ds_scope.metricColor(load)
    }
  })) : null);
}
Object.assign(__ds_scope, { NodeTile });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/data/NodeTile.jsx", error: String((e && e.message) || e) }); }

// components/data/PoolCard.jsx
try { (() => {
function PoolCard({
  name,
  cidr,
  description,
  roll,
  stats = [],
  members = [],
  onMemberClick,
  style
}) {
  return /*#__PURE__*/React.createElement("section", {
    style: {
      border: "1px solid var(--sn-border-hairline)",
      borderRadius: "var(--sn-radius-lg)",
      background: "var(--sn-surface-card)",
      overflow: "hidden",
      ...style
    }
  }, /*#__PURE__*/React.createElement("header", {
    style: {
      display: "flex",
      alignItems: "center",
      gap: 10,
      padding: "14px 16px",
      borderBottom: "1px solid var(--sn-border-hairline)"
    }
  }, /*#__PURE__*/React.createElement(__ds_scope.Icon, {
    name: "arch",
    size: 16,
    style: {
      color: "var(--sn-accent)"
    }
  }), /*#__PURE__*/React.createElement("span", {
    style: {
      fontWeight: 600,
      fontSize: 14
    }
  }, name), /*#__PURE__*/React.createElement("span", {
    style: {
      marginLeft: "auto",
      fontFamily: "var(--sn-font-mono)",
      fontSize: 10.5,
      color: "var(--sn-text-tertiary)"
    }
  }, cidr)), /*#__PURE__*/React.createElement("div", {
    style: {
      padding: "13px 16px"
    }
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      display: "flex",
      alignItems: "center",
      gap: 16
    }
  }, /*#__PURE__*/React.createElement(__ds_scope.HealthDonut, {
    roll: roll
  }), /*#__PURE__*/React.createElement("div", {
    style: {
      flex: 1,
      display: "grid",
      gridTemplateColumns: "1fr 1fr",
      gap: "9px 16px"
    }
  }, stats.map(s => /*#__PURE__*/React.createElement("div", {
    key: s.label
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 9.5,
      letterSpacing: ".12em",
      textTransform: "uppercase",
      color: "var(--sn-text-tertiary)"
    }
  }, s.label), /*#__PURE__*/React.createElement("div", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 16,
      fontWeight: 600,
      color: s.tone === "auto" ? __ds_scope.metricColor(parseInt(s.value, 10)) : s.tone || "var(--sn-text-primary)"
    }
  }, s.value))))), members.length ? /*#__PURE__*/React.createElement("div", {
    style: {
      display: "grid",
      gridTemplateColumns: "repeat(auto-fill, minmax(22px, 1fr))",
      gap: 4,
      marginTop: 13
    }
  }, members.map((m, i) => /*#__PURE__*/React.createElement("div", {
    key: i,
    title: m.name + " — " + m.state,
    onClick: onMemberClick ? () => onMemberClick(m) : undefined,
    style: {
      aspectRatio: "1",
      borderRadius: 3,
      cursor: onMemberClick ? "pointer" : "default",
      background: m.state === "up" ? "var(--sn-surface-control)" : __ds_scope.STATUS_COLOR[m.state],
      border: m.state === "up" ? "1px solid var(--sn-border-hairline)" : "1px solid transparent",
      opacity: m.state === "unknown" ? .5 : 1
    }
  }))) : null, description ? /*#__PURE__*/React.createElement("div", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 10.5,
      color: "var(--sn-text-tertiary)",
      marginTop: 10
    }
  }, description) : null));
}
Object.assign(__ds_scope, { PoolCard });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/data/PoolCard.jsx", error: String((e && e.message) || e) }); }

// components/feedback/CommandPalette.jsx
try { (() => {
function CommandPalette({
  open,
  onClose,
  commands = []
}) {
  const [q, setQ] = React.useState("");
  const [sel, setSel] = React.useState(0);
  const inputRef = React.useRef(null);
  React.useEffect(() => {
    if (open) {
      setQ("");
      setSel(0);
      const t = setTimeout(() => inputRef.current && inputRef.current.focus(), 30);
      return () => clearTimeout(t);
    }
  }, [open]);
  if (!open) return null;
  const ql = q.toLowerCase();
  const flat = commands.filter(c => !ql || (c.label + " " + (c.group || "") + " " + (c.keywords || "")).toLowerCase().includes(ql));
  const groups = {};
  flat.forEach(c => {
    (groups[c.group] = groups[c.group] || []).push(c);
  });
  function run(i) {
    const c = flat[i];
    if (c) {
      c.action && c.action();
      onClose();
    }
  }
  function onKey(e) {
    if (e.key === "ArrowDown") {
      e.preventDefault();
      setSel(s => Math.min(flat.length - 1, s + 1));
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      setSel(s => Math.max(0, s - 1));
    } else if (e.key === "Enter") {
      e.preventDefault();
      run(sel);
    } else if (e.key === "Escape") {
      onClose();
    }
  }
  let idx = -1;
  return /*#__PURE__*/React.createElement("div", {
    onClick: onClose,
    style: {
      position: "fixed",
      inset: 0,
      background: "var(--sn-scrim)",
      backdropFilter: "var(--sn-blur-overlay)",
      zIndex: 100,
      display: "flex",
      alignItems: "flex-start",
      justifyContent: "center",
      paddingTop: "12vh"
    }
  }, /*#__PURE__*/React.createElement("div", {
    onClick: e => e.stopPropagation(),
    style: {
      width: "min(620px, 92vw)",
      background: "var(--sn-surface-overlay)",
      border: "1px solid var(--sn-border-strong)",
      borderRadius: "var(--sn-radius-lg)",
      boxShadow: "var(--sn-shadow-overlay)",
      overflow: "hidden"
    }
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      display: "flex",
      alignItems: "center",
      gap: 12,
      padding: "16px 18px",
      borderBottom: "1px solid var(--sn-border-hairline)"
    }
  }, /*#__PURE__*/React.createElement(__ds_scope.Icon, {
    name: "search",
    size: 20,
    style: {
      color: "var(--sn-accent)"
    }
  }), /*#__PURE__*/React.createElement("input", {
    ref: inputRef,
    value: q,
    onChange: e => {
      setQ(e.target.value);
      setSel(0);
    },
    onKeyDown: onKey,
    placeholder: "Jump to a system, run a command\u2026",
    "aria-label": "Command",
    style: {
      flex: "1 1 auto",
      minWidth: 0,
      border: "none",
      background: "none",
      outline: "none",
      color: "var(--sn-text-primary)",
      fontSize: 17,
      fontFamily: "var(--sn-font-sans)"
    }
  }), /*#__PURE__*/React.createElement("span", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 10,
      border: "1px solid var(--sn-border-strong)",
      borderRadius: 5,
      padding: "2px 6px",
      color: "var(--sn-text-tertiary)"
    }
  }, "esc")), /*#__PURE__*/React.createElement("div", {
    style: {
      maxHeight: "50vh",
      overflowY: "auto",
      padding: 8
    }
  }, flat.length === 0 ? /*#__PURE__*/React.createElement("div", {
    style: {
      textAlign: "center",
      color: "var(--sn-text-tertiary)",
      fontFamily: "var(--sn-font-mono)",
      fontSize: 13,
      padding: "40px 20px"
    }
  }, "No matches") : null, Object.entries(groups).map(([g, items]) => /*#__PURE__*/React.createElement("div", {
    key: g
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 9.5,
      letterSpacing: ".18em",
      textTransform: "uppercase",
      color: "var(--sn-text-tertiary)",
      padding: "12px 12px 6px"
    }
  }, g), items.map(c => {
    idx++;
    const i = idx;
    const on = i === sel;
    return /*#__PURE__*/React.createElement("div", {
      key: c.id,
      onMouseEnter: () => setSel(i),
      onClick: () => run(i),
      style: {
        display: "flex",
        alignItems: "center",
        gap: 12,
        minHeight: "var(--sn-tap)",
        padding: "0 12px",
        borderRadius: "var(--sn-radius-md)",
        cursor: "pointer",
        background: on ? "var(--sn-surface-control-active)" : "transparent",
        color: on ? "var(--sn-text-primary)" : "var(--sn-text-secondary)"
      }
    }, c.state ? /*#__PURE__*/React.createElement(__ds_scope.StatusDot, {
      state: c.state,
      size: 8
    }) : /*#__PURE__*/React.createElement(__ds_scope.Icon, {
      name: c.icon || "enter",
      size: 18,
      style: {
        color: on ? "var(--sn-accent)" : "var(--sn-text-tertiary)"
      }
    }), /*#__PURE__*/React.createElement("span", null, c.label), /*#__PURE__*/React.createElement("span", {
      style: {
        marginLeft: "auto",
        fontFamily: "var(--sn-font-mono)",
        fontSize: 10.5,
        color: "var(--sn-text-tertiary)"
      }
    }, c.sub || (on ? "↵" : "")));
  }))))));
}
Object.assign(__ds_scope, { CommandPalette });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/feedback/CommandPalette.jsx", error: String((e && e.message) || e) }); }

// components/navigation/TopBar.jsx
try { (() => {
function TopBar({
  onMenu,
  onOpenSearch,
  controller,
  actions,
  style
}) {
  return /*#__PURE__*/React.createElement("header", {
    style: {
      height: "var(--sn-topbar-h)",
      flex: "0 0 auto",
      display: "flex",
      alignItems: "center",
      gap: 12,
      padding: "0 16px",
      borderBottom: "1px solid var(--sn-border-hairline)",
      background: "var(--sn-surface-card)",
      zIndex: 20,
      ...style
    }
  }, onMenu ? /*#__PURE__*/React.createElement(__ds_scope.IconButton, {
    icon: "menu",
    label: "Toggle navigation",
    onClick: onMenu,
    style: {
      flex: "0 0 auto"
    }
  }) : null, /*#__PURE__*/React.createElement(__ds_scope.SearchField, {
    readOnly: true,
    onClick: onOpenSearch
  }), /*#__PURE__*/React.createElement("span", {
    style: {
      flex: "1 1 auto"
    }
  }), controller ? /*#__PURE__*/React.createElement("span", {
    title: "Active control server holding the failover lease",
    style: {
      display: "inline-flex",
      alignItems: "center",
      gap: 8,
      height: 36,
      padding: "0 12px",
      border: "1px solid var(--sn-border-hairline)",
      borderRadius: "var(--sn-radius-pill)",
      background: "var(--sn-raised)",
      fontFamily: "var(--sn-font-mono)",
      fontSize: 11,
      color: "var(--sn-text-secondary)",
      whiteSpace: "nowrap",
      flex: "0 1 auto",
      minWidth: 0,
      overflow: "hidden"
    }
  }, /*#__PURE__*/React.createElement(__ds_scope.StatusDot, {
    state: "up",
    size: 8
  }), /*#__PURE__*/React.createElement("span", {
    style: {
      fontSize: 9.5,
      letterSpacing: ".14em",
      textTransform: "uppercase",
      color: "var(--sn-text-tertiary)"
    }
  }, "Active\xA0C2"), /*#__PURE__*/React.createElement("b", {
    style: {
      color: "var(--sn-text-primary)",
      fontWeight: 600
    }
  }, controller.primary), /*#__PURE__*/React.createElement("span", {
    style: {
      color: "var(--sn-text-tertiary)",
      overflow: "hidden",
      textOverflow: "ellipsis"
    }
  }, "\xB7 failover\xA0", controller.failover)) : null, /*#__PURE__*/React.createElement("span", {
    style: {
      display: "flex",
      gap: 12,
      flex: "0 0 auto"
    }
  }, actions));
}
Object.assign(__ds_scope, { TopBar });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/navigation/TopBar.jsx", error: String((e && e.message) || e) }); }

// components/status/StatusCell.jsx
try { (() => {
function StatusCell({
  state = "up",
  label,
  style
}) {
  const colour = state === "up" || state === "operational" ? "var(--sn-text-secondary)" : __ds_scope.STATUS_COLOR[state];
  return /*#__PURE__*/React.createElement("span", {
    style: {
      display: "inline-flex",
      alignItems: "center",
      gap: 7,
      ...style
    }
  }, /*#__PURE__*/React.createElement(__ds_scope.StatusDot, {
    state: state,
    size: 8
  }), /*#__PURE__*/React.createElement("span", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 11.5,
      color: colour
    }
  }, label || __ds_scope.STATUS_LABEL[state]));
}
Object.assign(__ds_scope, { StatusCell });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/status/StatusCell.jsx", error: String((e && e.message) || e) }); }

// components/status/StatusPill.jsx
try { (() => {
const tones = {
  crit: {
    bg: "var(--sn-crit-tint)",
    fg: "var(--sn-crit)"
  },
  warn: {
    bg: "var(--sn-warn-tint)",
    fg: "var(--sn-warn)"
  },
  info: {
    bg: "var(--sn-azure-tint)",
    fg: "var(--sn-accent)"
  },
  ok: {
    bg: "var(--sn-ok-tint)",
    fg: "var(--sn-ok)"
  },
  maint: {
    bg: "var(--sn-maint-tint)",
    fg: "var(--sn-maint)"
  },
  neutral: {
    bg: "var(--sn-unknown-tint)",
    fg: "var(--sn-text-secondary)"
  }
};
function StatusPill({
  severity = "info",
  children,
  style
}) {
  const t = tones[severity] || tones.neutral;
  return /*#__PURE__*/React.createElement("span", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 9.5,
      fontWeight: 700,
      letterSpacing: ".14em",
      textTransform: "uppercase",
      padding: "4px 9px",
      borderRadius: "var(--sn-radius-sm)",
      flex: "0 0 auto",
      background: t.bg,
      color: t.fg,
      ...style
    }
  }, children || severity);
}
Object.assign(__ds_scope, { StatusPill });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/status/StatusPill.jsx", error: String((e && e.message) || e) }); }

// components/feedback/AlertRow.jsx
try { (() => {
function AlertRow({
  severity = "info",
  title,
  detail,
  meta,
  cleared,
  onClick,
  style
}) {
  const edge = severity === "crit" ? "var(--sn-crit)" : severity === "warn" ? "var(--sn-warn)" : "var(--sn-accent)";
  return /*#__PURE__*/React.createElement("div", {
    onClick: onClick,
    style: {
      display: "flex",
      alignItems: "center",
      gap: 14,
      padding: "13px 16px",
      border: "1px solid var(--sn-border-hairline)",
      borderLeft: "3px solid " + edge,
      borderRadius: "var(--sn-radius-md)",
      background: "var(--sn-surface-card)",
      cursor: onClick ? "pointer" : "default",
      opacity: cleared ? .55 : 1,
      ...style
    }
  }, /*#__PURE__*/React.createElement(__ds_scope.StatusPill, {
    severity: severity
  }), /*#__PURE__*/React.createElement("div", {
    style: {
      flex: "1 1 auto",
      minWidth: 0
    }
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      fontWeight: 600,
      fontSize: 13.5
    }
  }, title), detail ? /*#__PURE__*/React.createElement("div", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 11.5,
      color: "var(--sn-text-secondary)",
      marginTop: 2,
      whiteSpace: "nowrap",
      overflow: "hidden",
      textOverflow: "ellipsis"
    }
  }, detail) : null), meta ? /*#__PURE__*/React.createElement("div", {
    style: {
      fontFamily: "var(--sn-font-mono)",
      fontSize: 11,
      color: "var(--sn-text-tertiary)",
      textAlign: "right",
      flex: "0 0 auto"
    }
  }, meta) : null);
}
Object.assign(__ds_scope, { AlertRow });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/feedback/AlertRow.jsx", error: String((e && e.message) || e) }); }

// src/js/01-0e23a051.jsx
try { (() => {
/* ============================================================
   SolariNet — mock data layer (deterministic, seeded)
   Exposes window.SOLARI = { nodes, segments, alerts, rules, server, ... }
   ============================================================ */
(function () {
  // ---- seeded RNG (mulberry32) for stable data across reloads ----
  function mulberry32(a) {
    return function () {
      a |= 0;
      a = a + 0x6D2B79F5 | 0;
      let t = Math.imul(a ^ a >>> 15, 1 | a);
      t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t;
      return ((t ^ t >>> 14) >>> 0) / 4294967296;
    };
  }
  const rng = mulberry32(20260606);
  const rint = (lo, hi) => Math.floor(rng() * (hi - lo + 1)) + lo;
  const pick = arr => arr[Math.floor(rng() * arr.length)];
  const chance = p => rng() < p;

  // ---- periodic table (symbol + name) ----
  const ELEMENTS = [["H", "hydrogen"], ["He", "helium"], ["Li", "lithium"], ["Be", "beryllium"], ["B", "boron"], ["C", "carbon"], ["N", "nitrogen"], ["O", "oxygen"], ["F", "fluorine"], ["Ne", "neon"], ["Na", "sodium"], ["Mg", "magnesium"], ["Al", "aluminium"], ["Si", "silicon"], ["P", "phosphorus"], ["S", "sulfur"], ["Cl", "chlorine"], ["Ar", "argon"], ["K", "potassium"], ["Ca", "calcium"], ["Sc", "scandium"], ["Ti", "titanium"], ["V", "vanadium"], ["Cr", "chromium"], ["Mn", "manganese"], ["Fe", "iron"], ["Co", "cobalt"], ["Ni", "nickel"], ["Cu", "copper"], ["Zn", "zinc"], ["Ga", "gallium"], ["Ge", "germanium"], ["As", "arsenic"], ["Se", "selenium"], ["Br", "bromine"], ["Kr", "krypton"], ["Rb", "rubidium"], ["Sr", "strontium"], ["Y", "yttrium"], ["Zr", "zirconium"], ["Nb", "niobium"], ["Mo", "molybdenum"], ["Tc", "technetium"], ["Ru", "ruthenium"], ["Rh", "rhodium"], ["Pd", "palladium"], ["Ag", "silver"], ["Cd", "cadmium"], ["In", "indium"], ["Sn", "tin"], ["Sb", "antimony"], ["Te", "tellurium"], ["I", "iodine"], ["Xe", "xenon"], ["Cs", "caesium"], ["Ba", "barium"], ["La", "lanthanum"], ["Ce", "cerium"], ["Pr", "praseodymium"], ["Nd", "neodymium"], ["Pm", "promethium"], ["Sm", "samarium"], ["Eu", "europium"], ["Gd", "gadolinium"], ["Tb", "terbium"], ["Dy", "dysprosium"], ["Ho", "holmium"], ["Er", "erbium"], ["Tm", "thulium"], ["Yb", "ytterbium"], ["Lu", "lutetium"], ["Hf", "hafnium"], ["Ta", "tantalum"], ["W", "tungsten"], ["Re", "rhenium"], ["Os", "osmium"], ["Ir", "iridium"], ["Pt", "platinum"], ["Au", "gold"], ["Hg", "mercury"], ["Tl", "thallium"], ["Pb", "lead"], ["Bi", "bismuth"], ["Po", "polonium"], ["At", "astatine"], ["Rn", "radon"], ["Fr", "francium"], ["Ra", "radium"], ["Ac", "actinium"], ["Th", "thorium"], ["Pa", "protactinium"], ["U", "uranium"], ["Np", "neptunium"], ["Pu", "plutonium"], ["Am", "americium"], ["Cm", "curium"], ["Bk", "berkelium"], ["Cf", "californium"], ["Es", "einsteinium"], ["Fm", "fermium"], ["Md", "mendelevium"], ["No", "nobelium"], ["Lr", "lawrencium"], ["Rf", "rutherfordium"], ["Db", "dubnium"], ["Sg", "seaborgium"], ["Bh", "bohrium"], ["Hs", "hassium"], ["Mt", "meitnerium"], ["Ds", "darmstadtium"], ["Rg", "roentgenium"], ["Cn", "copernicium"], ["Nh", "nihonium"], ["Fl", "flerovium"], ["Mc", "moscovium"], ["Lv", "livermorium"], ["Ts", "tennessine"], ["Og", "oganesson"]];

  // ---- network segments ----
  const SEGMENTS = [{
    id: "core",
    name: "Core",
    cidr: "10.42.0.0/24",
    desc: "Routers, gateways, infra services"
  }, {
    id: "compute",
    name: "Compute",
    cidr: "10.42.10.0/23",
    desc: "Hypervisors & app hosts"
  }, {
    id: "storage",
    name: "Storage",
    cidr: "10.42.20.0/24",
    desc: "NAS, object, backup"
  }, {
    id: "dmz",
    name: "DMZ",
    cidr: "10.42.30.0/24",
    desc: "Edge / reverse proxies"
  }, {
    id: "lab",
    name: "Lab",
    cidr: "10.42.40.0/23",
    desc: "ARM SBCs, experiments"
  }, {
    id: "iot",
    name: "IoT / OT",
    cidr: "10.42.50.0/24",
    desc: "Sensors, controllers"
  }];
  const OSES = [{
    os: "Debian 12",
    archs: ["x86_64", "arm64"]
  }, {
    os: "Ubuntu 24.04",
    archs: ["x86_64", "arm64"]
  }, {
    os: "Alpine 3.20",
    archs: ["x86_64", "armv7", "arm64"]
  }, {
    os: "Raspberry Pi OS",
    archs: ["arm64", "armv7"]
  }, {
    os: "FreeBSD 14",
    archs: ["x86_64"]
  }, {
    os: "Windows Server 2022",
    archs: ["x86_64"]
  }, {
    os: "macOS 14",
    archs: ["arm64"]
  }];
  const SERVICES = ["apache2", "mariadbd", "nginx", "sshd", "redis-server", "node_exporter", "haproxy", "postgres", "dockerd", "coredns", "vault", "step-ca"];
  const DISK_MOUNTS = ["/", "/var", "/data", "/srv", "/backup"];
  const IFACES = ["eth0", "eth1", "wlan0", "bond0", "usb0"];
  function fnv(str) {
    let h = 0xcbf29ce484222325n;
    for (let i = 0; i < str.length; i++) {
      h ^= BigInt(str.charCodeAt(i));
      h = h * 0x100000001b3n & 0xffffffffffffffffn;
    }
    return h.toString(16).padStart(16, "0");
  }
  function spark(n, base, vol, trend) {
    const out = [];
    let v = base;
    for (let i = 0; i < n; i++) {
      v += (rng() - 0.5) * vol + trend;
      v = Math.max(0, Math.min(100, v));
      out.push(Math.round(v * 10) / 10);
    }
    return out;
  }

  // ---- build fleet ----
  const nodes = [];
  let idx = 0;
  function makeNode(role, segId, forceState) {
    const [sym, baseName] = ELEMENTS[idx % ELEMENTS.length];
    const wrap = Math.floor(idx / ELEMENTS.length);
    const name = wrap > 0 ? baseName + (wrap + 1) : baseName; // e.g. iron2 on wrap
    idx++;
    const seg = SEGMENTS.find(s => s.id === segId);
    const osPick = pick(OSES);
    const arch = pick(osPick.archs);
    // state distribution
    let state = forceState;
    if (!state) {
      const r = rng();
      state = r < 0.80 ? "up" : r < 0.90 ? "degraded" : r < 0.965 ? "down" : "unknown";
    }
    const isDown = state === "down";
    const isUnknown = state === "unknown";
    const deg = state === "degraded";
    const cpuBase = isDown ? 0 : deg ? rint(72, 95) : rint(6, 55);
    const ramTotal = pick([4, 8, 16, 32, 64, 128]) * 1024 * 1024; // KB
    const ramPct = isDown ? 0 : deg && chance(0.5) ? rint(82, 97) : rint(20, 75);
    const swapTotal = ramTotal / 2;
    const swapPct = deg && chance(0.4) ? rint(30, 80) : rint(0, 15);
    const ncores = pick([2, 4, 4, 8, 8, 16, 32]);
    const cores = Array.from({
      length: ncores
    }, () => isDown ? 0 : Math.max(0, Math.min(100, Math.round(cpuBase + (rng() - 0.5) * 40))));
    const ndisks = rint(1, role === "server" ? 4 : 2);
    const disks = Array.from({
      length: ndisks
    }, (_, i) => {
      const totalGb = pick([64, 128, 256, 512, 1024, 2048, 4096]);
      const usedPct = deg && chance(0.5) ? rint(85, 98) : rint(18, 80);
      return {
        mount: DISK_MOUNTS[i] || `/mnt/d${i}`,
        totalGb,
        usedPct,
        fs: pick(["ext4", "xfs", "zfs", "btrfs"])
      };
    });
    const nif = rint(1, role === "monitor" ? 3 : 2);
    const ifaces = Array.from({
      length: nif
    }, (_, i) => {
      const cap = pick([100, 1000, 1000, 2500, 10000]); // Mbps
      const rxMbps = isDown ? 0 : Math.round(rng() * cap * (deg ? 0.85 : 0.4));
      const txMbps = isDown ? 0 : Math.round(rng() * cap * (deg ? 0.7 : 0.3));
      return {
        name: IFACES[i] || `eth${i}`,
        capMbps: cap,
        rxMbps,
        txMbps,
        errs: deg && chance(0.5) ? rint(20, 400) : rint(0, 4)
      };
    });
    const nproc = role === "client" ? rint(2, 5) : rint(2, 4);
    const usedServices = [];
    const procs = Array.from({
      length: nproc
    }, () => {
      let s = pick(SERVICES);
      let guard = 0;
      while (usedServices.includes(s) && guard++ < 6) s = pick(SERVICES);
      usedServices.push(s);
      const runState = isDown ? "Z" : deg && chance(0.3) ? "D" : "R";
      return {
        name: s,
        pid: rint(200, 39000),
        runState,
        nFiles: rint(8, 520),
        nSockets: rint(1, 180),
        rssKb: rint(8000, ramTotal * (ramPct / 100) / 2 | 0 || 200000)
      };
    });
    const lastSeenMin = isDown ? rint(3, 90) : isUnknown ? rint(120, 1440) : rint(0, 1);
    const alertsCount = isDown ? rint(1, 3) : deg ? rint(0, 2) : 0;
    const node = {
      nodeId: fnv(`${name}.akoria.net`),
      sym,
      name,
      hostFqdn: `${name}.akoria.net`,
      ip: `${seg.cidr.split(".").slice(0, 3).join(".")}.${rint(2, 250)}`,
      role,
      segId,
      segName: seg.name,
      cidr: seg.cidr,
      osName: osPick.os,
      arch,
      state,
      lastSeenMin,
      enrolledDaysAgo: rint(20, 700),
      configEpoch: rint(3, 240),
      converged: chance(0.9),
      // metrics
      cpuPct: isDown ? 0 : Math.round(cores.reduce((a, b) => a + b, 0) / cores.length),
      cores,
      ramPct,
      ramUsedKb: Math.round(ramTotal * ramPct / 100),
      ramTotalKb: ramTotal,
      swapPct,
      swapUsedKb: Math.round(swapTotal * swapPct / 100),
      swapTotalKb: swapTotal,
      disks,
      ifaces,
      procs,
      diskMaxPct: disks.reduce((m, d) => Math.max(m, d.usedPct), 0),
      netTotalMbps: ifaces.reduce((a, b) => a + b.rxMbps + b.txMbps, 0),
      netCapMbps: ifaces.reduce((a, b) => a + b.capMbps, 0),
      alertsCount,
      // history sparks (60 pts)
      hist: {
        cpu: spark(60, cpuBase || 5, 14, isDown ? -0.3 : deg ? 0.25 : 0),
        ram: spark(60, ramPct || 5, 7, 0),
        net: spark(60, isDown ? 0 : 30, 22, 0),
        disk: spark(60, disks[0] ? disks[0].usedPct : 30, 1.2, 0.06)
      },
      uptimeDays: isDown ? 0 : rint(1, 410)
    };
    nodes.push(node);
    return node;
  }

  // servers (2) in core
  const primary = makeNode("server", "core", "up");
  const failover = makeNode("server", "core", "up");
  primary.label = "Primary";
  failover.label = "Failover";

  // monitors (~12) spread across segments
  for (let i = 0; i < 12; i++) makeNode("monitor", pick(SEGMENTS).id);

  // clients — fill out a 100+ fleet across segments
  const segWeights = {
    core: 6,
    compute: 32,
    storage: 14,
    dmz: 12,
    lab: 22,
    iot: 16
  };
  Object.entries(segWeights).forEach(([segId, count]) => {
    for (let i = 0; i < count; i++) makeNode("client", segId);
  });

  // ---- probe targets (per-vantage) ----
  const monitors = nodes.filter(n => n.role === "monitor");
  const probeServices = [["tcp", 443, "web"], ["tcp", 5432, "postgres"], ["tcp", 3306, "mariadb"], ["icmp", 0, "gateway"], ["udp", 53, "dns"], ["tcp", 6379, "redis"], ["tcp", 22, "ssh"], ["tcp", 8200, "vault"], ["tcp", 9100, "node-exp"]];
  const probes = [];
  const probeHosts = nodes.filter(n => n.role !== "monitor").slice(0, 26);
  probeHosts.forEach(h => {
    const [proto, port, label] = pick(probeServices);
    const replFactor = 2;
    const vantages = [];
    const pickedMons = [...monitors].sort(() => rng() - 0.5).slice(0, replFactor + (chance(0.3) ? 1 : 0));
    const hostBad = h.state === "down";
    const partial = chance(0.12);
    pickedMons.forEach((m, vi) => {
      let outcome = "ok";
      if (hostBad) outcome = pick(["timeout", "unreachable", "refused"]);else if (partial && vi === 0) outcome = pick(["timeout", "tls_fail"]);else if (chance(0.06)) outcome = pick(["timeout", "proto_err"]);
      const ok = outcome === "ok";
      vantages.push({
        monitorNode: m.nodeId,
        monitorName: m.name,
        outcome,
        rttMicros: ok ? rint(180, proto === "icmp" ? 4000 : 28000) : 0,
        jitterMicros: ok ? rint(40, 2200) : 0,
        lossPermille: ok ? chance(0.7) ? 0 : rint(1, 45) : 1000,
        throughputKbps: ok ? rint(2000, 940000) : 0,
        sampledMin: rint(0, 2)
      });
    });
    const anyBad = vantages.some(v => v.outcome !== "ok");
    const allBad = vantages.every(v => v.outcome !== "ok");
    probes.push({
      targetId: `${proto}:${h.name}:${port || ""}`.replace(/:$/, ""),
      host: h.hostFqdn,
      hostNode: h.nodeId,
      port,
      proto,
      label,
      replFactor,
      vantages,
      state: allBad ? "down" : anyBad ? "degraded" : "up"
    });
  });

  // ---- alert rules ----
  const rules = [{
    ruleId: 1,
    name: "CPU saturation",
    scope: "host",
    metric: "cpuAvgMilli",
    op: "gt",
    threshold: 90,
    unit: "%",
    forSeconds: 120,
    severity: "warn",
    enabled: true
  }, {
    ruleId: 2,
    name: "Memory pressure",
    scope: "host",
    metric: "ramUsedPct",
    op: "gt",
    threshold: 92,
    unit: "%",
    forSeconds: 180,
    severity: "warn",
    enabled: true
  }, {
    ruleId: 3,
    name: "Disk almost full",
    scope: "host",
    metric: "diskUsedPct",
    op: "gt",
    threshold: 95,
    unit: "%",
    forSeconds: 0,
    severity: "crit",
    enabled: true
  }, {
    ruleId: 4,
    name: "Host unreachable",
    scope: "host",
    metric: "lastSeenSec",
    op: "gt",
    threshold: 60,
    unit: "s",
    forSeconds: 0,
    severity: "crit",
    enabled: true
  }, {
    ruleId: 5,
    name: "Swap thrash",
    scope: "host",
    metric: "swapUsedPct",
    op: "gt",
    threshold: 60,
    unit: "%",
    forSeconds: 300,
    severity: "info",
    enabled: true
  }, {
    ruleId: 6,
    name: "Probe packet loss",
    scope: "probe",
    metric: "lossPermille",
    op: "gt",
    threshold: 50,
    unit: "‰",
    forSeconds: 60,
    severity: "warn",
    enabled: true
  }, {
    ruleId: 7,
    name: "Probe latency high",
    scope: "probe",
    metric: "rttMicros",
    op: "gt",
    threshold: 25000,
    unit: "µs",
    forSeconds: 90,
    severity: "warn",
    enabled: true
  }, {
    ruleId: 8,
    name: "Service down (any vantage)",
    scope: "probe",
    metric: "outcome",
    op: "transition",
    threshold: 0,
    unit: "",
    forSeconds: 0,
    severity: "crit",
    enabled: true
  }, {
    ruleId: 9,
    name: "NIC error burst",
    scope: "host",
    metric: "ifErrs",
    op: "gt",
    threshold: 100,
    unit: "/s",
    forSeconds: 60,
    severity: "info",
    enabled: false
  }, {
    ruleId: 10,
    name: "Watched proc missing",
    scope: "host",
    metric: "procRunState",
    op: "transition",
    threshold: 0,
    unit: "",
    forSeconds: 0,
    severity: "crit",
    enabled: true
  }];

  // ---- alert events derived from fleet ----
  const alerts = [];
  let eid = 9000;
  function fire(node, rule, detail, mins, cleared) {
    alerts.push({
      eventId: eid++,
      ruleId: rule.ruleId,
      ruleName: rule.name,
      node: node ? node.name : null,
      nodeId: node ? node.nodeId : null,
      segName: node ? node.segName : null,
      severity: rule.severity,
      detail,
      firedMinAgo: mins,
      cleared: !!cleared
    });
  }
  nodes.forEach(n => {
    if (n.state === "down") {
      fire(n, rules[3], `No report received for ${n.lastSeenMin}m — last seen ${n.lastSeenMin}m ago`, n.lastSeenMin, false);
      const dead = n.procs.find(p => p.runState === "Z");
      if (dead) fire(n, rules[9], `Watched process '${dead.name}' not running (state Z)`, n.lastSeenMin - 1, false);
    } else if (n.state === "degraded") {
      if (n.cpuPct > 88) fire(n, rules[0], `CPU avg ${n.cpuPct}% sustained over threshold (90%)`, rint(2, 40), false);
      if (n.ramPct > 90) fire(n, rules[1], `RAM ${n.ramPct}% of ${(n.ramTotalKb / 1048576).toFixed(0)}GB`, rint(2, 50), false);
      if (n.diskMaxPct > 94) fire(n, rules[2], `Mount at ${n.diskMaxPct}% — ${(n.disks[0].totalGb * (100 - n.diskMaxPct) / 100).toFixed(0)}GB free`, rint(1, 20), false);
      if (n.swapPct > 60) fire(n, rules[4], `Swap ${n.swapPct}% — possible memory thrash`, rint(5, 120), false);
    }
  });
  probes.forEach(p => {
    if (p.state === "down") fire({
      name: p.host.split(".")[0],
      nodeId: p.hostNode,
      segName: "—"
    }, rules[7], `${p.targetId} unreachable from all ${p.vantages.length} vantages`, rint(1, 30), false);else if (p.state === "degraded") {
      const v = p.vantages.find(x => x.outcome !== "ok");
      fire({
        name: p.host.split(".")[0],
        nodeId: p.hostNode,
        segName: "—"
      }, rules[5], `${p.targetId}: ${v.outcome} from monitor '${v.monitorName}' (split vantage)`, rint(1, 25), false);
    }
  });
  // a few cleared (historical) ones
  for (let i = 0; i < 8; i++) {
    const n = pick(nodes.filter(x => x.state === "up"));
    fire(n, pick([rules[0], rules[1], rules[5]]), `Auto-cleared after recovery`, rint(60, 1200), true);
  }
  alerts.sort((a, b) => {
    const sev = {
      crit: 0,
      warn: 1,
      info: 2
    };
    if (a.cleared !== b.cleared) return a.cleared ? 1 : -1;
    if (sev[a.severity] !== sev[b.severity]) return sev[a.severity] - sev[b.severity];
    return a.firedMinAgo - b.firedMinAgo;
  });

  // ---- discovered (not-yet-monitored) ----
  const discovered = [{
    host: "tungsten.akoria.net",
    ip: "10.42.11.84",
    via: "mDNS",
    kind: "host",
    services: ["ssh:22", "http:80"],
    seen: 4,
    seg: "compute",
    arch: "x86_64"
  }, {
    host: "10.42.50.119",
    ip: "10.42.50.119",
    via: "ARP sweep",
    kind: "host",
    services: ["mqtt:1883"],
    seen: 11,
    seg: "iot",
    arch: "armv7"
  }, {
    host: "osmium.akoria.net",
    ip: "10.42.21.40",
    via: "SCP advert",
    kind: "service",
    services: ["minio:9000", "minio:9001"],
    seen: 2,
    seg: "storage",
    arch: "arm64"
  }, {
    host: "10.42.30.66",
    ip: "10.42.30.66",
    via: "port scan",
    kind: "service",
    services: ["https:443"],
    seen: 19,
    seg: "dmz",
    arch: "x86_64"
  }, {
    host: "rhenium.akoria.net",
    ip: "10.42.12.7",
    via: "mDNS",
    kind: "host",
    services: ["ssh:22", "postgres:5432", "node-exp:9100"],
    seen: 1,
    seg: "compute",
    arch: "arm64"
  }, {
    host: "10.42.40.158",
    ip: "10.42.40.158",
    via: "ARP sweep",
    kind: "host",
    services: ["ssh:22"],
    seen: 33,
    seg: "lab",
    arch: "armv7"
  }, {
    host: "iridium.akoria.net",
    ip: "10.42.20.91",
    via: "SCP advert",
    kind: "service",
    services: ["nfs:2049", "smb:445"],
    seen: 6,
    seg: "storage",
    arch: "x86_64"
  }, {
    host: "10.42.50.204",
    ip: "10.42.50.204",
    via: "port scan",
    kind: "host",
    services: ["coap:5683"],
    seen: 47,
    seg: "iot",
    arch: "armv7"
  }];

  // ---- binary builds / deploy convergence ----
  const archCount = {};
  nodes.forEach(n => {
    archCount[n.arch] = (archCount[n.arch] || 0) + 1;
  });
  const builds = [{
    arch: "x86_64",
    os: "Linux",
    version: "1.0.3",
    channel: "stable",
    nodes: archCount["x86_64"] || 0,
    status: "current"
  }, {
    arch: "arm64",
    os: "Linux",
    version: "1.0.3",
    channel: "stable",
    nodes: archCount["arm64"] || 0,
    status: "current"
  }, {
    arch: "armv7",
    os: "Linux",
    version: "1.0.2",
    channel: "stable",
    nodes: archCount["armv7"] || 0,
    status: "update"
  }, {
    arch: "x86_64",
    os: "Windows",
    version: "1.0.3",
    channel: "stable",
    nodes: archCount["x86_64"] ? 3 : 0,
    status: "current"
  }, {
    arch: "arm64",
    os: "macOS",
    version: "1.0.1",
    channel: "beta",
    nodes: 2,
    status: "update"
  }];

  // ---- pending enrollments (CSRs awaiting operator approval) ----
  const enrollments = [{
    host: "rhenium.akoria.net",
    ip: "10.42.12.7",
    role: "client",
    fp: "9F:2C:A1:88:E4:0B",
    requestedMin: 3,
    status: "pending"
  }, {
    host: "iridium.akoria.net",
    ip: "10.42.20.91",
    role: "monitor",
    fp: "47:DE:90:1A:3C:F5",
    requestedMin: 12,
    status: "pending"
  }, {
    host: "tungsten.akoria.net",
    ip: "10.42.11.84",
    role: "client",
    fp: "B0:11:7E:62:9A:D3",
    requestedMin: 26,
    status: "token"
  }];

  // ---- global configuration defaults ----
  const config = {
    schedule: {
      sampleIntervalSec: 15,
      watchdogIntervalSec: 5
    },
    probe: {
      roundIntervalSec: 30,
      probesPerRound: 5,
      replFactor: 2,
      gossipIntervalSec: 20
    },
    retention: {
      historyDays: 90,
      partitionByMonth: true
    },
    lease: {
      renewSec: 5,
      ttlSec: 15
    },
    ingest: {
      ports: {
        ingest: 7701,
        survey: 7702,
        pub: 7703
      },
      tls: "TLS 1.3 · mbedTLS"
    },
    ca: {
      issuer: "akoria internal CA (step-ca)",
      enroll: "token + CSR",
      certTtlDays: 365
    },
    autoDiscover: true,
    autoEnroll: false
  };

  // ---- network gear & LLDP-derived uplinks (for LAN-hierarchy topology) ----
  const NETGEAR = [{
    id: "gw",
    name: "gw-core",
    kind: "gateway",
    model: "pfSense+ · 10G",
    seg: "core",
    ports: 8,
    uplink: null
  }, {
    id: "sw-core",
    name: "sw-core-01",
    kind: "switch",
    model: "Arista 7050X · 48p",
    seg: "core",
    ports: 48,
    uplink: "gw"
  }, {
    id: "sw-compute",
    name: "sw-comp-01",
    kind: "switch",
    model: "Arista 7050X · 48p",
    seg: "compute",
    ports: 48,
    uplink: "sw-core"
  }, {
    id: "sw-storage",
    name: "sw-stor-01",
    kind: "switch",
    model: "MikroTik CRS · 24p",
    seg: "storage",
    ports: 24,
    uplink: "sw-core"
  }, {
    id: "sw-dmz",
    name: "sw-dmz-01",
    kind: "switch",
    model: "MikroTik CRS · 16p",
    seg: "dmz",
    ports: 16,
    uplink: "gw"
  }, {
    id: "ap-lab",
    name: "ap-lab-01",
    kind: "ap",
    model: "UniFi U6-Pro · wifi6",
    seg: "lab",
    ports: 0,
    uplink: "sw-core",
    wireless: true
  }, {
    id: "ap-iot",
    name: "ap-iot-01",
    kind: "ap",
    model: "UniFi U6-LR · wifi6",
    seg: "iot",
    ports: 0,
    uplink: "sw-core",
    wireless: true
  }];
  const SEG_GEAR = {
    core: "sw-core",
    compute: "sw-compute",
    storage: "sw-storage",
    dmz: "sw-dmz",
    lab: "ap-lab",
    iot: "ap-iot"
  };
  const WIRELESS_SEGS = {
    lab: true,
    iot: true
  };
  const portCtr = {};
  nodes.forEach(n => {
    const gid = SEG_GEAR[n.segId] || "sw-core";
    n.uplink = gid;
    n.linkType = WIRELESS_SEGS[n.segId] ? "wireless" : "wired";
    n.linkSpeedMbps = n.ifaces[0] ? n.ifaces[0].capMbps : 1000;
    if (n.linkType === "wired") {
      portCtr[gid] = (portCtr[gid] || 0) + 1;
      n.uplinkPort = "Gi1/0/" + portCtr[gid];
      n.lldp = true;
    } else {
      n.uplinkPort = "wlan0";
      n.rssi = -(42 + rint(0, 44));
      n.lldp = false;
    }
  });
  NETGEAR.forEach(g => {
    g.attached = nodes.filter(n => n.uplink === g.id).length;
  });

  // ---- rollups ----
  function rollup(list) {
    const r = {
      total: list.length,
      up: 0,
      degraded: 0,
      down: 0,
      unknown: 0
    };
    list.forEach(n => r[n.state]++);
    return r;
  }
  const fleetRoll = rollup(nodes);
  const segRollups = {};
  SEGMENTS.forEach(s => {
    segRollups[s.id] = rollup(nodes.filter(n => n.segId === s.id));
  });

  // ---- monitoring-wide summary (controller status) ----
  const appInstances = nodes.reduce((a, n) => a + n.procs.length, 0);
  const summary = {
    systems: nodes.length,
    // enrolled hosts (clients + monitors + servers)
    hosts: nodes.filter(n => n.role === "client").length,
    servers: nodes.filter(n => n.role === "server").length,
    monitors: nodes.filter(n => n.role === "monitor").length,
    applications: appInstances,
    // watched service/process instances across the fleet
    probes: probes.length,
    // network service endpoints under probe
    version: "1.0.3",
    uptimeStr: "134d 06h" // controller (active server) uptime
  };
  window.SOLARI = {
    nodes,
    segments: SEGMENTS,
    segRollups,
    fleetRoll,
    summary,
    probes,
    rules,
    alerts,
    discovered,
    builds,
    enrollments,
    config,
    netgear: NETGEAR,
    monitors,
    server: {
      primary: primary.name,
      primaryId: primary.nodeId,
      failover: failover.name,
      failoverId: failover.nodeId,
      leaseEpoch: 4471,
      leaseHealthy: true,
      leaseTtlSec: 15,
      leaseAgeSec: 2,
      dbHost: "127.0.0.1",
      historyDays: 90
    },
    activeCrit: alerts.filter(a => !a.cleared && a.severity === "crit").length,
    activeWarn: alerts.filter(a => !a.cleared && a.severity === "warn").length,
    fmt: {
      kb(kb) {
        if (kb >= 1048576) return (kb / 1048576).toFixed(1) + " GB";
        if (kb >= 1024) return (kb / 1024).toFixed(0) + " MB";
        return kb + " KB";
      },
      mbps(m) {
        if (m >= 1000) return (m / 1000).toFixed(1) + " Gb/s";
        return m + " Mb/s";
      },
      ago(min) {
        if (min < 1) return "just now";
        if (min < 60) return min + "m ago";
        if (min < 1440) return Math.floor(min / 60) + "h ago";
        return Math.floor(min / 1440) + "d ago";
      },
      rtt(us) {
        if (!us) return "—";
        if (us >= 1000) return (us / 1000).toFixed(1) + " ms";
        return us + " µs";
      }
    }
  };
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "src/js/01-0e23a051.jsx", error: String((e && e.message) || e) }); }

// src/js/02-91f5cdd6.jsx
try { (() => {
/* ============================================================
   SolariNet — icon set (solid geometric glyphs, currentColor)
   <Icon name="server" size={18} /> ; exported to window.
   ============================================================ */
(function () {
  const P = {
    // nav / structure
    overview: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
      x: "3",
      y: "3",
      width: "8",
      height: "8",
      rx: "1.5"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "13",
      y: "3",
      width: "8",
      height: "5",
      rx: "1.5"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "13",
      y: "10",
      width: "8",
      height: "11",
      rx: "1.5"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "3",
      y: "13",
      width: "8",
      height: "8",
      rx: "1.5"
    })),
    server: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
      x: "3",
      y: "3",
      width: "18",
      height: "7",
      rx: "2"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "3",
      y: "14",
      width: "18",
      height: "7",
      rx: "2"
    }), /*#__PURE__*/React.createElement("circle", {
      cx: "7",
      cy: "6.5",
      r: "1.2",
      fill: "#05080e"
    }), /*#__PURE__*/React.createElement("circle", {
      cx: "7",
      cy: "17.5",
      r: "1.2",
      fill: "#05080e"
    })),
    host: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
      x: "3",
      y: "4",
      width: "18",
      height: "12",
      rx: "2"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "8",
      y: "18",
      width: "8",
      height: "2.4",
      rx: "1.2"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "6",
      y: "19.6",
      width: "12",
      height: "1.8",
      rx: "0.9"
    })),
    monitor: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("circle", {
      cx: "12",
      cy: "12",
      r: "2.6"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M12 2a10 10 0 0 1 10 10h-3a7 7 0 0 0-7-7z",
      opacity: "0.85"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M12 22A10 10 0 0 1 2 12h3a7 7 0 0 0 7 7z",
      opacity: "0.55"
    })),
    reachability: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("circle", {
      cx: "5",
      cy: "6",
      r: "2.4"
    }), /*#__PURE__*/React.createElement("circle", {
      cx: "5",
      cy: "18",
      r: "2.4"
    }), /*#__PURE__*/React.createElement("circle", {
      cx: "19",
      cy: "12",
      r: "2.4"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M7 7l10 4M7 17l10-4",
      stroke: "currentColor",
      strokeWidth: "1.7",
      fill: "none"
    })),
    topology: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("circle", {
      cx: "12",
      cy: "5",
      r: "2.4"
    }), /*#__PURE__*/React.createElement("circle", {
      cx: "5",
      cy: "18",
      r: "2.4"
    }), /*#__PURE__*/React.createElement("circle", {
      cx: "19",
      cy: "18",
      r: "2.4"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M12 7v4m0 0l-6 6m6-6l6 6",
      stroke: "currentColor",
      strokeWidth: "1.7",
      fill: "none"
    })),
    alerts: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
      d: "M12 3l9 16H3z"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "11",
      y: "9",
      width: "2",
      height: "5",
      rx: "1",
      fill: "#05080e"
    }), /*#__PURE__*/React.createElement("circle", {
      cx: "12",
      cy: "16.5",
      r: "1.1",
      fill: "#05080e"
    })),
    discovery: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("circle", {
      cx: "11",
      cy: "11",
      r: "6.5",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "2"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M16 16l5 5",
      stroke: "currentColor",
      strokeWidth: "2.4",
      strokeLinecap: "round"
    }), /*#__PURE__*/React.createElement("circle", {
      cx: "11",
      cy: "11",
      r: "2"
    })),
    provision: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
      x: "4",
      y: "4",
      width: "16",
      height: "16",
      rx: "3",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "2"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M12 8v8M8 12h8",
      stroke: "currentColor",
      strokeWidth: "2.2",
      strokeLinecap: "round"
    })),
    settings: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("circle", {
      cx: "12",
      cy: "12",
      r: "3"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M12 2l1.5 3 3.3-.6L17 7.7 20 9l-1.3 3L20 15l-2.2 2.3.2 3.3L15 21l-3 1-3-1-2.9.6.2-3.3L4 15l1.3-3L4 9l3-1.3-.2-3.3L10 5z",
      opacity: "0.35"
    })),
    // metrics
    cpu: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
      x: "6",
      y: "6",
      width: "12",
      height: "12",
      rx: "2"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "9",
      y: "9",
      width: "6",
      height: "6",
      rx: "1",
      fill: "#05080e"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M9 2v3M15 2v3M9 19v3M15 19v3M2 9h3M2 15h3M19 9h3M19 15h3",
      stroke: "currentColor",
      strokeWidth: "1.8",
      strokeLinecap: "round"
    })),
    ram: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
      x: "2",
      y: "7",
      width: "20",
      height: "10",
      rx: "2"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "5",
      y: "10",
      width: "2.5",
      height: "7",
      fill: "#05080e"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "9",
      y: "10",
      width: "2.5",
      height: "7",
      fill: "#05080e"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "13",
      y: "10",
      width: "2.5",
      height: "7",
      fill: "#05080e"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "17",
      y: "10",
      width: "2.5",
      height: "7",
      fill: "#05080e"
    })),
    disk: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("circle", {
      cx: "12",
      cy: "12",
      r: "9"
    }), /*#__PURE__*/React.createElement("circle", {
      cx: "12",
      cy: "12",
      r: "2.4",
      fill: "#05080e"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M12 3v4",
      stroke: "#05080e",
      strokeWidth: "1.6"
    })),
    network: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
      x: "9",
      y: "2",
      width: "6",
      height: "5",
      rx: "1.4"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "2",
      y: "17",
      width: "6",
      height: "5",
      rx: "1.4"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "16",
      y: "17",
      width: "6",
      height: "5",
      rx: "1.4"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M12 7v4m0 0H5v6m7-6h7v6",
      stroke: "currentColor",
      strokeWidth: "1.7",
      fill: "none"
    })),
    bandwidth: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
      d: "M3 17a9 9 0 0 1 18 0",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "2"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M12 17l5-4",
      stroke: "currentColor",
      strokeWidth: "2.2",
      strokeLinecap: "round"
    }), /*#__PURE__*/React.createElement("circle", {
      cx: "12",
      cy: "17",
      r: "1.8"
    })),
    activity: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
      d: "M2 12h4l3 8 4-16 3 8h6",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "2.2",
      strokeLinecap: "round",
      strokeLinejoin: "round"
    })),
    process: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
      x: "3",
      y: "3",
      width: "18",
      height: "18",
      rx: "3",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "2"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M7 9l3 3-3 3M12 15h5",
      stroke: "currentColor",
      strokeWidth: "2",
      strokeLinecap: "round",
      strokeLinejoin: "round"
    })),
    // ui
    search: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("circle", {
      cx: "11",
      cy: "11",
      r: "6.5",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "2"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M16 16l5 5",
      stroke: "currentColor",
      strokeWidth: "2.4",
      strokeLinecap: "round"
    })),
    command: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
      d: "M9 3a3 3 0 1 1-3 3v12a3 3 0 1 1 3-3h6a3 3 0 1 1-3 3V6a3 3 0 1 1 3 3H9",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "2"
    })),
    grid: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
      x: "3",
      y: "3",
      width: "7",
      height: "7",
      rx: "1.5"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "14",
      y: "3",
      width: "7",
      height: "7",
      rx: "1.5"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "3",
      y: "14",
      width: "7",
      height: "7",
      rx: "1.5"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "14",
      y: "14",
      width: "7",
      height: "7",
      rx: "1.5"
    })),
    table: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
      x: "3",
      y: "4",
      width: "18",
      height: "16",
      rx: "2",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "2"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M3 9h18M3 14h18M9 9v11",
      stroke: "currentColor",
      strokeWidth: "1.7"
    })),
    cards: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
      x: "3",
      y: "4",
      width: "18",
      height: "7",
      rx: "2"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "3",
      y: "13",
      width: "18",
      height: "7",
      rx: "2",
      opacity: "0.5"
    })),
    bell: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
      d: "M6 16V11a6 6 0 0 1 12 0v5l2 2H4z"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M10 20a2 2 0 0 0 4 0",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "1.8"
    })),
    sun: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("circle", {
      cx: "12",
      cy: "12",
      r: "4.5"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M12 1v3M12 20v3M1 12h3M20 12h3M4.2 4.2l2.1 2.1M17.7 17.7l2.1 2.1M19.8 4.2l-2.1 2.1M6.3 17.7l-2.1 2.1",
      stroke: "currentColor",
      strokeWidth: "2",
      strokeLinecap: "round"
    })),
    moon: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
      d: "M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z"
    })),
    menu: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
      d: "M4 6h16M4 12h16M4 18h16",
      stroke: "currentColor",
      strokeWidth: "2.2",
      strokeLinecap: "round"
    })),
    close: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
      d: "M6 6l12 12M18 6L6 18",
      stroke: "currentColor",
      strokeWidth: "2.2",
      strokeLinecap: "round"
    })),
    chevronLeft: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
      d: "M15 5l-7 7 7 7",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "2.2",
      strokeLinecap: "round",
      strokeLinejoin: "round"
    })),
    chevronRight: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
      d: "M9 5l7 7-7 7",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "2.2",
      strokeLinecap: "round",
      strokeLinejoin: "round"
    })),
    check: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
      d: "M4 12l5 5L20 6",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "2.4",
      strokeLinecap: "round",
      strokeLinejoin: "round"
    })),
    refresh: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
      d: "M20 11a8 8 0 1 0-1 5",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "2.2",
      strokeLinecap: "round"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M20 4v6h-6",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "2.2",
      strokeLinecap: "round",
      strokeLinejoin: "round"
    })),
    survey: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("circle", {
      cx: "12",
      cy: "12",
      r: "2.2"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M12 12L4 6M12 12l8-6",
      stroke: "currentColor",
      strokeWidth: "1.8",
      strokeLinecap: "round"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M5 19a10 10 0 0 1 14 0",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "2",
      strokeLinecap: "round"
    })),
    filter: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
      d: "M3 5h18l-7 8v6l-4 2v-8z"
    })),
    shield: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
      d: "M12 2l8 3v6c0 5-3.5 8.5-8 11-4.5-2.5-8-6-8-11V5z"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M8.5 12l2.5 2.5 4.5-5",
      fill: "none",
      stroke: "#05080e",
      strokeWidth: "1.8",
      strokeLinecap: "round",
      strokeLinejoin: "round"
    })),
    pulse: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
      d: "M2 12h5l2-5 4 12 2-7h7",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "2",
      strokeLinecap: "round",
      strokeLinejoin: "round"
    })),
    clock: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("circle", {
      cx: "12",
      cy: "12",
      r: "9",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "2"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M12 7v5l3.5 2",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "2",
      strokeLinecap: "round",
      strokeLinejoin: "round"
    })),
    arch: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
      x: "4",
      y: "3",
      width: "16",
      height: "4",
      rx: "1.4"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "4",
      y: "10",
      width: "16",
      height: "4",
      rx: "1.4",
      opacity: "0.7"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "4",
      y: "17",
      width: "16",
      height: "4",
      rx: "1.4",
      opacity: "0.45"
    })),
    chip: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
      x: "6",
      y: "6",
      width: "12",
      height: "12",
      rx: "2"
    })),
    link: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
      d: "M9 15l6-6",
      stroke: "currentColor",
      strokeWidth: "2",
      strokeLinecap: "round"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M8 12l-2 2a3 3 0 0 0 4 4l2-2M16 12l2-2a3 3 0 0 0-4-4l-2 2",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "2",
      strokeLinecap: "round"
    })),
    plus: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
      d: "M12 5v14M5 12h14",
      stroke: "currentColor",
      strokeWidth: "2.4",
      strokeLinecap: "round"
    })),
    enter: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
      d: "M9 10l-4 4 4 4",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "1.8",
      strokeLinecap: "round",
      strokeLinejoin: "round"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M5 14h10a4 4 0 0 0 4-4V6",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "1.8",
      strokeLinecap: "round",
      strokeLinejoin: "round"
    })),
    gateway: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
      x: "3",
      y: "9",
      width: "18",
      height: "9",
      rx: "2"
    }), /*#__PURE__*/React.createElement("circle", {
      cx: "7",
      cy: "13.5",
      r: "1.3",
      fill: "#05080e"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "11",
      y: "12.5",
      width: "7",
      height: "2",
      rx: "1",
      fill: "#05080e"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M8 9V6a4 4 0 0 1 8 0v3",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "1.8"
    })),
    netswitch: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("rect", {
      x: "2",
      y: "8",
      width: "20",
      height: "8",
      rx: "1.6"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "5",
      y: "11",
      width: "2",
      height: "2.5",
      fill: "#05080e"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "8.5",
      y: "11",
      width: "2",
      height: "2.5",
      fill: "#05080e"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "12",
      y: "11",
      width: "2",
      height: "2.5",
      fill: "#05080e"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "15.5",
      y: "11",
      width: "2",
      height: "2.5",
      fill: "#05080e"
    })),
    wifi: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
      d: "M5 12.5a10 10 0 0 1 14 0",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "2",
      strokeLinecap: "round"
    }), /*#__PURE__*/React.createElement("path", {
      d: "M8 15.5a6 6 0 0 1 8 0",
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "2",
      strokeLinecap: "round"
    }), /*#__PURE__*/React.createElement("circle", {
      cx: "12",
      cy: "18.5",
      r: "1.6"
    })),
    close2: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("path", {
      d: "M6 6l12 12M18 6L6 18",
      stroke: "currentColor",
      strokeWidth: "2.2",
      strokeLinecap: "round"
    }))
  };
  function Icon({
    name,
    size = 18,
    className,
    style
  }) {
    const body = P[name];
    if (!body) return null;
    return /*#__PURE__*/React.createElement("svg", {
      className: className,
      style: style,
      width: size,
      height: size,
      viewBox: "0 0 24 24",
      fill: "currentColor",
      "aria-hidden": "true",
      focusable: "false"
    }, body);
  }
  window.Icon = Icon;
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "src/js/02-91f5cdd6.jsx", error: String((e && e.message) || e) }); }

// src/js/03-ff8ac535.jsx
try { (() => {
/* ============================================================
   SolariNet — shared components & charts
   ============================================================ */
(function () {
  const {
    useState,
    useEffect,
    useRef
  } = React;
  const Icon = window.Icon;

  // ---------- status helpers ----------
  const STATE_LABEL = {
    up: "Up",
    degraded: "Degraded",
    down: "Down",
    unknown: "Unknown"
  };
  function StatusDot({
    state,
    glow = true,
    size = 9,
    style
  }) {
    return /*#__PURE__*/React.createElement("span", {
      className: "dot " + (glow ? "glow " : "") + state,
      style: {
        width: size,
        height: size,
        ...style
      }
    });
  }
  function metricColor(pct) {
    if (pct >= 90) return "var(--crit)";
    if (pct >= 75) return "var(--warn)";
    return "var(--accent)";
  }

  // ---------- sparkline ----------
  function Sparkline({
    data,
    color = "var(--accent)",
    w = 120,
    h = 28,
    fill = true,
    strokeW = 1.6
  }) {
    if (!data || !data.length) return null;
    const min = Math.min(...data),
      max = Math.max(...data);
    const span = max - min || 1;
    const stepX = w / (data.length - 1);
    const pts = data.map((v, i) => [i * stepX, h - 3 - (v - min) / span * (h - 6)]);
    const line = pts.map((p, i) => (i ? "L" : "M") + p[0].toFixed(1) + " " + p[1].toFixed(1)).join(" ");
    const area = line + ` L ${w} ${h} L 0 ${h} Z`;
    const gid = "sg" + Math.random().toString(36).slice(2, 8);
    return /*#__PURE__*/React.createElement("svg", {
      width: "100%",
      height: h,
      viewBox: `0 0 ${w} ${h}`,
      preserveAspectRatio: "none",
      style: {
        display: "block"
      }
    }, fill && /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("defs", null, /*#__PURE__*/React.createElement("linearGradient", {
      id: gid,
      x1: "0",
      y1: "0",
      x2: "0",
      y2: "1"
    }, /*#__PURE__*/React.createElement("stop", {
      offset: "0",
      stopColor: color,
      stopOpacity: "0.35"
    }), /*#__PURE__*/React.createElement("stop", {
      offset: "1",
      stopColor: color,
      stopOpacity: "0"
    }))), /*#__PURE__*/React.createElement("path", {
      d: area,
      fill: `url(#${gid})`
    })), /*#__PURE__*/React.createElement("path", {
      d: line,
      fill: "none",
      stroke: color,
      strokeWidth: strokeW,
      strokeLinejoin: "round",
      strokeLinecap: "round",
      vectorEffect: "non-scaling-stroke"
    }));
  }

  // ---------- time-series (detail charts) ----------
  function TimeSeries({
    data,
    color = "var(--accent)",
    h = 150,
    unit = "%",
    max: forceMax
  }) {
    const w = 600;
    const max = forceMax != null ? forceMax : Math.max(10, Math.ceil(Math.max(...data) / 10) * 10);
    const stepX = w / (data.length - 1);
    const y = v => h - 22 - v / max * (h - 34);
    const pts = data.map((v, i) => [i * stepX, y(v)]);
    const line = pts.map((p, i) => (i ? "L" : "M") + p[0].toFixed(1) + " " + p[1].toFixed(1)).join(" ");
    const area = line + ` L ${w} ${h - 22} L 0 ${h - 22} Z`;
    const gid = "ts" + Math.random().toString(36).slice(2, 8);
    const grid = [0, 0.5, 1];
    const last = data[data.length - 1];
    return /*#__PURE__*/React.createElement("svg", {
      width: "100%",
      height: h,
      viewBox: `0 0 ${w} ${h}`,
      preserveAspectRatio: "none",
      style: {
        display: "block"
      }
    }, /*#__PURE__*/React.createElement("defs", null, /*#__PURE__*/React.createElement("linearGradient", {
      id: gid,
      x1: "0",
      y1: "0",
      x2: "0",
      y2: "1"
    }, /*#__PURE__*/React.createElement("stop", {
      offset: "0",
      stopColor: color,
      stopOpacity: "0.30"
    }), /*#__PURE__*/React.createElement("stop", {
      offset: "1",
      stopColor: color,
      stopOpacity: "0"
    }))), grid.map((g, i) => /*#__PURE__*/React.createElement("line", {
      key: i,
      x1: "0",
      x2: w,
      y1: h - 22 - g * (h - 34),
      y2: h - 22 - g * (h - 34),
      stroke: "var(--line)",
      strokeWidth: "1",
      vectorEffect: "non-scaling-stroke"
    })), /*#__PURE__*/React.createElement("path", {
      d: area,
      fill: `url(#${gid})`
    }), /*#__PURE__*/React.createElement("path", {
      d: line,
      fill: "none",
      stroke: color,
      strokeWidth: "1.8",
      strokeLinejoin: "round",
      strokeLinecap: "round",
      vectorEffect: "non-scaling-stroke"
    }), /*#__PURE__*/React.createElement("circle", {
      cx: w,
      cy: y(last),
      r: "3",
      fill: color
    }));
  }

  // ---------- horizontal bar gauge (bandwidth vs capacity) ----------
  function BandwidthGauge({
    label,
    used,
    cap,
    unit,
    color = "var(--accent)"
  }) {
    const pct = cap ? Math.min(100, used / cap * 100) : 0;
    const c = pct >= 85 ? "var(--crit)" : pct >= 65 ? "var(--warn)" : color;
    return /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        justifyContent: "space-between",
        fontFamily: "var(--mono)",
        fontSize: 11,
        marginBottom: 6
      }
    }, /*#__PURE__*/React.createElement("span", {
      className: "muted",
      style: {
        letterSpacing: ".08em",
        textTransform: "uppercase",
        fontSize: 10
      }
    }, label), /*#__PURE__*/React.createElement("span", {
      style: {
        color: c,
        fontWeight: 600
      }
    }, unit(used), " ", /*#__PURE__*/React.createElement("span", {
      className: "muted"
    }, "/ ", unit(cap)))), /*#__PURE__*/React.createElement("div", {
      className: "metricbar",
      style: {
        width: "100%",
        height: 9
      }
    }, /*#__PURE__*/React.createElement("i", {
      style: {
        width: pct + "%",
        background: c,
        boxShadow: `0 0 calc(8px*var(--glow)) ${c}`
      }
    })));
  }

  // ---------- radial gauge ----------
  function RadialGauge({
    value,
    max = 100,
    label,
    sub,
    size = 116,
    color
  }) {
    const pct = Math.min(1, value / max);
    const r = size / 2 - 9;
    const circ = 2 * Math.PI * r;
    const c = color || (pct >= 0.9 ? "var(--crit)" : pct >= 0.75 ? "var(--warn)" : "var(--accent)");
    return /*#__PURE__*/React.createElement("div", {
      className: "gauge",
      style: {
        width: size
      }
    }, /*#__PURE__*/React.createElement("svg", {
      width: size,
      height: size,
      viewBox: `0 0 ${size} ${size}`
    }, /*#__PURE__*/React.createElement("circle", {
      cx: size / 2,
      cy: size / 2,
      r: r,
      fill: "none",
      stroke: "var(--line)",
      strokeWidth: "8"
    }), /*#__PURE__*/React.createElement("circle", {
      cx: size / 2,
      cy: size / 2,
      r: r,
      fill: "none",
      stroke: c,
      strokeWidth: "8",
      strokeLinecap: "round",
      strokeDasharray: circ,
      strokeDashoffset: circ * (1 - pct),
      transform: `rotate(-90 ${size / 2} ${size / 2})`,
      style: {
        filter: "drop-shadow(0 0 calc(5px*var(--glow)) " + c + ")",
        transition: "stroke-dashoffset .5s ease"
      }
    }), /*#__PURE__*/React.createElement("text", {
      x: "50%",
      y: "48%",
      textAnchor: "middle",
      dominantBaseline: "middle",
      style: {
        fontFamily: "var(--mono)",
        fontSize: 22,
        fontWeight: 600,
        fill: "var(--ink)"
      }
    }, Math.round(value)), /*#__PURE__*/React.createElement("text", {
      x: "50%",
      y: "63%",
      textAnchor: "middle",
      style: {
        fontFamily: "var(--mono)",
        fontSize: 9,
        fill: "var(--ink-faint)"
      }
    }, sub)), /*#__PURE__*/React.createElement("div", {
      className: "lab"
    }, label));
  }

  // ---------- donut for segment rollup ----------
  function HealthDonut({
    roll,
    size = 76
  }) {
    const r = size / 2 - 7,
      circ = 2 * Math.PI * r;
    const order = [["up", "var(--ok)"], ["degraded", "var(--warn)"], ["down", "var(--crit)"], ["unknown", "var(--unknown)"]];
    let offset = 0;
    const total = roll.total || 1;
    return /*#__PURE__*/React.createElement("div", {
      style: {
        position: "relative",
        width: size,
        height: size,
        flex: "0 0 " + size + "px"
      }
    }, /*#__PURE__*/React.createElement("svg", {
      width: size,
      height: size,
      viewBox: `0 0 ${size} ${size}`,
      style: {
        transform: "rotate(-90deg)"
      }
    }, /*#__PURE__*/React.createElement("circle", {
      cx: size / 2,
      cy: size / 2,
      r: r,
      fill: "none",
      stroke: "var(--line)",
      strokeWidth: "7"
    }), order.map(([k, c]) => {
      const frac = roll[k] / total;
      if (!frac) return null;
      const seg = /*#__PURE__*/React.createElement("circle", {
        key: k,
        cx: size / 2,
        cy: size / 2,
        r: r,
        fill: "none",
        stroke: c,
        strokeWidth: "7",
        strokeDasharray: `${frac * circ} ${circ}`,
        strokeDashoffset: -offset * circ
      });
      offset += frac;
      return seg;
    })), /*#__PURE__*/React.createElement("div", {
      style: {
        position: "absolute",
        inset: 0,
        display: "grid",
        placeItems: "center"
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        textAlign: "center"
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        fontFamily: "var(--mono)",
        fontSize: 18,
        fontWeight: 700,
        lineHeight: 1
      }
    }, roll.total), /*#__PURE__*/React.createElement("div", {
      style: {
        fontFamily: "var(--mono)",
        fontSize: 8,
        color: "var(--ink-faint)",
        letterSpacing: ".1em"
      }
    }, "NODES"))));
  }

  // ---------- RTT distribution (per vantage mini histogram) ----------
  function RTTBars({
    vantages,
    fmt
  }) {
    const max = Math.max(1, ...vantages.map(v => v.rttMicros || 0));
    return /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        alignItems: "flex-end",
        gap: 6,
        height: 56
      }
    }, vantages.map((v, i) => {
      const ok = v.outcome === "ok";
      const hpx = ok ? Math.max(4, v.rttMicros / max * 48) : 48;
      const c = !ok ? "var(--crit)" : v.lossPermille > 0 ? "var(--warn)" : "var(--accent)";
      return /*#__PURE__*/React.createElement("div", {
        key: i,
        style: {
          flex: 1,
          textAlign: "center"
        }
      }, /*#__PURE__*/React.createElement("div", {
        style: {
          height: 48,
          display: "flex",
          alignItems: "flex-end"
        }
      }, /*#__PURE__*/React.createElement("div", {
        style: {
          width: "100%",
          height: hpx,
          background: ok ? c : "var(--crit-bg)",
          border: ok ? "none" : "1px solid var(--crit)",
          borderRadius: 3,
          boxShadow: ok ? `0 0 calc(6px*var(--glow)) ${c}` : "none"
        }
      })), /*#__PURE__*/React.createElement("div", {
        style: {
          fontFamily: "var(--mono)",
          fontSize: 8.5,
          color: "var(--ink-faint)",
          marginTop: 4,
          whiteSpace: "nowrap",
          overflow: "hidden",
          textOverflow: "ellipsis"
        }
      }, v.monitorName));
    }));
  }

  // ---------- sidebar ----------
  const NAV = [{
    group: "Monitor"
  }, {
    id: "fleet",
    label: "Fleet Overview",
    icon: "overview"
  }, {
    id: "reachability",
    label: "Reachability",
    icon: "reachability"
  }, {
    id: "topology",
    label: "Topology Map",
    icon: "topology"
  }, {
    id: "alerts",
    label: "Alerts",
    icon: "alerts",
    badge: "alerts"
  }, {
    group: "Manage"
  }, {
    id: "discovery",
    label: "Discovery",
    icon: "discovery",
    badge: "discovery"
  }, {
    id: "provision",
    label: "Provisioning",
    icon: "provision"
  }, {
    id: "settings",
    label: "Config & Rules",
    icon: "settings"
  }];
  function Sidebar({
    active,
    onNav,
    collapsed,
    onToggle,
    hidden,
    summary,
    activeCrit
  }) {
    return /*#__PURE__*/React.createElement("aside", {
      className: "sidebar" + (collapsed ? " collapsed" : "") + (hidden ? " hidden" : "")
    }, /*#__PURE__*/React.createElement("div", {
      className: "brand"
    }, /*#__PURE__*/React.createElement("div", {
      className: "brand__mark"
    }, /*#__PURE__*/React.createElement("svg", {
      width: "26",
      height: "26",
      viewBox: "0 0 24 24",
      "aria-hidden": "true"
    }, /*#__PURE__*/React.createElement("g", {
      fill: "none",
      stroke: "currentColor",
      strokeWidth: "1.3",
      opacity: ".55"
    }, /*#__PURE__*/React.createElement("path", {
      d: "M12 12V4M12 12l6.9 4M12 12L5.1 16"
    })), /*#__PURE__*/React.createElement("rect", {
      x: "9.2",
      y: "9.2",
      width: "5.6",
      height: "5.6",
      rx: "1.5",
      fill: "currentColor"
    }), /*#__PURE__*/React.createElement("g", {
      fill: "var(--ink)"
    }, /*#__PURE__*/React.createElement("rect", {
      x: "10.4",
      y: "2.4",
      width: "3.2",
      height: "3.2",
      rx: ".5",
      transform: "rotate(45 12 4)"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "17.3",
      y: "14.4",
      width: "3.2",
      height: "3.2",
      rx: ".5",
      transform: "rotate(45 18.9 16)"
    }), /*#__PURE__*/React.createElement("rect", {
      x: "3.5",
      y: "14.4",
      width: "3.2",
      height: "3.2",
      rx: ".5",
      transform: "rotate(45 5.1 16)"
    })))), /*#__PURE__*/React.createElement("div", {
      className: "brand__name"
    }, "solari", /*#__PURE__*/React.createElement("b", null, "Net"))), /*#__PURE__*/React.createElement("nav", {
      className: "nav"
    }, NAV.map((item, i) => {
      if (item.group) return /*#__PURE__*/React.createElement("div", {
        key: i,
        className: "nav-section"
      }, item.group);
      const isActive = active === item.id;
      return /*#__PURE__*/React.createElement("div", {
        key: item.id,
        className: "nav-item" + (isActive ? " active" : "") + (item.planned ? " planned" : ""),
        onClick: () => !item.planned && onNav(item.id)
      }, /*#__PURE__*/React.createElement(Icon, {
        name: item.icon,
        size: 19,
        className: "ico"
      }), /*#__PURE__*/React.createElement("span", {
        className: "navlabel"
      }, item.label), item.planned && /*#__PURE__*/React.createElement("span", {
        className: "nav-item__pill"
      }, "soon"), item.badge === "alerts" && activeCrit > 0 && /*#__PURE__*/React.createElement("span", {
        className: "nav-item__count"
      }, activeCrit), item.badge === "discovery" && /*#__PURE__*/React.createElement("span", {
        className: "nav-item__count",
        style: {
          background: "var(--ok-bg)",
          color: "var(--ok)"
        }
      }, window.SOLARI.discovered.length));
    })), /*#__PURE__*/React.createElement("div", {
      className: "sysstat"
    }, /*#__PURE__*/React.createElement("div", {
      className: "sysstat__grid"
    }, /*#__PURE__*/React.createElement("div", {
      className: "sysstat__cell"
    }, /*#__PURE__*/React.createElement("div", {
      className: "sysstat__v"
    }, summary.systems), /*#__PURE__*/React.createElement("div", {
      className: "sysstat__k"
    }, "Systems")), /*#__PURE__*/React.createElement("div", {
      className: "sysstat__cell"
    }, /*#__PURE__*/React.createElement("div", {
      className: "sysstat__v"
    }, summary.applications), /*#__PURE__*/React.createElement("div", {
      className: "sysstat__k"
    }, "Applications")), /*#__PURE__*/React.createElement("div", {
      className: "sysstat__cell"
    }, /*#__PURE__*/React.createElement("div", {
      className: "sysstat__v"
    }, summary.uptimeStr), /*#__PURE__*/React.createElement("div", {
      className: "sysstat__k"
    }, "Uptime")), /*#__PURE__*/React.createElement("div", {
      className: "sysstat__cell"
    }, /*#__PURE__*/React.createElement("div", {
      className: "sysstat__v"
    }, "v", summary.version), /*#__PURE__*/React.createElement("div", {
      className: "sysstat__k"
    }, "Version")))), /*#__PURE__*/React.createElement("button", {
      className: "collapse-btn",
      onClick: onToggle,
      "aria-label": collapsed ? "Expand sidebar" : "Collapse sidebar",
      title: "Collapse / expand"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: collapsed ? "chevronRight" : "chevronLeft",
      size: 18
    }), /*#__PURE__*/React.createElement("span", {
      className: "navlabel"
    }, "Collapse")));
  }

  // ---------- top bar ----------
  function TopBar({
    onMenu,
    onOpenCmd,
    theme,
    onToggleTheme,
    server,
    lastTick,
    onSurvey
  }) {
    return /*#__PURE__*/React.createElement("header", {
      className: "topbar"
    }, /*#__PURE__*/React.createElement("button", {
      className: "iconbtn",
      onClick: onMenu,
      "aria-label": "Toggle navigation"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "menu",
      size: 20
    })), /*#__PURE__*/React.createElement("div", {
      className: "search",
      onClick: onOpenCmd
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "search",
      size: 17
    }), /*#__PURE__*/React.createElement("input", {
      placeholder: "Search nodes, probes, alerts\u2026  press / or \u2318K",
      readOnly: true
    }), /*#__PURE__*/React.createElement("span", {
      className: "kbd"
    }, "\u2318K")), /*#__PURE__*/React.createElement("div", {
      className: "topbar__spacer"
    }), /*#__PURE__*/React.createElement("div", {
      className: "statuschip",
      title: "Active control server holding the failover lease"
    }, /*#__PURE__*/React.createElement("span", {
      className: "dot up glow",
      style: {
        width: 8,
        height: 8
      }
    }), /*#__PURE__*/React.createElement("span", {
      style: {
        fontSize: 9.5,
        letterSpacing: ".14em",
        textTransform: "uppercase",
        color: "var(--ink-faint)"
      }
    }, "Active\xA0C2"), /*#__PURE__*/React.createElement("b", null, server.primary), /*#__PURE__*/React.createElement("span", {
      style: {
        color: "var(--ink-faint)"
      }
    }, "\xB7"), /*#__PURE__*/React.createElement("span", {
      style: {
        color: "var(--ink-faint)"
      }
    }, "failover\xA0", server.failover)), /*#__PURE__*/React.createElement("button", {
      className: "iconbtn",
      onClick: onSurvey,
      "aria-label": "Survey now",
      title: "Survey fleet now"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "survey",
      size: 20
    })), /*#__PURE__*/React.createElement("button", {
      className: "iconbtn",
      onClick: onToggleTheme,
      "aria-label": "Toggle theme"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: theme === "dark" ? "sun" : "moon",
      size: 19
    })));
  }

  // ---------- command palette ----------
  function CommandPalette({
    open,
    onClose,
    commands
  }) {
    const [q, setQ] = useState("");
    const [sel, setSel] = useState(0);
    const inputRef = useRef(null);
    useEffect(() => {
      if (open) {
        setQ("");
        setSel(0);
        setTimeout(() => inputRef.current && inputRef.current.focus(), 30);
      }
    }, [open]);
    if (!open) return null;
    const ql = q.toLowerCase();
    const filtered = commands.filter(c => !ql || (c.label + " " + (c.group || "") + " " + (c.keywords || "")).toLowerCase().includes(ql));
    const groups = {};
    filtered.forEach(c => {
      (groups[c.group] = groups[c.group] || []).push(c);
    });
    const flat = filtered;
    function run(i) {
      const c = flat[i];
      if (c) {
        c.action();
        onClose();
      }
    }
    function onKey(e) {
      if (e.key === "ArrowDown") {
        e.preventDefault();
        setSel(s => Math.min(flat.length - 1, s + 1));
      } else if (e.key === "ArrowUp") {
        e.preventDefault();
        setSel(s => Math.max(0, s - 1));
      } else if (e.key === "Enter") {
        e.preventDefault();
        run(sel);
      } else if (e.key === "Escape") {
        onClose();
      }
    }
    let runningIndex = -1;
    return /*#__PURE__*/React.createElement("div", {
      className: "cmdk-overlay",
      onClick: onClose
    }, /*#__PURE__*/React.createElement("div", {
      className: "cmdk",
      onClick: e => e.stopPropagation()
    }, /*#__PURE__*/React.createElement("div", {
      className: "cmdk__input"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "search",
      size: 20,
      style: {
        color: "var(--accent)"
      }
    }), /*#__PURE__*/React.createElement("input", {
      ref: inputRef,
      value: q,
      onChange: e => {
        setQ(e.target.value);
        setSel(0);
      },
      onKeyDown: onKey,
      placeholder: "Jump to a node, run a command\u2026"
    }), /*#__PURE__*/React.createElement("span", {
      className: "kbd"
    }, "esc")), /*#__PURE__*/React.createElement("div", {
      className: "cmdk__list"
    }, flat.length === 0 && /*#__PURE__*/React.createElement("div", {
      className: "empty"
    }, "No matches"), Object.entries(groups).map(([g, items]) => /*#__PURE__*/React.createElement("div", {
      key: g
    }, /*#__PURE__*/React.createElement("div", {
      className: "cmdk__group"
    }, g), items.map(c => {
      runningIndex++;
      const i = runningIndex;
      return /*#__PURE__*/React.createElement("div", {
        key: c.id,
        className: "cmdk__item" + (i === sel ? " sel" : ""),
        onMouseEnter: () => setSel(i),
        onClick: () => run(i)
      }, c.dot ? /*#__PURE__*/React.createElement("span", {
        className: "dot " + c.dot
      }) : /*#__PURE__*/React.createElement(Icon, {
        name: c.icon || "enter",
        size: 18,
        className: "ico"
      }), /*#__PURE__*/React.createElement("span", null, c.label), /*#__PURE__*/React.createElement("span", {
        className: "sub"
      }, c.sub || (i === sel ? "↵" : "")));
    }))))));
  }

  // ---------- toasts ----------
  function Toasts({
    items
  }) {
    return /*#__PURE__*/React.createElement("div", {
      className: "toasts"
    }, items.map(t => /*#__PURE__*/React.createElement("div", {
      key: t.id,
      className: "toast"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: t.icon || "check",
      size: 18
    }), /*#__PURE__*/React.createElement("span", null, t.msg))));
  }
  Object.assign(window, {
    StatusDot,
    Sparkline,
    TimeSeries,
    BandwidthGauge,
    RadialGauge,
    HealthDonut,
    RTTBars,
    Sidebar,
    TopBar,
    CommandPalette,
    Toasts,
    NAV,
    STATE_LABEL,
    metricColor
  });
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "src/js/03-ff8ac535.jsx", error: String((e && e.message) || e) }); }

// src/js/04-b4da7cd4.jsx
try { (() => {
/* ============================================================
   SolariNet — screens: FleetOverview, NodeDetail, AlertsScreen
   ============================================================ */
(function () {
  const {
    useState,
    useMemo
  } = React;
  const Icon = window.Icon;
  const {
    StatusDot,
    Sparkline,
    TimeSeries,
    BandwidthGauge,
    RadialGauge,
    HealthDonut,
    RTTBars,
    metricColor
  } = window;
  const S = window.SOLARI;
  const fmt = S.fmt;
  const ROLE_ICON = {
    server: "server",
    monitor: "monitor",
    client: "host"
  };
  const STATE_COLOR = {
    up: "var(--ok)",
    degraded: "var(--warn)",
    down: "var(--crit)",
    unknown: "var(--unknown)"
  };

  /* ===================== FLEET OVERVIEW ===================== */
  function FleetOverview({
    onOpenNode,
    view,
    setView,
    fleet
  }) {
    const [stateFilter, setStateFilter] = useState("all");
    const [roleFilter, setRoleFilter] = useState("all");
    const [dense, setDense] = useState(true);
    const [sort, setSort] = useState({
      key: "name",
      dir: 1
    });
    const filtered = useMemo(() => fleet.filter(n => (stateFilter === "all" || n.state === stateFilter) && (roleFilter === "all" || n.role === roleFilter)), [fleet, stateFilter, roleFilter]);
    const roll = S.fleetRoll;
    const avgCpu = Math.round(fleet.filter(n => n.state !== "down").reduce((a, n) => a + n.cpuPct, 0) / Math.max(1, fleet.filter(n => n.state !== "down").length));
    const monsUp = fleet.filter(n => n.role === "monitor" && n.state === "up").length;
    const monsTotal = fleet.filter(n => n.role === "monitor").length;
    return /*#__PURE__*/React.createElement("div", {
      className: "page"
    }, /*#__PURE__*/React.createElement("div", {
      className: "page-head"
    }, /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("h1", {
      className: "page-title"
    }, "Fleet Overview"), /*#__PURE__*/React.createElement("div", {
      className: "page-sub"
    }, roll.total, " systems \xB7 ", S.summary.applications, " applications \xB7 ", S.segments.length, " segments \xB7 live")), /*#__PURE__*/React.createElement("div", {
      className: "page-head__right"
    }, /*#__PURE__*/React.createElement("div", {
      className: "seg"
    }, /*#__PURE__*/React.createElement("button", {
      className: view === "heat" ? "on" : "",
      onClick: () => setView("heat")
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "grid",
      size: 14
    }), "Heatmap"), /*#__PURE__*/React.createElement("button", {
      className: view === "table" ? "on" : "",
      onClick: () => setView("table")
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "table",
      size: 14
    }), "Table"), /*#__PURE__*/React.createElement("button", {
      className: view === "cards" ? "on" : "",
      onClick: () => setView("cards")
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "cards",
      size: 14
    }), "Cards")))), /*#__PURE__*/React.createElement("div", {
      className: "kpis"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Systems"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v"
    }, roll.total), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, "monitored hosts"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__bar"
    })), /*#__PURE__*/React.createElement("div", {
      className: "kpi ok"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Operational"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v"
    }, roll.up), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, Math.round(roll.up / roll.total * 100), "% healthy"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__bar"
    })), /*#__PURE__*/React.createElement("div", {
      className: "kpi warn"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Degraded"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v"
    }, roll.degraded), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, "over tolerance"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__bar"
    })), /*#__PURE__*/React.createElement("div", {
      className: "kpi crit"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Down"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v"
    }, roll.down), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, S.activeCrit, " critical alerts"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__bar"
    }))), /*#__PURE__*/React.createElement("div", {
      className: "kpis-sec"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Avg CPU"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v",
      style: {
        color: avgCpu >= 75 ? metricColor(avgCpu) : "var(--ink)"
      }
    }, avgCpu, "%"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, "across live hosts")), /*#__PURE__*/React.createElement("div", {
      className: "kpi"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Monitors"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v"
    }, monsUp, /*#__PURE__*/React.createElement("span", {
      style: {
        fontSize: 16,
        color: "var(--ink-faint)"
      }
    }, "/", monsTotal)), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, "vantages online"))), /*#__PURE__*/React.createElement("div", {
      className: "filters"
    }, [["all", "All", roll.total], ["up", "Up", roll.up], ["degraded", "Degraded", roll.degraded], ["down", "Down", roll.down], ["unknown", "Unknown", roll.unknown]].map(([k, lbl, n]) => /*#__PURE__*/React.createElement("button", {
      key: k,
      className: "chip" + (stateFilter === k ? " on" : ""),
      onClick: () => setStateFilter(k)
    }, k !== "all" && /*#__PURE__*/React.createElement("span", {
      className: "dot " + k
    }), lbl, /*#__PURE__*/React.createElement("span", {
      className: "chip__n"
    }, n))), /*#__PURE__*/React.createElement("div", {
      style: {
        width: 1,
        height: 24,
        background: "var(--line)",
        margin: "0 4px"
      }
    }), [["all", "All roles"], ["client", "Clients"], ["monitor", "Monitors"], ["server", "Servers"]].map(([k, lbl]) => /*#__PURE__*/React.createElement("button", {
      key: k,
      className: "chip" + (roleFilter === k ? " on" : ""),
      onClick: () => setRoleFilter(k)
    }, k !== "all" && /*#__PURE__*/React.createElement(Icon, {
      name: ROLE_ICON[k],
      size: 14
    }), lbl)), view === "heat" && /*#__PURE__*/React.createElement("button", {
      className: "chip",
      style: {
        marginLeft: "auto"
      },
      onClick: () => setDense(d => !d)
    }, /*#__PURE__*/React.createElement(Icon, {
      name: dense ? "cards" : "grid",
      size: 14
    }), dense ? "Compact" : "Detailed")), view === "heat" && /*#__PURE__*/React.createElement(HeatView, {
      nodes: filtered,
      dense: dense,
      onOpenNode: onOpenNode
    }), view === "table" && /*#__PURE__*/React.createElement(TableView, {
      nodes: filtered,
      sort: sort,
      setSort: setSort,
      onOpenNode: onOpenNode
    }), view === "cards" && /*#__PURE__*/React.createElement(CardsView, {
      nodes: filtered,
      onOpenNode: onOpenNode
    }));
  }
  function Cell({
    n,
    dense,
    onOpenNode
  }) {
    const load = n.state === "down" ? 0 : n.cpuPct;
    return /*#__PURE__*/React.createElement("div", {
      className: "cell " + n.state + (dense ? "" : " cozy-cell"),
      onClick: () => onOpenNode(n),
      title: `${n.hostFqdn} — ${n.state}`
    }, /*#__PURE__*/React.createElement("div", {
      className: "cell__top"
    }, /*#__PURE__*/React.createElement("span", {
      className: "cell__name"
    }, n.name), /*#__PURE__*/React.createElement("span", {
      className: "cell__dot",
      style: {
        background: STATE_COLOR[n.state],
        boxShadow: n.state !== "unknown" ? `0 0 6px ${STATE_COLOR[n.state]}` : "none"
      }
    })), /*#__PURE__*/React.createElement("div", {
      className: "cell__meta"
    }, n.role === "client" ? n.ip : n.role.toUpperCase()), n.alertsCount > 0 && /*#__PURE__*/React.createElement("span", {
      className: "cell__badge"
    }, n.alertsCount), /*#__PURE__*/React.createElement("div", {
      className: "cell__spark"
    }, /*#__PURE__*/React.createElement(Sparkline, {
      data: n.hist.cpu,
      color: STATE_COLOR[n.state],
      h: dense ? 20 : 26,
      fill: true,
      strokeW: 1.5
    })), /*#__PURE__*/React.createElement("div", {
      className: "cell__load"
    }, /*#__PURE__*/React.createElement("i", {
      style: {
        width: load + "%",
        background: metricColor(load),
        boxShadow: `0 0 5px ${metricColor(load)}`
      }
    })));
  }
  function HeatView({
    nodes,
    dense,
    onOpenNode
  }) {
    return /*#__PURE__*/React.createElement("div", null, S.segments.map(seg => {
      const segNodes = nodes.filter(n => n.segId === seg.id);
      if (!segNodes.length) return null;
      const roll = {
        up: 0,
        degraded: 0,
        down: 0,
        unknown: 0
      };
      segNodes.forEach(n => roll[n.state]++);
      return /*#__PURE__*/React.createElement("div", {
        className: "segment-block",
        key: seg.id
      }, /*#__PURE__*/React.createElement("div", {
        className: "segment-head"
      }, /*#__PURE__*/React.createElement("h3", null, seg.name), /*#__PURE__*/React.createElement("span", {
        className: "cidr"
      }, seg.cidr), /*#__PURE__*/React.createElement("span", {
        className: "rule"
      }), /*#__PURE__*/React.createElement("div", {
        className: "roll"
      }, roll.down > 0 && /*#__PURE__*/React.createElement("span", {
        className: "roll-pip"
      }, /*#__PURE__*/React.createElement("span", {
        className: "dot down"
      }), roll.down), roll.degraded > 0 && /*#__PURE__*/React.createElement("span", {
        className: "roll-pip"
      }, /*#__PURE__*/React.createElement("span", {
        className: "dot degraded"
      }), roll.degraded), /*#__PURE__*/React.createElement("span", {
        className: "roll-pip"
      }, /*#__PURE__*/React.createElement("span", {
        className: "dot up"
      }), roll.up))), /*#__PURE__*/React.createElement("div", {
        className: "heat" + (dense ? "" : " cozy")
      }, segNodes.map(n => /*#__PURE__*/React.createElement(Cell, {
        key: n.nodeId,
        n: n,
        dense: dense,
        onOpenNode: onOpenNode
      }))));
    }));
  }
  function TableView({
    nodes,
    sort,
    setSort,
    onOpenNode
  }) {
    const sorted = useMemo(() => {
      const arr = [...nodes];
      const k = sort.key;
      arr.sort((a, b) => {
        let va, vb;
        if (k === "state") {
          const ord = {
            down: 0,
            degraded: 1,
            unknown: 2,
            up: 3
          };
          va = ord[a.state];
          vb = ord[b.state];
        } else if (k === "name") {
          va = a.name;
          vb = b.name;
        } else if (k === "seg") {
          va = a.segName;
          vb = b.segName;
        } else if (k === "role") {
          va = a.role;
          vb = b.role;
        } else va = a[k], vb = b[k];
        if (va < vb) return -1 * sort.dir;
        if (va > vb) return 1 * sort.dir;
        return 0;
      });
      return arr;
    }, [nodes, sort]);
    function Th({
      k,
      children,
      num
    }) {
      const on = sort.key === k;
      return /*#__PURE__*/React.createElement("th", {
        onClick: () => setSort(s => ({
          key: k,
          dir: s.key === k ? -s.dir : 1
        })),
        style: {
          textAlign: num ? "right" : "left"
        }
      }, children, on && /*#__PURE__*/React.createElement("span", {
        className: "arr"
      }, sort.dir === 1 ? "▲" : "▼"));
    }
    return /*#__PURE__*/React.createElement("div", {
      className: "tablewrap"
    }, /*#__PURE__*/React.createElement("table", {
      className: "grid"
    }, /*#__PURE__*/React.createElement("thead", null, /*#__PURE__*/React.createElement("tr", null, /*#__PURE__*/React.createElement(Th, {
      k: "state"
    }, "State"), /*#__PURE__*/React.createElement(Th, {
      k: "name"
    }, "Host"), /*#__PURE__*/React.createElement(Th, {
      k: "role"
    }, "Role"), /*#__PURE__*/React.createElement(Th, {
      k: "seg"
    }, "Segment"), /*#__PURE__*/React.createElement(Th, {
      k: "cpuPct",
      num: true
    }, "CPU"), /*#__PURE__*/React.createElement(Th, {
      k: "ramPct",
      num: true
    }, "RAM"), /*#__PURE__*/React.createElement(Th, {
      k: "diskMaxPct",
      num: true
    }, "Disk"), /*#__PURE__*/React.createElement(Th, {
      k: "netTotalMbps",
      num: true
    }, "Net"), /*#__PURE__*/React.createElement(Th, {
      k: "lastSeenMin",
      num: true
    }, "Seen"), /*#__PURE__*/React.createElement(Th, {
      k: "alertsCount",
      num: true
    }, "Alerts"))), /*#__PURE__*/React.createElement("tbody", null, sorted.map(n => /*#__PURE__*/React.createElement("tr", {
      key: n.nodeId,
      onClick: () => onOpenNode(n)
    }, /*#__PURE__*/React.createElement("td", null, /*#__PURE__*/React.createElement("span", {
      style: {
        display: "inline-flex",
        alignItems: "center",
        gap: 7
      }
    }, /*#__PURE__*/React.createElement(StatusDot, {
      state: n.state
    }), /*#__PURE__*/React.createElement("span", {
      className: "td-mono",
      style: {
        fontSize: 11.5,
        color: n.state === "up" ? "var(--ink-dim)" : STATE_COLOR[n.state]
      }
    }, n.state))), /*#__PURE__*/React.createElement("td", null, /*#__PURE__*/React.createElement("div", {
      className: "td-host"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: ROLE_ICON[n.role],
      size: 15,
      className: "ico"
    }), n.name, /*#__PURE__*/React.createElement("span", {
      className: "td-mono",
      style: {
        fontSize: 10,
        color: "var(--ink-faint)"
      }
    }, ".akoria.net"))), /*#__PURE__*/React.createElement("td", null, /*#__PURE__*/React.createElement("span", {
      className: "tag"
    }, n.role)), /*#__PURE__*/React.createElement("td", {
      className: "td-mono"
    }, n.segName, " ", /*#__PURE__*/React.createElement("span", {
      style: {
        color: "var(--ink-faint)"
      }
    }, n.ip)), /*#__PURE__*/React.createElement("td", {
      style: {
        textAlign: "right"
      }
    }, /*#__PURE__*/React.createElement(Bar, {
      pct: n.cpuPct
    })), /*#__PURE__*/React.createElement("td", {
      style: {
        textAlign: "right"
      }
    }, /*#__PURE__*/React.createElement(Bar, {
      pct: n.ramPct
    })), /*#__PURE__*/React.createElement("td", {
      style: {
        textAlign: "right"
      }
    }, /*#__PURE__*/React.createElement(Bar, {
      pct: n.diskMaxPct
    })), /*#__PURE__*/React.createElement("td", {
      className: "td-mono",
      style: {
        textAlign: "right"
      }
    }, n.state === "down" ? "—" : fmt.mbps(n.netTotalMbps)), /*#__PURE__*/React.createElement("td", {
      className: "td-mono",
      style: {
        textAlign: "right",
        color: n.lastSeenMin > 2 ? "var(--crit)" : "var(--ink-dim)"
      }
    }, fmt.ago(n.lastSeenMin)), /*#__PURE__*/React.createElement("td", {
      style: {
        textAlign: "right"
      }
    }, n.alertsCount > 0 ? /*#__PURE__*/React.createElement("span", {
      className: "cell__badge",
      style: {
        position: "static",
        display: "inline-flex"
      }
    }, n.alertsCount) : /*#__PURE__*/React.createElement("span", {
      className: "td-mono",
      style: {
        color: "var(--ink-faint)"
      }
    }, "0")))))));
  }
  function Bar({
    pct
  }) {
    const c = metricColor(pct);
    return /*#__PURE__*/React.createElement("span", {
      style: {
        display: "inline-flex",
        alignItems: "center",
        gap: 8,
        justifyContent: "flex-end"
      }
    }, /*#__PURE__*/React.createElement("span", {
      className: "td-mono",
      style: {
        width: 34,
        textAlign: "right",
        color: c
      }
    }, pct, "%"), /*#__PURE__*/React.createElement("span", {
      className: "metricbar"
    }, /*#__PURE__*/React.createElement("i", {
      style: {
        width: pct + "%",
        background: c
      }
    })));
  }
  function CardsView({
    nodes,
    onOpenNode
  }) {
    return /*#__PURE__*/React.createElement("div", {
      className: "cards"
    }, S.segments.map(seg => {
      const segNodes = nodes.filter(n => n.segId === seg.id);
      if (!segNodes.length) return null;
      const roll = {
        total: segNodes.length,
        up: 0,
        degraded: 0,
        down: 0,
        unknown: 0
      };
      segNodes.forEach(n => roll[n.state]++);
      const avgCpu = Math.round(segNodes.reduce((a, n) => a + n.cpuPct, 0) / segNodes.length);
      const avgRam = Math.round(segNodes.reduce((a, n) => a + n.ramPct, 0) / segNodes.length);
      return /*#__PURE__*/React.createElement("div", {
        className: "scard",
        key: seg.id
      }, /*#__PURE__*/React.createElement("div", {
        className: "scard__head"
      }, /*#__PURE__*/React.createElement(Icon, {
        name: "arch",
        size: 16,
        style: {
          color: "var(--accent)"
        }
      }), /*#__PURE__*/React.createElement("span", {
        className: "scard__title"
      }, seg.name), /*#__PURE__*/React.createElement("span", {
        className: "scard__cidr",
        style: {
          marginLeft: "auto"
        }
      }, seg.cidr)), /*#__PURE__*/React.createElement("div", {
        className: "scard__body"
      }, /*#__PURE__*/React.createElement("div", {
        className: "donut-row"
      }, /*#__PURE__*/React.createElement(HealthDonut, {
        roll: roll
      }), /*#__PURE__*/React.createElement("div", {
        style: {
          flex: 1
        }
      }, /*#__PURE__*/React.createElement("div", {
        className: "scard__stats"
      }, /*#__PURE__*/React.createElement("div", {
        className: "scard__stat"
      }, /*#__PURE__*/React.createElement("div", {
        className: "k"
      }, "Up"), /*#__PURE__*/React.createElement("div", {
        className: "v",
        style: {
          color: "var(--ok)"
        }
      }, roll.up)), /*#__PURE__*/React.createElement("div", {
        className: "scard__stat"
      }, /*#__PURE__*/React.createElement("div", {
        className: "k"
      }, "Issues"), /*#__PURE__*/React.createElement("div", {
        className: "v",
        style: {
          color: roll.down ? "var(--crit)" : "var(--warn)"
        }
      }, roll.down + roll.degraded)), /*#__PURE__*/React.createElement("div", {
        className: "scard__stat"
      }, /*#__PURE__*/React.createElement("div", {
        className: "k"
      }, "Avg CPU"), /*#__PURE__*/React.createElement("div", {
        className: "v",
        style: {
          color: metricColor(avgCpu)
        }
      }, avgCpu, "%")), /*#__PURE__*/React.createElement("div", {
        className: "scard__stat"
      }, /*#__PURE__*/React.createElement("div", {
        className: "k"
      }, "Avg RAM"), /*#__PURE__*/React.createElement("div", {
        className: "v",
        style: {
          color: metricColor(avgRam)
        }
      }, avgRam, "%"))))), /*#__PURE__*/React.createElement("div", {
        className: "minigrid"
      }, segNodes.map(n => /*#__PURE__*/React.createElement("div", {
        key: n.nodeId,
        className: "minicell",
        title: `${n.name} — ${n.state}`,
        onClick: () => onOpenNode(n),
        style: {
          background: STATE_COLOR[n.state],
          boxShadow: n.state === "down" ? "0 0 7px var(--crit)" : "none",
          opacity: n.state === "unknown" ? 0.5 : 1
        }
      }))), /*#__PURE__*/React.createElement("div", {
        style: {
          fontFamily: "var(--mono)",
          fontSize: 10.5,
          color: "var(--ink-faint)",
          marginTop: 10
        }
      }, seg.desc)));
    }));
  }

  /* ===================== NODE DETAIL ===================== */
  function NodeDetail({
    node,
    onBack,
    onSurvey
  }) {
    const [metric, setMetric] = useState("cpu");
    const n = node;
    const nodeAlerts = S.alerts.filter(a => a.nodeId === n.nodeId && !a.cleared);
    const nodeProbes = S.probes.filter(p => p.hostNode === n.nodeId);
    const metricMap = {
      cpu: {
        data: n.hist.cpu,
        color: "var(--accent)",
        label: "CPU %",
        max: 100
      },
      ram: {
        data: n.hist.ram,
        color: "var(--violet)",
        label: "RAM %",
        max: 100
      },
      net: {
        data: n.hist.net,
        color: "var(--ok)",
        label: "Net Mb/s",
        max: 100
      },
      disk: {
        data: n.hist.disk,
        color: "var(--warn)",
        label: "Disk %",
        max: 100
      }
    };
    const cur = metricMap[metric];
    return /*#__PURE__*/React.createElement("div", {
      className: "page"
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        alignItems: "center",
        gap: 12,
        marginBottom: 18,
        flexWrap: "wrap"
      }
    }, /*#__PURE__*/React.createElement("button", {
      className: "backbtn",
      onClick: onBack
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "chevronLeft",
      size: 16
    }), "Fleet"), /*#__PURE__*/React.createElement("span", {
      className: "td-mono",
      style: {
        color: "var(--ink-faint)",
        fontSize: 12
      }
    }, "/ ", n.segName, " / ", n.hostFqdn), /*#__PURE__*/React.createElement("div", {
      style: {
        marginLeft: "auto",
        display: "flex",
        gap: 9
      }
    }, /*#__PURE__*/React.createElement("button", {
      className: "backbtn",
      onClick: () => onSurvey(n)
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "survey",
      size: 15
    }), "Survey now"), /*#__PURE__*/React.createElement("button", {
      className: "backbtn",
      onClick: () => onSurvey(n, "config")
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "settings",
      size: 15
    }), "Push config"))), /*#__PURE__*/React.createElement("div", {
      className: "node-hero",
      style: {
        marginBottom: 20
      }
    }, /*#__PURE__*/React.createElement("div", {
      className: "node-hero__id"
    }, /*#__PURE__*/React.createElement("div", {
      className: "node-hero__mark"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: ROLE_ICON[n.role],
      size: 26
    })), /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("h1", null, n.name, /*#__PURE__*/React.createElement("span", {
      style: {
        color: "var(--ink-faint)",
        fontWeight: 400
      }
    }, ".akoria.net")), /*#__PURE__*/React.createElement("div", {
      className: "meta"
    }, /*#__PURE__*/React.createElement("span", null, /*#__PURE__*/React.createElement(StatusDot, {
      state: n.state
    }), " ", /*#__PURE__*/React.createElement("span", {
      className: "statetext " + n.state,
      style: {
        fontWeight: 600
      }
    }, window.STATE_LABEL[n.state])), /*#__PURE__*/React.createElement("span", null, n.ip), /*#__PURE__*/React.createElement("span", null, n.role, n.label ? ` · ${n.label}` : ""), /*#__PURE__*/React.createElement("span", null, n.osName, " \xB7 ", n.arch), /*#__PURE__*/React.createElement("span", null, "up ", n.uptimeDays, "d"), /*#__PURE__*/React.createElement("span", {
      style: {
        color: n.lastSeenMin > 2 ? "var(--crit)" : "inherit"
      }
    }, "seen ", fmt.ago(n.lastSeenMin)), /*#__PURE__*/React.createElement("span", null, "cfg epoch ", n.configEpoch, " ", n.converged ? /*#__PURE__*/React.createElement("span", {
      style: {
        color: "var(--ok)"
      }
    }, "\u2713") : /*#__PURE__*/React.createElement("span", {
      style: {
        color: "var(--warn)"
      }
    }, "drift")))))), /*#__PURE__*/React.createElement("div", {
      className: "panel",
      style: {
        marginBottom: 16
      }
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel__body"
    }, /*#__PURE__*/React.createElement("div", {
      className: "gauge-grid"
    }, /*#__PURE__*/React.createElement(RadialGauge, {
      value: n.state === "down" ? 0 : n.cpuPct,
      label: "CPU LOAD",
      sub: `${n.cores.length} cores`
    }), /*#__PURE__*/React.createElement(RadialGauge, {
      value: n.ramPct,
      label: "MEMORY",
      sub: fmt.kb(n.ramTotalKb),
      color: "var(--violet)"
    }), /*#__PURE__*/React.createElement(RadialGauge, {
      value: n.diskMaxPct,
      label: "DISK PEAK",
      sub: `${n.disks.length} vols`
    }), /*#__PURE__*/React.createElement(RadialGauge, {
      value: n.swapPct,
      label: "SWAP",
      sub: fmt.kb(n.swapTotalKb)
    }), /*#__PURE__*/React.createElement("div", {
      className: "gauge",
      style: {
        width: 116
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        height: 116,
        display: "grid",
        placeItems: "center"
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        textAlign: "center"
      }
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "network",
      size: 26,
      style: {
        color: "var(--accent)"
      }
    }), /*#__PURE__*/React.createElement("div", {
      style: {
        fontFamily: "var(--mono)",
        fontWeight: 600,
        fontSize: 18,
        marginTop: 6
      }
    }, n.state === "down" ? "—" : fmt.mbps(n.netTotalMbps)))), /*#__PURE__*/React.createElement("div", {
      className: "lab"
    }, "THROUGHPUT"))))), /*#__PURE__*/React.createElement("div", {
      className: "two-col",
      style: {
        marginBottom: 16
      }
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel"
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "activity",
      size: 16
    }), /*#__PURE__*/React.createElement("h3", null, "Trend \u2014 last 15 min"), /*#__PURE__*/React.createElement("div", {
      className: "right"
    }, /*#__PURE__*/React.createElement("div", {
      className: "seg",
      style: {
        transform: "scale(.92)",
        transformOrigin: "right"
      }
    }, ["cpu", "ram", "net", "disk"].map(m => /*#__PURE__*/React.createElement("button", {
      key: m,
      className: metric === m ? "on" : "",
      onClick: () => setMetric(m)
    }, m))))), /*#__PURE__*/React.createElement("div", {
      className: "panel__body"
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        justifyContent: "space-between",
        marginBottom: 8,
        fontFamily: "var(--mono)",
        fontSize: 12
      }
    }, /*#__PURE__*/React.createElement("span", {
      className: "muted",
      style: {
        textTransform: "uppercase",
        letterSpacing: ".08em",
        fontSize: 10
      }
    }, cur.label), /*#__PURE__*/React.createElement("span", {
      style: {
        color: cur.color,
        fontWeight: 600,
        fontSize: 16
      }
    }, cur.data[cur.data.length - 1].toFixed(0), metric === "net" ? "" : "%")), /*#__PURE__*/React.createElement(TimeSeries, {
      data: cur.data,
      color: cur.color,
      max: cur.max,
      h: 170
    }))), /*#__PURE__*/React.createElement("div", {
      className: "panel"
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "bandwidth",
      size: 16
    }), /*#__PURE__*/React.createElement("h3", null, "Interfaces"), /*#__PURE__*/React.createElement("div", {
      className: "right"
    }, n.ifaces.length, " NIC")), /*#__PURE__*/React.createElement("div", {
      className: "panel__body",
      style: {
        display: "flex",
        flexDirection: "column",
        gap: 16
      }
    }, n.ifaces.map((f, i) => /*#__PURE__*/React.createElement("div", {
      key: i
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        justifyContent: "space-between",
        marginBottom: 8
      }
    }, /*#__PURE__*/React.createElement("span", {
      className: "td-mono",
      style: {
        fontWeight: 600
      }
    }, f.name, " ", /*#__PURE__*/React.createElement("span", {
      className: "tag"
    }, fmt.mbps(f.capMbps))), f.errs > 10 && /*#__PURE__*/React.createElement("span", {
      className: "td-mono",
      style: {
        color: "var(--warn)"
      }
    }, f.errs, " err/s")), /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        flexDirection: "column",
        gap: 8
      }
    }, /*#__PURE__*/React.createElement(BandwidthGauge, {
      label: "RX",
      used: f.rxMbps,
      cap: f.capMbps,
      unit: fmt.mbps
    }), /*#__PURE__*/React.createElement(BandwidthGauge, {
      label: "TX",
      used: f.txMbps,
      cap: f.capMbps,
      unit: fmt.mbps,
      color: "var(--violet)"
    }))))))), /*#__PURE__*/React.createElement("div", {
      className: "two-col",
      style: {
        marginBottom: 16
      }
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel"
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "cpu",
      size: 16
    }), /*#__PURE__*/React.createElement("h3", null, "Per-core utilisation"), /*#__PURE__*/React.createElement("div", {
      className: "right"
    }, n.cores.length, " \xD7 ", n.arch)), /*#__PURE__*/React.createElement("div", {
      className: "panel__body"
    }, /*#__PURE__*/React.createElement("div", {
      className: "cores"
    }, n.cores.map((c, i) => /*#__PURE__*/React.createElement("div", {
      className: "core",
      key: i
    }, /*#__PURE__*/React.createElement("div", {
      className: "n"
    }, "c", i), /*#__PURE__*/React.createElement("div", {
      className: "bar"
    }, /*#__PURE__*/React.createElement("i", {
      style: {
        height: c + "%",
        background: metricColor(c),
        boxShadow: `0 0 5px ${metricColor(c)}`
      }
    })), /*#__PURE__*/React.createElement("div", {
      className: "pct",
      style: {
        color: metricColor(c)
      }
    }, c)))))), /*#__PURE__*/React.createElement("div", {
      className: "panel"
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "disk",
      size: 16
    }), /*#__PURE__*/React.createElement("h3", null, "Storage")), /*#__PURE__*/React.createElement("div", {
      className: "panel__body"
    }, n.disks.map((d, i) => /*#__PURE__*/React.createElement("div", {
      className: "metric-row",
      key: i
    }, /*#__PURE__*/React.createElement("span", {
      className: "lbl"
    }, d.mount), /*#__PURE__*/React.createElement("span", {
      className: "track"
    }, /*#__PURE__*/React.createElement("i", {
      style: {
        width: d.usedPct + "%",
        background: metricColor(d.usedPct)
      }
    })), /*#__PURE__*/React.createElement("span", {
      className: "val",
      style: {
        color: metricColor(d.usedPct)
      }
    }, d.usedPct, "% ", /*#__PURE__*/React.createElement("span", {
      className: "muted",
      style: {
        fontSize: 11
      }
    }, d.totalGb, "G ", d.fs)))), /*#__PURE__*/React.createElement("div", {
      className: "metric-row"
    }, /*#__PURE__*/React.createElement("span", {
      className: "lbl"
    }, "Memory"), /*#__PURE__*/React.createElement("span", {
      className: "track"
    }, /*#__PURE__*/React.createElement("i", {
      style: {
        width: n.ramPct + "%",
        background: "var(--violet)"
      }
    })), /*#__PURE__*/React.createElement("span", {
      className: "val",
      style: {
        color: "var(--violet)"
      }
    }, fmt.kb(n.ramUsedKb))), /*#__PURE__*/React.createElement("div", {
      className: "metric-row"
    }, /*#__PURE__*/React.createElement("span", {
      className: "lbl"
    }, "Swap"), /*#__PURE__*/React.createElement("span", {
      className: "track"
    }, /*#__PURE__*/React.createElement("i", {
      style: {
        width: n.swapPct + "%",
        background: metricColor(n.swapPct)
      }
    })), /*#__PURE__*/React.createElement("span", {
      className: "val"
    }, n.swapPct, "%"))))), /*#__PURE__*/React.createElement("div", {
      className: "two-col",
      style: {
        marginBottom: 16
      }
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel"
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "process",
      size: 16
    }), /*#__PURE__*/React.createElement("h3", null, "Watched processes"), /*#__PURE__*/React.createElement("div", {
      className: "right"
    }, n.procs.length)), /*#__PURE__*/React.createElement("div", {
      className: "panel__body",
      style: {
        padding: 0
      }
    }, /*#__PURE__*/React.createElement("table", {
      className: "proc-table"
    }, /*#__PURE__*/React.createElement("thead", null, /*#__PURE__*/React.createElement("tr", null, /*#__PURE__*/React.createElement("th", null, "Process"), /*#__PURE__*/React.createElement("th", null, "PID"), /*#__PURE__*/React.createElement("th", null, "St"), /*#__PURE__*/React.createElement("th", null, "Files"), /*#__PURE__*/React.createElement("th", null, "Sock"), /*#__PURE__*/React.createElement("th", null, "RSS"))), /*#__PURE__*/React.createElement("tbody", null, n.procs.map((p, i) => /*#__PURE__*/React.createElement("tr", {
      key: i
    }, /*#__PURE__*/React.createElement("td", {
      style: {
        fontWeight: 600
      }
    }, p.name), /*#__PURE__*/React.createElement("td", {
      className: "muted"
    }, p.runState === "Z" ? "—" : p.pid), /*#__PURE__*/React.createElement("td", null, /*#__PURE__*/React.createElement("span", {
      style: {
        color: p.runState === "R" ? "var(--ok)" : p.runState === "Z" ? "var(--crit)" : "var(--warn)"
      }
    }, p.runState)), /*#__PURE__*/React.createElement("td", {
      className: "muted"
    }, p.runState === "Z" ? "—" : p.nFiles), /*#__PURE__*/React.createElement("td", {
      className: "muted"
    }, p.runState === "Z" ? "—" : p.nSockets), /*#__PURE__*/React.createElement("td", {
      className: "muted"
    }, p.runState === "Z" ? "—" : fmt.kb(p.rssKb)))))))), /*#__PURE__*/React.createElement("div", {
      className: "panel"
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "reachability",
      size: 16
    }), /*#__PURE__*/React.createElement("h3", null, "Reachability & alerts")), /*#__PURE__*/React.createElement("div", {
      className: "panel__body"
    }, nodeProbes.length > 0 ? nodeProbes.map(p => /*#__PURE__*/React.createElement("div", {
      key: p.targetId,
      style: {
        marginBottom: 16
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        justifyContent: "space-between",
        marginBottom: 8
      }
    }, /*#__PURE__*/React.createElement("span", {
      className: "td-mono",
      style: {
        fontWeight: 600
      }
    }, /*#__PURE__*/React.createElement(StatusDot, {
      state: p.state,
      size: 8
    }), " ", p.targetId), /*#__PURE__*/React.createElement("span", {
      className: "tag"
    }, p.label)), /*#__PURE__*/React.createElement(RTTBars, {
      vantages: p.vantages,
      fmt: fmt
    }))) : /*#__PURE__*/React.createElement("div", {
      className: "td-mono muted",
      style: {
        fontSize: 12,
        marginBottom: 14
      }
    }, "No probe targets assigned to this host."), /*#__PURE__*/React.createElement("div", {
      className: "divider"
    }), nodeAlerts.length ? nodeAlerts.map(a => /*#__PURE__*/React.createElement("div", {
      key: a.eventId,
      className: "alert-row " + a.severity,
      style: {
        marginBottom: 8
      }
    }, /*#__PURE__*/React.createElement("span", {
      className: "alert-sev " + a.severity
    }, a.severity), /*#__PURE__*/React.createElement("div", {
      className: "alert-main"
    }, /*#__PURE__*/React.createElement("div", {
      className: "t"
    }, a.ruleName), /*#__PURE__*/React.createElement("div", {
      className: "d"
    }, a.detail)), /*#__PURE__*/React.createElement("div", {
      className: "alert-meta"
    }, fmt.ago(a.firedMinAgo)))) : /*#__PURE__*/React.createElement("div", {
      className: "td-mono",
      style: {
        color: "var(--ok)",
        fontSize: 12
      }
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "check",
      size: 14,
      style: {
        verticalAlign: "-2px"
      }
    }), " No active alerts on this node.")))));
  }

  /* ===================== ALERTS + RULES ===================== */
  function AlertsScreen({
    onOpenNode,
    rules,
    setRules,
    toast
  }) {
    const [tab, setTab] = useState("active");
    const list = S.alerts.filter(a => tab === "all" ? true : !a.cleared);
    const crit = S.alerts.filter(a => !a.cleared && a.severity === "crit").length;
    const warn = S.alerts.filter(a => !a.cleared && a.severity === "warn").length;
    const info = S.alerts.filter(a => !a.cleared && a.severity === "info").length;
    function setThreshold(id, val) {
      setRules(rs => rs.map(r => r.ruleId === id ? {
        ...r,
        threshold: val
      } : r));
    }
    function toggleRule(id) {
      setRules(rs => rs.map(r => r.ruleId === id ? {
        ...r,
        enabled: !r.enabled
      } : r));
      const r = rules.find(x => x.ruleId === id);
      toast(`Rule "${r.name}" ${r.enabled ? "disabled" : "enabled"}`, r.enabled ? "close" : "check");
    }
    return /*#__PURE__*/React.createElement("div", {
      className: "page"
    }, /*#__PURE__*/React.createElement("div", {
      className: "page-head"
    }, /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("h1", {
      className: "page-title"
    }, "Alerts & Tolerances"), /*#__PURE__*/React.createElement("div", {
      className: "page-sub"
    }, crit + warn + info, " active \xB7 ", rules.filter(r => r.enabled).length, "/", rules.length, " rules armed")), /*#__PURE__*/React.createElement("div", {
      className: "page-head__right"
    }, /*#__PURE__*/React.createElement("div", {
      className: "seg"
    }, /*#__PURE__*/React.createElement("button", {
      className: tab === "active" ? "on" : "",
      onClick: () => setTab("active")
    }, "Active"), /*#__PURE__*/React.createElement("button", {
      className: tab === "all" ? "on" : "",
      onClick: () => setTab("all")
    }, "All")))), /*#__PURE__*/React.createElement("div", {
      className: "kpis",
      style: {
        gridTemplateColumns: "repeat(3, minmax(0, 1fr))"
      }
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi crit"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Critical"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v"
    }, crit), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, "need attention"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__bar"
    })), /*#__PURE__*/React.createElement("div", {
      className: "kpi warn"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Warning"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v"
    }, warn), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, "over tolerance"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__bar"
    })), /*#__PURE__*/React.createElement("div", {
      className: "kpi"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Info"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v"
    }, info), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, "advisory"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__bar"
    }))), /*#__PURE__*/React.createElement("div", {
      className: "two-col",
      style: {
        alignItems: "start"
      }
    }, /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("div", {
      className: "page-sub",
      style: {
        marginBottom: 12
      }
    }, tab === "active" ? "Active events" : "All events (incl. cleared)"), list.length === 0 && /*#__PURE__*/React.createElement("div", {
      className: "empty"
    }, "No alerts \u2014 all systems nominal."), list.map(a => /*#__PURE__*/React.createElement("div", {
      key: a.eventId,
      className: "alert-row " + a.severity + (a.cleared ? " cleared" : ""),
      onClick: () => a.nodeId && onOpenNode(a.nodeId)
    }, /*#__PURE__*/React.createElement("span", {
      className: "alert-sev " + a.severity
    }, a.severity), /*#__PURE__*/React.createElement("div", {
      className: "alert-main"
    }, /*#__PURE__*/React.createElement("div", {
      className: "t"
    }, a.ruleName), /*#__PURE__*/React.createElement("div", {
      className: "d"
    }, a.detail)), a.node && /*#__PURE__*/React.createElement("div", {
      className: "alert-node"
    }, a.node, /*#__PURE__*/React.createElement("div", {
      style: {
        color: "var(--ink-faint)",
        fontSize: 10
      }
    }, a.segName)), /*#__PURE__*/React.createElement("div", {
      className: "alert-meta"
    }, a.cleared ? "cleared" : "", /*#__PURE__*/React.createElement("div", null, fmt.ago(a.firedMinAgo)))))), /*#__PURE__*/React.createElement("div", {
      className: "panel"
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "settings",
      size: 16
    }), /*#__PURE__*/React.createElement("h3", null, "Alert rules & tolerances")), /*#__PURE__*/React.createElement("div", {
      className: "panel__body",
      style: {
        padding: 0
      }
    }, rules.map(r => /*#__PURE__*/React.createElement("div", {
      key: r.ruleId,
      style: {
        padding: "14px 16px",
        borderBottom: "1px solid var(--line)",
        opacity: r.enabled ? 1 : 0.55
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        alignItems: "center",
        gap: 12
      }
    }, /*#__PURE__*/React.createElement("span", {
      className: "alert-sev " + r.severity
    }, r.severity), /*#__PURE__*/React.createElement("div", {
      style: {
        flex: 1
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        fontWeight: 600,
        fontSize: 13
      }
    }, r.name), /*#__PURE__*/React.createElement("div", {
      className: "rule-cond"
    }, r.metric, " ", /*#__PURE__*/React.createElement("b", null, {
      gt: ">",
      lt: "<",
      eq: "=",
      transition: "Δ"
    }[r.op]), " ", r.op !== "transition" ? `${r.threshold}${r.unit}` : "change", r.forSeconds ? ` for ${r.forSeconds}s` : "")), /*#__PURE__*/React.createElement("span", {
      className: "tag"
    }, r.scope), /*#__PURE__*/React.createElement("button", {
      className: "switch" + (r.enabled ? " on" : ""),
      onClick: () => toggleRule(r.ruleId),
      "aria-label": "toggle rule"
    }, /*#__PURE__*/React.createElement("i", null))), r.op !== "transition" && r.enabled && /*#__PURE__*/React.createElement("div", {
      className: "thr",
      style: {
        marginTop: 12
      }
    }, /*#__PURE__*/React.createElement("input", {
      type: "range",
      min: r.metric === "rttMicros" ? 1000 : 0,
      max: r.metric === "rttMicros" ? 60000 : r.unit === "s" ? 300 : 100,
      step: r.metric === "rttMicros" ? 1000 : 1,
      value: r.threshold,
      onChange: e => setThreshold(r.ruleId, +e.target.value)
    }), /*#__PURE__*/React.createElement("span", {
      className: "num"
    }, r.threshold, r.unit))))))));
  }
  Object.assign(window, {
    FleetOverview,
    NodeDetail,
    AlertsScreen
  });
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "src/js/04-b4da7cd4.jsx", error: String((e && e.message) || e) }); }

// src/js/05-d9cca021.jsx
try { (() => {
/* ============================================================
   SolariNet — screens2: Reachability matrix, Topology map
   ============================================================ */
(function () {
  const {
    useState,
    useMemo
  } = React;
  const Icon = window.Icon;
  const {
    StatusDot,
    RTTBars,
    metricColor
  } = window;
  const S = window.SOLARI;
  const fmt = S.fmt;
  const STATE_COLOR = {
    up: "var(--ok)",
    degraded: "var(--warn)",
    down: "var(--crit)",
    unknown: "var(--unknown)"
  };
  const OUTCOME_ABBR = {
    ok: "OK",
    timeout: "T/O",
    refused: "RST",
    unreachable: "UNR",
    dns_fail: "DNS",
    tls_fail: "TLS",
    proto_err: "ERR"
  };
  const GEAR_ICON = {
    gateway: "gateway",
    switch: "netswitch",
    ap: "wifi"
  };
  const GEAR_FILL = {
    gateway: "var(--violet)",
    switch: "var(--accent)",
    ap: "var(--warn)"
  };

  /* ===================== REACHABILITY MATRIX ===================== */
  function Reachability({
    onOpenNode
  }) {
    const [proto, setProto] = useState("all");
    const [stateF, setStateF] = useState("all");
    const [selId, setSelId] = useState(null);
    const probes = S.probes.filter(p => (proto === "all" || p.proto === proto) && (stateF === "all" || p.state === stateF));
    const sel = selId ? S.probes.find(p => p.targetId === selId) : null;
    const monCols = useMemo(() => {
      const map = new Map();
      S.probes.forEach(p => p.vantages.forEach(v => map.set(v.monitorNode, v.monitorName)));
      return [...map.entries()].map(([id, name]) => ({
        id,
        name
      })).sort((a, b) => a.name.localeCompare(b.name));
    }, []);
    const roll = {
      total: S.probes.length,
      up: 0,
      degraded: 0,
      down: 0
    };
    S.probes.forEach(p => roll[p.state]++);
    const rtts = S.probes.flatMap(p => p.vantages.filter(v => v.outcome === "ok").map(v => v.rttMicros));
    const avgRtt = rtts.length ? Math.round(rtts.reduce((a, b) => a + b, 0) / rtts.length) : 0;
    function cellFor(p, monId) {
      const v = p.vantages.find(x => x.monitorNode === monId);
      if (!v) return /*#__PURE__*/React.createElement("td", {
        key: monId,
        className: "mx-cell empty"
      }, /*#__PURE__*/React.createElement("span", {
        className: "mx-empty"
      }));
      const ok = v.outcome === "ok";
      const c = !ok ? "var(--crit)" : v.lossPermille > 0 ? "var(--warn)" : "var(--ok)";
      return /*#__PURE__*/React.createElement("td", {
        key: monId,
        className: "mx-cell"
      }, /*#__PURE__*/React.createElement("div", {
        className: "mx-blk",
        style: {
          background: ok ? `color-mix(in srgb, ${c} 18%, transparent)` : "var(--crit-bg)",
          borderColor: c,
          color: c
        }
      }, /*#__PURE__*/React.createElement("span", {
        className: "mx-rtt"
      }, ok ? fmt.rtt(v.rttMicros) : OUTCOME_ABBR[v.outcome]), ok && v.lossPermille > 0 && /*#__PURE__*/React.createElement("span", {
        className: "mx-loss"
      }, (v.lossPermille / 10).toFixed(0), "% loss")));
    }
    return /*#__PURE__*/React.createElement("div", {
      className: "page"
    }, /*#__PURE__*/React.createElement("div", {
      className: "page-head"
    }, /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("h1", {
      className: "page-title"
    }, "Reachability Matrix"), /*#__PURE__*/React.createElement("div", {
      className: "page-sub"
    }, S.probes.length, " probe targets \xD7 ", monCols.length, " monitor vantages \xB7 HRW-assigned")), /*#__PURE__*/React.createElement("div", {
      className: "page-head__right"
    }, /*#__PURE__*/React.createElement("button", {
      className: "backbtn",
      onClick: () => window.__solariToast && window.__solariToast("Survey dispatched to monitor fleet", "survey")
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "survey",
      size: 15
    }), "Survey now"))), /*#__PURE__*/React.createElement("div", {
      className: "kpis"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Targets"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v"
    }, roll.total), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, "under probe"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__bar"
    })), /*#__PURE__*/React.createElement("div", {
      className: "kpi ok"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Reachable"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v"
    }, roll.up), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, "all vantages OK"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__bar"
    })), /*#__PURE__*/React.createElement("div", {
      className: "kpi warn"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Split vantage"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v"
    }, roll.degraded), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, "partial reachability"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__bar"
    })), /*#__PURE__*/React.createElement("div", {
      className: "kpi crit"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Unreachable"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v"
    }, roll.down), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, "from every vantage"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__bar"
    }))), /*#__PURE__*/React.createElement("div", {
      className: "kpis-sec"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Avg RTT"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v"
    }, fmt.rtt(avgRtt)), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, "across OK probes"))), /*#__PURE__*/React.createElement("div", {
      className: "filters"
    }, [["all", "All protos"], ["tcp", "TCP"], ["udp", "UDP"], ["icmp", "ICMP"]].map(([k, l]) => /*#__PURE__*/React.createElement("button", {
      key: k,
      className: "chip" + (proto === k ? " on" : ""),
      onClick: () => setProto(k)
    }, l)), /*#__PURE__*/React.createElement("div", {
      style: {
        width: 1,
        height: 24,
        background: "var(--line)",
        margin: "0 4px"
      }
    }), [["all", "All"], ["up", "Reachable"], ["degraded", "Split"], ["down", "Down"]].map(([k, l]) => /*#__PURE__*/React.createElement("button", {
      key: k,
      className: "chip" + (stateF === k ? " on" : ""),
      onClick: () => setStateF(k)
    }, k !== "all" && /*#__PURE__*/React.createElement("span", {
      className: "dot " + k
    }), l)), !sel && /*#__PURE__*/React.createElement("span", {
      className: "td-mono muted",
      style: {
        marginLeft: "auto",
        fontSize: 11
      }
    }, "tap a row for per-vantage detail")), /*#__PURE__*/React.createElement("div", {
      style: sel ? {
        display: "grid",
        gridTemplateColumns: "minmax(0, 1.55fr) minmax(0, 1fr)",
        gap: 16,
        alignItems: "start"
      } : undefined
    }, /*#__PURE__*/React.createElement("div", {
      className: "tablewrap",
      style: {
        overflowX: "auto"
      }
    }, /*#__PURE__*/React.createElement("table", {
      className: "mx"
    }, /*#__PURE__*/React.createElement("thead", null, /*#__PURE__*/React.createElement("tr", null, /*#__PURE__*/React.createElement("th", {
      className: "mx-corner"
    }, "Target"), monCols.map(m => /*#__PURE__*/React.createElement("th", {
      key: m.id,
      className: "mx-mon"
    }, /*#__PURE__*/React.createElement("span", null, /*#__PURE__*/React.createElement(Icon, {
      name: "monitor",
      size: 12
    }), m.name))))), /*#__PURE__*/React.createElement("tbody", null, probes.map(p => /*#__PURE__*/React.createElement("tr", {
      key: p.targetId,
      className: (selId === p.targetId ? "sel " : "") + (p.state === "degraded" ? "diverge" : ""),
      onClick: () => setSelId(selId === p.targetId ? null : p.targetId)
    }, /*#__PURE__*/React.createElement("td", {
      className: "mx-target"
    }, /*#__PURE__*/React.createElement(StatusDot, {
      state: p.state,
      size: 8
    }), /*#__PURE__*/React.createElement("div", {
      className: "mx-target__main"
    }, /*#__PURE__*/React.createElement("span", {
      className: "mx-target__id"
    }, p.targetId), /*#__PURE__*/React.createElement("span", {
      className: "mx-target__lbl"
    }, p.label, p.state === "degraded" && /*#__PURE__*/React.createElement("span", {
      className: "diverge-tag"
    }, "split")))), monCols.map(m => cellFor(p, m.id))))))), sel && /*#__PURE__*/React.createElement("div", {
      className: "panel"
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "reachability",
      size: 16
    }), /*#__PURE__*/React.createElement("h3", {
      style: {
        flex: 1,
        minWidth: 0,
        whiteSpace: "nowrap",
        overflow: "hidden",
        textOverflow: "ellipsis"
      }
    }, sel.targetId), /*#__PURE__*/React.createElement("span", {
      className: "tag"
    }, sel.proto), /*#__PURE__*/React.createElement("button", {
      className: "iconbtn",
      style: {
        width: 32,
        height: 32,
        flex: "0 0 32px"
      },
      onClick: () => setSelId(null),
      "aria-label": "Close detail"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "close",
      size: 15
    }))), /*#__PURE__*/React.createElement("div", {
      className: "panel__body"
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        display: "grid",
        gridTemplateColumns: "1fr 1fr",
        gap: "12px 16px",
        marginBottom: 16
      }
    }, /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Service"), /*#__PURE__*/React.createElement("div", {
      className: "td-mono",
      style: {
        fontWeight: 600
      }
    }, sel.label)), /*#__PURE__*/React.createElement("div", {
      style: {
        minWidth: 0
      }
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Host"), /*#__PURE__*/React.createElement("div", {
      className: "td-mono",
      style: {
        whiteSpace: "nowrap",
        overflow: "hidden",
        textOverflow: "ellipsis"
      }
    }, sel.host)), /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Repl factor"), /*#__PURE__*/React.createElement("div", {
      className: "td-mono"
    }, "\xD7", sel.replFactor)), /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "State"), /*#__PURE__*/React.createElement("div", {
      className: "td-mono statetext " + sel.state,
      style: {
        fontWeight: 600
      }
    }, sel.state))), /*#__PURE__*/React.createElement("div", {
      className: "kpi__k",
      style: {
        marginBottom: 8
      }
    }, "RTT per vantage"), /*#__PURE__*/React.createElement(RTTBars, {
      vantages: sel.vantages,
      fmt: fmt
    }), /*#__PURE__*/React.createElement("div", {
      className: "divider"
    }), /*#__PURE__*/React.createElement("table", {
      className: "proc-table",
      style: {
        width: "100%"
      }
    }, /*#__PURE__*/React.createElement("thead", null, /*#__PURE__*/React.createElement("tr", null, /*#__PURE__*/React.createElement("th", null, "Vantage"), /*#__PURE__*/React.createElement("th", null, "Outcome"), /*#__PURE__*/React.createElement("th", null, "RTT"), /*#__PURE__*/React.createElement("th", null, "Jitter"), /*#__PURE__*/React.createElement("th", null, "Loss"), /*#__PURE__*/React.createElement("th", null, "Thrpt"))), /*#__PURE__*/React.createElement("tbody", null, sel.vantages.map((v, i) => /*#__PURE__*/React.createElement("tr", {
      key: i
    }, /*#__PURE__*/React.createElement("td", {
      style: {
        fontWeight: 600
      }
    }, v.monitorName), /*#__PURE__*/React.createElement("td", null, /*#__PURE__*/React.createElement("span", {
      style: {
        color: v.outcome === "ok" ? "var(--ok)" : "var(--crit)"
      }
    }, OUTCOME_ABBR[v.outcome])), /*#__PURE__*/React.createElement("td", {
      className: "muted"
    }, fmt.rtt(v.rttMicros)), /*#__PURE__*/React.createElement("td", {
      className: "muted"
    }, v.outcome === "ok" ? fmt.rtt(v.jitterMicros) : "—"), /*#__PURE__*/React.createElement("td", {
      className: "muted",
      style: {
        color: v.lossPermille > 0 ? "var(--warn)" : "inherit"
      }
    }, v.outcome === "ok" ? (v.lossPermille / 10).toFixed(1) + "%" : "100%"), /*#__PURE__*/React.createElement("td", {
      className: "muted"
    }, v.outcome === "ok" ? (v.throughputKbps / 1000).toFixed(1) + "M" : "—"))))), /*#__PURE__*/React.createElement("button", {
      className: "backbtn",
      style: {
        marginTop: 14
      },
      onClick: () => onOpenNode(sel.hostNode)
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "host",
      size: 14
    }), "Open host ", sel.host.split(".")[0])))));
  }

  /* ===================== TOPOLOGY MAP ===================== */
  function NodeGlyph({
    p
  }) {
    const fill = STATE_COLOR[p.n.state];
    const role = p.n.role;
    if (role === "server") return /*#__PURE__*/React.createElement("rect", {
      x: -p.r,
      y: -p.r,
      width: p.r * 2,
      height: p.r * 2,
      rx: "3",
      fill: fill
    });
    if (role === "monitor") return /*#__PURE__*/React.createElement("rect", {
      x: -p.r,
      y: -p.r,
      width: p.r * 2,
      height: p.r * 2,
      transform: "rotate(45)",
      fill: fill
    });
    return /*#__PURE__*/React.createElement("circle", {
      r: p.r,
      fill: fill
    });
  }
  function Topology({
    onOpenNode
  }) {
    const [view, setView] = useState("infra");
    const [sel, setSel] = useState(null);
    const W = 1000;
    const infra = useMemo(() => {
      const cx = W / 2,
        cy = 360,
        H = 720;
      const pos = {};
      const anchors = [];
      const servers = S.nodes.filter(n => n.role === "server");
      const monitors = S.nodes.filter(n => n.role === "monitor");
      servers.forEach((s, i) => {
        pos[s.nodeId] = {
          x: cx + (i === 0 ? -26 : 26),
          y: cy,
          r: 13,
          n: s
        };
      });
      monitors.forEach((m, i) => {
        const a = i / monitors.length * Math.PI * 2 - Math.PI / 2;
        pos[m.nodeId] = {
          x: cx + Math.cos(a) * 132,
          y: cy + Math.sin(a) * 132,
          r: 8,
          n: m
        };
      });
      S.segments.forEach((seg, si) => {
        const clients = S.nodes.filter(n => n.role === "client" && n.segId === seg.id);
        const baseA = si / S.segments.length * Math.PI * 2 - Math.PI / 2;
        const cols = Math.ceil(Math.sqrt(clients.length * 1.6)) || 1;
        clients.forEach((cn, ci) => {
          const col = ci % cols,
            row = Math.floor(ci / cols);
          const a = baseA + (col / Math.max(1, cols - 1) - 0.5) * 0.52;
          const rad = 235 + row * 26;
          pos[cn.nodeId] = {
            x: cx + Math.cos(a) * rad,
            y: cy + Math.sin(a) * rad * 0.92,
            r: 5,
            n: cn
          };
        });
        anchors.push({
          id: seg.id,
          name: seg.name,
          x: cx + Math.cos(baseA) * 200,
          y: cy + Math.sin(baseA) * 188
        });
      });
      const edges = [];
      monitors.forEach(m => {
        if (pos[m.nodeId] && servers[0]) edges.push({
          from: m.nodeId,
          to: servers[0].nodeId,
          kind: "report",
          label: "Telemetry report",
          sub: "SCP/TLS · PUSH :7701"
        });
      });
      S.probes.forEach(p => p.vantages.forEach(v => {
        if (pos[v.monitorNode] && pos[p.hostNode]) edges.push({
          from: v.monitorNode,
          to: p.hostNode,
          kind: "probe",
          ok: v.outcome === "ok",
          label: `${p.proto.toUpperCase()} probe · ${p.label}`,
          sub: `${v.outcome} · ${fmt.rtt(v.rttMicros)}${v.lossPermille ? ` · ${(v.lossPermille / 10).toFixed(0)}% loss` : ""}`,
          openId: p.hostNode
        });
      }));
      if (servers[1]) edges.push({
        from: servers[0].nodeId,
        to: servers[1].nodeId,
        kind: "lease",
        label: "Failover lease",
        sub: "DB-mediated mutex · TTL 15s"
      });
      return {
        pos,
        edges,
        anchors,
        hub: {
          x: cx,
          y: cy
        },
        H,
        curve: 18
      };
    }, []);
    const lan = useMemo(() => {
      const pos = {};
      const edges = [];
      const gw = S.netgear.find(g => g.kind === "gateway");
      const core = S.netgear.find(g => g.id === "sw-core");
      const others = S.netgear.filter(g => g.id !== gw.id && g.id !== core.id);
      const mid = Math.floor(others.length / 2);
      const gearRow = [...others.slice(0, mid), core, ...others.slice(mid)]; // core centered as spine
      const yGw = 60,
        yGear = 198,
        ySys = 288,
        n = gearRow.length;
      const margin = 78,
        span = W - margin * 2;
      pos["gear:" + gw.id] = {
        x: W / 2,
        y: yGw,
        gear: gw,
        r: 16
      };
      gearRow.forEach((g, i) => {
        const x = margin + (n > 1 ? span * (i / (n - 1)) : span / 2);
        pos["gear:" + g.id] = {
          x,
          y: yGear,
          gear: g,
          r: g.id === "sw-core" ? 13 : 11
        };
      });
      gearRow.forEach(g => {
        edges.push({
          from: "gear:" + g.id,
          to: "gear:" + g.uplink,
          kind: "uplink",
          label: g.id === "sw-core" ? "Core uplink" : g.wireless ? "AP uplink" : "Switch uplink",
          sub: g.id === "sw-core" ? "40G fiber · LACP" : g.wireless ? "1G PoE+" : "10G SFP+"
        });
        const sys = S.nodes.filter(nd => nd.uplink === g.id);
        const cols = Math.max(3, Math.round(Math.sqrt(sys.length * 1.3)));
        const gx = pos["gear:" + g.id].x;
        const cellW = Math.min(19, (span / n - 8) / cols);
        sys.forEach((nd, si) => {
          const col = si % cols,
            row = Math.floor(si / cols);
          const sx = gx - (cols - 1) * cellW / 2 + col * cellW;
          const sy = ySys + row * 21;
          pos[nd.nodeId] = {
            x: sx,
            y: sy,
            n: nd,
            r: 5
          };
          edges.push({
            from: nd.nodeId,
            to: "gear:" + g.id,
            kind: nd.linkType === "wireless" ? "wireless" : "wired",
            label: nd.linkType === "wireless" ? "Wireless association" : "Wired link",
            sub: `${fmt.mbps(nd.linkSpeedMbps)} · ${nd.uplinkPort}${nd.linkType === "wireless" ? ` · ${nd.rssi} dBm` : " · LLDP"}`,
            openId: nd.nodeId
          });
        });
      });
      let maxY = ySys;
      Object.values(pos).forEach(p => {
        if (p.y > maxY) maxY = p.y;
      });
      return {
        pos,
        edges,
        anchors: [],
        H: Math.max(560, maxY + 50),
        curve: 0
      };
    }, []);
    const L = view === "infra" ? infra : lan;
    const H = L.H;
    const {
      activeNodes,
      activeEdge
    } = useMemo(() => {
      if (!sel) return {
        activeNodes: null,
        activeEdge: null
      };
      if (sel.kind === "edge") {
        const e = L.edges[sel.i];
        return e ? {
          activeNodes: new Set([e.from, e.to]),
          activeEdge: sel.i
        } : {
          activeNodes: null,
          activeEdge: null
        };
      }
      const set = new Set([sel.id]);
      L.edges.forEach(e => {
        if (e.from === sel.id) set.add(e.to);
        if (e.to === sel.id) set.add(e.from);
      });
      return {
        activeNodes: set,
        activeEdge: null
      };
    }, [sel, L]);
    function edgePath(e) {
      const a = L.pos[e.from],
        b = L.pos[e.to];
      if (!a || !b) return null;
      const mx = (a.x + b.x) / 2,
        my = (a.y + b.y) / 2 - L.curve;
      return `M${a.x} ${a.y} Q ${mx} ${my} ${b.x} ${b.y}`;
    }
    function nm(id) {
      const p = L.pos[id];
      return p ? p.n ? p.n.name : p.gear ? p.gear.name : id : id;
    }
    const selNode = sel && sel.kind === "node" ? L.pos[sel.id] : null;
    const selGear = sel && sel.kind === "gear" ? L.pos[sel.id] : null;
    const selEdge = sel && sel.kind === "edge" ? L.edges[sel.i] : null;
    const probeEdges = infra.edges.filter(e => e.kind === "probe").length;
    return /*#__PURE__*/React.createElement("div", {
      className: "page"
    }, /*#__PURE__*/React.createElement("div", {
      className: "page-head"
    }, /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("h1", {
      className: "page-title"
    }, "Topology Map"), /*#__PURE__*/React.createElement("div", {
      className: "page-sub"
    }, view === "infra" ? `monitoring C2 view · ${S.nodes.length} nodes · ${probeEdges} probe edges` : `LAN hierarchy · ${S.netgear.length} network devices · LLDP + agent-derived`, " \xB7 live")), /*#__PURE__*/React.createElement("div", {
      className: "page-head__right"
    }, /*#__PURE__*/React.createElement("div", {
      className: "seg"
    }, /*#__PURE__*/React.createElement("button", {
      className: view === "infra" ? "on" : "",
      onClick: () => {
        setView("infra");
        setSel(null);
      }
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "topology",
      size: 14
    }), "Infrastructure"), /*#__PURE__*/React.createElement("button", {
      className: view === "lan" ? "on" : "",
      onClick: () => {
        setView("lan");
        setSel(null);
      }
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "netswitch",
      size: 14
    }), "LAN hierarchy")))), /*#__PURE__*/React.createElement("div", {
      className: "topo-legend",
      style: {
        marginBottom: 14
      }
    }, /*#__PURE__*/React.createElement("span", null, /*#__PURE__*/React.createElement("span", {
      className: "dot up"
    }), "Up"), /*#__PURE__*/React.createElement("span", null, /*#__PURE__*/React.createElement("span", {
      className: "dot degraded"
    }), "Degraded"), /*#__PURE__*/React.createElement("span", null, /*#__PURE__*/React.createElement("span", {
      className: "dot down"
    }), "Down"), view === "infra" ? /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("span", null, /*#__PURE__*/React.createElement("i", {
      className: "leg-line report"
    }), "report"), /*#__PURE__*/React.createElement("span", null, /*#__PURE__*/React.createElement("i", {
      className: "leg-line probe"
    }), "probe"), /*#__PURE__*/React.createElement("span", null, /*#__PURE__*/React.createElement("i", {
      className: "leg-line lease"
    }), "lease")) : /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("span", null, /*#__PURE__*/React.createElement("i", {
      className: "leg-line wired"
    }), "wired"), /*#__PURE__*/React.createElement("span", null, /*#__PURE__*/React.createElement("i", {
      className: "leg-line wireless"
    }), "wireless"), /*#__PURE__*/React.createElement("span", null, /*#__PURE__*/React.createElement(Icon, {
      name: "gateway",
      size: 13,
      style: {
        color: "var(--violet)"
      }
    }), "gateway"), /*#__PURE__*/React.createElement("span", null, /*#__PURE__*/React.createElement(Icon, {
      name: "netswitch",
      size: 13,
      style: {
        color: "var(--accent)"
      }
    }), "switch"), /*#__PURE__*/React.createElement("span", null, /*#__PURE__*/React.createElement(Icon, {
      name: "wifi",
      size: 13,
      style: {
        color: "var(--warn)"
      }
    }), "AP"))), /*#__PURE__*/React.createElement("div", {
      className: "panel topo-panel"
    }, /*#__PURE__*/React.createElement("svg", {
      viewBox: `0 0 ${W} ${H}`,
      className: "topo-svg",
      onClick: () => setSel(null),
      preserveAspectRatio: "xMidYMid meet"
    }, L.anchors.map(seg => /*#__PURE__*/React.createElement("text", {
      key: seg.id,
      x: seg.x,
      y: seg.y,
      className: "topo-seglabel",
      textAnchor: "middle"
    }, seg.name.toUpperCase())), /*#__PURE__*/React.createElement("g", null, L.edges.map((e, i) => {
      const d = edgePath(e);
      if (!d) return null;
      const inActive = activeNodes && activeNodes.has(e.from) && activeNodes.has(e.to);
      const active = activeEdge === i || sel && sel.kind === "node" && inActive;
      const dim = sel && !active;
      return /*#__PURE__*/React.createElement("path", {
        key: i,
        d: d,
        fill: "none",
        className: "topo-edge " + e.kind + (e.ok === false ? " bad" : "") + (dim ? " dim" : "") + (active ? " active" : "")
      });
    })), /*#__PURE__*/React.createElement("g", null, L.edges.map((e, i) => {
      const d = edgePath(e);
      if (!d) return null;
      return /*#__PURE__*/React.createElement("path", {
        key: i,
        d: d,
        className: "topo-hit",
        onClick: ev => {
          ev.stopPropagation();
          setSel({
            kind: "edge",
            i
          });
        }
      });
    })), view === "infra" && /*#__PURE__*/React.createElement("circle", {
      cx: L.hub.x,
      cy: L.hub.y,
      r: "46",
      className: "topo-hub-glow"
    }), /*#__PURE__*/React.createElement("g", null, Object.entries(L.pos).map(([id, p]) => {
      const dim = sel && activeNodes && !activeNodes.has(id);
      const isSel = selNode && sel.id === id || selGear && sel.id === id;
      if (p.gear) {
        return /*#__PURE__*/React.createElement("g", {
          key: id,
          className: "topo-node gear " + p.gear.kind + (dim ? " dim" : "") + (isSel ? " sel" : ""),
          transform: `translate(${p.x},${p.y})`,
          onClick: ev => {
            ev.stopPropagation();
            setSel(isSel ? null : {
              kind: "gear",
              id
            });
          }
        }, /*#__PURE__*/React.createElement("rect", {
          x: -p.r,
          y: -p.r * 0.72,
          width: p.r * 2,
          height: p.r * 1.44,
          rx: "3",
          fill: GEAR_FILL[p.gear.kind]
        }), /*#__PURE__*/React.createElement("text", {
          y: -p.r - 5,
          className: "topo-nodelabel strong",
          textAnchor: "middle"
        }, p.gear.name));
      }
      return /*#__PURE__*/React.createElement("g", {
        key: id,
        className: "topo-node " + p.n.role + (dim ? " dim" : "") + (isSel ? " sel" : ""),
        transform: `translate(${p.x},${p.y})`,
        onClick: ev => {
          ev.stopPropagation();
          setSel(isSel ? null : {
            kind: "node",
            id
          });
        }
      }, /*#__PURE__*/React.createElement(NodeGlyph, {
        p: p
      }), (view === "infra" && p.n.role !== "client" || isSel) && /*#__PURE__*/React.createElement("text", {
        y: -p.r - 5,
        className: "topo-nodelabel",
        textAnchor: "middle"
      }, p.n.name));
    }))), selNode && /*#__PURE__*/React.createElement("div", {
      className: "topo-card"
    }, /*#__PURE__*/React.createElement("button", {
      className: "topo-card__close",
      onClick: () => setSel(null),
      "aria-label": "Close"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "close",
      size: 14
    })), /*#__PURE__*/React.createElement("div", {
      className: "topo-card__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: selNode.n.role === "server" ? "server" : selNode.n.role === "monitor" ? "monitor" : "host",
      size: 18,
      style: {
        color: "var(--accent)"
      }
    }), /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("div", {
      style: {
        fontFamily: "var(--mono)",
        fontWeight: 600
      }
    }, selNode.n.name, /*#__PURE__*/React.createElement("span", {
      className: "muted"
    }, ".akoria.net")), /*#__PURE__*/React.createElement("div", {
      className: "td-mono muted",
      style: {
        fontSize: 11
      }
    }, selNode.n.role, selNode.n.label ? " · " + selNode.n.label : "", " \xB7 ", selNode.n.segName, " \xB7 ", selNode.n.ip))), /*#__PURE__*/React.createElement("div", {
      className: "topo-card__stats"
    }, /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "CPU"), /*#__PURE__*/React.createElement("div", {
      className: "td-mono",
      style: {
        color: metricColor(selNode.n.cpuPct)
      }
    }, selNode.n.state === "down" ? "—" : selNode.n.cpuPct + "%")), /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "RAM"), /*#__PURE__*/React.createElement("div", {
      className: "td-mono",
      style: {
        color: metricColor(selNode.n.ramPct)
      }
    }, selNode.n.ramPct, "%")), /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Link"), /*#__PURE__*/React.createElement("div", {
      className: "td-mono"
    }, fmt.mbps(selNode.n.linkSpeedMbps))), /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Uplink"), /*#__PURE__*/React.createElement("div", {
      className: "td-mono"
    }, selNode.n.uplink))), /*#__PURE__*/React.createElement("button", {
      className: "backbtn",
      style: {
        width: "100%",
        justifyContent: "center"
      },
      onClick: () => onOpenNode(selNode.n.nodeId)
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "enter",
      size: 14
    }), "Open node detail")), selGear && /*#__PURE__*/React.createElement("div", {
      className: "topo-card"
    }, /*#__PURE__*/React.createElement("button", {
      className: "topo-card__close",
      onClick: () => setSel(null),
      "aria-label": "Close"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "close",
      size: 14
    })), /*#__PURE__*/React.createElement("div", {
      className: "topo-card__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: GEAR_ICON[selGear.gear.kind],
      size: 18,
      style: {
        color: GEAR_FILL[selGear.gear.kind]
      }
    }), /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("div", {
      style: {
        fontFamily: "var(--mono)",
        fontWeight: 600
      }
    }, selGear.gear.name), /*#__PURE__*/React.createElement("div", {
      className: "td-mono muted",
      style: {
        fontSize: 11
      }
    }, selGear.gear.kind, " \xB7 ", selGear.gear.model))), /*#__PURE__*/React.createElement("div", {
      className: "topo-card__stats"
    }, /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Attached"), /*#__PURE__*/React.createElement("div", {
      className: "td-mono",
      style: {
        color: "var(--accent)"
      }
    }, selGear.gear.attached)), /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Ports"), /*#__PURE__*/React.createElement("div", {
      className: "td-mono"
    }, selGear.gear.ports || "—")), /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Uplink"), /*#__PURE__*/React.createElement("div", {
      className: "td-mono"
    }, selGear.gear.uplink || "WAN")), /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Segment"), /*#__PURE__*/React.createElement("div", {
      className: "td-mono"
    }, selGear.gear.seg))), /*#__PURE__*/React.createElement("button", {
      className: "backbtn",
      style: {
        width: "100%",
        justifyContent: "center"
      },
      onClick: () => window.__solariToast && window.__solariToast(`Port map for ${selGear.gear.name} — ${selGear.gear.attached} active LLDP neighbours`, "netswitch")
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "link",
      size: 14
    }), "View LLDP neighbours")), selEdge && /*#__PURE__*/React.createElement("div", {
      className: "topo-card"
    }, /*#__PURE__*/React.createElement("button", {
      className: "topo-card__close",
      onClick: () => setSel(null),
      "aria-label": "Close"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "close",
      size: 14
    })), /*#__PURE__*/React.createElement("div", {
      className: "topo-card__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: selEdge.kind === "probe" ? "reachability" : selEdge.kind === "wireless" ? "wifi" : selEdge.kind === "report" ? "activity" : "link",
      size: 18,
      style: {
        color: selEdge.ok === false ? "var(--crit)" : "var(--accent)"
      }
    }), /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("div", {
      style: {
        fontFamily: "var(--mono)",
        fontWeight: 600
      }
    }, selEdge.label), /*#__PURE__*/React.createElement("div", {
      className: "td-mono muted",
      style: {
        fontSize: 11
      }
    }, "connection \xB7 ", selEdge.kind)), /*#__PURE__*/React.createElement("span", {
      className: "alert-sev " + (selEdge.ok === false ? "crit" : "info"),
      style: {
        marginLeft: "auto"
      }
    }, selEdge.ok === false ? "fault" : "ok")), /*#__PURE__*/React.createElement("div", {
      className: "edge-link"
    }, /*#__PURE__*/React.createElement("span", {
      className: "td-mono"
    }, nm(selEdge.from)), /*#__PURE__*/React.createElement("span", {
      className: "edge-arrow"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "chevronRight",
      size: 14
    })), /*#__PURE__*/React.createElement("span", {
      className: "td-mono"
    }, nm(selEdge.to))), /*#__PURE__*/React.createElement("div", {
      className: "td-mono muted",
      style: {
        fontSize: 12,
        margin: "10px 0 12px"
      }
    }, selEdge.sub), selEdge.openId && /*#__PURE__*/React.createElement("button", {
      className: "backbtn",
      style: {
        width: "100%",
        justifyContent: "center"
      },
      onClick: () => onOpenNode(selEdge.openId)
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "enter",
      size: 14
    }), "Open endpoint host")), !sel && /*#__PURE__*/React.createElement("div", {
      className: "topo-hint"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "topology",
      size: 14
    }), "Tap any node, device, or connection to inspect")));
  }
  Object.assign(window, {
    Reachability,
    Topology
  });
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "src/js/05-d9cca021.jsx", error: String((e && e.message) || e) }); }

// src/js/06-2381da4a.jsx
try { (() => {
/* ============================================================
   SolariNet — screens3: Discovery, Provisioning, Config & Rules
   ============================================================ */
(function () {
  const {
    useState
  } = React;
  const Icon = window.Icon;
  const {
    StatusDot
  } = window;
  const S = window.SOLARI;
  const fmt = S.fmt;
  function toast(msg, icon) {
    if (window.__solariToast) window.__solariToast(msg, icon);
  }

  /* ===================== DISCOVERY ===================== */
  function Discovery({
    onOpenNode
  }) {
    const [staged, setStaged] = useState({}); // host -> "staged" | "ignored"
    const [auto, setAuto] = useState(S.config.autoDiscover);
    const items = S.discovered;
    const active = items.filter(d => staged[d.host] !== "ignored");
    const byVia = {};
    items.forEach(d => {
      byVia[d.via] = (byVia[d.via] || 0) + 1;
    });
    function stage(d) {
      setStaged(s => ({
        ...s,
        [d.host]: "staged"
      }));
      toast(`Enrollment token issued → ${d.host}`, "shield");
    }
    function ignore(d) {
      setStaged(s => ({
        ...s,
        [d.host]: "ignored"
      }));
      toast(`${d.host} ignored`, "close");
    }
    return /*#__PURE__*/React.createElement("div", {
      className: "page"
    }, /*#__PURE__*/React.createElement("div", {
      className: "page-head"
    }, /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("h1", {
      className: "page-title"
    }, "Discovery"), /*#__PURE__*/React.createElement("div", {
      className: "page-sub"
    }, items.length, " candidates found \xB7 not yet monitored")), /*#__PURE__*/React.createElement("div", {
      className: "page-head__right"
    }, /*#__PURE__*/React.createElement("div", {
      className: "statuschip"
    }, /*#__PURE__*/React.createElement("span", {
      className: "dot up glow",
      style: {
        width: 8,
        height: 8,
        animation: "pulse 1.6s infinite"
      }
    }), "scanning ", S.segments.length, " segments"), /*#__PURE__*/React.createElement("div", {
      className: "chip",
      onClick: () => {
        setAuto(a => !a);
        toast(`Auto-discovery ${auto ? "paused" : "resumed"}`, auto ? "close" : "check");
      }
    }, /*#__PURE__*/React.createElement("button", {
      className: "switch" + (auto ? " on" : ""),
      style: {
        pointerEvents: "none"
      }
    }, /*#__PURE__*/React.createElement("i", null)), "Auto-discover"))), /*#__PURE__*/React.createElement("div", {
      className: "kpis"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Candidates"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v"
    }, active.filter(d => staged[d.host] !== "staged").length), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, "awaiting decision"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__bar"
    })), /*#__PURE__*/React.createElement("div", {
      className: "kpi ok"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Staged"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v"
    }, Object.values(staged).filter(v => v === "staged").length), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, "enrollment issued"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__bar"
    })), Object.entries(byVia).slice(0, 3).map(([via, n]) => /*#__PURE__*/React.createElement("div", {
      className: "kpi",
      key: via
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, via), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v"
    }, n), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, "via this method"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__bar"
    })))), /*#__PURE__*/React.createElement("div", {
      className: "page-sub",
      style: {
        margin: "4px 0 12px"
      }
    }, "Discovered hosts & services"), active.map(d => {
      const isStaged = staged[d.host] === "staged";
      return /*#__PURE__*/React.createElement("div", {
        key: d.host,
        className: "disc-row"
      }, /*#__PURE__*/React.createElement(Icon, {
        name: d.kind === "service" ? "link" : "host",
        size: 20,
        style: {
          color: "var(--accent)",
          flex: "0 0 auto"
        }
      }), /*#__PURE__*/React.createElement("div", {
        className: "disc-main"
      }, /*#__PURE__*/React.createElement("div", {
        className: "disc-host"
      }, d.host, /*#__PURE__*/React.createElement("span", {
        className: "muted",
        style: {
          fontSize: 11,
          marginLeft: 8
        }
      }, d.ip)), /*#__PURE__*/React.createElement("div", {
        className: "disc-svcs"
      }, d.services.map(s => /*#__PURE__*/React.createElement("span", {
        key: s,
        className: "svc-chip"
      }, s)))), /*#__PURE__*/React.createElement("div", {
        className: "disc-meta"
      }, /*#__PURE__*/React.createElement("span", {
        className: "tag"
      }, d.via), /*#__PURE__*/React.createElement("span", {
        className: "td-mono muted"
      }, d.seg, " \xB7 ", d.arch), /*#__PURE__*/React.createElement("span", {
        className: "td-mono muted"
      }, d.seen, "m ago")), isStaged ? /*#__PURE__*/React.createElement("span", {
        className: "alert-sev info",
        style: {
          flex: "0 0 auto"
        }
      }, /*#__PURE__*/React.createElement(Icon, {
        name: "check",
        size: 12,
        style: {
          verticalAlign: "-2px"
        }
      }), " staged") : /*#__PURE__*/React.createElement("div", {
        className: "disc-actions"
      }, /*#__PURE__*/React.createElement("button", {
        className: "btn-ghost",
        onClick: () => ignore(d)
      }, "Ignore"), /*#__PURE__*/React.createElement("button", {
        className: "btn-primary",
        onClick: () => stage(d)
      }, /*#__PURE__*/React.createElement(Icon, {
        name: "plus",
        size: 14
      }), "Monitor")));
    }));
  }

  /* ===================== PROVISIONING ===================== */
  function Provisioning({
    onOpenNode
  }) {
    const [enr, setEnr] = useState(S.enrollments);
    const [token, setToken] = useState(null);
    const drifted = S.nodes.filter(n => !n.converged);
    const converged = S.nodes.length - drifted.length;
    function approve(e) {
      setEnr(list => list.filter(x => x.host !== e.host));
      toast(`CSR signed — ${e.host} enrolled as ${e.role}`, "shield");
    }
    function deny(e) {
      setEnr(list => list.filter(x => x.host !== e.host));
      toast(`Enrollment denied — ${e.host}`, "close");
    }
    function issueToken() {
      const t = "SLR-" + Math.random().toString(36).slice(2, 8).toUpperCase() + "-" + Math.random().toString(36).slice(2, 6).toUpperCase();
      setToken({
        value: t,
        ttl: 900
      });
      toast("Single-use enrollment token generated (TTL 15m)", "shield");
    }
    return /*#__PURE__*/React.createElement("div", {
      className: "page"
    }, /*#__PURE__*/React.createElement("div", {
      className: "page-head"
    }, /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("h1", {
      className: "page-title"
    }, "Provisioning"), /*#__PURE__*/React.createElement("div", {
      className: "page-sub"
    }, "enrollment \xB7 binary deploy \xB7 config convergence"))), /*#__PURE__*/React.createElement("div", {
      className: "kpis"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi warn"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Pending CSRs"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v"
    }, enr.length), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, "awaiting approval"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__bar"
    })), /*#__PURE__*/React.createElement("div", {
      className: "kpi ok"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Converged"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v"
    }, converged), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, "at target epoch"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__bar"
    })), /*#__PURE__*/React.createElement("div", {
      className: "kpi crit"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Config drift"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v"
    }, drifted.length), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, "need re-push"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__bar"
    })), /*#__PURE__*/React.createElement("div", {
      className: "kpi"
    }, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "Build channels"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__v"
    }, S.builds.length), /*#__PURE__*/React.createElement("div", {
      className: "kpi__sub"
    }, "arch \xD7 OS targets"), /*#__PURE__*/React.createElement("div", {
      className: "kpi__bar"
    }))), /*#__PURE__*/React.createElement("div", {
      className: "two-col",
      style: {
        alignItems: "start"
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        flexDirection: "column",
        gap: 16
      }
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel"
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "shield",
      size: 16
    }), /*#__PURE__*/React.createElement("h3", null, "Enrollment queue"), /*#__PURE__*/React.createElement("div", {
      className: "right"
    }, /*#__PURE__*/React.createElement("button", {
      className: "btn-primary",
      onClick: issueToken
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "plus",
      size: 14
    }), "Issue token"))), /*#__PURE__*/React.createElement("div", {
      className: "panel__body",
      style: {
        padding: enr.length ? 0 : 16
      }
    }, token && /*#__PURE__*/React.createElement("div", {
      className: "token-banner"
    }, /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("div", {
      className: "kpi__k"
    }, "One-time enrollment token"), /*#__PURE__*/React.createElement("div", {
      className: "token-val"
    }, token.value)), /*#__PURE__*/React.createElement("div", {
      className: "td-mono muted"
    }, "TTL 15:00 \xB7 single-use")), enr.length === 0 && /*#__PURE__*/React.createElement("div", {
      className: "td-mono muted",
      style: {
        fontSize: 13
      }
    }, "No pending enrollments."), enr.map(e => /*#__PURE__*/React.createElement("div", {
      key: e.host,
      className: "enr-row"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: e.role === "monitor" ? "monitor" : "host",
      size: 18,
      style: {
        color: "var(--ink-dim)",
        flex: "0 0 auto"
      }
    }), /*#__PURE__*/React.createElement("div", {
      style: {
        flex: 1,
        minWidth: 0
      }
    }, /*#__PURE__*/React.createElement("div", {
      className: "td-mono",
      style: {
        fontWeight: 600
      }
    }, e.host), /*#__PURE__*/React.createElement("div", {
      className: "td-mono muted",
      style: {
        fontSize: 11
      }
    }, e.ip, " \xB7 ", e.role, " \xB7 CSR ", e.fp)), /*#__PURE__*/React.createElement("span", {
      className: "tag"
    }, e.status), /*#__PURE__*/React.createElement("span", {
      className: "td-mono muted",
      style: {
        fontSize: 11
      }
    }, e.requestedMin, "m"), /*#__PURE__*/React.createElement("div", {
      className: "disc-actions"
    }, /*#__PURE__*/React.createElement("button", {
      className: "btn-ghost",
      onClick: () => deny(e)
    }, "Deny"), /*#__PURE__*/React.createElement("button", {
      className: "btn-primary",
      onClick: () => approve(e)
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "check",
      size: 14
    }), "Sign")))))), /*#__PURE__*/React.createElement("div", {
      className: "panel"
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "refresh",
      size: 16
    }), /*#__PURE__*/React.createElement("h3", null, "Config convergence"), /*#__PURE__*/React.createElement("div", {
      className: "right"
    }, converged, "/", S.nodes.length, " ok")), /*#__PURE__*/React.createElement("div", {
      className: "panel__body",
      style: {
        padding: 0
      }
    }, drifted.slice(0, 8).map(n => /*#__PURE__*/React.createElement("div", {
      key: n.nodeId,
      className: "enr-row",
      onClick: () => onOpenNode(n.nodeId),
      style: {
        cursor: "pointer"
      }
    }, /*#__PURE__*/React.createElement("span", {
      className: "dot degraded",
      style: {
        flex: "0 0 auto"
      }
    }), /*#__PURE__*/React.createElement("div", {
      style: {
        flex: 1,
        minWidth: 0
      }
    }, /*#__PURE__*/React.createElement("div", {
      className: "td-mono",
      style: {
        fontWeight: 600
      }
    }, n.name, /*#__PURE__*/React.createElement("span", {
      className: "muted",
      style: {
        fontSize: 11
      }
    }, " \xB7 ", n.segName)), /*#__PURE__*/React.createElement("div", {
      className: "td-mono muted",
      style: {
        fontSize: 11
      }
    }, "target epoch ", n.configEpoch, " \xB7 applied ", n.configEpoch - 1, " \xB7 drift")), /*#__PURE__*/React.createElement("button", {
      className: "btn-primary",
      onClick: ev => {
        ev.stopPropagation();
        toast(`Config re-pushed → ${n.name} (SCP_MSG_CONTROL)`, "settings");
      }
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "refresh",
      size: 13
    }), "Re-push"))), drifted.length === 0 && /*#__PURE__*/React.createElement("div", {
      className: "td-mono muted",
      style: {
        fontSize: 13,
        padding: 16
      }
    }, "All nodes converged.")))), /*#__PURE__*/React.createElement("div", {
      className: "panel"
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "arch",
      size: 16
    }), /*#__PURE__*/React.createElement("h3", null, "Binary builds & deploy")), /*#__PURE__*/React.createElement("div", {
      className: "panel__body",
      style: {
        padding: 0
      }
    }, S.builds.map((b, i) => /*#__PURE__*/React.createElement("div", {
      key: i,
      className: "build-row"
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        flex: 1
      }
    }, /*#__PURE__*/React.createElement("div", {
      className: "td-mono",
      style: {
        fontWeight: 600
      }
    }, b.os, " \xB7 ", b.arch), /*#__PURE__*/React.createElement("div", {
      className: "td-mono muted",
      style: {
        fontSize: 11
      }
    }, b.nodes, " nodes \xB7 ", b.channel)), /*#__PURE__*/React.createElement("div", {
      className: "td-mono",
      style: {
        textAlign: "right"
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        color: b.status === "current" ? "var(--ok)" : "var(--warn)",
        fontWeight: 600
      }
    }, "v", b.version), /*#__PURE__*/React.createElement("div", {
      className: "muted",
      style: {
        fontSize: 10
      }
    }, b.status === "current" ? "up to date" : "update available")), b.status === "update" ? /*#__PURE__*/React.createElement("button", {
      className: "btn-primary",
      onClick: () => toast(`Rolling v1.0.3 → ${b.nodes} ${b.arch} nodes`, "arch")
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "refresh",
      size: 13
    }), "Push") : /*#__PURE__*/React.createElement("span", {
      className: "alert-sev info",
      style: {
        flex: "0 0 auto"
      }
    }, "current")))))));
  }

  /* ===================== CONFIG & RULES ===================== */
  function ConfigScreen({
    onNav
  }) {
    const [tab, setTab] = useState("global");
    const [cfg, setCfg] = useState(JSON.parse(JSON.stringify(S.config)));
    const [dirty, setDirty] = useState(false);
    function set(path, val) {
      setCfg(c => {
        const n = JSON.parse(JSON.stringify(c));
        let o = n;
        const parts = path.split(".");
        for (let i = 0; i < parts.length - 1; i++) o = o[parts[i]];
        o[parts[parts.length - 1]] = val;
        return n;
      });
      setDirty(true);
    }
    function Row({
      label,
      path,
      min,
      max,
      step,
      unit
    }) {
      const parts = path.split(".");
      let v = cfg;
      parts.forEach(p => v = v[p]);
      return /*#__PURE__*/React.createElement("div", {
        className: "metric-row"
      }, /*#__PURE__*/React.createElement("span", {
        className: "lbl",
        style: {
          width: 150,
          flex: "0 0 150px"
        }
      }, label), /*#__PURE__*/React.createElement("input", {
        type: "range",
        min: min,
        max: max,
        step: step || 1,
        value: v,
        onChange: e => set(path, +e.target.value),
        style: {
          flex: 1,
          accentColor: "var(--accent)"
        }
      }), /*#__PURE__*/React.createElement("span", {
        className: "val",
        style: {
          width: 90,
          flex: "0 0 90px",
          color: "var(--accent)"
        }
      }, v, unit));
    }
    return /*#__PURE__*/React.createElement("div", {
      className: "page"
    }, /*#__PURE__*/React.createElement("div", {
      className: "page-head"
    }, /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("h1", {
      className: "page-title"
    }, "Config & Rules"), /*#__PURE__*/React.createElement("div", {
      className: "page-sub"
    }, "global tolerances, schedules & retention \xB7 pushed as SCP control frames")), /*#__PURE__*/React.createElement("div", {
      className: "page-head__right"
    }, /*#__PURE__*/React.createElement("div", {
      className: "seg"
    }, /*#__PURE__*/React.createElement("button", {
      className: tab === "global" ? "on" : "",
      onClick: () => setTab("global")
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "settings",
      size: 14
    }), "Global"), /*#__PURE__*/React.createElement("button", {
      className: tab === "agents" ? "on" : "",
      onClick: () => setTab("agents")
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "host",
      size: 14
    }), "Per-agent")), /*#__PURE__*/React.createElement("button", {
      className: "backbtn",
      onClick: () => onNav("alerts")
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "alerts",
      size: 15
    }), "Alert rules"), tab === "global" && /*#__PURE__*/React.createElement("button", {
      className: "btn-primary" + (dirty ? "" : " disabled"),
      onClick: () => {
        if (dirty) {
          toast("Config staged → pushed to fleet on next epoch", "settings");
          setDirty(false);
        }
      }
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "check",
      size: 14
    }), "Save & push"))), tab === "global" && /*#__PURE__*/React.createElement("div", {
      className: "three-col",
      style: {
        alignItems: "start"
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        flexDirection: "column",
        gap: 16
      }
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel"
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "clock",
      size: 16
    }), /*#__PURE__*/React.createElement("h3", null, "Sampling schedule")), /*#__PURE__*/React.createElement("div", {
      className: "panel__body"
    }, /*#__PURE__*/React.createElement(Row, {
      label: "Sample interval",
      path: "schedule.sampleIntervalSec",
      min: 5,
      max: 120,
      unit: "s"
    }), /*#__PURE__*/React.createElement(Row, {
      label: "Watchdog interval",
      path: "schedule.watchdogIntervalSec",
      min: 1,
      max: 30,
      unit: "s"
    }))), /*#__PURE__*/React.createElement("div", {
      className: "panel"
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "disk",
      size: 16
    }), /*#__PURE__*/React.createElement("h3", null, "Retention")), /*#__PURE__*/React.createElement("div", {
      className: "panel__body"
    }, /*#__PURE__*/React.createElement(Row, {
      label: "History",
      path: "retention.historyDays",
      min: 7,
      max: 365,
      unit: "d"
    }), /*#__PURE__*/React.createElement("div", {
      className: "cfg-toggle"
    }, /*#__PURE__*/React.createElement("span", null, "Monthly partition pruning"), /*#__PURE__*/React.createElement("button", {
      className: "switch" + (cfg.retention.partitionByMonth ? " on" : ""),
      onClick: () => set("retention.partitionByMonth", !cfg.retention.partitionByMonth)
    }, /*#__PURE__*/React.createElement("i", null)))))), /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        flexDirection: "column",
        gap: 16
      }
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel"
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "reachability",
      size: 16
    }), /*#__PURE__*/React.createElement("h3", null, "Probe defaults")), /*#__PURE__*/React.createElement("div", {
      className: "panel__body"
    }, /*#__PURE__*/React.createElement(Row, {
      label: "Round interval",
      path: "probe.roundIntervalSec",
      min: 10,
      max: 120,
      unit: "s"
    }), /*#__PURE__*/React.createElement(Row, {
      label: "Probes / round",
      path: "probe.probesPerRound",
      min: 1,
      max: 20,
      unit: ""
    }), /*#__PURE__*/React.createElement(Row, {
      label: "Replication factor",
      path: "probe.replFactor",
      min: 1,
      max: 5,
      unit: "\xD7"
    }), /*#__PURE__*/React.createElement(Row, {
      label: "Peer gossip",
      path: "probe.gossipIntervalSec",
      min: 5,
      max: 60,
      unit: "s"
    }))), /*#__PURE__*/React.createElement("div", {
      className: "panel"
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "shield",
      size: 16
    }), /*#__PURE__*/React.createElement("h3", null, "Failover lease")), /*#__PURE__*/React.createElement("div", {
      className: "panel__body"
    }, /*#__PURE__*/React.createElement(Row, {
      label: "Renew interval",
      path: "lease.renewSec",
      min: 1,
      max: 30,
      unit: "s"
    }), /*#__PURE__*/React.createElement(Row, {
      label: "Lease TTL",
      path: "lease.ttlSec",
      min: 5,
      max: 60,
      unit: "s"
    })))), /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        flexDirection: "column",
        gap: 16
      }
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel"
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "settings",
      size: 16
    }), /*#__PURE__*/React.createElement("h3", null, "Discovery & enrollment")), /*#__PURE__*/React.createElement("div", {
      className: "panel__body"
    }, /*#__PURE__*/React.createElement("div", {
      className: "cfg-toggle"
    }, /*#__PURE__*/React.createElement("span", null, "Auto-discovery"), /*#__PURE__*/React.createElement("button", {
      className: "switch" + (cfg.autoDiscover ? " on" : ""),
      onClick: () => set("autoDiscover", !cfg.autoDiscover)
    }, /*#__PURE__*/React.createElement("i", null))), /*#__PURE__*/React.createElement("div", {
      className: "cfg-toggle"
    }, /*#__PURE__*/React.createElement("span", null, "Auto-enroll discovered ", /*#__PURE__*/React.createElement("span", {
      className: "muted",
      style: {
        fontSize: 11
      }
    }, "(requires approval off)")), /*#__PURE__*/React.createElement("button", {
      className: "switch" + (cfg.autoEnroll ? " on" : ""),
      onClick: () => set("autoEnroll", !cfg.autoEnroll)
    }, /*#__PURE__*/React.createElement("i", null))), /*#__PURE__*/React.createElement("div", {
      className: "divider"
    }), /*#__PURE__*/React.createElement("div", {
      className: "cfg-info"
    }, /*#__PURE__*/React.createElement("span", {
      className: "kpi__k"
    }, "Internal CA"), /*#__PURE__*/React.createElement("span", {
      className: "td-mono"
    }, cfg.ca.issuer)), /*#__PURE__*/React.createElement("div", {
      className: "cfg-info"
    }, /*#__PURE__*/React.createElement("span", {
      className: "kpi__k"
    }, "Cert TTL"), /*#__PURE__*/React.createElement("span", {
      className: "td-mono"
    }, cfg.ca.certTtlDays, " days")), /*#__PURE__*/React.createElement("div", {
      className: "cfg-info"
    }, /*#__PURE__*/React.createElement("span", {
      className: "kpi__k"
    }, "Enroll method"), /*#__PURE__*/React.createElement("span", {
      className: "td-mono"
    }, cfg.ca.enroll)))), /*#__PURE__*/React.createElement("div", {
      className: "panel"
    }, /*#__PURE__*/React.createElement("div", {
      className: "panel__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "network",
      size: 16
    }), /*#__PURE__*/React.createElement("h3", null, "Transport"), /*#__PURE__*/React.createElement("div", {
      className: "right"
    }, cfg.ingest.tls)), /*#__PURE__*/React.createElement("div", {
      className: "panel__body"
    }, /*#__PURE__*/React.createElement("div", {
      className: "cfg-info"
    }, /*#__PURE__*/React.createElement("span", {
      className: "kpi__k"
    }, "Ingest"), /*#__PURE__*/React.createElement("span", {
      className: "td-mono"
    }, ":", cfg.ingest.ports.ingest, " \xB7 PULL")), /*#__PURE__*/React.createElement("div", {
      className: "cfg-info"
    }, /*#__PURE__*/React.createElement("span", {
      className: "kpi__k"
    }, "Survey"), /*#__PURE__*/React.createElement("span", {
      className: "td-mono"
    }, ":", cfg.ingest.ports.survey, " \xB7 SURVEYOR")), /*#__PURE__*/React.createElement("div", {
      className: "cfg-info"
    }, /*#__PURE__*/React.createElement("span", {
      className: "kpi__k"
    }, "Publish"), /*#__PURE__*/React.createElement("span", {
      className: "td-mono"
    }, ":", cfg.ingest.ports.pub, " \xB7 PUB")))))), tab === "agents" && /*#__PURE__*/React.createElement(AgentDirectory, null));
  }

  /* per-agent directory + modal config editor */
  function AgentDirectory() {
    const [type, setType] = useState("client");
    const [q, setQ] = useState("");
    const [driftOnly, setDriftOnly] = useState(false);
    const [modalId, setModalId] = useState(null);
    const pool = S.nodes.filter(n => n.role === type);
    const list = pool.filter(n => (!q || n.name.includes(q.toLowerCase()) || (n.ip || "").includes(q)) && (!driftOnly || !n.converged));
    const drift = pool.filter(n => !n.converged).length;
    const modalNode = modalId ? S.nodes.find(n => n.nodeId === modalId) : null;
    const TYPE_ICON = {
      server: "server",
      monitor: "monitor",
      client: "host"
    };
    const TYPE_LABEL = {
      server: "Server",
      monitor: "Network",
      client: "Application"
    };
    return /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("div", {
      className: "filters"
    }, ["server", "monitor", "client"].map(t => /*#__PURE__*/React.createElement("button", {
      key: t,
      className: "chip" + (type === t ? " on" : ""),
      onClick: () => setType(t)
    }, /*#__PURE__*/React.createElement(Icon, {
      name: TYPE_ICON[t],
      size: 14
    }), TYPE_LABEL[t], /*#__PURE__*/React.createElement("span", {
      className: "chip__n"
    }, S.nodes.filter(n => n.role === t).length))), /*#__PURE__*/React.createElement("div", {
      style: {
        width: 1,
        height: 24,
        background: "var(--line)",
        margin: "0 4px"
      }
    }), /*#__PURE__*/React.createElement("button", {
      className: "chip" + (driftOnly ? " on" : ""),
      onClick: () => setDriftOnly(d => !d)
    }, /*#__PURE__*/React.createElement("span", {
      className: "dot degraded"
    }), "Drift only", /*#__PURE__*/React.createElement("span", {
      className: "chip__n"
    }, drift)), /*#__PURE__*/React.createElement("div", {
      className: "search",
      style: {
        marginLeft: "auto",
        maxWidth: 250,
        height: 36
      }
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "search",
      size: 15
    }), /*#__PURE__*/React.createElement("input", {
      value: q,
      onChange: e => setQ(e.target.value),
      placeholder: "Find agent\u2026"
    }))), /*#__PURE__*/React.createElement("div", {
      className: "agent-dir"
    }, list.map(n => /*#__PURE__*/React.createElement("div", {
      key: n.nodeId,
      className: "agent-card",
      onClick: () => setModalId(n.nodeId)
    }, /*#__PURE__*/React.createElement(Icon, {
      name: TYPE_ICON[n.role],
      size: 18,
      style: {
        color: "var(--ink-dim)",
        flex: "0 0 auto"
      }
    }), /*#__PURE__*/React.createElement("div", {
      style: {
        flex: 1,
        minWidth: 0
      }
    }, /*#__PURE__*/React.createElement("div", {
      className: "td-mono",
      style: {
        fontWeight: 600,
        fontSize: 13,
        display: "flex",
        alignItems: "center",
        gap: 7
      }
    }, /*#__PURE__*/React.createElement(StatusDot, {
      state: n.state,
      size: 8
    }), n.name), /*#__PURE__*/React.createElement("div", {
      className: "td-mono muted",
      style: {
        fontSize: 10.5,
        whiteSpace: "nowrap",
        overflow: "hidden",
        textOverflow: "ellipsis"
      }
    }, n.segName, " \xB7 ", n.ip, " \xB7 epoch ", n.configEpoch)), !n.converged && /*#__PURE__*/React.createElement("span", {
      className: "tag",
      style: {
        color: "var(--warn)",
        borderColor: "var(--warn)"
      }
    }, "drift"), /*#__PURE__*/React.createElement(Icon, {
      name: "chevronRight",
      size: 16,
      style: {
        color: "var(--ink-faint)",
        flex: "0 0 auto"
      }
    }))), list.length === 0 && /*#__PURE__*/React.createElement("div", {
      className: "empty",
      style: {
        gridColumn: "1 / -1"
      }
    }, "No agents match.")), modalNode && /*#__PURE__*/React.createElement(AgentModal, {
      node: modalNode,
      onClose: () => setModalId(null)
    }));
  }
  function AgentModal({
    node,
    onClose
  }) {
    function initDraft(n) {
      if (n.role === "client") return {
        sampleSec: 15,
        logPat: "(error|fail)",
        watched: Object.fromEntries(n.procs.map(p => [p.name, true]))
      };
      if (n.role === "monitor") {
        const ts = S.probes.filter(p => p.vantages.some(v => v.monitorNode === n.nodeId));
        return {
          roundSec: 30,
          perRound: 5,
          repl: 2,
          targets: Object.fromEntries(ts.map(t => [t.targetId, true]))
        };
      }
      return {
        renewSec: 5,
        ttlSec: 15,
        pool: 8
      };
    }
    const [draft, setDraft] = useState(() => initDraft(node));
    const TYPE_ICON = {
      server: "server",
      monitor: "monitor",
      client: "host"
    };
    React.useEffect(() => {
      function onKey(e) {
        if (e.key === "Escape") onClose();
      }
      window.addEventListener("keydown", onKey);
      return () => window.removeEventListener("keydown", onKey);
    }, []);
    function push() {
      toast("Config pushed → " + node.name + " (SCP_MSG_CONTROL · epoch " + (node.configEpoch + 1) + ")", "settings");
      onClose();
    }
    function Slider({
      label,
      k,
      min,
      max,
      unit
    }) {
      return /*#__PURE__*/React.createElement("div", {
        className: "metric-row"
      }, /*#__PURE__*/React.createElement("span", {
        className: "lbl",
        style: {
          width: 150,
          flex: "0 0 150px"
        }
      }, label), /*#__PURE__*/React.createElement("input", {
        type: "range",
        min: min,
        max: max,
        value: draft[k] != null ? draft[k] : min,
        onChange: e => setDraft(d => ({
          ...d,
          [k]: +e.target.value
        })),
        style: {
          flex: 1,
          accentColor: "var(--accent)"
        }
      }), /*#__PURE__*/React.createElement("span", {
        className: "val",
        style: {
          width: 84,
          flex: "0 0 84px",
          color: "var(--accent)"
        }
      }, draft[k], unit));
    }
    return /*#__PURE__*/React.createElement("div", {
      className: "modal-overlay",
      onClick: onClose
    }, /*#__PURE__*/React.createElement("div", {
      className: "modal",
      onClick: e => e.stopPropagation()
    }, /*#__PURE__*/React.createElement("div", {
      className: "modal__head"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: TYPE_ICON[node.role],
      size: 20,
      style: {
        color: "var(--accent)"
      }
    }), /*#__PURE__*/React.createElement("div", {
      style: {
        flex: 1,
        minWidth: 0
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        fontFamily: "var(--mono)",
        fontWeight: 600,
        fontSize: 15
      }
    }, node.name, /*#__PURE__*/React.createElement("span", {
      className: "muted"
    }, ".akoria.net")), /*#__PURE__*/React.createElement("div", {
      className: "td-mono muted",
      style: {
        fontSize: 11
      }
    }, node.role, node.label ? " · " + node.label : "", " \xB7 ", node.segName, " \xB7 ", node.osName, " \xB7 ", node.arch, " \xB7 epoch ", node.configEpoch)), /*#__PURE__*/React.createElement("button", {
      className: "iconbtn",
      style: {
        width: 36,
        height: 36,
        flex: "0 0 36px"
      },
      onClick: onClose,
      "aria-label": "Close"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "close",
      size: 16
    }))), /*#__PURE__*/React.createElement("div", {
      className: "modal__body"
    }, node.role === "client" && /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement(Slider, {
      label: "Sample interval",
      k: "sampleSec",
      min: 5,
      max: 120,
      unit: "s"
    }), /*#__PURE__*/React.createElement("div", {
      className: "agent-sect"
    }, "Watched applications"), /*#__PURE__*/React.createElement("div", {
      className: "agent-apps"
    }, node.procs.map(p => /*#__PURE__*/React.createElement("div", {
      key: p.name,
      className: "agent-app"
    }, /*#__PURE__*/React.createElement("span", {
      className: "td-mono",
      style: {
        fontWeight: 600,
        flex: 1
      }
    }, p.name), /*#__PURE__*/React.createElement("span", {
      className: "td-mono muted",
      style: {
        fontSize: 10
      }
    }, p.runState === "Z" ? "not running" : "pid " + p.pid), /*#__PURE__*/React.createElement("button", {
      className: "switch" + (draft.watched && draft.watched[p.name] ? " on" : ""),
      onClick: () => setDraft(d => ({
        ...d,
        watched: {
          ...d.watched,
          [p.name]: !d.watched[p.name]
        }
      }))
    }, /*#__PURE__*/React.createElement("i", null))))), /*#__PURE__*/React.createElement("div", {
      className: "metric-row"
    }, /*#__PURE__*/React.createElement("span", {
      className: "lbl",
      style: {
        width: 150,
        flex: "0 0 150px"
      }
    }, "Log watch regex"), /*#__PURE__*/React.createElement("input", {
      className: "agent-text",
      value: draft.logPat || "",
      onChange: e => setDraft(d => ({
        ...d,
        logPat: e.target.value
      }))
    }))), node.role === "monitor" && /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement(Slider, {
      label: "Round interval",
      k: "roundSec",
      min: 10,
      max: 120,
      unit: "s"
    }), /*#__PURE__*/React.createElement(Slider, {
      label: "Probes / round",
      k: "perRound",
      min: 1,
      max: 20,
      unit: ""
    }), /*#__PURE__*/React.createElement(Slider, {
      label: "Repl factor hint",
      k: "repl",
      min: 1,
      max: 5,
      unit: "\xD7"
    }), /*#__PURE__*/React.createElement("div", {
      className: "agent-sect"
    }, "Assigned probe targets ", /*#__PURE__*/React.createElement("span", {
      className: "muted"
    }, "(HRW)")), /*#__PURE__*/React.createElement("div", {
      className: "agent-apps"
    }, (!draft.targets || Object.keys(draft.targets).length === 0) && /*#__PURE__*/React.createElement("div", {
      className: "td-mono muted",
      style: {
        fontSize: 12
      }
    }, "No targets assigned to this vantage."), S.probes.filter(p => draft.targets && p.targetId in draft.targets).map(p => /*#__PURE__*/React.createElement("div", {
      key: p.targetId,
      className: "agent-app"
    }, /*#__PURE__*/React.createElement("span", {
      className: "td-mono",
      style: {
        fontWeight: 600,
        flex: 1,
        minWidth: 0,
        whiteSpace: "nowrap",
        overflow: "hidden",
        textOverflow: "ellipsis"
      }
    }, p.targetId), /*#__PURE__*/React.createElement("span", {
      className: "td-mono muted",
      style: {
        fontSize: 10
      }
    }, p.label), /*#__PURE__*/React.createElement("button", {
      className: "switch" + (draft.targets[p.targetId] ? " on" : ""),
      onClick: () => setDraft(d => ({
        ...d,
        targets: {
          ...d.targets,
          [p.targetId]: !d.targets[p.targetId]
        }
      }))
    }, /*#__PURE__*/React.createElement("i", null)))))), node.role === "server" && /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement(Slider, {
      label: "Lease renew",
      k: "renewSec",
      min: 1,
      max: 30,
      unit: "s"
    }), /*#__PURE__*/React.createElement(Slider, {
      label: "Lease TTL",
      k: "ttlSec",
      min: 5,
      max: 60,
      unit: "s"
    }), /*#__PURE__*/React.createElement(Slider, {
      label: "DB pool size",
      k: "pool",
      min: 2,
      max: 32,
      unit: ""
    }), /*#__PURE__*/React.createElement("div", {
      className: "agent-sect"
    }, "Bind addresses"), /*#__PURE__*/React.createElement("div", {
      className: "cfg-info"
    }, /*#__PURE__*/React.createElement("span", {
      className: "kpi__k"
    }, "Ingest"), /*#__PURE__*/React.createElement("span", {
      className: "td-mono"
    }, "0.0.0.0:", S.config.ingest.ports.ingest, " \xB7 PULL")), /*#__PURE__*/React.createElement("div", {
      className: "cfg-info"
    }, /*#__PURE__*/React.createElement("span", {
      className: "kpi__k"
    }, "Survey"), /*#__PURE__*/React.createElement("span", {
      className: "td-mono"
    }, "0.0.0.0:", S.config.ingest.ports.survey, " \xB7 SURVEYOR")), /*#__PURE__*/React.createElement("div", {
      className: "cfg-info"
    }, /*#__PURE__*/React.createElement("span", {
      className: "kpi__k"
    }, "Publish"), /*#__PURE__*/React.createElement("span", {
      className: "td-mono"
    }, "0.0.0.0:", S.config.ingest.ports.pub, " \xB7 PUB")))), /*#__PURE__*/React.createElement("div", {
      className: "modal__foot"
    }, /*#__PURE__*/React.createElement("button", {
      className: "btn-ghost",
      onClick: onClose
    }, "Cancel"), /*#__PURE__*/React.createElement("button", {
      className: "btn-primary",
      onClick: push
    }, /*#__PURE__*/React.createElement(Icon, {
      name: "check",
      size: 14
    }), "Push to node"))));
  }
  Object.assign(window, {
    Discovery,
    Provisioning,
    ConfigScreen
  });
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "src/js/06-2381da4a.jsx", error: String((e && e.message) || e) }); }

// src/js/07-f83da9fd.jsx
try { (() => {
/* ============================================================
   SolariNet — app root
   ============================================================ */
(function () {
  const {
    useState,
    useEffect,
    useRef,
    useCallback
  } = React;
  const Icon = window.Icon;
  const S = window.SOLARI;
  const {
    Sidebar,
    TopBar,
    CommandPalette,
    Toasts,
    FleetOverview,
    NodeDetail,
    AlertsScreen,
    Reachability,
    Topology,
    Discovery,
    Provisioning,
    ConfigScreen
  } = window;
  const PLANNED_LABEL = {
    reachability: ["Reachability Matrix", "Probe targets × monitor vantages — RTT, loss, and split-vantage divergence rendered as a live matrix."],
    topology: ["Topology Map", "Live C2 map of nodes and monitor→target assignment edges (HRW result) across every segment."],
    discovery: ["Discovery", "Auto-found hosts & services (mDNS, ARP, SCP advert) staged for one-tap enrolment into monitoring."],
    provision: ["Provisioning", "Issue enrolment tokens, sign CSRs, and push binaries to new nodes."],
    settings: ["Config & Rules", "Global tolerances, retention, and per-node config overlays — managed centrally, pushed as SCP control frames."]
  };
  function PlannedPage({
    id
  }) {
    const [title, desc] = PLANNED_LABEL[id] || ["Planned", ""];
    const preview = id === "discovery";
    return /*#__PURE__*/React.createElement("div", {
      className: "page"
    }, /*#__PURE__*/React.createElement("div", {
      className: "placeholder-page"
    }, /*#__PURE__*/React.createElement("div", {
      className: "box"
    }, /*#__PURE__*/React.createElement(Icon, {
      name: id === "discovery" ? "discovery" : id === "topology" ? "topology" : id === "reachability" ? "reachability" : id === "provision" ? "provision" : "settings",
      size: 52,
      className: "ico"
    }), /*#__PURE__*/React.createElement("h2", null, title), /*#__PURE__*/React.createElement("p", null, desc), /*#__PURE__*/React.createElement("div", {
      className: "tag",
      style: {
        display: "inline-block",
        marginTop: 8
      }
    }, "next iteration"), preview && /*#__PURE__*/React.createElement("div", {
      style: {
        marginTop: 26,
        textAlign: "left"
      }
    }, /*#__PURE__*/React.createElement("div", {
      className: "page-sub",
      style: {
        marginBottom: 10
      }
    }, "Discovered \xB7 not yet monitored"), S.discovered.map((d, i) => /*#__PURE__*/React.createElement("div", {
      key: i,
      className: "alert-row info",
      style: {
        cursor: "default"
      }
    }, /*#__PURE__*/React.createElement(Icon, {
      name: d.kind === "service" ? "link" : "host",
      size: 18,
      style: {
        color: "var(--accent)",
        flex: "0 0 auto"
      }
    }), /*#__PURE__*/React.createElement("div", {
      className: "alert-main"
    }, /*#__PURE__*/React.createElement("div", {
      className: "t"
    }, d.host), /*#__PURE__*/React.createElement("div", {
      className: "d"
    }, d.services.join(" · "), " \u2014 via ", d.via)), /*#__PURE__*/React.createElement("div", {
      className: "alert-meta"
    }, d.seen, "m ago", /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("span", {
      className: "tag",
      style: {
        borderColor: "var(--line-glow)",
        color: "var(--accent)"
      }
    }, "+ monitor")))))))));
  }
  function App() {
    const [theme, setTheme] = useState(() => localStorage.getItem("solari-theme") || "dark");
    const [route, setRoute] = useState({
      name: "fleet"
    });
    const [fleetView, setFleetView] = useState("heat");
    const [collapsed, setCollapsed] = useState(false);
    const [navHidden, setNavHidden] = useState(false);
    const [cmdOpen, setCmdOpen] = useState(false);
    const [toasts, setToasts] = useState([]);
    const [rules, setRules] = useState(S.rules);
    const [, force] = useState(0);
    useEffect(() => {
      document.documentElement.setAttribute("data-theme", theme);
      localStorage.setItem("solari-theme", theme);
    }, [theme]);
    const toast = useCallback((msg, icon) => {
      const id = Math.random().toString(36).slice(2);
      setToasts(t => [...t, {
        id,
        msg,
        icon
      }]);
      setTimeout(() => setToasts(t => t.filter(x => x.id !== id)), 2600);
    }, []);
    useEffect(() => {
      window.__solariToast = toast;
    }, [toast]);

    // keyboard: ⌘K / "/" open palette
    useEffect(() => {
      function onKey(e) {
        if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "k") {
          e.preventDefault();
          setCmdOpen(true);
        } else if (e.key === "/" && document.activeElement.tagName !== "INPUT") {
          e.preventDefault();
          setCmdOpen(true);
        }
      }
      window.addEventListener("keydown", onKey);
      return () => window.removeEventListener("keydown", onKey);
    }, []);

    // live tick — gentle moving sparklines + jittered load
    useEffect(() => {
      const iv = setInterval(() => {
        S.nodes.forEach(n => {
          if (n.state === "down") return;
          const base = n.cpuPct;
          let nv = base + (Math.random() - 0.5) * 9;
          nv = Math.max(0, Math.min(100, nv));
          n.hist.cpu = [...n.hist.cpu.slice(1), Math.round(nv)];
          n.hist.net = [...n.hist.net.slice(1), Math.max(0, Math.min(100, n.hist.net[n.hist.net.length - 1] + (Math.random() - 0.5) * 16))];
          n.cpuPct = Math.round(nv);
        });
        force(x => x + 1);
      }, 5000);
      return () => clearInterval(iv);
    }, []);
    const openNode = useCallback(nodeOrId => {
      const node = typeof nodeOrId === "string" ? S.nodes.find(n => n.nodeId === nodeOrId) : nodeOrId;
      if (node) {
        setRoute({
          name: "node",
          node
        });
        document.querySelector(".content") && (document.querySelector(".content").scrollTop = 0);
      }
    }, []);
    const go = useCallback(id => {
      setRoute({
        name: id
      });
      if (window.innerWidth <= 980) setNavHidden(true);
      const c = document.querySelector(".content");
      if (c) c.scrollTop = 0;
    }, []);
    const survey = useCallback((node, kind) => {
      if (kind === "config") toast(`Config push staged for ${node.name} → solariCtl`, "settings");else if (node) toast(`Survey requested — ${node.name} (SCP_MSG_SURVEY)`, "survey");else toast("Fleet survey dispatched to all vantages", "survey");
    }, [toast]);
    function toggleNav() {
      if (window.innerWidth <= 980) setNavHidden(h => !h);else setCollapsed(c => !c);
    }

    // command palette commands
    const commands = (() => {
      const cmds = [{
        id: "go-fleet",
        group: "Navigate",
        label: "Fleet Overview",
        icon: "overview",
        action: () => go("fleet")
      }, {
        id: "go-alerts",
        group: "Navigate",
        label: "Alerts & Tolerances",
        icon: "alerts",
        action: () => go("alerts"),
        sub: `${S.activeCrit + S.activeWarn} active`
      }, {
        id: "go-reach",
        group: "Navigate",
        label: "Reachability Matrix",
        icon: "reachability",
        action: () => go("reachability")
      }, {
        id: "go-topo",
        group: "Navigate",
        label: "Topology Map",
        icon: "topology",
        action: () => go("topology")
      }, {
        id: "go-disc",
        group: "Navigate",
        label: "Discovery",
        icon: "discovery",
        action: () => go("discovery"),
        sub: `${S.discovered.length} new`
      }, {
        id: "go-prov",
        group: "Navigate",
        label: "Provisioning",
        icon: "provision",
        action: () => go("provision"),
        sub: `${S.enrollments.length} pending`
      }, {
        id: "go-cfg",
        group: "Navigate",
        label: "Config & Rules",
        icon: "settings",
        action: () => go("settings")
      }, {
        id: "view-heat",
        group: "Actions",
        label: "Fleet: Heatmap view",
        icon: "grid",
        action: () => {
          setFleetView("heat");
          go("fleet");
        }
      }, {
        id: "view-table",
        group: "Actions",
        label: "Fleet: Table view",
        icon: "table",
        action: () => {
          setFleetView("table");
          go("fleet");
        }
      }, {
        id: "view-cards",
        group: "Actions",
        label: "Fleet: Cards view",
        icon: "cards",
        action: () => {
          setFleetView("cards");
          go("fleet");
        }
      }, {
        id: "survey-all",
        group: "Actions",
        label: "Survey entire fleet now",
        icon: "survey",
        action: () => survey(null)
      }, {
        id: "theme",
        group: "Actions",
        label: "Toggle dark / light theme",
        icon: theme === "dark" ? "sun" : "moon",
        action: () => setTheme(t => t === "dark" ? "light" : "dark")
      }];
      // node jump targets — prioritise problem nodes, bound the list
      const problem = S.nodes.filter(n => n.state === "down" || n.state === "degraded");
      const healthy = S.nodes.filter(n => n.state === "up").slice(0, 40);
      [...problem, ...healthy].forEach(n => {
        cmds.push({
          id: "node-" + n.nodeId,
          group: "Jump to node",
          label: `${n.name}.akoria.net`,
          dot: n.state,
          sub: n.segName,
          keywords: n.role + " " + n.ip + " " + n.segName,
          action: () => openNode(n)
        });
      });
      return cmds;
    })();
    return /*#__PURE__*/React.createElement("div", {
      className: "app"
    }, !navHidden && window.innerWidth <= 980 && /*#__PURE__*/React.createElement("div", {
      className: "scrim",
      onClick: () => setNavHidden(true)
    }), /*#__PURE__*/React.createElement(Sidebar, {
      active: route.name === "node" ? "fleet" : route.name,
      onNav: go,
      collapsed: collapsed,
      onToggle: () => setCollapsed(c => !c),
      hidden: navHidden,
      summary: S.summary,
      activeCrit: S.activeCrit
    }), /*#__PURE__*/React.createElement("div", {
      className: "main"
    }, /*#__PURE__*/React.createElement(TopBar, {
      onMenu: toggleNav,
      onOpenCmd: () => setCmdOpen(true),
      theme: theme,
      onToggleTheme: () => setTheme(t => t === "dark" ? "light" : "dark"),
      server: S.server,
      onSurvey: () => survey(null)
    }), /*#__PURE__*/React.createElement("div", {
      className: "content"
    }, route.name === "fleet" && /*#__PURE__*/React.createElement(FleetOverview, {
      onOpenNode: openNode,
      view: fleetView,
      setView: setFleetView,
      fleet: S.nodes
    }), route.name === "node" && /*#__PURE__*/React.createElement(NodeDetail, {
      node: route.node,
      onBack: () => setRoute({
        name: "fleet"
      }),
      onSurvey: survey
    }), route.name === "alerts" && /*#__PURE__*/React.createElement(AlertsScreen, {
      onOpenNode: openNode,
      rules: rules,
      setRules: setRules,
      toast: toast
    }), route.name === "reachability" && /*#__PURE__*/React.createElement(Reachability, {
      onOpenNode: openNode
    }), route.name === "topology" && /*#__PURE__*/React.createElement(Topology, {
      onOpenNode: openNode
    }), route.name === "discovery" && /*#__PURE__*/React.createElement(Discovery, {
      onOpenNode: openNode
    }), route.name === "provision" && /*#__PURE__*/React.createElement(Provisioning, {
      onOpenNode: openNode
    }), route.name === "settings" && /*#__PURE__*/React.createElement(ConfigScreen, {
      onNav: go
    }))), /*#__PURE__*/React.createElement(CommandPalette, {
      open: cmdOpen,
      onClose: () => setCmdOpen(false),
      commands: commands
    }), /*#__PURE__*/React.createElement(Toasts, {
      items: toasts
    }));
  }
  ReactDOM.createRoot(document.getElementById("root")).render(/*#__PURE__*/React.createElement(App, null));
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "src/js/07-f83da9fd.jsx", error: String((e && e.message) || e) }); }

// ui_kits/monitoring-dashboard/AlertsScreen.jsx
try { (() => {
/* Alerts & Tolerances — the triage list plus the rules that produce it. */
(function () {
  const {
    useState
  } = React;
  const {
    PageHeader,
    Chip,
    AlertRow,
    Panel,
    Switch,
    StatusPill,
    Button,
    EmptyState,
    MetricCard
  } = window.SolariNetDesignSystem_95eca0;
  const D = window.SN_DATA;
  function AlertsScreen({
    onOpenSystem,
    onToast
  }) {
    const [sev, setSev] = useState("all");
    const [rules, setRules] = useState(D.rules);
    const active = D.alerts.filter(a => !a.cleared);
    const shown = active.filter(a => sev === "all" || a.severity === sev);
    const counts = {
      crit: active.filter(a => a.severity === "crit").length,
      warn: active.filter(a => a.severity === "warn").length,
      info: active.filter(a => a.severity === "info").length
    };
    return /*#__PURE__*/React.createElement("div", {
      className: "sn-page"
    }, /*#__PURE__*/React.createElement(PageHeader, {
      title: "Alerts & Tolerances",
      meta: active.length + " active · " + counts.crit + " critical · " + counts.warn + " degraded",
      actions: /*#__PURE__*/React.createElement(Button, {
        icon: "check",
        onClick: () => onToast("All visible alerts acknowledged", "check")
      }, "Acknowledge all")
    }), /*#__PURE__*/React.createElement("div", {
      style: {
        display: "grid",
        gridTemplateColumns: "repeat(4, minmax(0,1fr))",
        gap: "var(--sn-card-gap)",
        marginBottom: 20
      }
    }, /*#__PURE__*/React.createElement(MetricCard, {
      label: "Active",
      value: active.length,
      caption: "unacknowledged"
    }), /*#__PURE__*/React.createElement(MetricCard, {
      label: "Critical",
      value: counts.crit,
      caption: "reachability or heartbeat lost",
      state: counts.crit ? "down" : "nominal"
    }), /*#__PURE__*/React.createElement(MetricCard, {
      label: "Degraded",
      value: counts.warn,
      caption: "over tolerance",
      state: counts.warn ? "degraded" : "nominal"
    }), /*#__PURE__*/React.createElement(MetricCard, {
      label: "Rules",
      value: rules.filter(r => r.enabled).length,
      unit: "/" + rules.length,
      caption: "enabled"
    })), /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        gap: 8,
        marginBottom: 16,
        flexWrap: "wrap"
      }
    }, /*#__PURE__*/React.createElement(Chip, {
      active: sev === "all",
      count: active.length,
      onClick: () => setSev("all")
    }, "All"), /*#__PURE__*/React.createElement(Chip, {
      active: sev === "crit",
      state: "down",
      count: counts.crit,
      onClick: () => setSev("crit")
    }, "Critical"), /*#__PURE__*/React.createElement(Chip, {
      active: sev === "warn",
      state: "degraded",
      count: counts.warn,
      onClick: () => setSev("warn")
    }, "Degraded"), /*#__PURE__*/React.createElement(Chip, {
      active: sev === "info",
      count: counts.info,
      onClick: () => setSev("info")
    }, "Info")), /*#__PURE__*/React.createElement("div", {
      style: {
        display: "grid",
        gridTemplateColumns: "minmax(0,1.3fr) minmax(0,1fr)",
        gap: 16,
        alignItems: "start"
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        flexDirection: "column",
        gap: 9
      }
    }, shown.length === 0 ? /*#__PURE__*/React.createElement(EmptyState, {
      kind: "filtered",
      icon: "filter",
      title: "No alerts at this severity",
      description: "Nothing is currently firing in this band.",
      actionLabel: "Clear filter",
      onAction: () => setSev("all")
    }) : shown.map(a => /*#__PURE__*/React.createElement(AlertRow, {
      key: a.id,
      severity: a.severity,
      title: a.title,
      detail: a.detail,
      meta: a.meta,
      onClick: a.systemId ? () => onOpenSystem(D.systems.find(s => s.id === a.systemId)) : undefined
    }))), /*#__PURE__*/React.createElement(Panel, {
      title: "Tolerance rules",
      icon: "settings",
      right: rules.length + " defined",
      padded: false
    }, rules.map((r, i) => /*#__PURE__*/React.createElement("div", {
      key: r.id,
      style: {
        display: "grid",
        gridTemplateColumns: "1fr auto auto",
        gap: 12,
        alignItems: "center",
        padding: "12px 16px",
        borderBottom: i === rules.length - 1 ? "none" : "1px solid var(--sn-border-hairline)"
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        minWidth: 0
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        fontWeight: 600,
        fontSize: 13
      }
    }, r.name), /*#__PURE__*/React.createElement("div", {
      style: {
        fontFamily: "var(--sn-font-mono)",
        fontSize: 11,
        color: "var(--sn-text-secondary)",
        marginTop: 2
      }
    }, r.condition), /*#__PURE__*/React.createElement("div", {
      style: {
        fontFamily: "var(--sn-font-mono)",
        fontSize: 10.5,
        color: "var(--sn-text-tertiary)",
        marginTop: 2
      }
    }, r.scope)), /*#__PURE__*/React.createElement(StatusPill, {
      severity: r.severity
    }), /*#__PURE__*/React.createElement(Switch, {
      checked: r.enabled,
      label: "Enable " + r.name,
      onChange: v => {
        setRules(rs => rs.map(x => x.id === r.id ? {
          ...x,
          enabled: v
        } : x));
        onToast(r.name + (v ? " enabled" : " disabled"), "settings");
      }
    }))))));
  }
  Object.assign(window, {
    AlertsScreen
  });
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/monitoring-dashboard/AlertsScreen.jsx", error: String((e && e.message) || e) }); }

// ui_kits/monitoring-dashboard/App.jsx
try { (() => {
/* SolariNet Monitoring — app shell. Rail + top bar + routed content, plus the
   command palette and toast stack that live above every screen. */
(function () {
  const {
    useState,
    useEffect,
    useCallback
  } = React;
  const {
    SidebarNav,
    TopBar,
    IconButton,
    CommandPalette,
    ToastStack,
    EmptyState
  } = window.SolariNetDesignSystem_95eca0;
  const {
    FleetOverview,
    SystemDetail,
    AlertsScreen,
    ReachabilityScreen,
    DiscoveryScreen
  } = window;
  const D = window.SN_DATA;
  const NAV = [{
    group: "Monitor"
  }, {
    id: "fleet",
    label: "Fleet Overview",
    icon: "overview"
  }, {
    id: "reachability",
    label: "Reachability",
    icon: "reachability"
  }, {
    id: "topology",
    label: "Topology Map",
    icon: "topology"
  }, {
    id: "alerts",
    label: "Alerts",
    icon: "alerts"
  }, {
    group: "Manage"
  }, {
    id: "discovery",
    label: "Discovery",
    icon: "discovery"
  }, {
    id: "provision",
    label: "Provisioning",
    icon: "provision"
  }, {
    id: "settings",
    label: "Config & Rules",
    icon: "settings"
  }];
  function App() {
    const [theme, setTheme] = useState(() => localStorage.getItem("sn-kit-theme") || "dark");
    const [route, setRoute] = useState({
      name: "fleet"
    });
    const [view, setView] = useState("heat");
    const [collapsed, setCollapsed] = useState(false);
    const [drawerHidden, setDrawerHidden] = useState(true);
    const [narrow, setNarrow] = useState(false);
    const [paletteOpen, setPaletteOpen] = useState(false);
    const [toasts, setToasts] = useState([]);
    useEffect(() => {
      document.documentElement.setAttribute("data-sn-theme", theme);
      localStorage.setItem("sn-kit-theme", theme);
    }, [theme]);
    const toast = useCallback((message, icon) => {
      const id = Math.random().toString(36).slice(2);
      setToasts(t => [...t, {
        id,
        message,
        icon
      }]);
      setTimeout(() => setToasts(t => t.filter(x => x.id !== id)), 2600);
    }, []);

    /* Breakpoints ported from the source build: rail auto-collapses under 1200px
       (tablet landscape, the primary target) and becomes a fixed overlay drawer
       with a scrim under 980px (tablet portrait). */
    useEffect(() => {
      function measure() {
        const w = window.innerWidth;
        const isNarrow = w <= 980;
        setNarrow(isNarrow);
        setCollapsed(!isNarrow && w < 1200);
        if (!isNarrow) setDrawerHidden(true);
      }
      measure();
      window.addEventListener("resize", measure);
      return () => window.removeEventListener("resize", measure);
    }, []);
    function toggleNav() {
      if (narrow) setDrawerHidden(h => !h);else setCollapsed(c => !c);
    }
    useEffect(() => {
      function onKey(e) {
        if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "k") {
          e.preventDefault();
          setPaletteOpen(true);
        } else if (e.key === "/" && document.activeElement.tagName !== "INPUT") {
          e.preventDefault();
          setPaletteOpen(true);
        }
      }
      window.addEventListener("keydown", onKey);
      return () => window.removeEventListener("keydown", onKey);
    }, []);
    const go = useCallback(id => {
      setRoute({
        name: id
      });
      if (window.innerWidth <= 980) setDrawerHidden(true);
      const c = document.querySelector(".sn-content");
      if (c) c.scrollTop = 0;
    }, []);
    const openSystem = useCallback(system => {
      if (!system) return;
      setRoute({
        name: "system",
        system
      });
      const c = document.querySelector(".sn-content");
      if (c) c.scrollTop = 0;
    }, []);
    const activeCrit = D.alerts.filter(a => a.severity === "crit" && !a.cleared).length;
    const items = NAV.map(n => n.id === "alerts" ? {
      ...n,
      badge: activeCrit,
      badgeTone: "crit"
    } : n.id === "discovery" ? {
      ...n,
      badge: D.discovered.length
    } : n);
    const commands = [{
      id: "c1",
      group: "Navigate",
      label: "Fleet Overview",
      icon: "overview",
      action: () => go("fleet")
    }, {
      id: "c2",
      group: "Navigate",
      label: "Reachability",
      icon: "reachability",
      action: () => go("reachability")
    }, {
      id: "c3",
      group: "Navigate",
      label: "Alerts & Tolerances",
      icon: "alerts",
      sub: activeCrit + " critical",
      action: () => go("alerts")
    }, {
      id: "c4",
      group: "Navigate",
      label: "Discovery",
      icon: "discovery",
      sub: D.discovered.length + " new",
      action: () => go("discovery")
    }, {
      id: "c5",
      group: "Actions",
      label: "Fleet: Heatmap view",
      icon: "grid",
      action: () => {
        setView("heat");
        go("fleet");
      }
    }, {
      id: "c6",
      group: "Actions",
      label: "Fleet: Table view",
      icon: "table",
      action: () => {
        setView("table");
        go("fleet");
      }
    }, {
      id: "c7",
      group: "Actions",
      label: "Fleet: Cards view",
      icon: "cards",
      action: () => {
        setView("cards");
        go("fleet");
      }
    }, {
      id: "c8",
      group: "Actions",
      label: "Survey entire fleet now",
      icon: "survey",
      action: () => toast("Fleet survey dispatched to all vantages", "survey")
    }, {
      id: "c9",
      group: "Actions",
      label: "Toggle dark / light theme",
      icon: theme === "dark" ? "sun" : "moon",
      action: () => setTheme(t => t === "dark" ? "light" : "dark")
    }, ...[...D.systems.filter(s => s.state === "down" || s.state === "degraded"), ...D.systems.filter(s => s.state === "up").slice(0, 30)].map(s => ({
      id: "n-" + s.id,
      group: "Jump to system",
      label: s.name,
      state: s.state,
      sub: s.poolName,
      keywords: s.ip + " " + s.role + " " + s.os,
      action: () => openSystem(s)
    }))];
    return /*#__PURE__*/React.createElement("div", {
      className: "sn-app"
    }, narrow && !drawerHidden ? /*#__PURE__*/React.createElement("div", {
      className: "sn-scrim",
      onClick: () => setDrawerHidden(true)
    }) : null, /*#__PURE__*/React.createElement(SidebarNav, {
      items: items,
      active: route.name === "system" ? "fleet" : route.name,
      onNavigate: go,
      style: narrow ? {
        position: "fixed",
        top: 0,
        bottom: 0,
        left: 0,
        boxShadow: "var(--sn-shadow-panel)",
        transform: drawerHidden ? "translateX(-100%)" : "none",
        transition: "transform var(--sn-dur-slow) var(--sn-ease)"
      } : null,
      collapsed: narrow ? false : collapsed,
      onToggleCollapse: toggleNav,
      summary: [{
        label: "Systems",
        value: D.fleetRoll.total
      }, {
        label: "Applications",
        value: D.applications
      }, {
        label: "Uptime",
        value: D.controller.uptime
      }, {
        label: "Version",
        value: "v" + D.controller.version
      }]
    }), /*#__PURE__*/React.createElement("div", {
      style: {
        flex: "1 1 auto",
        minWidth: 0,
        display: "flex",
        flexDirection: "column"
      }
    }, /*#__PURE__*/React.createElement(TopBar, {
      onMenu: toggleNav,
      onOpenSearch: () => setPaletteOpen(true),
      controller: D.controller,
      actions: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement(IconButton, {
        icon: "survey",
        label: "Survey fleet now",
        onClick: () => toast("Fleet survey dispatched to all vantages", "survey")
      }), /*#__PURE__*/React.createElement(IconButton, {
        icon: theme === "dark" ? "sun" : "moon",
        label: "Toggle theme",
        onClick: () => setTheme(t => t === "dark" ? "light" : "dark")
      }))
    }), /*#__PURE__*/React.createElement("div", {
      className: "sn-content"
    }, route.name === "fleet" ? /*#__PURE__*/React.createElement(FleetOverview, {
      view: view,
      setView: setView,
      onOpenSystem: openSystem
    }) : null, route.name === "system" ? /*#__PURE__*/React.createElement(SystemDetail, {
      system: route.system,
      onBack: () => go("fleet"),
      onToast: toast
    }) : null, route.name === "alerts" ? /*#__PURE__*/React.createElement(AlertsScreen, {
      onOpenSystem: openSystem,
      onToast: toast
    }) : null, route.name === "reachability" ? /*#__PURE__*/React.createElement(ReachabilityScreen, null) : null, route.name === "discovery" ? /*#__PURE__*/React.createElement(DiscoveryScreen, {
      onToast: toast
    }) : null, ["topology", "provision", "settings"].includes(route.name) ? /*#__PURE__*/React.createElement("div", {
      className: "sn-page"
    }, /*#__PURE__*/React.createElement(EmptyState, {
      kind: "not-configured",
      icon: route.name === "topology" ? "topology" : route.name === "provision" ? "provision" : "settings",
      title: route.name === "topology" ? "Topology Map" : route.name === "provision" ? "Provisioning" : "Config & Rules",
      description: "Not recreated in this UI kit \u2014 the source build ships these screens, but they are outside the five surfaces reproduced here."
    })) : null)), /*#__PURE__*/React.createElement(CommandPalette, {
      open: paletteOpen,
      onClose: () => setPaletteOpen(false),
      commands: commands
    }), /*#__PURE__*/React.createElement(ToastStack, {
      items: toasts
    }));
  }
  ReactDOM.createRoot(document.getElementById("root")).render(/*#__PURE__*/React.createElement(App, null));
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/monitoring-dashboard/App.jsx", error: String((e && e.message) || e) }); }

// ui_kits/monitoring-dashboard/DiscoveryScreen.jsx
try { (() => {
/* Discovery — hosts found on the wire but not yet enrolled. */
(function () {
  const {
    useState
  } = React;
  const {
    PageHeader,
    Panel,
    Button,
    Tag,
    Modal,
    MetricCard,
    EmptyState,
    Icon,
    StatusDot
  } = window.SolariNetDesignSystem_95eca0;
  const D = window.SN_DATA;
  function DiscoveryScreen({
    onToast
  }) {
    const [pending, setPending] = useState(D.discovered);
    const [enrolling, setEnrolling] = useState(null);
    function enrol(host) {
      setPending(p => p.filter(x => x.host !== host.host));
      setEnrolling(null);
      onToast(host.host + " enrolled into Compute · 2 vantages assigned", "check");
    }
    return /*#__PURE__*/React.createElement("div", {
      className: "sn-page"
    }, /*#__PURE__*/React.createElement(PageHeader, {
      title: "Discovery",
      meta: pending.length + " found · not yet monitored · mDNS · ARP · SCP advert",
      actions: /*#__PURE__*/React.createElement(Button, {
        variant: "primary",
        icon: "refresh",
        onClick: () => onToast("Discovery sweep dispatched", "survey")
      }, "Sweep now")
    }), /*#__PURE__*/React.createElement("div", {
      style: {
        display: "grid",
        gridTemplateColumns: "repeat(4, minmax(0,1fr))",
        gap: "var(--sn-card-gap)",
        marginBottom: 20
      }
    }, /*#__PURE__*/React.createElement(MetricCard, {
      label: "Discovered",
      value: pending.length,
      caption: "awaiting enrolment"
    }), /*#__PURE__*/React.createElement(MetricCard, {
      label: "Enrolled",
      value: D.fleetRoll.total,
      caption: "under monitoring"
    }), /*#__PURE__*/React.createElement(MetricCard, {
      label: "Segments swept",
      value: D.pools.length,
      caption: "CIDR ranges configured"
    }), /*#__PURE__*/React.createElement(MetricCard, {
      label: "Last sweep",
      value: "6m",
      caption: "ago \xB7 every 15m"
    })), /*#__PURE__*/React.createElement(Panel, {
      title: "Found on the wire",
      icon: "discovery",
      right: pending.length + " hosts",
      padded: false
    }, pending.length === 0 ? /*#__PURE__*/React.createElement(EmptyState, {
      kind: "no-data",
      icon: "discovery",
      title: "Nothing new on the wire",
      description: "Every host answering in the configured segments is already enrolled. The next sweep runs in 9 minutes."
    }) : pending.map((h, i) => /*#__PURE__*/React.createElement("div", {
      key: h.host,
      style: {
        display: "flex",
        alignItems: "center",
        gap: 14,
        padding: "13px 16px",
        borderBottom: i === pending.length - 1 ? "none" : "1px solid var(--sn-border-hairline)"
      }
    }, /*#__PURE__*/React.createElement(StatusDot, {
      state: "unknown",
      size: 10
    }), /*#__PURE__*/React.createElement("div", {
      style: {
        flex: "1 1 auto",
        minWidth: 0
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        fontFamily: "var(--sn-font-mono)",
        fontWeight: 600,
        fontSize: 13.5
      }
    }, h.host), /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        gap: 6,
        flexWrap: "wrap",
        marginTop: 5
      }
    }, h.services.map(s => /*#__PURE__*/React.createElement(Tag, {
      key: s
    }, s)))), /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        flexDirection: "column",
        alignItems: "flex-end",
        gap: 4,
        flex: "0 0 auto",
        fontFamily: "var(--sn-font-mono)",
        fontSize: 11,
        color: "var(--sn-text-secondary)"
      }
    }, /*#__PURE__*/React.createElement("span", null, h.ip), /*#__PURE__*/React.createElement("span", {
      style: {
        color: "var(--sn-text-tertiary)"
      }
    }, "via ", h.via, " \xB7 ", h.seen, "m ago")), /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        gap: 8,
        flex: "0 0 auto"
      }
    }, /*#__PURE__*/React.createElement(Button, {
      variant: "ghost",
      size: "sm"
    }, "Ignore"), /*#__PURE__*/React.createElement(Button, {
      variant: "primary",
      size: "sm",
      icon: "plus",
      onClick: () => setEnrolling(h)
    }, "Enrol"))))), /*#__PURE__*/React.createElement(Modal, {
      open: !!enrolling,
      title: enrolling ? "Enrol " + enrolling.host : "",
      icon: "provision",
      onClose: () => setEnrolling(null),
      footer: /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement(Button, {
        variant: "ghost",
        onClick: () => setEnrolling(null)
      }, "Cancel"), /*#__PURE__*/React.createElement(Button, {
        variant: "primary",
        icon: "check",
        onClick: () => enrol(enrolling)
      }, "Enrol system"))
    }, enrolling ? /*#__PURE__*/React.createElement("div", {
      style: {
        fontSize: 13.5,
        lineHeight: 1.45,
        color: "var(--sn-text-secondary)"
      }
    }, /*#__PURE__*/React.createElement("p", {
      style: {
        margin: 0
      }
    }, enrolling.host, " answered on ", enrolling.ip, " advertising ", enrolling.services.join(", "), ", discovered via ", enrolling.via, "."), /*#__PURE__*/React.createElement("p", {
      style: {
        marginTop: 12
      }
    }, "Enrolling issues a single-use token, assigns the system to the ", /*#__PURE__*/React.createElement("b", {
      style: {
        color: "var(--sn-text-primary)"
      }
    }, "Compute"), " pool and puts two vantages onto it. Its applications become watched targets at the default tolerances."), /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        gap: 8,
        marginTop: 14,
        flexWrap: "wrap"
      }
    }, enrolling.services.map(s => /*#__PURE__*/React.createElement(Tag, {
      key: s,
      tone: "accent"
    }, s)))) : null));
  }
  Object.assign(window, {
    DiscoveryScreen
  });
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/monitoring-dashboard/DiscoveryScreen.jsx", error: String((e && e.message) || e) }); }

// ui_kits/monitoring-dashboard/FleetOverview.jsx
try { (() => {
/* Fleet Overview — the product's default view. Three renderings of one fleet:
   heatmap of tiles, sortable table, pool cards. */
(function () {
  const {
    useState,
    useMemo
  } = React;
  const {
    PageHeader,
    SegmentedControl,
    Chip,
    MetricCard,
    NodeTile,
    DataTable,
    PoolCard,
    StatusCell,
    MetricBar,
    Heartbeat,
    Tag,
    Icon,
    EmptyState,
    StatusDot
  } = window.SolariNetDesignSystem_95eca0;
  const D = window.SN_DATA;

  /* The roll strip is derived from the full six-state list, so no member can be
     silently dropped from a pool's count — one count per concept, rendered from
     the same source as the tiles beneath it. */
  const ROLL_STATES = [["down", "down"], ["degraded", "degraded"], ["maintenance", "maintenance"], ["unknown", "unknown"], ["up", "operational"]];
  const VIEWS = [{
    value: "heat",
    label: "Heatmap",
    icon: "grid"
  }, {
    value: "table",
    label: "Table",
    icon: "table"
  }, {
    value: "cards",
    label: "Cards",
    icon: "cards"
  }];
  const ROLE_ICON = {
    server: "server",
    monitor: "monitor",
    client: "host"
  };
  function FleetOverview({
    view,
    setView,
    onOpenSystem
  }) {
    const [stateFilter, setStateFilter] = useState("all");
    const [roleFilter, setRoleFilter] = useState("all");
    const [detailed, setDetailed] = useState(false);
    const [sort, setSort] = useState({
      key: "state",
      dir: 1
    });
    const filtered = useMemo(() => D.systems.filter(s => (stateFilter === "all" || s.state === stateFilter) && (roleFilter === "all" || s.role === roleFilter)), [stateFilter, roleFilter]);
    const roll = D.fleetRoll;
    const live = D.systems.filter(s => s.state !== "down" && s.state !== "unknown");
    const avgCpu = Math.round(live.reduce((a, s) => a + s.cpu, 0) / Math.max(1, live.length));
    const monsUp = D.monitors.filter(m => m.state === "up").length;
    return /*#__PURE__*/React.createElement("div", {
      className: "sn-page"
    }, /*#__PURE__*/React.createElement(PageHeader, {
      title: "Fleet Overview",
      meta: roll.total + " systems · " + D.applications + " applications · " + D.pools.length + " pools · live",
      actions: /*#__PURE__*/React.createElement(SegmentedControl, {
        value: view,
        onChange: setView,
        options: VIEWS
      })
    }), /*#__PURE__*/React.createElement("div", {
      style: {
        display: "grid",
        gridTemplateColumns: "repeat(4, minmax(0,1fr))",
        gap: "var(--sn-card-gap)",
        marginBottom: "var(--sn-card-gap)"
      }
    }, /*#__PURE__*/React.createElement(MetricCard, {
      label: "Systems",
      value: roll.total,
      caption: "monitored hosts"
    }), /*#__PURE__*/React.createElement(MetricCard, {
      label: "Operational",
      value: roll.up,
      caption: Math.round(roll.up / roll.total * 100) + "% in tolerance"
    }), /*#__PURE__*/React.createElement(MetricCard, {
      label: "Degraded",
      value: roll.degraded,
      caption: "over tolerance",
      state: roll.degraded ? "degraded" : "nominal"
    }), /*#__PURE__*/React.createElement(MetricCard, {
      label: "Down",
      value: roll.down,
      caption: D.alerts.filter(a => a.severity === "crit" && !a.cleared).length + " critical alerts",
      state: roll.down ? "down" : "nominal"
    })), /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        gap: "var(--sn-card-gap)",
        marginBottom: 20
      }
    }, /*#__PURE__*/React.createElement(MetricCard, {
      compact: true,
      label: "Avg CPU",
      value: avgCpu + "%",
      caption: "across reporting hosts",
      style: {
        flex: "1 1 200px"
      }
    }), /*#__PURE__*/React.createElement(MetricCard, {
      compact: true,
      label: "Vantages",
      value: monsUp,
      unit: "/" + D.monitors.length,
      caption: "monitors online",
      style: {
        flex: "1 1 200px"
      }
    })), /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        alignItems: "center",
        gap: 8,
        flexWrap: "wrap",
        marginBottom: 16
      }
    }, /*#__PURE__*/React.createElement(Chip, {
      active: stateFilter === "all",
      count: roll.total,
      onClick: () => setStateFilter("all")
    }, "All"), ["up", "degraded", "down", "maintenance", "unknown"].map(k => /*#__PURE__*/React.createElement(Chip, {
      key: k,
      state: k,
      count: roll[k] || 0,
      active: stateFilter === k,
      onClick: () => setStateFilter(k)
    }, k === "up" ? "Operational" : k[0].toUpperCase() + k.slice(1))), /*#__PURE__*/React.createElement("span", {
      style: {
        width: 1,
        height: 24,
        background: "var(--sn-border-hairline)",
        margin: "0 4px"
      }
    }), /*#__PURE__*/React.createElement(Chip, {
      active: roleFilter === "all",
      onClick: () => setRoleFilter("all")
    }, "All roles"), ["client", "monitor", "server"].map(k => /*#__PURE__*/React.createElement(Chip, {
      key: k,
      icon: ROLE_ICON[k],
      active: roleFilter === k,
      onClick: () => setRoleFilter(k)
    }, k[0].toUpperCase() + k.slice(1) + "s")), view === "heat" ? /*#__PURE__*/React.createElement(Chip, {
      icon: detailed ? "grid" : "cards",
      style: {
        marginLeft: "auto"
      },
      onClick: () => setDetailed(d => !d)
    }, detailed ? "Compact" : "Detailed") : null), filtered.length === 0 ? /*#__PURE__*/React.createElement(EmptyState, {
      kind: "filtered",
      icon: "filter",
      title: "No systems match this filter",
      description: "248 systems are enrolled; none of them are in this state and role.",
      actionLabel: "Clear filter",
      onAction: () => {
        setStateFilter("all");
        setRoleFilter("all");
      }
    }) : view === "heat" ? /*#__PURE__*/React.createElement(HeatView, {
      systems: filtered,
      detailed: detailed,
      onOpenSystem: onOpenSystem
    }) : view === "table" ? /*#__PURE__*/React.createElement(TableView, {
      systems: filtered,
      sort: sort,
      setSort: setSort,
      onOpenSystem: onOpenSystem
    }) : /*#__PURE__*/React.createElement(CardsView, {
      systems: filtered,
      onOpenSystem: onOpenSystem
    }));
  }
  function HeatView({
    systems,
    detailed,
    onOpenSystem
  }) {
    return /*#__PURE__*/React.createElement("div", null, D.pools.map(pool => {
      const members = systems.filter(s => s.poolId === pool.id);
      if (!members.length) return null;
      const r = D.roll(members);
      return /*#__PURE__*/React.createElement("div", {
        key: pool.id,
        style: {
          marginBottom: 22
        }
      }, /*#__PURE__*/React.createElement("div", {
        style: {
          display: "flex",
          alignItems: "center",
          gap: 12,
          marginBottom: 11
        }
      }, /*#__PURE__*/React.createElement("h3", {
        style: {
          margin: 0,
          fontSize: 14,
          fontWeight: 600,
          letterSpacing: "-.005em"
        }
      }, pool.name), /*#__PURE__*/React.createElement("span", {
        style: {
          fontFamily: "var(--sn-font-mono)",
          fontSize: 11,
          color: "var(--sn-text-tertiary)"
        }
      }, pool.cidr), /*#__PURE__*/React.createElement("span", {
        style: {
          flex: "1 1 auto",
          height: 1,
          background: "var(--sn-border-hairline)"
        }
      }), /*#__PURE__*/React.createElement("div", {
        style: {
          display: "flex",
          gap: 12,
          fontFamily: "var(--sn-font-mono)",
          fontSize: 10.5,
          color: "var(--sn-text-secondary)"
        }
      }, ROLL_STATES.filter(([k]) => r[k]).map(([k, label]) => /*#__PURE__*/React.createElement("span", {
        key: k,
        style: {
          display: "inline-flex",
          alignItems: "center",
          gap: 5
        }
      }, /*#__PURE__*/React.createElement(StatusDot, {
        state: k,
        size: 7
      }), r[k], " ", label)), /*#__PURE__*/React.createElement("span", {
        style: {
          color: "var(--sn-text-tertiary)"
        }
      }, r.total, " total"))), /*#__PURE__*/React.createElement("div", {
        style: {
          display: "grid",
          gridTemplateColumns: "repeat(auto-fill, minmax(" + (detailed ? 150 : 132) + "px, 1fr))",
          gap: "var(--sn-tile-gap)"
        }
      }, members.map(s => /*#__PURE__*/React.createElement(NodeTile, {
        key: s.id,
        name: s.name,
        state: s.state,
        detailed: detailed,
        meta: s.state === "maintenance" ? "maintenance · 2h" : s.state === "down" ? s.ip + " · " + s.age : s.state === "degraded" ? s.ip + " · cpu " + s.cpu + "%" : s.ip,
        history: s.state === "maintenance" || s.state === "unknown" ? null : s.hist.cpu,
        load: s.state === "unknown" || s.state === "maintenance" ? null : s.cpu,
        badge: s.alerts || null,
        onClick: () => onOpenSystem(s)
      }))));
    }));
  }
  const STATE_ORDER = {
    down: 0,
    degraded: 1,
    unknown: 2,
    maintenance: 3,
    up: 4
  };
  function TableView({
    systems,
    sort,
    setSort,
    onOpenSystem
  }) {
    const rows = useMemo(() => {
      const arr = [...systems];
      arr.sort((a, b) => {
        const va = sort.key === "state" ? STATE_ORDER[a.state] : a[sort.key];
        const vb = sort.key === "state" ? STATE_ORDER[b.state] : b[sort.key];
        return va < vb ? -sort.dir : va > vb ? sort.dir : 0;
      });
      return arr;
    }, [systems, sort]);
    return /*#__PURE__*/React.createElement(DataTable, {
      rows: rows,
      sort: sort,
      onRowClick: onOpenSystem,
      onSort: key => setSort(s => ({
        key,
        dir: s.key === key ? -s.dir : 1
      })),
      columns: [{
        key: "state",
        label: "State",
        render: r => /*#__PURE__*/React.createElement(StatusCell, {
          state: r.state
        })
      }, {
        key: "name",
        label: "System",
        render: r => /*#__PURE__*/React.createElement("span", {
          style: {
            display: "inline-flex",
            alignItems: "center",
            gap: 9,
            fontFamily: "var(--sn-font-mono)",
            fontWeight: 600
          }
        }, /*#__PURE__*/React.createElement(Icon, {
          name: ROLE_ICON[r.role],
          size: 15,
          style: {
            color: "var(--sn-text-tertiary)"
          }
        }), r.name, /*#__PURE__*/React.createElement("span", {
          style: {
            fontSize: 10,
            color: "var(--sn-text-tertiary)",
            fontWeight: 400
          }
        }, ".akoria.net"))
      }, {
        key: "role",
        label: "Role",
        render: r => /*#__PURE__*/React.createElement(Tag, null, r.role)
      }, {
        key: "poolName",
        label: "Pool",
        render: r => /*#__PURE__*/React.createElement("span", {
          style: {
            fontFamily: "var(--sn-font-mono)",
            fontSize: 12,
            color: "var(--sn-text-secondary)"
          }
        }, r.poolName, " ", /*#__PURE__*/React.createElement("span", {
          style: {
            color: "var(--sn-text-tertiary)"
          }
        }, r.ip))
      }, {
        key: "cpu",
        label: "CPU",
        align: "right",
        render: r => /*#__PURE__*/React.createElement(MetricBar, {
          pct: r.cpu
        })
      }, {
        key: "ram",
        label: "RAM",
        align: "right",
        render: r => /*#__PURE__*/React.createElement(MetricBar, {
          pct: r.ram
        })
      }, {
        key: "os",
        label: "OS",
        render: r => /*#__PURE__*/React.createElement("span", {
          style: {
            fontFamily: "var(--sn-font-mono)",
            fontSize: 12,
            color: "var(--sn-text-secondary)"
          }
        }, r.os, " ", /*#__PURE__*/React.createElement("span", {
          style: {
            color: "var(--sn-text-tertiary)"
          }
        }, r.arch))
      }, {
        key: "hb",
        label: "Heartbeat",
        render: r => /*#__PURE__*/React.createElement(Heartbeat, {
          age: r.age,
          missed: r.heartbeatMissed
        })
      }]
    });
  }
  function CardsView({
    systems,
    onOpenSystem
  }) {
    return /*#__PURE__*/React.createElement("div", {
      style: {
        display: "grid",
        gridTemplateColumns: "repeat(auto-fill, minmax(320px, 1fr))",
        gap: 14
      }
    }, D.pools.map(pool => {
      const members = systems.filter(s => s.poolId === pool.id);
      if (!members.length) return null;
      const r = D.roll(members);
      const avg = k => Math.round(members.reduce((a, s) => a + s[k], 0) / members.length);
      return /*#__PURE__*/React.createElement(PoolCard, {
        key: pool.id,
        name: pool.name,
        cidr: pool.cidr,
        description: pool.desc,
        roll: r,
        stats: [{
          label: "Operational",
          value: r.up,
          tone: "var(--sn-ok)"
        }, {
          label: "Issues",
          value: r.down + r.degraded,
          tone: r.down ? "var(--sn-crit)" : "var(--sn-warn)"
        }, {
          label: "Avg CPU",
          value: avg("cpu") + "%",
          tone: "auto"
        }, {
          label: "Avg RAM",
          value: avg("ram") + "%",
          tone: "auto"
        }],
        members: members.map(s => ({
          name: s.name,
          state: s.state,
          id: s.id
        })),
        onMemberClick: m => onOpenSystem(members.find(s => s.id === m.id))
      });
    }));
  }
  Object.assign(window, {
    FleetOverview
  });
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/monitoring-dashboard/FleetOverview.jsx", error: String((e && e.message) || e) }); }

// ui_kits/monitoring-dashboard/ReachabilityScreen.jsx
try { (() => {
/* Reachability — probe targets against monitor vantages. Divergence between
   vantages is the finding this screen exists to surface. */
(function () {
  const {
    useState
  } = React;
  const {
    PageHeader,
    Panel,
    StatusDot,
    StatusCell,
    Tag,
    RTTBars,
    MetricCard,
    Button,
    Chip
  } = window.SolariNetDesignSystem_95eca0;
  const D = window.SN_DATA;
  function cellTone(v) {
    if (v.outcome === "offline") return {
      fg: "var(--sn-unknown)",
      bg: "transparent",
      bd: "var(--sn-unknown)",
      dashed: true
    };
    if (v.outcome !== "ok") return {
      fg: "var(--sn-crit)",
      bg: "var(--sn-crit-tint)",
      bd: "var(--sn-crit)"
    };
    if (v.lossPermille > 0) return {
      fg: "var(--sn-warn)",
      bg: "var(--sn-warn-tint)",
      bd: "var(--sn-warn)"
    };
    return {
      fg: "var(--sn-text-secondary)",
      bg: "transparent",
      bd: "var(--sn-border-hairline)"
    };
  }
  function ReachabilityScreen() {
    const [sel, setSel] = useState(D.targets[2].id);
    /* Divergence is computed across reporting vantages only — an offline monitor
       cannot disagree about a path it never probed. */
    const rows = D.targets.map(t => {
      const vs = D.matrix[t.id];
      const reporting = vs.filter(v => v.outcome !== "offline");
      const fails = reporting.filter(v => v.outcome !== "ok").length;
      return {
        t,
        vs,
        reporting,
        fails,
        diverge: fails > 0 && fails < reporting.length
      };
    });
    const selRow = rows.find(r => r.t.id === sel);
    const reportingCount = D.monitors.filter(m => m.state === "up").length;
    const offline = D.monitors.length - reportingCount;
    const totalProbes = rows.length * reportingCount;
    const failing = rows.reduce((a, r) => a + r.fails, 0);
    return /*#__PURE__*/React.createElement("div", {
      className: "sn-page"
    }, /*#__PURE__*/React.createElement(PageHeader, {
      title: "Reachability",
      meta: D.targets.length + " targets × " + D.monitors.length + " vantages · 30s interval",
      actions: /*#__PURE__*/React.createElement(Button, {
        icon: "refresh"
      }, "Probe now")
    }), /*#__PURE__*/React.createElement("div", {
      style: {
        display: "grid",
        gridTemplateColumns: "repeat(4, minmax(0,1fr))",
        gap: "var(--sn-card-gap)",
        marginBottom: 20
      }
    }, /*#__PURE__*/React.createElement(MetricCard, {
      label: "Targets",
      value: D.targets.length,
      caption: "probed endpoints"
    }), /*#__PURE__*/React.createElement(MetricCard, {
      label: "Vantages",
      value: reportingCount,
      unit: "/" + D.monitors.length,
      caption: offline ? offline + " monitor offline" : "all monitors reporting",
      state: offline ? "degraded" : "nominal"
    }), /*#__PURE__*/React.createElement(MetricCard, {
      label: "Failing probes",
      value: failing,
      caption: "of " + totalProbes + " this interval",
      state: failing ? "degraded" : "nominal"
    }), /*#__PURE__*/React.createElement(MetricCard, {
      label: "Split vantage",
      value: rows.filter(r => r.diverge).length,
      caption: "targets disagree across vantages",
      state: rows.some(r => r.diverge) ? "degraded" : "nominal"
    })), /*#__PURE__*/React.createElement("div", {
      style: {
        display: "grid",
        gridTemplateColumns: "minmax(0,1.55fr) minmax(0,1fr)",
        gap: 16,
        alignItems: "start"
      }
    }, /*#__PURE__*/React.createElement(Panel, {
      title: "Probe matrix",
      icon: "reachability",
      right: "rtt \xB7 loss",
      padded: false
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        overflowX: "auto"
      }
    }, /*#__PURE__*/React.createElement("table", {
      style: {
        borderCollapse: "separate",
        borderSpacing: 0,
        width: "100%",
        fontVariantNumeric: "tabular-nums"
      }
    }, /*#__PURE__*/React.createElement("thead", null, /*#__PURE__*/React.createElement("tr", null, /*#__PURE__*/React.createElement("th", {
      style: {
        textAlign: "left",
        fontFamily: "var(--sn-font-mono)",
        fontSize: 10,
        letterSpacing: ".1em",
        textTransform: "uppercase",
        color: "var(--sn-text-tertiary)",
        padding: "10px 14px",
        background: "var(--sn-raised)",
        borderBottom: "1px solid var(--sn-border-hairline)",
        minWidth: 200
      }
    }, "Target"), D.monitors.map(m => /*#__PURE__*/React.createElement("th", {
      key: m.id,
      style: {
        padding: "9px 6px",
        background: "var(--sn-raised)",
        borderBottom: "1px solid var(--sn-border-hairline)",
        borderLeft: "1px solid var(--sn-border-hairline)",
        width: 74
      }
    }, /*#__PURE__*/React.createElement("span", {
      style: {
        display: "inline-flex",
        alignItems: "center",
        gap: 4,
        fontFamily: "var(--sn-font-mono)",
        fontSize: 10,
        color: "var(--sn-text-secondary)"
      }
    }, /*#__PURE__*/React.createElement(StatusDot, {
      state: m.state,
      size: 7
    }), m.name.slice(0, 8)))))), /*#__PURE__*/React.createElement("tbody", null, rows.map(({
      t,
      vs,
      diverge
    }) => /*#__PURE__*/React.createElement("tr", {
      key: t.id,
      onClick: () => setSel(t.id),
      style: {
        cursor: "pointer"
      }
    }, /*#__PURE__*/React.createElement("td", {
      style: {
        padding: "8px 14px",
        borderBottom: "1px solid var(--sn-border-hairline)",
        background: sel === t.id ? "var(--sn-surface-control-active)" : "transparent",
        boxShadow: sel === t.id ? "inset 3px 0 0 var(--sn-accent)" : diverge ? "inset 3px 0 0 var(--sn-warn)" : "none"
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        fontFamily: "var(--sn-font-mono)",
        fontWeight: 600,
        fontSize: 12
      }
    }, t.host), /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        alignItems: "center",
        gap: 6,
        marginTop: 2
      }
    }, /*#__PURE__*/React.createElement("span", {
      style: {
        fontFamily: "var(--sn-font-mono)",
        fontSize: 10,
        color: "var(--sn-text-tertiary)"
      }
    }, t.label, " \xB7 ", t.proto, t.port ? "/" + t.port : ""), diverge ? /*#__PURE__*/React.createElement(Tag, {
      tone: "neutral",
      style: {
        color: "var(--sn-warn)",
        borderColor: "var(--sn-warn)",
        background: "transparent"
      }
    }, "split") : null)), vs.map((v, i) => {
      const tone = cellTone(v);
      return /*#__PURE__*/React.createElement("td", {
        key: i,
        style: {
          padding: 5,
          borderBottom: "1px solid var(--sn-border-hairline)",
          borderLeft: "1px solid var(--sn-border-hairline)",
          textAlign: "center"
        }
      }, /*#__PURE__*/React.createElement("div", {
        style: {
          display: "flex",
          flexDirection: "column",
          alignItems: "center",
          justifyContent: "center",
          minHeight: 36,
          border: "1px " + (tone.dashed ? "dashed " : "solid ") + tone.bd,
          background: tone.bg,
          borderRadius: 5,
          opacity: tone.dashed ? .6 : 1,
          fontFamily: "var(--sn-font-mono)",
          color: tone.fg
        }
      }, /*#__PURE__*/React.createElement("span", {
        style: {
          fontSize: 11,
          fontWeight: 600
        }
      }, v.outcome === "ok" ? D.fmt.rtt(v.rttMicros) : "—"), /*#__PURE__*/React.createElement("span", {
        style: {
          fontSize: 8,
          opacity: .85
        }
      }, v.outcome === "ok" ? (v.lossPermille / 10).toFixed(1) + "%" : v.outcome === "offline" ? "offline" : "timeout")));
    }))))))), /*#__PURE__*/React.createElement(Panel, {
      title: selRow.t.host,
      icon: "pulse",
      right: selRow.t.proto + (selRow.t.port ? "/" + selRow.t.port : "")
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        alignItems: "center",
        gap: 12,
        marginBottom: 14
      }
    }, /*#__PURE__*/React.createElement(StatusCell, {
      state: selRow.fails === 0 ? "up" : selRow.diverge ? "degraded" : "down"
    }), /*#__PURE__*/React.createElement("span", {
      style: {
        fontFamily: "var(--sn-font-mono)",
        fontSize: 11.5,
        color: "var(--sn-text-secondary)"
      }
    }, selRow.reporting.length - selRow.fails, " of ", selRow.reporting.length, " reporting vantages answering", offline ? " · " + offline + " offline" : "")), /*#__PURE__*/React.createElement("div", {
      style: {
        fontFamily: "var(--sn-font-mono)",
        fontSize: 10,
        letterSpacing: ".12em",
        textTransform: "uppercase",
        color: "var(--sn-text-tertiary)",
        marginBottom: 8
      }
    }, "Round trip by vantage"), /*#__PURE__*/React.createElement(RTTBars, {
      vantages: selRow.reporting,
      height: 72
    }), /*#__PURE__*/React.createElement("div", {
      style: {
        fontSize: 13.5,
        lineHeight: 1.45,
        color: "var(--sn-text-secondary)",
        marginTop: 14
      }
    }, selRow.diverge ? "This target answers some vantages and not others — the fault is in the path, not the host." : selRow.fails ? "No vantage can reach this target. Treat it as down." : "Every vantage reaches this target inside tolerance."))));
  }
  Object.assign(window, {
    ReachabilityScreen
  });
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/monitoring-dashboard/ReachabilityScreen.jsx", error: String((e && e.message) || e) }); }

// ui_kits/monitoring-dashboard/SystemDetail.jsx
try { (() => {
/* System Detail — one host: identity, metrics history, resources, applications,
   and the alerts open against it. */
(function () {
  const {
    useState
  } = React;
  const {
    Panel,
    MetricCard,
    MetricRow,
    TimeSeries,
    RadialGauge,
    BandwidthGauge,
    DataTable,
    StatusCell,
    StatusPill,
    Heartbeat,
    Button,
    Tag,
    Icon,
    SegmentedControl,
    AlertRow
  } = window.SolariNetDesignSystem_95eca0;
  const D = window.SN_DATA;
  const ROLE_ICON = {
    server: "server",
    monitor: "monitor",
    client: "host"
  };
  const METRICS = [{
    value: "cpu",
    label: "CPU",
    icon: "cpu"
  }, {
    value: "ram",
    label: "Memory",
    icon: "ram"
  }, {
    value: "net",
    label: "Network",
    icon: "network"
  }, {
    value: "disk",
    label: "Disk",
    icon: "disk"
  }];
  const METRIC_COLOR = {
    cpu: "var(--sn-accent)",
    ram: "var(--sn-maint)",
    net: "var(--sn-ok)",
    disk: "var(--sn-warn)"
  };
  function SystemDetail({
    system,
    onBack,
    onToast
  }) {
    const [metric, setMetric] = useState("cpu");
    const s = system;
    const open = D.alerts.filter(a => a.systemId === s.id && !a.cleared);
    return /*#__PURE__*/React.createElement("div", {
      className: "sn-page"
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        alignItems: "center",
        gap: 12,
        marginBottom: 18,
        flexWrap: "wrap"
      }
    }, /*#__PURE__*/React.createElement(Button, {
      icon: "chevronLeft",
      onClick: onBack
    }, "Fleet"), /*#__PURE__*/React.createElement("span", {
      style: {
        fontFamily: "var(--sn-font-mono)",
        fontSize: 12,
        color: "var(--sn-text-tertiary)"
      }
    }, "/ ", s.poolName, " / ", s.fqdn), /*#__PURE__*/React.createElement("div", {
      style: {
        marginLeft: "auto",
        display: "flex",
        gap: 9
      }
    }, /*#__PURE__*/React.createElement(Button, {
      icon: "survey",
      onClick: () => onToast("Survey requested — " + s.name + " (SCP_MSG_SURVEY)", "survey")
    }, "Survey now"), /*#__PURE__*/React.createElement(Button, {
      icon: "settings",
      onClick: () => onToast("Config push staged for " + s.name, "settings")
    }, "Push config"))), /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        alignItems: "flex-start",
        gap: 18,
        flexWrap: "wrap",
        marginBottom: 20
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        alignItems: "center",
        gap: 14
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        width: 52,
        height: 52,
        flex: "0 0 52px",
        borderRadius: "var(--sn-radius-md)",
        display: "grid",
        placeItems: "center",
        border: "1px solid var(--sn-border-strong)",
        background: "var(--sn-raised)",
        color: "var(--sn-accent)"
      }
    }, /*#__PURE__*/React.createElement(Icon, {
      name: ROLE_ICON[s.role],
      size: 26
    })), /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("h1", {
      style: {
        margin: 0,
        fontFamily: "var(--sn-font-mono)",
        fontSize: 22,
        fontWeight: 600
      }
    }, s.name), /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        gap: 14,
        flexWrap: "wrap",
        marginTop: 4,
        fontFamily: "var(--sn-font-mono)",
        fontSize: 11.5,
        color: "var(--sn-text-secondary)"
      }
    }, /*#__PURE__*/React.createElement("span", null, s.ip), /*#__PURE__*/React.createElement("span", null, s.os, " \xB7 ", s.arch), /*#__PURE__*/React.createElement("span", null, s.cores, " cores"), /*#__PURE__*/React.createElement("span", null, "surveyed ", s.lastSurvey)))), /*#__PURE__*/React.createElement("div", {
      style: {
        marginLeft: "auto",
        display: "flex",
        alignItems: "center",
        gap: 16
      }
    }, /*#__PURE__*/React.createElement(StatusCell, {
      state: s.state
    }), /*#__PURE__*/React.createElement(Heartbeat, {
      age: s.age,
      missed: s.heartbeatMissed
    }), /*#__PURE__*/React.createElement(Tag, null, s.role))), open.length ? /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        flexDirection: "column",
        gap: 9,
        marginBottom: 16
      }
    }, open.map(a => /*#__PURE__*/React.createElement(AlertRow, {
      key: a.id,
      severity: a.severity,
      title: a.title,
      detail: a.detail,
      meta: a.meta
    }))) : null, /*#__PURE__*/React.createElement("div", {
      style: {
        display: "grid",
        gridTemplateColumns: "minmax(0,1.4fr) minmax(0,1fr)",
        gap: 16,
        marginBottom: 16
      }
    }, /*#__PURE__*/React.createElement(Panel, {
      title: "Metric history",
      icon: "activity",
      right: "40 samples \xB7 30s interval",
      bodyStyle: {
        padding: "0 16px 16px"
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        padding: "12px 0"
      }
    }, /*#__PURE__*/React.createElement(SegmentedControl, {
      value: metric,
      onChange: setMetric,
      options: METRICS
    })), /*#__PURE__*/React.createElement(TimeSeries, {
      data: s.hist[metric],
      max: 100,
      color: METRIC_COLOR[metric],
      height: 190
    })), /*#__PURE__*/React.createElement(Panel, {
      title: "Utilisation",
      icon: "cpu"
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        display: "flex",
        gap: 14,
        justifyContent: "space-around",
        flexWrap: "wrap"
      }
    }, /*#__PURE__*/React.createElement(RadialGauge, {
      value: s.cpu,
      label: "CPU",
      sub: "%",
      size: 104
    }), /*#__PURE__*/React.createElement(RadialGauge, {
      value: s.ram,
      label: "Memory",
      sub: "%",
      size: 104
    })), /*#__PURE__*/React.createElement("div", {
      style: {
        marginTop: 16,
        display: "flex",
        flexDirection: "column",
        gap: 12
      }
    }, s.ifaces.map(n => /*#__PURE__*/React.createElement(BandwidthGauge, {
      key: n.name,
      label: n.name + " rx",
      used: n.rxMbps,
      cap: n.capMbps,
      format: D.fmt.mbps
    }))))), /*#__PURE__*/React.createElement("div", {
      style: {
        display: "grid",
        gridTemplateColumns: "minmax(0,1fr) minmax(0,1.3fr)",
        gap: 16
      }
    }, /*#__PURE__*/React.createElement(Panel, {
      title: "Storage",
      icon: "disk",
      right: s.disks.length + " mounts"
    }, s.disks.map((d, i) => /*#__PURE__*/React.createElement(MetricRow, {
      key: d.mount,
      label: d.mount + " (" + d.fs + ")",
      pct: d.usedPct,
      value: Math.round(d.totalGb * d.usedPct / 100) + " / " + D.fmt.gb(d.totalGb),
      last: i === s.disks.length - 1
    }))), /*#__PURE__*/React.createElement(Panel, {
      title: "Applications",
      icon: "process",
      right: s.applications.length + " watched",
      padded: false
    }, /*#__PURE__*/React.createElement(DataTable, {
      style: {
        border: "none",
        borderRadius: 0
      },
      rows: s.applications.map((a, i) => ({
        ...a,
        id: i
      })),
      columns: [{
        key: "state",
        label: "State",
        render: r => /*#__PURE__*/React.createElement(StatusCell, {
          state: r.state
        })
      }, {
        key: "name",
        label: "Application",
        render: r => /*#__PURE__*/React.createElement("span", {
          style: {
            fontFamily: "var(--sn-font-mono)",
            fontWeight: 600
          }
        }, r.name)
      }, {
        key: "pid",
        label: "PID",
        align: "right",
        render: r => /*#__PURE__*/React.createElement("span", {
          style: {
            fontFamily: "var(--sn-font-mono)",
            color: "var(--sn-text-secondary)"
          }
        }, r.pid)
      }, {
        key: "cpu",
        label: "CPU",
        align: "right",
        render: r => /*#__PURE__*/React.createElement("span", {
          style: {
            fontFamily: "var(--sn-font-mono)"
          }
        }, r.cpu, "%")
      }, {
        key: "memMb",
        label: "RSS",
        align: "right",
        render: r => /*#__PURE__*/React.createElement("span", {
          style: {
            fontFamily: "var(--sn-font-mono)",
            color: "var(--sn-text-secondary)"
          }
        }, r.memMb, " MB")
      }]
    }))));
  }
  Object.assign(window, {
    SystemDetail
  });
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/monitoring-dashboard/SystemDetail.jsx", error: String((e && e.message) || e) }); }

// ui_kits/monitoring-dashboard/data.js
try { (() => {
/* SolariNet UI kit — deterministic mock fleet.
   Element names, segment CIDRs, OS list and service list are the product's own
   (lifted from the dashboard's mock data layer); the generator is seeded so the
   kit renders identically on every load. */
(function () {
  function mulberry32(a) {
    return function () {
      a |= 0;
      a = a + 0x6D2B79F5 | 0;
      let t = Math.imul(a ^ a >>> 15, 1 | a);
      t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t;
      return ((t ^ t >>> 14) >>> 0) / 4294967296;
    };
  }
  const rng = mulberry32(20260731);
  const rint = (lo, hi) => Math.floor(rng() * (hi - lo + 1)) + lo;
  const pick = a => a[Math.floor(rng() * a.length)];
  const ELEMENTS = "hydrogen helium lithium beryllium boron carbon nitrogen oxygen fluorine neon sodium magnesium aluminium silicon phosphorus sulfur chlorine argon potassium calcium scandium titanium vanadium chromium manganese iron cobalt nickel copper zinc gallium germanium arsenic selenium bromine krypton rubidium strontium yttrium zirconium niobium molybdenum technetium ruthenium rhodium palladium silver cadmium indium tin antimony tellurium iodine xenon caesium barium lanthanum cerium praseodymium neodymium promethium samarium europium gadolinium terbium dysprosium holmium erbium thulium ytterbium lutetium hafnium tantalum tungsten rhenium osmium iridium platinum gold mercury thallium lead bismuth".split(" ");
  const POOLS = [{
    id: "core",
    name: "Core",
    cidr: "10.42.0.0/24",
    desc: "Routers, gateways, infra services",
    count: 9
  }, {
    id: "compute",
    name: "Compute",
    cidr: "10.42.10.0/23",
    desc: "Hypervisors & app hosts",
    count: 22
  }, {
    id: "storage",
    name: "Storage",
    cidr: "10.42.20.0/24",
    desc: "NAS, object, backup",
    count: 8
  }, {
    id: "dmz",
    name: "DMZ",
    cidr: "10.42.30.0/24",
    desc: "Edge / reverse proxies",
    count: 6
  }, {
    id: "lab",
    name: "Lab",
    cidr: "10.42.40.0/23",
    desc: "ARM SBCs, experiments",
    count: 14
  }, {
    id: "iot",
    name: "IoT / OT",
    cidr: "10.42.50.0/24",
    desc: "Sensors, controllers",
    count: 11
  }];
  const OSES = ["Debian 12", "Ubuntu 24.04", "Alpine 3.20", "Raspberry Pi OS", "FreeBSD 14", "Windows Server 2022", "macOS 14"];
  const ARCHS = ["x86_64", "arm64", "armv7"];
  const SERVICES = ["sshd", "nginx", "mariadbd", "redis-server", "node_exporter", "haproxy", "postgres", "coredns", "dockerd", "vault", "step-ca", "apache2"];
  const MOUNTS = ["/", "/var", "/data", "/srv", "/backup"];
  const IFACES = ["eth0", "eth1", "wlan0", "bond0"];
  function spark(n, base, vol) {
    const out = [];
    let v = base;
    for (let i = 0; i < n; i++) {
      v = Math.max(2, Math.min(99, v + (rng() - .5) * vol));
      out.push(Math.round(v * 10) / 10);
    }
    return out;
  }
  const systems = [];
  let i = 0;
  POOLS.forEach(pool => {
    for (let k = 0; k < pool.count; k++) {
      const name = ELEMENTS[i % ELEMENTS.length];
      i++;
      const r = rng();
      const state = r < .845 ? "up" : r < .915 ? "degraded" : r < .955 ? "down" : r < .98 ? "maintenance" : "unknown";
      const role = k === 0 ? "server" : k < 3 ? "monitor" : "client";
      const down = state === "down",
        deg = state === "degraded";
      const cpu = down ? 0 : deg ? rint(76, 96) : rint(4, 58);
      const ram = down ? 0 : deg && rng() < .5 ? rint(84, 97) : rint(18, 74);
      const octet = rint(4, 250);
      systems.push({
        id: "sys-" + i,
        name,
        poolId: pool.id,
        poolName: pool.name,
        role,
        state,
        fqdn: name + ".akoria.net",
        ip: pool.cidr.split("/")[0].replace(/\.\d+$/, "." + octet),
        os: pick(OSES),
        arch: pick(ARCHS),
        cores: pick([2, 4, 4, 8, 8, 16, 32]),
        cpu,
        ram,
        disks: Array.from({
          length: rint(1, role === "server" ? 4 : 2)
        }, (_, d) => ({
          mount: MOUNTS[d] || "/mnt/d" + d,
          fs: pick(["ext4", "xfs", "zfs", "btrfs"]),
          totalGb: pick([128, 256, 512, 1024, 2048, 4096]),
          usedPct: deg && rng() < .5 ? rint(86, 98) : rint(16, 79)
        })),
        ifaces: Array.from({
          length: rint(1, 2)
        }, (_, n) => ({
          name: IFACES[n] || "eth" + n,
          capMbps: pick([1000, 1000, 2500, 10000]),
          rxMbps: down ? 0 : rint(1, 900),
          txMbps: down ? 0 : rint(1, 400)
        })),
        applications: Array.from({
          length: rint(2, 6)
        }, () => ({
          name: pick(SERVICES),
          pid: rint(400, 32000),
          cpu: +(rng() * 18).toFixed(1),
          memMb: rint(24, 2400),
          state: down ? "unknown" : rng() < .06 ? "degraded" : "up"
        })),
        hist: {
          cpu: spark(40, cpu || 6, deg ? 9 : 14),
          ram: spark(40, ram || 8, 7),
          net: spark(40, rint(10, 70), 18),
          disk: spark(40, rint(20, 80), 3)
        },
        heartbeatMissed: down ? 6 : state === "unknown" ? 8 : 0,
        age: down ? "4m" : state === "unknown" ? "—" : rint(3, 29) + "s",
        alerts: down ? 2 : deg ? 1 : 0,
        lastSurvey: rint(1, 58) + "s ago"
      });
    }
  });
  const roll = list => list.reduce((a, s) => {
    a[s.state] = (a[s.state] || 0) + 1;
    a.total++;
    return a;
  }, {
    total: 0,
    up: 0,
    degraded: 0,
    down: 0,
    maintenance: 0,
    unknown: 0
  });
  const fleetRoll = roll(systems);
  const applications = systems.reduce((a, s) => a + s.applications.length, 0);
  const alerts = [];
  systems.filter(s => s.state === "down").forEach(s => {
    alerts.push({
      id: "a" + alerts.length,
      severity: "crit",
      systemId: s.id,
      title: s.name + " unreachable",
      detail: s.poolName.toLowerCase() + " · all 2 vantages · " + s.age,
      meta: s.age
    });
    alerts.push({
      id: "a" + alerts.length,
      severity: "crit",
      systemId: s.id,
      title: "Heartbeat lost on " + s.name,
      detail: s.ip + " · 6 intervals missed",
      meta: s.age
    });
  });
  systems.filter(s => s.state === "degraded").forEach(s => {
    const worst = s.disks.find(d => d.usedPct > 85);
    alerts.push({
      id: "a" + alerts.length,
      severity: "warn",
      systemId: s.id,
      title: worst ? "Disk " + worst.mount + " over tolerance on " + s.name : "CPU over tolerance on " + s.name,
      detail: worst ? s.ip + " · " + worst.usedPct + "% of " + worst.totalGb + " GB" : s.ip + " · cpu " + s.cpu + "% for 12m",
      meta: rint(4, 50) + "m"
    });
  });
  alerts.push({
    id: "a" + alerts.length,
    severity: "info",
    systemId: null,
    title: "Enrolment token issued",
    detail: "expires in 24h · 1 use · issued by operator",
    meta: "2h",
    cleared: true
  });
  const targets = [{
    id: "t1",
    label: "gateway",
    host: "10.42.0.1",
    proto: "icmp",
    port: null
  }, {
    id: "t2",
    label: "resolver",
    host: "10.42.0.53",
    proto: "udp",
    port: 53
  }, {
    id: "t3",
    label: "edge proxy",
    host: "10.42.30.10",
    proto: "tcp",
    port: 443
  }, {
    id: "t4",
    label: "object store",
    host: "10.42.20.9",
    proto: "tcp",
    port: 9000
  }, {
    id: "t5",
    label: "controller",
    host: "helium.akoria.net",
    proto: "tcp",
    port: 8443
  }, {
    id: "t6",
    label: "upstream dns",
    host: "1.1.1.1",
    proto: "udp",
    port: 53
  }];
  const monitors = systems.filter(s => s.role === "monitor").slice(0, 5);
  /* A monitor that is itself down is a dead vantage, not a target-level failure —
     its cells read "offline" and are excluded from divergence. Only a reporting
     vantage that cannot reach a target counts as a probe timeout. */
  const matrix = {};
  targets.forEach(t => {
    matrix[t.id] = monitors.map((m, mi) => {
      if (m.state !== "up") return {
        monitorName: m.name,
        monitorId: m.id,
        outcome: "offline",
        rttMicros: 0,
        lossPermille: 0
      };
      const fail = t.id === "t3" && mi === 1 || t.id === "t6" && mi === 3;
      return {
        monitorName: m.name,
        monitorId: m.id,
        outcome: fail ? "timeout" : "ok",
        rttMicros: fail ? 0 : rint(300, 42000),
        lossPermille: fail ? 1000 : rng() < .15 ? rint(1, 40) : 0
      };
    });
  });
  const discovered = [{
    host: "unknown-8f2c",
    ip: "10.42.40.88",
    services: ["sshd", "node_exporter"],
    via: "mDNS",
    seen: 4,
    kind: "host"
  }, {
    host: "printer-hall",
    ip: "10.42.50.31",
    services: ["ipp/631"],
    via: "ARP sweep",
    seen: 11,
    kind: "host"
  }, {
    host: "bismuth",
    ip: "10.42.10.204",
    services: ["sshd", "dockerd", "nginx"],
    via: "SCP advert",
    seen: 2,
    kind: "host"
  }, {
    host: "cam-loading-bay",
    ip: "10.42.50.77",
    services: ["rtsp/554", "http/80"],
    via: "ARP sweep",
    seen: 26,
    kind: "host"
  }, {
    host: "nas-archive",
    ip: "10.42.20.40",
    services: ["smb/445", "nfs/2049"],
    via: "mDNS",
    seen: 38,
    kind: "host"
  }];
  const rules = [{
    id: "r1",
    name: "CPU sustained",
    scope: "all systems",
    condition: "cpu > 90% for 10m",
    severity: "warn",
    enabled: true
  }, {
    id: "r2",
    name: "Disk capacity",
    scope: "all mounts",
    condition: "used > 90%",
    severity: "warn",
    enabled: true
  }, {
    id: "r3",
    name: "Reachability lost",
    scope: "all targets",
    condition: "fail from every vantage",
    severity: "crit",
    enabled: true
  }, {
    id: "r4",
    name: "Split vantage",
    scope: "tcp targets",
    condition: "fail from 1 of n vantages",
    severity: "warn",
    enabled: true
  }, {
    id: "r5",
    name: "Heartbeat stale",
    scope: "all systems",
    condition: "no report for 3 intervals",
    severity: "crit",
    enabled: false
  }];
  window.SN_DATA = {
    pools: POOLS,
    systems,
    fleetRoll,
    applications,
    alerts,
    targets,
    monitors,
    matrix,
    discovered,
    rules,
    controller: {
      primary: "helium",
      failover: "argon",
      version: "2.4.1",
      uptime: "31d"
    },
    roll,
    fmt: {
      mbps: v => v >= 1000 ? (v / 1000).toFixed(1) + " Gb/s" : Math.round(v) + " Mb/s",
      gb: v => v >= 1024 ? (v / 1024).toFixed(1) + " TB" : v + " GB",
      rtt: us => us ? (us / 1000).toFixed(us < 10000 ? 2 : 1) + "ms" : "—"
    }
  };
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/monitoring-dashboard/data.js", error: String((e && e.message) || e) }); }

__ds_ns.BandwidthGauge = __ds_scope.BandwidthGauge;

__ds_ns.HealthDonut = __ds_scope.HealthDonut;

__ds_ns.MetricBar = __ds_scope.MetricBar;

__ds_ns.RTTBars = __ds_scope.RTTBars;

__ds_ns.RadialGauge = __ds_scope.RadialGauge;

__ds_ns.Sparkline = __ds_scope.Sparkline;

__ds_ns.TimeSeries = __ds_scope.TimeSeries;

__ds_ns.Button = __ds_scope.Button;

__ds_ns.Chip = __ds_scope.Chip;

__ds_ns.ICON_NAMES = __ds_scope.ICON_NAMES;

__ds_ns.Icon = __ds_scope.Icon;

__ds_ns.IconButton = __ds_scope.IconButton;

__ds_ns.SearchField = __ds_scope.SearchField;

__ds_ns.SegmentedControl = __ds_scope.SegmentedControl;

__ds_ns.Switch = __ds_scope.Switch;

__ds_ns.Tag = __ds_scope.Tag;

__ds_ns.DataTable = __ds_scope.DataTable;

__ds_ns.MetricCard = __ds_scope.MetricCard;

__ds_ns.MetricRow = __ds_scope.MetricRow;

__ds_ns.NodeTile = __ds_scope.NodeTile;

__ds_ns.Panel = __ds_scope.Panel;

__ds_ns.PoolCard = __ds_scope.PoolCard;

__ds_ns.AlertRow = __ds_scope.AlertRow;

__ds_ns.CommandPalette = __ds_scope.CommandPalette;

__ds_ns.EmptyState = __ds_scope.EmptyState;

__ds_ns.Modal = __ds_scope.Modal;

__ds_ns.Toast = __ds_scope.Toast;

__ds_ns.ToastStack = __ds_scope.ToastStack;

__ds_ns.BrandMark = __ds_scope.BrandMark;

__ds_ns.PageHeader = __ds_scope.PageHeader;

__ds_ns.SidebarNav = __ds_scope.SidebarNav;

__ds_ns.TopBar = __ds_scope.TopBar;

__ds_ns.Heartbeat = __ds_scope.Heartbeat;

__ds_ns.StatusCell = __ds_scope.StatusCell;

__ds_ns.STATUS_COLOR = __ds_scope.STATUS_COLOR;

__ds_ns.STATUS_LABEL = __ds_scope.STATUS_LABEL;

__ds_ns.StatusDot = __ds_scope.StatusDot;

__ds_ns.StatusPill = __ds_scope.StatusPill;

})();
