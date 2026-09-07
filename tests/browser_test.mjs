// Uses installed Firefox + WebDriver BiDi; no npm packages or downloads.
import {spawn} from 'node:child_process';
import {mkdtempSync,writeFileSync,rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {resolve,dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
import assert from 'node:assert/strict';
const root=resolve(dirname(fileURLToPath(import.meta.url)),'..');
const profile=mkdtempSync(tmpdir()+'/floor01-browser-');
writeFileSync(profile+'/user.js',`user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.startup.homepage", "about:blank");
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("toolkit.telemetry.enabled", false);
user_pref("network.captive-portal-service.enabled", false);
user_pref("network.connectivity-service.enabled", false);
user_pref("app.update.auto", false);`);
let firefox,app,ws;
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
function waitOutput(child,pattern){return new Promise((ok,fail)=>{let data='';const timer=setTimeout(()=>fail(Error('startup timeout: '+data)),15000);const collect=b=>{data+=b.toString();const m=data.match(pattern);if(m){clearTimeout(timer);ok(m);}};child.stdout.on('data',collect);child.stderr.on('data',collect);child.on('exit',code=>{clearTimeout(timer);fail(Error('Exited '+code+': '+data));});});}
try{
 app=spawn(root+'/build/floor01',['--lan','--lan-quiet','--lan-address','127.0.0.1','--lan-port','18089'],{env:{...process.env,QT_QPA_PLATFORM:'offscreen',QT_QPA_PLATFORMTHEME:'generic',SDL_AUDIODRIVER:'dummy'}});
 const url=(await waitOutput(app,/FLOOR_LAN_URL=(\S+)/))[1];
 firefox=spawn('firefox',['--headless','--no-remote','--profile',profile,'--remote-debugging-port','19229','about:blank'],{env:{...process.env,MOZ_HEADLESS_WIDTH:'1180',MOZ_HEADLESS_HEIGHT:'900'}});
 await waitOutput(firefox,/WebDriver BiDi listening on (\S+)/);
 ws=new WebSocket('ws://127.0.0.1:19229/session');
 await new Promise((ok,fail)=>{ws.onopen=ok;ws.onerror=fail;});
 let id=0;const pending=new Map();
 ws.onmessage=e=>{const m=JSON.parse(e.data);if(pending.has(m.id)){const p=pending.get(m.id);pending.delete(m.id);clearTimeout(p.timer);m.type==='error'?p.reject(Error(JSON.stringify(m))):p.resolve(m.result);}};
 function call(method,params={}){return new Promise((resolve,reject)=>{const n=++id;const timer=setTimeout(()=>{pending.delete(n);reject(Error('timeout '+method));},10000);pending.set(n,{resolve,reject,timer});ws.send(JSON.stringify({id:n,method,params}));});}
 await call('session.new',{capabilities:{}});
 const {contexts}=await call('browsingContext.getTree');const context=contexts[0].context;
 await call('browsingContext.setViewport',{context,viewport:{width:1180,height:900},devicePixelRatio:1});
 await call('browsingContext.navigate',{context,url,wait:'complete'});
 async function evaluate(expression){const r=await call('script.evaluate',{expression,target:{context},awaitPromise:true});assert.equal(r.type,'success',JSON.stringify(r));return r.result.value;}
 await sleep(500);
 assert.equal(await evaluate('document.body.classList.contains("offline")'),false);
  assert.equal(await evaluate('document.querySelectorAll(".step").length'),128);
  await evaluate('document.getElementById("gridScroll").scrollIntoView({block:"center"})');
  const pts=JSON.parse(await evaluate('JSON.stringify((()=>{const g=document.getElementById("grid");return [112,113,114,115].map(idx=>{const r=g.querySelectorAll(".step")[idx].getBoundingClientRect();return{x:Math.round(r.x+r.width/2),y:Math.round(r.y+r.height/2)};})})())'));
  await call('input.performActions',{context,actions:[{type:'pointer',id:'p',parameters:{pointerType:'mouse'},actions:[{type:'pointerMove',x:pts[0].x,y:pts[0].y},{type:'pointerDown',button:0},{type:'pointerMove',x:pts[1].x,y:pts[1].y,duration:120},{type:'pointerMove',x:pts[2].x,y:pts[2].y,duration:120},{type:'pointerMove',x:pts[3].x,y:pts[3].y,duration:120},{type:'pointerUp',button:0}]}]});
  await sleep(500);
  assert.equal(await evaluate('state.pattern[7][0]===1&&state.pattern[7][1]===1&&state.pattern[7][2]===1&&state.pattern[7][3]===1'),true);
  await evaluate('document.querySelectorAll(".step")[112].click()');await sleep(400);
  assert.equal(await evaluate('state.pattern[7][0]'),0);
 await evaluate('document.getElementById("play").click()');await sleep(300);
 assert.equal(await evaluate('state.playing'),true);
 await evaluate('document.getElementById("stop").click()');await sleep(300);
 assert.equal(await evaluate('state.playing'),false);
 await evaluate('document.getElementById("accent").click();document.querySelectorAll(".step")[1].click()');await sleep(300);
 assert.equal(await evaluate('state.pattern[0][1]'),2);
 await evaluate('document.getElementById("level0").value=31;document.getElementById("level0").dispatchEvent(new Event("input"))');await sleep(300);
 assert.ok(Math.abs(await evaluate('state.levels[0]')-.31)<.001);
 await evaluate('document.getElementById("fill").scrollIntoView({block:"center"})');
 const point=JSON.parse(await evaluate('JSON.stringify((()=>{const r=document.getElementById("fill").getBoundingClientRect();return {x:Math.round(r.x+r.width/2),y:Math.round(r.y+r.height/2)}})())'));
 await call('input.performActions',{context,actions:[{type:'pointer',id:'finger',parameters:{pointerType:'mouse'},actions:[{type:'pointerMove',x:point.x,y:point.y},{type:'pointerDown',button:0}]}]});
 await sleep(1600);assert.equal(await evaluate('state.fill'),true);
 await call('input.performActions',{context,actions:[{type:'pointer',id:'finger',parameters:{pointerType:'mouse'},actions:[{type:'pointerUp',button:0}]}]});
 await sleep(350);assert.equal(await evaluate('state.fill'),false);
 await evaluate('window.scrollTo(0,0)');
 const image=await call('browsingContext.captureScreenshot',{context,origin:'document'});
 writeFileSync(root+'/web-touch.png',Buffer.from(image.data,'base64'));
 await call('browsingContext.setViewport',{context,viewport:{width:810,height:1080},devicePixelRatio:1});
 assert.equal(await evaluate('document.documentElement.scrollWidth<=innerWidth'),true);
 await call('browsingContext.navigate',{context,url:url.split('#')[0]+'#invalid',wait:'complete'});await sleep(300);
 assert.equal(await evaluate('document.body.classList.contains("offline")'),true);
 console.log('PASS: Firefox renders 128 steps, play/stop, accent, fader sync, pointer hold/release, portrait layout, invalid-token lockout; web-touch.png saved');
 await call('session.end');
}finally{
 ws?.close();firefox?.kill();app?.kill();
 await sleep(700);
 if(app&&app.exitCode===null)app.kill('SIGKILL');
 if(firefox&&firefox.exitCode===null)firefox.kill('SIGKILL');
 await sleep(200);rmSync(profile,{recursive:true,force:true});
 for(const child of [firefox,app]){child?.stdout.destroy();child?.stderr.destroy();child?.unref();}
}

// Firefox may leave the BiDi WebSocket close handshake pending after session.end.
process.exit(0);
