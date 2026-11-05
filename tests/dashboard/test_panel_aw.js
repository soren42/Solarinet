#!/usr/bin/env node
/* Dashboard controls and screen map for CONTRACT-V2's sixteen panel slots. */
"use strict";
const fs=require("fs"),path=require("path"),ROOT=path.join(__dirname,"..",".."),PUB=path.join(ROOT,"dashboard/public");
let src=fs.readFileSync(path.join(PUB,"screens-panel.jsx"),"utf8");
const anchor="Object.assign(window, { PanelScreen: PanelScreen });";
if(!src.includes(anchor)){console.error("FAIL: panel JSX export anchor missing");process.exit(2);}
src=src.replace(anchor,"module.exports={SCREENS,THEMES,N_SCREENS,WEIGHTS,readScreenCfg,packScreenEn,packScreenWt,cmdGroup,cmdLabel,CMD_KIND};");
const React={Fragment:"FRAGMENT",useState(){},useEffect(){},useRef(){},useMemo(){},useCallback(){}};
const babel=require(path.join(PUB,"vendor/babel.min.js")),mod={exports:{}};
new Function("React","module","window","document","URLSearchParams","Icon",babel.transform(src,{presets:["react"],filename:"screens-panel.jsx"}).code)(React,mod,{location:{search:""}},{createElement(){return null}},class{get(){return null;}},function Icon(){});
const M=mod.exports;let pass=0,fail=0;
function ok(label,yes,detail){if(yes){pass++;console.log("ok:   "+label);}else{fail++;console.log("FAIL: "+label+(detail?" ["+detail+"]":""));}}
const ids="A0 A1 A2 A3 B0 B1 B2 B3 C0 C1 C2 C3 D0 D1 D2 D3".split(" ");
ok("sixteen slots are configured",M.N_SCREENS===16,String(M.N_SCREENS));
ok("screens follow theme*4+slot order",M.SCREENS.map(s=>s.id).join(" ")===ids.join(" "));
ok("four themes have firmware names",M.THEMES.map(t=>t.name).join("/")==="FLEET/TRAFFIC/TROUBLE/RESOURCES");
const cfg=ids.map((_,i)=>({enabled:i%2===0,weightCode:i%6}));
const decoded=M.readScreenCfg(cfg);
ok("the API's sixteen-entry config decodes",!!decoded&&decoded.length===16);
ok("each config slot keeps enable and dwell",!!decoded&&decoded.every((x,i)=>x.enabled===(i%2===0)&&x.weightCode===i%6));
const legacy=M.readScreenCfg(Array.from({length:12},()=>5));
ok("a twelve-entry legacy report fills four v2 slots with enabled 1x",!!legacy&&legacy.length===16&&legacy.slice(12).every(x=>x.enabled&&x.weightCode===2));
ok("screen 15 enable command packs to kind 8 arg 31",M.packScreenEn(15,true)===31);
ok("screen 15 dwell command packs to kind 9 arg 125",M.packScreenWt(15,5)===125);
ok("per-screen enable commands have isolated keys",M.cmdGroup(M.CMD_KIND.screenEn,31)==="screenEn:15");
ok("per-screen dwell commands have isolated keys",M.cmdGroup(M.CMD_KIND.screenWt,125)==="screenWt:15");
ok("screen 15 command labels identify D3",M.cmdLabel(M.CMD_KIND.screenEn,31)==="D3 enable"&&M.cmdLabel(M.CMD_KIND.screenWt,125)==="D3 weight");
const hex=M.readScreenCfg("05050505050505050505050505050505");
ok("sixteen-byte hex config decodes",!!hex&&hex.length===16&&hex.every(x=>x.enabled&&x.weightCode===2));
ok("sixteen screens are each described",ids.every(id=>typeof M.SCREENS[ids.indexOf(id)].title==="string"&&M.SCREENS[ids.indexOf(id)].title.length>0));
console.log(`${pass} passed, ${fail} failed`);process.exit(fail?1:0);
