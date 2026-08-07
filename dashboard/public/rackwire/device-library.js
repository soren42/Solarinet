/* RackWire — port glyphs, cable standards, seeded device library.
   Specs sourced from vendor tech-spec sheets (Ubiquiti techspecs.ui.com, Minisforum, Apple, RPi). */
(function () {
  // ---------- port glyph geometry (line art, local coords) ----------
  var G = {
    rj45: { w: 20, h: 18, shapes: [
      { t: 'path', d: 'M1 1 H19 V11 H14.5 V17 H5.5 V11 H1 Z' },
      { t: 'line', x1: 5, y1: 1, x2: 5, y2: 5 }, { t: 'line', x1: 8, y1: 1, x2: 8, y2: 5 },
      { t: 'line', x1: 11, y1: 1, x2: 11, y2: 5 }, { t: 'line', x1: 14, y1: 1, x2: 14, y2: 5 } ] },
    sfp: { w: 26, h: 14, shapes: [
      { t: 'rect', x: 1, y: 1, w: 24, h: 12, r: 1.5 },
      { t: 'rect', x: 4, y: 4.5, w: 18, h: 5, r: 1 } ] },
    usba: { w: 17, h: 9, shapes: [
      { t: 'rect', x: 1, y: 1, w: 15, h: 7, r: 0.8 },
      { t: 'rect', x: 3.5, y: 4.4, w: 10, h: 2.2, r: 0.5 } ] },
    usbc: { w: 15, h: 8, shapes: [
      { t: 'rect', x: 1, y: 1, w: 13, h: 6, r: 3 },
      { t: 'rect', x: 3.5, y: 3, w: 8, h: 2, r: 1 } ] },
    hdmi: { w: 23, h: 10, shapes: [
      { t: 'path', d: 'M2 1 H21 L19 9 H4 Z' },
      { t: 'path', d: 'M5 3.5 H18 L17 6.5 H6 Z' } ] },
    microhdmi: { w: 14, h: 7, shapes: [
      { t: 'path', d: 'M1.5 1 H12.5 L11.5 6 H2.5 Z' } ] },
    iec_in: { w: 25, h: 17, shapes: [
      { t: 'path', d: 'M1 5 L5 1 H20 L24 5 V16 H1 Z' },
      { t: 'rect', x: 5, y: 6.5, w: 3, h: 6, r: 0.6 },
      { t: 'rect', x: 11, y: 6.5, w: 3, h: 6, r: 0.6 },
      { t: 'rect', x: 17, y: 6.5, w: 3, h: 6, r: 0.6 } ] },
    iec_out: { w: 25, h: 17, shapes: [
      { t: 'path', d: 'M1 1 H24 V12 L20 16 H5 L1 12 Z' },
      { t: 'rect', x: 5, y: 4.5, w: 3, h: 6, r: 0.6 },
      { t: 'rect', x: 11, y: 4.5, w: 3, h: 6, r: 0.6 },
      { t: 'rect', x: 17, y: 4.5, w: 3, h: 6, r: 0.6 } ] },
    nema: { w: 22, h: 18, shapes: [
      { t: 'rect', x: 1, y: 1, w: 20, h: 16, r: 4 },
      { t: 'rect', x: 5.5, y: 4, w: 2.4, h: 6, r: 0.6 },
      { t: 'rect', x: 14, y: 4, w: 2.4, h: 6, r: 0.6 },
      { t: 'circle', cx: 11, cy: 13, r: 2 } ] },
    barrel: { w: 14, h: 14, shapes: [
      { t: 'circle', cx: 7, cy: 7, r: 6 }, { t: 'circle', cx: 7, cy: 7, r: 2 } ] },
    audio: { w: 12, h: 12, shapes: [
      { t: 'circle', cx: 6, cy: 6, r: 5 }, { t: 'circle', cx: 6, cy: 6, r: 1.8 } ] },
    xlr: { w: 16, h: 16, shapes: [
      { t: 'circle', cx: 8, cy: 8, r: 7 }, { t: 'circle', cx: 5.4, cy: 6.5, r: 1.3 },
      { t: 'circle', cx: 10.6, cy: 6.5, r: 1.3 }, { t: 'circle', cx: 8, cy: 10.6, r: 1.3 } ] },
    bnc: { w: 14, h: 14, shapes: [
      { t: 'circle', cx: 7, cy: 7, r: 6 }, { t: 'circle', cx: 7, cy: 7, r: 1.4 },
      { t: 'line', x1: 1, y1: 3, x2: 2.4, y2: 3 }, { t: 'line', x1: 11.6, y1: 3, x2: 13, y2: 3 } ] },
    sd: { w: 16, h: 7, shapes: [ { t: 'path', d: 'M1 1 H12.5 L15 3 V6 H1 Z' } ] },
    pcie: { w: 66, h: 9, shapes: [
      { t: 'rect', x: 1, y: 1, w: 64, h: 7, r: 1 }, { t: 'line', x1: 13, y1: 1, x2: 13, y2: 8 } ] },
    gpio: { w: 48, h: 11, shapes: [
      { t: 'rect', x: 1, y: 1, w: 46, h: 9, r: 1 },
      { t: 'line', x1: 4, y1: 3.2, x2: 44, y2: 3.2 }, { t: 'line', x1: 4, y1: 7.8, x2: 44, y2: 7.8 } ] },
    csi: { w: 20, h: 6, shapes: [ { t: 'rect', x: 1, y: 1, w: 18, h: 4, r: 0.8 } ] },
    db9: { w: 22, h: 11, shapes: [
      { t: 'path', d: 'M2 1 H20 L18 10 H4 Z' }, { t: 'line', x1: 5, y1: 4, x2: 17, y2: 4 },
      { t: 'line', x1: 6.5, y1: 7, x2: 15.5, y2: 7 } ] },
    lc: { w: 22, h: 12, shapes: [
      { t: 'rect', x: 1, y: 1, w: 9, h: 10, r: 1 }, { t: 'rect', x: 12, y: 1, w: 9, h: 10, r: 1 } ] },
    rj11: { w: 15, h: 15, shapes: [
      { t: 'path', d: 'M1 1 H14 V9 H10.5 V14 H4.5 V9 H1 Z' } ] }
  };

  // ---------- signal domains ----------
  // net = blue, power = green, usb/serial = purple, other = orange
  var DOMAIN = { net: 'net', power: 'power', usb: 'usb', other: 'other' };

  // ---------- cable standards ----------
  var CABLES = [
    { id: 'cat5e', name: 'Cat5e U/UTP', domain: 'net', gbps: 2.5, maxM: 100, conn: 'rj45' },
    { id: 'cat6', name: 'Cat6 U/UTP', domain: 'net', gbps: 10, maxM: 55, conn: 'rj45',
      note: '10G limited to 55 m; 5G/2.5G to 100 m' },
    { id: 'cat6a', name: 'Cat6a F/UTP', domain: 'net', gbps: 10, maxM: 100, conn: 'rj45' },
    { id: 'cat7', name: 'Cat7 S/FTP', domain: 'net', gbps: 10, maxM: 100, conn: 'rj45' },
    { id: 'cat8', name: 'Cat8 S/FTP', domain: 'net', gbps: 25, maxM: 30, conn: 'rj45' },
    { id: 'modseat', name: 'Transceiver seated in cage', domain: 'net', gbps: 25, maxM: 0.1, conn: 'sfp',
      note: 'Not a cable — the module occupying the SFP+ cage' },
    { id: 'dac10', name: 'SFP+ DAC (passive)', domain: 'net', gbps: 10, maxM: 7, conn: 'sfp' },
    { id: 'aoc10', name: 'SFP+ AOC (active optical)', domain: 'net', gbps: 10, maxM: 100, conn: 'sfp' },
    { id: 'om3', name: 'OM3 duplex LC + SFP+ SR', domain: 'net', gbps: 10, maxM: 300, conn: 'sfp' },
    { id: 'os2', name: 'OS2 duplex LC + SFP+ LR', domain: 'net', gbps: 10, maxM: 10000, conn: 'sfp' },
    { id: 'usb2a', name: 'USB 2.0 A→B/A', domain: 'usb', gbps: 0.48, maxM: 5, conn: 'usba' },
    { id: 'usb3a', name: 'USB 3.2 Gen1 Type-A', domain: 'usb', gbps: 5, maxM: 3, conn: 'usba' },
    { id: 'usb3a10', name: 'USB 3.2 Gen2 Type-A', domain: 'usb', gbps: 10, maxM: 1, conn: 'usba' },
    { id: 'usbc10', name: 'USB-C 10 Gbps', domain: 'usb', gbps: 10, maxM: 1, conn: 'usbc' },
    { id: 'usbc20', name: 'USB4 / TB4 passive', domain: 'usb', gbps: 40, maxM: 0.8, conn: 'usbc' },
    { id: 'tb5', name: 'Thunderbolt 5 passive', domain: 'usb', gbps: 120, maxM: 1, conn: 'usbc' },
    { id: 'tb5o', name: 'Thunderbolt 5 optical', domain: 'usb', gbps: 80, maxM: 50, conn: 'usbc' },
    { id: 'usbapd', name: 'USB-A charging cable', domain: 'power', watts: 18, maxM: 2, conn: 'usba' },
    { id: 'usbcpd', name: 'USB-C PD power', domain: 'power', watts: 240, maxM: 2, conn: 'usbc' },
    { id: 'nemaext', name: 'NEMA 5-15P→5-15R extension', domain: 'power', amps: 15, volts: 120, maxM: 15, conn: 'iec' },
    { id: 'c13c14', name: 'IEC C13→C14 (18 AWG)', domain: 'power', amps: 10, volts: 120, maxM: 5, conn: 'iec' },
    { id: 'c13c14h', name: 'IEC C13→C14 (14 AWG)', domain: 'power', amps: 15, volts: 120, maxM: 5, conn: 'iec' },
    { id: 'nemac7', name: 'NEMA 5-15P→C7 figure-8 (18 AWG)', domain: 'power', amps: 2.5, volts: 120, maxM: 3, conn: 'iec',
      note: 'C7 coupler rated 2.5 A / 300 W — Mac mini, Apple TV, Time Capsule' },
    { id: 'nemac13', name: 'NEMA 5-15P→C13', domain: 'power', amps: 13, volts: 120, maxM: 5, conn: 'iec' },
    { id: 'dcbarrel', name: 'DC barrel (PSU lead)', domain: 'power', watts: 250, maxM: 2, conn: 'barrel' },
    { id: 'hdmi20', name: 'HDMI 2.0 High Speed', domain: 'other', gbps: 18, maxM: 5, conn: 'hdmi' },
    { id: 'hdmi21', name: 'HDMI 2.1 Ultra High Speed', domain: 'other', gbps: 48, maxM: 3, conn: 'hdmi' },
    { id: 'hdmi21o', name: 'HDMI 2.1 active optical', domain: 'other', gbps: 48, maxM: 30, conn: 'hdmi' },
    { id: 'dp14', name: 'DisplayPort 1.4', domain: 'other', gbps: 32.4, maxM: 3, conn: 'usbc' },
    { id: 'trs35', name: '3.5 mm TRS analog', domain: 'other', maxM: 8, conn: 'audio' },
    { id: 'xlr', name: 'XLR balanced analog', domain: 'other', maxM: 100, conn: 'xlr' },
    { id: 'sdi', name: '12G-SDI coax (BNC)', domain: 'other', gbps: 12, maxM: 80, conn: 'bnc' },
    { id: 'serial', name: 'RS-232 console (DB9)', domain: 'other', gbps: 0.000115, maxM: 15, conn: 'db9' },
    { id: 'rj45console', name: 'RJ45 rollover console', domain: 'other', gbps: 0.000115, maxM: 15, conn: 'rj45' },
    { id: 'other', name: 'Other / custom', domain: 'other', maxM: 999, conn: 'any' }
  ];

  // ---------- device library ----------
  // group: {k,label,glyph,count,rows,role,gbps,watts,amps,poe,domain,conn}
  var DEVICES = [
    { id: 'udm-pro-max', vendor: 'Ubiquiti', name: 'Dream Machine Pro Max', model: 'UDM-Pro-Max',
      kind: 'gateway', ru: 1, upc: '', drawW: 40, note: '5 Gbps IPS routing, dual WAN, redundant NVR',
      groups: [
        { k: 'lan', label: 'LAN 1–8 · 1 GbE', glyph: 'rj45', count: 8, rows: 2, role: 'lan', gbps: 1, domain: 'net', conn: 'rj45' },
        { k: 'wan', label: 'WAN 2.5 GbE', glyph: 'rj45', count: 1, rows: 1, role: 'wan', gbps: 2.5, domain: 'net', conn: 'rj45' },
        { k: 'sfpwan', label: 'SFP+ WAN', glyph: 'sfp', count: 1, rows: 1, role: 'wan', gbps: 10, domain: 'net', conn: 'sfp' },
        { k: 'sfplan', label: 'SFP+ LAN', glyph: 'sfp', count: 1, rows: 1, role: 'lan', gbps: 10, domain: 'net', conn: 'sfp' },
        { k: 'ac', label: 'AC IN 100–240 V', glyph: 'iec_in', count: 1, rows: 1, role: 'power-in', watts: 100, domain: 'power', conn: 'iec' },
        { k: 'rps', label: 'USP-RPS DC', glyph: 'barrel', count: 1, rows: 1, role: 'power-in', watts: 150, domain: 'power', conn: 'barrel' }
      ] },
    { id: 'usw-pro-max-16-poe', vendor: 'Ubiquiti', name: 'Switch Pro Max 16 PoE', model: 'USW-Pro-Max-16-PoE',
      kind: 'switch', ru: 1, drawW: 30, poeBudgetW: 180, note: '180 W PoE budget · 42 Gbps non-blocking',
      groups: [
        { k: 'ge', label: '1–12 · 1 GbE PoE+', glyph: 'rj45', count: 12, rows: 2, role: 'lan', gbps: 1, poe: 'poe+', poeW: 30, domain: 'net', conn: 'rj45' },
        { k: 'mg', label: '13–16 · 2.5 GbE PoE++', glyph: 'rj45', count: 4, rows: 2, role: 'lan', gbps: 2.5, poe: 'poe++', poeW: 60, domain: 'net', conn: 'rj45' },
        { k: 'sfp', label: '17–18 · 10G SFP+', glyph: 'sfp', count: 2, rows: 1, role: 'uplink', gbps: 10, domain: 'net', conn: 'sfp' },
        { k: 'dc', label: 'DC IN 54 V 210 W', glyph: 'barrel', count: 1, rows: 1, role: 'power-in', watts: 210, domain: 'power', conn: 'barrel' }
      ] },
    { id: 'usw-ultra', vendor: 'Ubiquiti', name: 'Switch Ultra', model: 'USW-Ultra',
      kind: 'switch', ru: 0, drawW: 9, poeBudgetW: 42, note: 'PoE++ input powers switch; 42 W PoE out on PoE++ in, 202 W on 210 W adapter',
      groups: [
        { k: 'in', label: 'Port 1 · PoE++ IN', glyph: 'rj45', count: 1, rows: 1, role: 'poe-in', gbps: 1, poeDrawW: 9, domain: 'net', conn: 'rj45' },
        { k: 'out', label: 'Ports 2–8 · 1 GbE PoE+', glyph: 'rj45', count: 7, rows: 1, role: 'lan', gbps: 1, poe: 'poe+', poeW: 30, domain: 'net', conn: 'rj45' },
        { k: 'dc', label: 'DC IN 54 V (opt.)', glyph: 'barrel', count: 1, rows: 1, role: 'power-in', watts: 210, domain: 'power', conn: 'barrel' }
      ] },
    { id: 'u7-pro', vendor: 'Ubiquiti', name: 'Access Point U7 Pro', model: 'U7-Pro',
      kind: 'ap', ru: 0, drawW: 0, note: 'WiFi 7 · powered by 802.3at PoE+',
      groups: [
        { k: 'eth', label: 'PoE+ IN · 2.5 GbE', glyph: 'rj45', count: 1, rows: 1, role: 'poe-in', gbps: 2.5, poeDrawW: 13, domain: 'net', conn: 'rj45' }
      ] },
    { id: 'u7-pro-xgs', vendor: 'Ubiquiti', name: 'Access Point U7 Pro XGS', model: 'U7-Pro-XGS',
      kind: 'ap', ru: 0, drawW: 0, note: 'WiFi 7 · 10 GbE uplink · 802.3at PoE+',
      groups: [
        { k: 'eth', label: 'PoE+ IN · 10 GbE', glyph: 'rj45', count: 1, rows: 1, role: 'poe-in', gbps: 10, poeDrawW: 21, domain: 'net', conn: 'rj45' }
      ] },
    { id: 'rpi5', vendor: 'Raspberry Pi', name: 'Raspberry Pi 5', model: 'RPi 5 8GB',
      kind: 'sbc', ru: 0, drawW: 27, note: '5 V/5 A USB-C PD required for full peripheral power',
      groups: [
        { k: 'eth', label: '1 GbE', glyph: 'rj45', count: 1, rows: 1, role: 'lan', gbps: 1, domain: 'net', conn: 'rj45' },
        { k: 'usb3', label: 'USB 3.0 ×2', glyph: 'usba', count: 2, rows: 2, role: 'data', gbps: 5, domain: 'usb', conn: 'usba' },
        { k: 'usb2', label: 'USB 2.0 ×2', glyph: 'usba', count: 2, rows: 2, role: 'data', gbps: 0.48, domain: 'usb', conn: 'usba' },
        { k: 'hdmi', label: 'micro-HDMI ×2', glyph: 'microhdmi', count: 2, rows: 2, role: 'video', gbps: 18, domain: 'other', conn: 'hdmi' },
        { k: 'pwr', label: 'USB-C PD IN 27 W', glyph: 'usbc', count: 1, rows: 1, role: 'power-in', watts: 27, domain: 'power', conn: 'usbc' },
        { k: 'gpio', label: '40-pin GPIO', glyph: 'gpio', count: 1, rows: 1, role: 'gpio', domain: 'other', conn: 'any' },
        { k: 'csi', label: 'CSI/DSI ×2', glyph: 'csi', count: 2, rows: 2, role: 'data', domain: 'other', conn: 'any' }
      ] },
    { id: 'macmini-m4pro', vendor: 'Apple', name: 'Mac mini (M4 Pro)', model: 'Mac mini M4 Pro',
      kind: 'server', ru: 0, drawW: 155, note: 'Thunderbolt 5 ×3 rear · 10 GbE option · 2 front USB-C',
      groups: [
        { k: 'tb', label: 'Thunderbolt 5 ×3', glyph: 'usbc', count: 3, rows: 1, role: 'data', gbps: 120, domain: 'usb', conn: 'usbc' },
        { k: 'hdmi', label: 'HDMI 2.1', glyph: 'hdmi', count: 1, rows: 1, role: 'video', gbps: 48, domain: 'other', conn: 'hdmi' },
        { k: 'eth', label: '10 GbE (BTO)', glyph: 'rj45', count: 1, rows: 1, role: 'lan', gbps: 10, domain: 'net', conn: 'rj45' },
        { k: 'fusb', label: 'Front USB-C ×2', glyph: 'usbc', count: 2, rows: 1, role: 'data', gbps: 10, domain: 'usb', conn: 'usbc' },
        { k: 'aud', label: 'Headphone', glyph: 'audio', count: 1, rows: 1, role: 'audio', domain: 'other', conn: 'audio' },
        { k: 'ac', label: 'AC IN', glyph: 'iec_in', count: 1, rows: 1, role: 'power-in', watts: 155, domain: 'power', conn: 'iec' }
      ] },
    { id: 'ms-r1', vendor: 'Minisforum', name: 'MS-R1 Workstation', model: 'MS-R1 (CIX CP8180)',
      kind: 'server', ru: 0, drawW: 180, note: 'Dual RTL8127 10 GbE · PCIe x16 (x8 electrical) · 19 V 180 W brick',
      groups: [
        { k: 'eth', label: '10 GbE ×2', glyph: 'rj45', count: 2, rows: 1, role: 'lan', gbps: 10, domain: 'net', conn: 'rj45' },
        { k: 'usbc', label: 'USB-C (DP 1.4 alt) ×2', glyph: 'usbc', count: 2, rows: 1, role: 'data', gbps: 10, domain: 'usb', conn: 'usbc' },
        { k: 'hdmi', label: 'HDMI 2.0', glyph: 'hdmi', count: 1, rows: 1, role: 'video', gbps: 18, domain: 'other', conn: 'hdmi' },
        { k: 'usb3', label: 'Rear USB 3.2 G2 ×2', glyph: 'usba', count: 2, rows: 2, role: 'data', gbps: 10, domain: 'usb', conn: 'usba' },
        { k: 'usb2', label: 'Rear USB 2.0 ×2', glyph: 'usba', count: 2, rows: 2, role: 'data', gbps: 0.48, domain: 'usb', conn: 'usba' },
        { k: 'fusb', label: 'Front USB-A ×3', glyph: 'usba', count: 3, rows: 1, role: 'data', gbps: 5, domain: 'usb', conn: 'usba' },
        { k: 'aud', label: 'Combo audio', glyph: 'audio', count: 1, rows: 1, role: 'audio', domain: 'other', conn: 'audio' },
        { k: 'pcie', label: 'PCIe x16 (x8)', glyph: 'pcie', count: 1, rows: 1, role: 'data', domain: 'other', conn: 'any' },
        { k: 'dc', label: 'DC IN 19 V 180 W', glyph: 'barrel', count: 1, rows: 1, role: 'power-in', watts: 180, domain: 'power', conn: 'barrel' }
      ] },
    { id: 'pdu-8', vendor: 'Generic', name: 'Rack PDU · 8 outlet', model: 'PDU-8-15A',
      kind: 'power', ru: 1, drawW: 0, circuitA: 15, circuitV: 120, note: '15 A / 1800 W branch circuit, 80% derate = 1440 W',
      groups: [
        { k: 'out', label: 'C13 outlets 1–8', glyph: 'iec_out', count: 8, rows: 1, role: 'power-out', amps: 15, volts: 120, domain: 'power', conn: 'iec' },
        { k: 'in', label: 'Feed · NEMA 5-15P', glyph: 'iec_in', count: 1, rows: 1, role: 'power-in', amps: 15, volts: 120, domain: 'power', conn: 'iec' }
      ] },
    { id: 'outlet-duplex', vendor: 'Generic', name: 'Wall outlet · duplex', model: 'NEMA 5-15R',
      kind: 'power', ru: 0, drawW: 0, circuitA: 15, circuitV: 120, note: 'Shared 15 A branch circuit',
      groups: [
        { k: 'r', label: 'Receptacles A/B', glyph: 'nema', count: 2, rows: 1, role: 'power-out', amps: 15, volts: 120, domain: 'power', conn: 'iec' }
      ] },
    { id: 'ups-1500', vendor: 'Generic', name: 'UPS · 1500 VA', model: 'UPS-1500',
      kind: 'power', isBattery: true, ru: 2, drawW: 0, circuitA: 12, circuitV: 120, budgetW: 900, note: '900 W usable output',
      groups: [
        { k: 'out', label: 'Battery outlets ×6', glyph: 'iec_out', count: 6, rows: 1, role: 'power-out', amps: 12, volts: 120, domain: 'power', conn: 'iec', batt: true },
        { k: 'in', label: 'AC IN', glyph: 'iec_in', count: 1, rows: 1, role: 'power-in', amps: 12, volts: 120, domain: 'power', conn: 'iec' },
        { k: 'usb', label: 'USB mgmt', glyph: 'usba', count: 1, rows: 1, role: 'data', gbps: 0.48, domain: 'usb', conn: 'usba' }
      ] },
    { id: 'sabrent-voltik-252', vendor: 'Sabrent', name: 'VOLTIK 252W charging station', model: 'AX-8PTC',
      kind: 'power', ru: 0, drawW: 252, budgetW: 252, note: '252 W shared across all ports · 4× USB-C at 100 W max (125 W per pair) · 4× USB-A at 18 W max (36 W per pair) · LCD per-port metering',
      groups: [
        { k: 'c', label: 'USB-C out ×4 · 100 W', glyph: 'usbc', count: 4, rows: 1, role: 'power-out', watts: 100, domain: 'power', conn: 'usbc' },
        { k: 'a', label: 'USB-A out ×4 · 18 W', glyph: 'usba', count: 4, rows: 2, role: 'power-out', watts: 18, domain: 'power', conn: 'usba' },
        { k: 'in', label: 'AC IN (C7)', glyph: 'iec_in', count: 1, rows: 1, role: 'power-in', amps: 3, volts: 120, domain: 'power', conn: 'iec' }
      ] },
    { id: 'superdanny-22ac-6usb', vendor: 'SuperDanny', name: 'Surge strip · 22 AC + 6 USB', model: '22AC-6USB-2100J',
      kind: 'power', ru: 0, drawW: 0, budgetW: 1500, circuitA: 15, circuitV: 120,
      note: '1875 W / 15 A total · 2100 J surge rating · 6.5 ft cord. Continuous load kept to 1500 W (80% of 15 A).',
      groups: [
        { k: 'ac', label: 'AC outlets 1–22', glyph: 'nema', count: 22, rows: 2, role: 'power-out', amps: 15, volts: 120, domain: 'power', conn: 'iec' },
        { k: 'usb', label: 'USB charging ×6', glyph: 'usba', count: 6, rows: 2, role: 'power-out', watts: 18, domain: 'power', conn: 'usba' },
        { k: 'in', label: 'Cord · NEMA 5-15P', glyph: 'iec_in', count: 1, rows: 1, role: 'power-in', amps: 15, volts: 120, domain: 'power', conn: 'iec' }
      ] },
    { id: 'cyberpower-cp1500avrlcd', vendor: 'CyberPower', name: 'UPS 1500VA / 900W AVR', model: 'CP1500AVRLCD',
      kind: 'power', isBattery: true, ru: 0, drawW: 0, budgetW: 900, circuitA: 12, circuitV: 120,
      note: '1500 VA / 900 W · 12 outlets: 6 battery + surge, 6 surge only · AVR · mini-tower · USB data port for management',
      groups: [
        { k: 'bat', label: 'Battery + surge ×6', glyph: 'nema', count: 6, rows: 2, role: 'power-out', amps: 12, volts: 120, domain: 'power', conn: 'iec', batt: true },
        { k: 'sur', label: 'Surge only ×6', glyph: 'nema', count: 6, rows: 2, role: 'power-out', amps: 12, volts: 120, domain: 'power', conn: 'iec' },
        { k: 'in', label: 'Cord · NEMA 5-15P', glyph: 'iec_in', count: 1, rows: 1, role: 'power-in', amps: 12, volts: 120, domain: 'power', conn: 'iec' },
        { k: 'usb', label: 'USB data (mgmt)', glyph: 'usba', count: 1, rows: 1, role: 'data', gbps: 0.48, domain: 'usb', conn: 'usba' }
      ] },
    { id: 'apc-be1050g3', vendor: 'APC', name: 'Back-UPS 1050VA / 600W', model: 'BE1050G3',
      kind: 'power', isBattery: true, ru: 0, drawW: 0, budgetW: 600, circuitA: 12, circuitV: 120,
      note: '1050 VA / 600 W · 12 outlets: 6 battery + surge, 6 surge only · 2 USB charging ports (A + C)',
      groups: [
        { k: 'bat', label: 'Battery + surge ×6', glyph: 'nema', count: 6, rows: 2, role: 'power-out', amps: 12, volts: 120, domain: 'power', conn: 'iec', batt: true },
        { k: 'sur', label: 'Surge only ×6', glyph: 'nema', count: 6, rows: 2, role: 'power-out', amps: 12, volts: 120, domain: 'power', conn: 'iec' },
        { k: 'in', label: 'Cord · NEMA 5-15P', glyph: 'iec_in', count: 1, rows: 1, role: 'power-in', amps: 12, volts: 120, domain: 'power', conn: 'iec' },
        { k: 'uc', label: 'USB-C charge', glyph: 'usbc', count: 1, rows: 1, role: 'power-out', watts: 15, domain: 'power', conn: 'usbc' },
        { k: 'ua', label: 'USB-A charge', glyph: 'usba', count: 1, rows: 1, role: 'power-out', watts: 12, domain: 'power', conn: 'usba' }
      ] },
    { id: 'internet-service', vendor: 'Service', name: 'Internet', model: 'WAN service',
      kind: 'service', ru: 0, drawW: 0, isInternet: true,
      note: 'The demarcation point — everything reachable from here is considered online. Set the subscribed rate on the hand-off port.',
      groups: [
        { k: 'hand', label: 'Service hand-off', glyph: 'lc', count: 1, rows: 1, role: 'wan', gbps: 8, domain: 'net', conn: 'sfp' }
      ] },
    { id: 'gfiber-8g-jack', vendor: 'Google Fiber', name: 'Fiber Jack · 8 Gig', model: 'GOXP330C',
      kind: 'wan', ru: 0, drawW: 12, isInternet: true, note: '8 Gbps symmetric service · fiber in, 10 GbE RJ45 hand-off (service capped at 8 Gbps) · external DC brick',
      groups: [
        { k: 'fib', label: 'Fiber IN · SC/APC', glyph: 'lc', count: 1, rows: 1, role: 'wan', gbps: 8, domain: 'net', conn: 'sfp' },
        { k: 'lan', label: 'LAN OUT · 10 GbE', glyph: 'rj45', count: 1, rows: 1, role: 'wan', gbps: 10, domain: 'net', conn: 'rj45' },
        { k: 'dc', label: 'DC IN 12 V', glyph: 'barrel', count: 1, rows: 1, role: 'power-in', watts: 12, domain: 'power', conn: 'barrel' }
      ] },
    { id: 'qsfptek-10g-t', vendor: 'QSFPTEK', name: 'SFP+ to RJ45 10GBASE-T module', model: 'SFP-10G-T (QSFPTEK)',
      kind: 'transceiver', ru: 0, drawW: 2.5,
      note: 'Copper 10GBASE-T transceiver · Cat6a/Cat7 · compatible with Cisco SFP-10G-T-S, Ubiquiti UF-RJ45-10G, Netgear, MikroTik, D-Link, Supermicro, TP-Link, Linksys · draws ~2.5 W from the host cage',
      groups: [
        { k: 'cage', label: 'SFP+ cage side', glyph: 'sfp', count: 1, rows: 1, role: 'module', gbps: 10, domain: 'net', conn: 'sfp' },
        { k: 'rj', label: 'RJ45 10GBASE-T', glyph: 'rj45', count: 1, rows: 1, role: 'lan', gbps: 10, domain: 'net', conn: 'rj45' }
      ] },
    { id: '10gtek-10g-t', vendor: '10Gtek', name: 'SFP+ to RJ45 10GBASE-T module', model: 'ASF-10G-T (10Gtek)',
      kind: 'transceiver', ru: 0, drawW: 2.5,
      note: 'Copper 10GBASE-T transceiver · Cat6a to 30 m · compatible with Cisco SFP-10G-T-S, Meraki, Fortinet · draws ~2.5 W from the host cage',
      groups: [
        { k: 'cage', label: 'SFP+ cage side', glyph: 'sfp', count: 1, rows: 1, role: 'module', gbps: 10, domain: 'net', conn: 'sfp' },
        { k: 'rj', label: 'RJ45 10GBASE-T · 30 m', glyph: 'rj45', count: 1, rows: 1, role: 'lan', gbps: 10, domain: 'net', conn: 'rj45' }
      ] },
    { id: 'ipolex-10g-t', vendor: 'ipolex', name: 'SFP+ to RJ45 multi-rate module', model: 'SFP-10G-T (ipolex)',
      kind: 'transceiver', ru: 0, drawW: 2.5,
      note: 'Negotiates 1G / 2.5G / 5G / 10GBASE-T · up to 30 m · compatible with Cisco SFP-10G-T, Ubiquiti UACC-CM-RJ45-MG, MikroTik, Netgear, TP-Link, D-Link · draws ~2.5 W from the host cage',
      groups: [
        { k: 'cage', label: 'SFP+ cage side', glyph: 'sfp', count: 1, rows: 1, role: 'module', gbps: 10, domain: 'net', conn: 'sfp' },
        { k: 'rj', label: 'RJ45 1/2.5/5/10G · 30 m', glyph: 'rj45', count: 1, rows: 1, role: 'lan', gbps: 10, domain: 'net', conn: 'rj45' }
      ] },
    { id: 'uacc-ac-210w', vendor: 'Ubiquiti', name: 'Power adapter · 54V 210W', model: 'UACC-Adapter-AC-210W',
      kind: 'adapter', ru: 0, drawW: 0, budgetW: 210, eff: 0.9,
      note: 'Output DC 54 V 3.9 A / 210.6 W · input AC 100–240 V 50/60 Hz · barrel tip. Ships with USW-Pro-Max-16-PoE (180 W PoE), USW-Ultra-210W, USW-Flex-2.5G-8-PoE, USW-Pro-XG-8-PoE, US-XG-6POE.',
      groups: [
        { k: 'ac', label: 'AC IN 100–240 V', glyph: 'iec_in', count: 1, rows: 1, role: 'power-in', amps: 2, volts: 120, domain: 'power', conn: 'iec' },
        { k: 'dc', label: 'DC OUT 54 V 3.9 A', glyph: 'barrel', count: 1, rows: 1, role: 'power-out', watts: 210, volts: 54, domain: 'power', conn: 'barrel' }
      ] },
    { id: 'uacc-ac-60w', vendor: 'Ubiquiti', name: 'Power adapter · 54V 60W', model: 'UACC-Adapter-AC-60W',
      kind: 'adapter', ru: 0, drawW: 0, budgetW: 60, eff: 0.9,
      note: 'Output DC 54 V ~1.1 A / 60 W · input AC 100–240 V. The adapter shipped with USW-Ultra-60W; on a bare USW-Ultra it lifts PoE output above the 42 W available from PoE++ input.',
      groups: [
        { k: 'ac', label: 'AC IN 100–240 V', glyph: 'iec_in', count: 1, rows: 1, role: 'power-in', amps: 1, volts: 120, domain: 'power', conn: 'iec' },
        { k: 'dc', label: 'DC OUT 54 V 60 W', glyph: 'barrel', count: 1, rows: 1, role: 'power-out', watts: 60, volts: 54, domain: 'power', conn: 'barrel' }
      ] },
    { id: 'msf-19v-180w', vendor: 'Minisforum', name: 'Power adapter · 19V 180W', model: 'SOY-1900947-454',
      kind: 'adapter', ru: 0, drawW: 0, budgetW: 180, eff: 0.9,
      note: 'Output DC 19 V 9.47 A / 180 W · 5.5 × 2.5 mm barrel · input AC 100–240 V 50/60 Hz. Ships with MS-R1 and MS-01. MS-R1 will alternatively take 100 W over USB-C at 20 V.',
      groups: [
        { k: 'ac', label: 'AC IN 100–240 V', glyph: 'iec_in', count: 1, rows: 1, role: 'power-in', amps: 2, volts: 120, domain: 'power', conn: 'iec' },
        { k: 'dc', label: 'DC OUT 19 V 9.47 A', glyph: 'barrel', count: 1, rows: 1, role: 'power-out', watts: 180, volts: 19, domain: 'power', conn: 'barrel' }
      ] },
    { id: 'rpi-27w-psu', vendor: 'Raspberry Pi', name: 'USB-C PSU · 27W', model: 'SC1150 (5.1V 5A)',
      kind: 'adapter', ru: 0, drawW: 0, budgetW: 27, eff: 0.9,
      note: 'Output 5.1 V 5 A / 27 W USB-C PD · captive cable · required for a Pi 5 to allow the full 1.6 A of downstream USB current.',
      groups: [
        { k: 'ac', label: 'AC IN (captive plug)', glyph: 'iec_in', count: 1, rows: 1, role: 'power-in', amps: 1, volts: 120, domain: 'power', conn: 'iec' },
        { k: 'out', label: 'USB-C OUT 5.1 V 5 A', glyph: 'usbc', count: 1, rows: 1, role: 'power-out', watts: 27, volts: 5.1, domain: 'power', conn: 'usbc' }
      ] },
    { id: 'psu-12v-generic', vendor: 'Generic', name: 'Power adapter · 12V barrel', model: 'PSU-12V-24W',
      kind: 'adapter', ru: 0, drawW: 0, budgetW: 24, eff: 0.88,
      note: 'Output DC 12 V up to 2 A / 24 W · barrel tip. Placeholder for small 12 V bricks such as the one feeding a Fiber Jack — confirm polarity, tip size and rating against the unit on the shelf.',
      groups: [
        { k: 'ac', label: 'AC IN 100–240 V', glyph: 'iec_in', count: 1, rows: 1, role: 'power-in', amps: 1, volts: 120, domain: 'power', conn: 'iec' },
        { k: 'dc', label: 'DC OUT 12 V 2 A', glyph: 'barrel', count: 1, rows: 1, role: 'power-out', watts: 24, volts: 12, domain: 'power', conn: 'barrel' }
      ] },
    { id: 'poe-injector-60w', vendor: 'Generic', name: 'PoE++ injector · 60W', model: 'POE-INJ-60W',
      kind: 'adapter', ru: 0, drawW: 0, budgetW: 60, eff: 0.9,
      note: '802.3bt injector · gigabit data pass-through · 60 W to the powered device.',
      groups: [
        { k: 'ac', label: 'AC IN', glyph: 'iec_in', count: 1, rows: 1, role: 'power-in', amps: 1, volts: 120, domain: 'power', conn: 'iec' },
        { k: 'data', label: 'DATA IN', glyph: 'rj45', count: 1, rows: 1, role: 'lan', gbps: 1, domain: 'net', conn: 'rj45' },
        { k: 'poe', label: 'DATA + PoE OUT', glyph: 'rj45', count: 1, rows: 1, role: 'lan', gbps: 1, poe: 'poe++', poeW: 60, domain: 'net', conn: 'rj45' }
      ] },
    { id: 'macmini-psu', vendor: 'Apple', name: 'Mac mini internal PSU · 155W', model: '661-43677 / PA-1161-1A',
      kind: 'adapter', ru: 0, drawW: 0, budgetW: 155, eff: 0.92,
      note: 'Internal to the chassis — there is no external brick. 155 W maximum continuous; M4 Pro peaks at 140 W, idles near 5 W. Mains side is a detachable IEC C7 figure-8 cord, same part across every Mac mini generation. Place this only if your inventory tracks the PSU as its own asset; otherwise cable the Mac mini AC inlet straight to the outlet with a NEMA 5-15P→C7 cord.',
      groups: [
        { k: 'ac', label: 'AC inlet (C8)', glyph: 'iec_in', count: 1, rows: 1, role: 'power-in', amps: 2.5, volts: 120, domain: 'power', conn: 'iec' },
        { k: 'dc', label: 'DC OUT · internal 155 W', glyph: 'barrel', count: 1, rows: 1, role: 'power-out', watts: 155, domain: 'power', conn: 'barrel' }
      ] },
    { id: 'patch-24', vendor: 'Generic', name: 'Patch panel · 24 port', model: 'PP-24-C6A',
      kind: 'passive', ru: 1, drawW: 0, note: 'Keystone Cat6a, front/rear pass-through',
      groups: [
        { k: 'p', label: 'Ports 1–24', glyph: 'rj45', count: 24, rows: 2, role: 'passive', gbps: 10, domain: 'net', conn: 'rj45' }
      ] },
    { id: 'display-generic', vendor: 'Generic', name: 'Display · 4K', model: 'MON-4K',
      kind: 'peripheral', ru: 0, drawW: 60,
      groups: [
        { k: 'hdmi', label: 'HDMI IN ×2', glyph: 'hdmi', count: 2, rows: 1, role: 'video', gbps: 48, domain: 'other', conn: 'hdmi' },
        { k: 'usbc', label: 'USB-C DP IN', glyph: 'usbc', count: 1, rows: 1, role: 'video', gbps: 40, domain: 'usb', conn: 'usbc' },
        { k: 'ac', label: 'AC IN', glyph: 'iec_in', count: 1, rows: 1, role: 'power-in', watts: 60, domain: 'power', conn: 'iec' }
      ] },
    { id: 'nas-4bay', vendor: 'Generic', name: 'NAS · 4 bay', model: 'NAS-4B',
      kind: 'server', ru: 0, drawW: 90,
      groups: [
        { k: 'eth', label: '2.5 GbE ×2', glyph: 'rj45', count: 2, rows: 1, role: 'lan', gbps: 2.5, domain: 'net', conn: 'rj45' },
        { k: 'usb', label: 'USB 3.2 ×2', glyph: 'usba', count: 2, rows: 1, role: 'data', gbps: 5, domain: 'usb', conn: 'usba' },
        { k: 'dc', label: 'DC IN', glyph: 'barrel', count: 1, rows: 1, role: 'power-in', watts: 120, domain: 'power', conn: 'barrel' }
      ] },
    { id: 'blank', vendor: '—', name: 'Blank device', model: 'custom',
      kind: 'other', ru: 0, drawW: 0, note: 'Start empty and add your own ports',
      groups: [] }
  ];

  // ---------- layout engine: turns groups into positioned ports ----------
  var PAD_X = 16, BAND_TOP = 28, BAND_BOT = 38, GAP = 22, IN_GAP = 5;
  function labelW(s) { return (s || '').length * 4.9 + 4; }

  function layout(def) {
    var groups = (def.groups || []).map(function (g) {
      var gl = G[g.glyph] || G.rj45;
      var rows = Math.max(1, Math.min(g.rows || 1, g.count));
      var cols = Math.ceil(g.count / rows);
      var gw = cols * gl.w + (cols - 1) * IN_GAP;
      var gh = rows * gl.h + (rows - 1) * IN_GAP;
      return { g: g, gl: gl, rows: rows, cols: cols, gw: gw, gh: gh, blockW: Math.max(gw, labelW(g.label)) };
    });
    var innerH = groups.reduce(function (m, x) { return Math.max(m, x.gh); }, 20);
    var sumW = groups.reduce(function (s, x) { return s + x.blockW; }, 0) + Math.max(0, groups.length - 1) * GAP;
    // the header row (name left, model right) also sets a floor on width
    var headW = PAD_X * 2 + (def.name || '').length * 6.5 + 20 + (def.model || '').length * 5.3;
    var w = Math.max(200, sumW + PAD_X * 2, headW);
    var h = BAND_TOP + innerH + BAND_BOT;
    var ports = [], x = PAD_X;
    groups.forEach(function (b) {
      var gx = x + (b.blockW - b.gw) / 2;
      var gy = BAND_TOP + (innerH - b.gh) / 2;
      for (var i = 0; i < b.g.count; i++) {
        var col = Math.floor(i / b.rows), row = i % b.rows;
        var px = gx + col * (b.gl.w + IN_GAP), py = gy + row * (b.gl.h + IN_GAP);
        ports.push({
          id: b.g.k + '-' + (i + 1),
          label: b.g.count > 1 ? b.g.k.toUpperCase() + (i + 1) : b.g.k.toUpperCase(),
          groupLabel: b.g.label, glyph: b.g.glyph, gw: b.gl.w, gh: b.gl.h,
          x: px, y: py, cx: px + b.gl.w / 2, cy: py + b.gl.h / 2,
          role: b.g.role, gbps: b.g.gbps, watts: b.g.watts, amps: b.g.amps, volts: b.g.volts, batt: b.g.batt,
          poe: b.g.poe, poeW: b.g.poeW, poeDrawW: b.g.poeDrawW,
          domain: b.g.domain, conn: b.g.conn
        });
      }
      ports.groupMarks = ports.groupMarks || [];
      ports.groupMarks.push({ label: b.g.label, x: x + b.blockW / 2, y: BAND_TOP + innerH + 12 });
      x += b.blockW + GAP;
    });
    return { w: w, h: h, ports: ports, marks: ports.groupMarks || [] };
  }

  window.RW = { GLYPHS: G, CABLES: CABLES, DEVICES: DEVICES, DOMAIN: DOMAIN, layout: layout };
})();
