#!/usr/bin/env node
/* Dashboard v2 framebuffer parity against the firmware's committed goldens. */
"use strict";
const fs = require("fs"), path = require("path");
const ROOT = path.join(__dirname, "..", ".."), PUB = path.join(ROOT, "dashboard/public");
const FIXTURE = path.join(ROOT, "status-panel/fixtures/parity-fixture.json");
const GOLDEN = path.join(ROOT, "status-panel/fixtures/golden");
function react() { return { Fragment: "FRAGMENT", useState(){}, useEffect(){}, useRef(){}, useMemo(){}, useCallback(){} }; }
const R=react(), babel=require(path.join(PUB,"vendor/babel.min.js"));
let src=fs.readFileSync(path.join(PUB,"screens-panel.jsx"),"utf8");
const anchor="Object.assign(window, { PanelScreen: PanelScreen });";
if(!src.includes(anchor)){console.error("FAIL: dashboard module export anchor missing");process.exit(2);}
src=src.replace(anchor,"module.exports = { buildEnv, SCREENS, fbNew, fbUse, fbClear, PW, PH, inlay, helpOverlay, screenLinkLost, screenNoData, titleCard };");
const mod={exports:{}};
new Function("React","module","window","document","URLSearchParams","Icon",babel.transform(src,{presets:["react"],filename:"screens-panel.jsx"}).code)(R,mod,{location:{search:""}},{createElement(){return null}},class{get(){return null;}},function Icon(){});
const M=mod.exports, fixture=JSON.parse(fs.readFileSync(FIXTURE,"utf8"));
const env=M.buildEnv(fixture.history,fixture.cadenceSec*1000), dt=.04, picks=[0,125,250];
let pass=0,fail=0,maxDelta=0;
function golden(id){const lines=fs.readFileSync(path.join(GOLDEN,"panel-"+id+".txt"),"utf8").split(/\r?\n/);let f=-1,frames=[];for(const line of lines){if(!line||line[0]==="#")continue;if(line.startsWith("frame")){f++;frames[f]=[];continue;}if(f>=0)frames[f].push(line.trim().split(/\s+/).map(x=>[0,2,4].map(i=>parseInt(x.slice(i,i+2),16))));}return frames;}
for(let i=0;i<16;i++){
 const sc=M.SCREENS[i], fb=M.fbNew(), st={};let shots=[];
 for(let k=0;k<=250;k++){M.fbUse(fb);M.fbClear();sc.render(env,k*dt,dt,st);if(picks.includes(k))shots.push(fb.slice());}
 const g=golden(sc.id);let local=0,where="";
 for(let fi=0;fi<3;fi++){let n=0;for(let y=0;y<M.PH;y++)for(let x=0;x<M.PW;x++){const at=(y*M.PW+x)*3,hex=g[fi][y][x];for(let c=0;c<3;c++){const got=Math.max(0,Math.min(255,Math.round(shots[fi][at+c]))),d=Math.abs(got-hex[c]);if(d>local){local=d;where=`frame ${fi} (${x},${y}) channel ${c}: ${got} vs ${hex[c]}`;}n+=d>1?1:0;}}if(n)break;}
 maxDelta=Math.max(maxDelta,local);if(local<=1){pass++;console.log(`ok:   ${sc.id} framebuffer equals firmware golden within 1/channel`);}else{fail++;console.log(`FAIL: ${sc.id} framebuffer differs; max delta ${local}, ${where}`);}
}
function compareCase(id, frames, render) {
 const g=golden(id), fb=M.fbNew(), picks=id==="help"?Array.from({length:g.length},(_,i)=>i):[0,125,250], shots=[];
 for(let k=0;k<frames;k++){M.fbUse(fb);M.fbClear();render(fb,k);if(picks.includes(k))shots.push(fb.slice());}
 let local=0,where="";
 for(let fi=0;fi<picks.length;fi++){let n=0;for(let y=0;y<M.PH;y++)for(let x=0;x<M.PW;x++){const at=(y*M.PW+x)*3,px=g[fi][y][x];for(let c=0;c<3;c++){const got=Math.max(0,Math.min(255,Math.round(shots[fi][at+c]))),d=Math.abs(got-px[c]);if(d>local){local=d;where="frame "+fi+" ("+x+","+y+") channel "+c+": "+got+" vs "+px[c];}if(d>1)n++;}}if(n)break;}
 maxDelta=Math.max(maxDelta,local);if(local<=1){pass++;console.log("ok:   "+id+" framebuffer equals firmware golden within 1/channel");}else{fail++;console.log("FAIL: "+id+" framebuffer differs; max delta "+local+", "+where);}
}
compareCase("inlay",251,(fb,k)=>{M.SCREENS[0].render(env,k*dt,dt,{});M.inlay(env,k*dt);});
compareCase("help",12,(fb,k)=>M.helpOverlay(k));
compareCase("linklost",251,(fb,k)=>M.screenLinkLost(k*dt));
compareCase("nodata",251,(fb,k)=>M.screenNoData(k*dt,true));
compareCase("title",251,(fb,k)=>{M.SCREENS[0].render(env,k*dt,dt,{});M.titleCard(11,.5);});

console.log(pass+" passed, "+fail+" failed; allowed max channel delta "+maxDelta+" (tolerance 1/255 for float quantization)");
process.exit(fail?1:0);
