#pragma once
// Single-page web UI. Served from flash at http://<scope-ip>/
// Waveforms and commands go over the WebSocket at /ws.

static const char INDEX_HTML[] = R"rawliteral(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>ESP32 Scope</title>
<style>
:root{--bg:#0b0d10;--panel:#14181d;--line:#232a32;--txt:#d7dde4;--dim:#7d8894;--ch:#f4d03f;--trg:#ff8a3d;--grid:#26303a;--gridc:#3a4653;--acc:#4fc3f7;--ok:#5ad67d;--bad:#ff5c5c;--fft:#b388ff;--ca:#4fc3f7;--cb:#ff6fb5}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--txt);font:14px/1.35 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{display:flex;align-items:center;gap:10px;padding:8px 12px;border-bottom:1px solid var(--line);flex-wrap:wrap}
header h1{font-size:15px;margin:0;font-weight:600;letter-spacing:.3px}
.dot{width:9px;height:9px;border-radius:50%;background:var(--bad);display:inline-block}
.dot.on{background:var(--ok)}
.badge{font:600 12px ui-monospace,Menlo,Consolas,monospace;padding:2px 7px;border-radius:4px;background:#222a33;color:var(--dim)}
.badge.TRIG{background:#1f3b26;color:var(--ok)}.badge.AUTO{background:#3a3417;color:var(--ch)}
.badge.WAIT{background:#3b2a17;color:var(--trg)}.badge.STOP{background:#3b1d1d;color:var(--bad)}.badge.ROLL{background:#1b2f3d;color:var(--acc)}
.sp{flex:1}
.mono{font-family:ui-monospace,Menlo,Consolas,monospace}
main{display:grid;grid-template-columns:minmax(0,1fr) 330px;gap:12px;padding:12px}
@media(max-width:900px){main{grid-template-columns:minmax(0,1fr);padding:8px}}
.scope{background:#000;border:1px solid var(--line);border-radius:8px;position:relative;overflow:hidden;touch-action:none}
canvas{display:block;width:100%}
#cv{aspect-ratio:10/8;cursor:crosshair}
.ovl{position:absolute;left:8px;top:6px;font:12px ui-monospace,Menlo,Consolas,monospace;color:var(--dim);pointer-events:none;text-shadow:0 0 3px #000}
.ovl b{color:var(--ch);font-weight:600}.ovl .t{color:var(--trg)}
.ovr{position:absolute;right:8px;top:6px;font:12px ui-monospace,Menlo,Consolas,monospace;color:var(--dim);pointer-events:none;text-align:right}
.ovl,.ovr{white-space:nowrap}
@media(max-width:600px){.ovl,.ovr{font-size:10px}.ovr{top:auto;bottom:5px}.m b{font-size:13px}}
.m b{white-space:nowrap}
.meas{display:grid;grid-template-columns:repeat(auto-fill,minmax(112px,1fr));gap:6px;margin-top:8px}
.m{background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:5px 8px}
.m span{display:block;font-size:11px;color:var(--dim)}
.m b{font:600 15px ui-monospace,Menlo,Consolas,monospace;color:var(--txt)}
.curs{margin-top:6px;font:12px ui-monospace,Menlo,Consolas,monospace;color:var(--dim);min-height:16px}
.curs i{font-style:normal;color:var(--txt)}
aside{display:flex;flex-direction:column;gap:10px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:10px}
.card h3{margin:0 0 8px;font-size:12px;text-transform:uppercase;letter-spacing:.8px;color:var(--dim);font-weight:600}
.row{display:flex;align-items:center;gap:6px;margin:6px 0;flex-wrap:wrap}
.row label{min-width:70px;color:var(--dim);font-size:13px}
button,select,input{font:inherit;color:var(--txt);background:#1c232b;border:1px solid #2c3640;border-radius:6px;padding:6px 10px}
button{cursor:pointer;min-height:34px}
button:hover{border-color:#41505e}
button.pri{background:#1d3a52;border-color:#2c5a7d}
button.on{background:#244a2f;border-color:#3d7a4c;color:#c9f5d4}
button.warn{background:#4a2424;border-color:#7a3d3d}
input[type=number]{width:96px}
input[type=text],input[type=password]{width:100%}
input[type=range]{flex:1;padding:0;accent-color:var(--acc)}
.seg{display:inline-flex}.seg button{border-radius:0;margin-left:-1px}.seg button:first-child{border-radius:6px 0 0 6px}.seg button:last-child{border-radius:0 6px 6px 0}
.step{display:inline-flex;align-items:center;gap:4px}.step b{min-width:74px;text-align:center;font:600 14px ui-monospace,Menlo,Consolas,monospace}
.big{display:grid;grid-template-columns:1fr 1fr 1fr;gap:6px}
.big button{min-height:42px;font-weight:600}
details summary{cursor:pointer;color:var(--dim);font-size:12px;text-transform:uppercase;letter-spacing:.8px;font-weight:600}
#log{white-space:pre-wrap;font:11px ui-monospace,Menlo,Consolas,monospace;color:var(--dim);max-height:180px;overflow:auto;margin:6px 0 0}
.hint{font-size:12px;color:var(--dim)}
</style></head><body>
<header>
  <span class="dot" id="conn"></span><h1>ESP32 Scope</h1>
  <span class="badge" id="state">----</span>
  <span class="sp"></span>
  <span class="mono hint" id="hdr"></span>
</header>
<main>
<section>
  <div class="scope" id="wrap">
    <canvas id="cv"></canvas>
    <div class="ovl" id="ovl"></div>
    <div class="ovr" id="ovr"></div>
  </div>
  <div class="curs" id="curs"></div>
  <div class="meas" id="meas"></div>
  <p class="hint">Drag on the right edge for trigger level, left edge for vertical position, top edge for trigger position. Mouse wheel changes time/div (Shift for volts/div).</p>
</section>
<aside>
  <div class="card">
    <div class="big">
      <button id="bRun" class="pri">Run / Stop</button>
      <button id="bSingle">Single</button>
      <button id="bAuto">Autoset</button>
    </div>
  </div>
  <div class="card"><h3>Horizontal</h3>
    <div class="row"><label>Time/div</label><span class="step"><button data-c="tdiv -">&#9664;</button><b id="tdivL">-</b><button data-c="tdiv +">&#9654;</button></span></div>
    <div class="row"><label>Trig pos</label><input type="range" id="hpos" min="0.05" max="0.95" step="0.01"></div>
  </div>
  <div class="card"><h3>Vertical</h3>
    <div class="row"><label>Volts/div</label><span class="step"><button data-c="vdiv -">&#9664;</button><b id="vdivL">-</b><button data-c="vdiv +">&#9654;</button></span></div>
    <div class="row"><label>Offset V</label><input type="number" id="vpos" step="0.1"><button id="bCenter">Center</button><button data-c="vpos 0">0</button></div>
    <div class="row"><label>Coupling</label><span class="seg"><button id="cDC" data-c="coupling dc">DC</button><button id="cAC" data-c="coupling ac">AC</button></span></div>
  </div>
  <div class="card"><h3>Trigger</h3>
    <div class="row"><label>Mode</label><span class="seg"><button id="tm0" data-c="tmode auto">Auto</button><button id="tm1" data-c="tmode normal">Normal</button><button id="tm2" data-c="single">Single</button></span></div>
    <div class="row"><label>Edge</label><span class="seg"><button id="te0" data-c="tedge rise">&#8593; Rise</button><button id="te1" data-c="tedge fall">&#8595; Fall</button></span></div>
    <div class="row"><label>Level V</label><input type="number" id="tlevel" step="0.05"><button id="b50">50%</button></div>
  </div>
  <div class="card"><h3>Display</h3>
    <div class="row"><button id="bFFT">FFT</button><button id="bPers">Persist</button><button id="bDots">Dots</button></div>
    <div class="row"><label>Cursors</label><span class="seg"><button id="cu0">Off</button><button id="cu1">Time</button><button id="cu2">Volt</button></span></div>
    <div class="row"><button id="bCSV">Save CSV</button><button id="bPNG">Save PNG</button></div>
  </div>
  <div class="card"><h3>Signal generator</h3>
    <div class="row"><label>Square</label><button id="gSq">Off</button><input type="number" id="gSqHz" min="1" max="10000000" step="any"> Hz</div>
    <div class="row"><label>Duty %</label><input type="number" id="gSqDuty" min="0" max="100" step="1"><span class="hint mono" id="gSqAct"></span></div>
    <div class="row"><label>Sine</label><button id="gSin">Off</button><input type="number" id="gSinHz" min="130" max="55000" step="any"> Hz</div>
    <div class="hint">Square on GPIO26 (crystal-accurate). Sine on GPIO25, ~1.6 Vpp, frequency approximate.</div>
  </div>
  <div class="card"><details><summary>Front-end &amp; calibration</summary>
    <div class="row"><label>Front-end</label><select id="fe"><option value="direct">Direct 0-3.1 V</option><option value="bias">Bias divider &plusmn;15 V</option><option value="custom">Custom (calibrated)</option></select></div>
    <div class="row"><button id="bZero">Zero cal</button><span class="hint">probe shorted to GND</span></div>
    <div class="row"><input type="number" id="calV" step="0.001" placeholder="known V"><button id="bGain">Gain cal</button></div>
    <div class="row"><button id="bSelf">Self-test</button><span class="hint">jumper GPIO26 &rarr; GPIO34 first</span></div>
    <div class="hint mono" id="calInfo"></div>
  </details></div>
  <div class="card"><details><summary>WiFi</summary>
    <div class="row"><input type="text" id="ssid" placeholder="network name" autocomplete="off"></div>
    <div class="row"><input type="password" id="pass" placeholder="password" autocomplete="off"></div>
    <div class="row"><button id="bJoin">Join network</button><button id="bAP">AP mode</button></div>
    <div class="hint">After joining, open http://scope.local or the IP on the OLED Info screen.</div>
  </details></div>
  <div class="card"><details open><summary>Console</summary>
    <div class="row"><input type="text" id="cmd" placeholder="command (help)" style="flex:1"><button id="bCmd">Send</button></div>
    <div id="log"></div>
  </details></div>
</aside>
</main>
<script>
const TD=[5e-6,1e-5,2e-5,5e-5,1e-4,2e-4,5e-4,1e-3,2e-3,5e-3,.01,.02,.05,.1,.2,.5,1,2,5];
const VD=[.01,.02,.05,.1,.2,.5,1,2,5,10,20];
const $=id=>document.getElementById(id);
const cv=$('cv'),ctx=cv.getContext('2d');
const tr=document.createElement('canvas'),tctx=tr.getContext('2d');
let ws=null,st=null,fr=null,need=true,pers=false,dots=false,fftOn=false,curMode=0;
let cur={tA:null,tB:null,vA:null,vB:null},spec=null,lastFrameT=0,fpsWin=[];

function si(v,u,d=4){if(v===null||!isFinite(v))return '---'+u;const a=Math.abs(v);const P=[[1e6,'M'],[1e3,'k'],[1,''],[1e-3,'m'],[1e-6,'µ'],[1e-9,'n']];
 let p=P.find(x=>a>=x[0]*0.9995)||P[P.length-1];if(a===0)p=[1,''];let s=(v/p[0]).toPrecision(d);if(s.includes('e'))s=(+s).toString();return s+' '+p[1]+u}
function lbl(v,u){return si(v,u,3).replace(/\.0+ /,' ').replace(/(\.\d*?)0+ /,'$1 ')}
function log(t){const l=$('log');l.textContent=(t+'\n'+l.textContent).slice(0,6000)}
function send(c){if(ws&&ws.readyState===1){ws.send(c)}}
let thr={};function sendT(k,c,ms=120){clearTimeout(thr[k]);thr[k]=setTimeout(()=>send(c),ms)}

function connect(){
 ws=new WebSocket('ws://'+location.host+'/ws');ws.binaryType='arraybuffer';
 ws.onopen=()=>{$('conn').classList.add('on');log('connected')};
 ws.onclose=()=>{$('conn').classList.remove('on');setTimeout(connect,1500)};
 ws.onerror=()=>ws.close();
 ws.onmessage=e=>{if(typeof e.data==='string'){let j;try{j=JSON.parse(e.data)}catch(_){return}
   if(j.type==='status'){st=j;updUI()}else if(j.type==='reply'){log(j.text)}}else onBin(e.data)};
}
function onBin(b){const d=new DataView(b);if(d.getUint8(0)!==1)return;const n=d.getUint16(2,true);
 const f={flags:d.getUint8(1),n,valid:d.getUint16(4,true),tdiv:d.getUint8(6),seq:d.getUint32(8,true),dt:d.getFloat32(12,true),t0:d.getFloat32(16,true)};
 const sc=d.getFloat32(20,true);const k=['acOff','vmax','vmin','vavg','vrms','vac','freq','duty','dcMean','dcMin','dcMax'];
 k.forEach((x,i)=>f[x]=d.getFloat32(24+4*i,true));
 f.v=new Float32Array(n);for(let i=0;i<n;i++)f.v[i]=d.getInt16(72+2*i,true)*sc;
 fr=f;if(fftOn)spec=fft(f);need=true;updMeas();
 const now=performance.now();fpsWin.push(now);while(fpsWin.length&&now-fpsWin[0]>2000)fpsWin.shift();
}

// ---------- geometry
function dims(){return{W:cv.width,H:cv.height,PH:fftOn?Math.round(cv.height*0.58):cv.height}}
function vdiv(){return st?VD[st.vdiv]:1}
function tdiv(){return st?TD[st.tdiv]:1e-3}
function vy(v,PH){return PH/2-(v+(st?st.vpos:0))/vdiv()*(PH/8)}
function yv(y,PH){return (PH/2-y)/(PH/8)*vdiv()-(st?st.vpos:0)}
function winT(){return fr?fr.n*fr.dt:10*tdiv()}
function tx(t,W){return ((st?st.hpos:.5)+t/winT())*W}
function xt(x,W){return (x/W-(st?st.hpos:.5))*winT()}

function resize(){const r=cv.getBoundingClientRect(),dp=window.devicePixelRatio||1;
 const w=Math.round(r.width*dp),h=Math.round(r.width*0.8*dp);if(cv.width!==w||cv.height!==h){cv.width=tr.width=w;cv.height=tr.height=h;need=true}}
window.addEventListener('resize',resize);

function grid(W,H,dp){ctx.strokeStyle='#26303a';ctx.lineWidth=1;ctx.beginPath();
 for(let i=1;i<10;i++){const x=Math.round(i*W/10)+.5;ctx.moveTo(x,0);ctx.lineTo(x,H)}
 for(let i=1;i<8;i++){const y=Math.round(i*H/8)+.5;ctx.moveTo(0,y);ctx.lineTo(W,y)}ctx.stroke();
 ctx.strokeStyle='#3a4653';ctx.beginPath();const cx=Math.round(W/2)+.5,cy=Math.round(H/2)+.5;ctx.moveTo(cx,0);ctx.lineTo(cx,H);ctx.moveTo(0,cy);ctx.lineTo(W,cy);
 for(let i=0;i<50;i++){const x=Math.round(i*W/50)+.5;ctx.moveTo(x,cy-3*dp);ctx.lineTo(x,cy+3*dp)}
 for(let i=0;i<40;i++){const y=Math.round(i*H/40)+.5;ctx.moveTo(cx-3*dp,y);ctx.lineTo(cx+3*dp,y)}ctx.stroke();
 ctx.strokeStyle='#2c3640';ctx.strokeRect(.5,.5,W-1,H-1)}

function trace(c,W,PH,dp){if(!fr)return;const v=fr.v,n=fr.n,off=n-fr.valid;c.strokeStyle=c.fillStyle='#f4d03f';c.lineWidth=1.4*dp;c.beginPath();
 const x0=(i)=>((st?st.hpos:.5)+(fr.t0+i)/n)*W;
 if(dots){for(let i=off;i<n;i++){c.fillRect(x0(i)-dp,vy(v[i],PH)-dp,2*dp,2*dp)}return}
 for(let i=off;i<n;i++){const x=x0(i),y=Math.max(-5,Math.min(PH+5,vy(v[i],PH)));i===off?c.moveTo(x,y):c.lineTo(x,y)}c.stroke()}

function draw(){resize();if(!need){requestAnimationFrame(draw);return}need=false;
 const dp=window.devicePixelRatio||1,{W,H,PH}=dims();ctx.fillStyle='#000';ctx.fillRect(0,0,W,H);
 ctx.save();ctx.beginPath();ctx.rect(0,0,W,PH);ctx.clip();
 grid(W,PH,dp);
 if(pers){tctx.globalCompositeOperation='destination-out';tctx.fillStyle='rgba(0,0,0,0.18)';tctx.fillRect(0,0,W,H);tctx.globalCompositeOperation='source-over';trace(tctx,W,PH,dp);ctx.drawImage(tr,0,0)}
 else trace(ctx,W,PH,dp);
 markers(W,PH,dp);cursors(W,PH,dp);ctx.restore();
 if(fftOn)drawFFT(W,H,PH,dp);
 overlay();requestAnimationFrame(draw)}

function markers(W,PH,dp){if(!st)return;ctx.font=(11*dp)+'px ui-monospace,monospace';
 // ground
 const gy=vy(0,PH);ctx.fillStyle='#f4d03f';tri(0,gy,1,dp);
 // trigger level
 const ty=vy(st.tlevel,PH);ctx.fillStyle='#ff8a3d';tri(W,ty,-1,dp);
 ctx.strokeStyle='rgba(255,138,61,.35)';ctx.setLineDash([4*dp,4*dp]);ctx.beginPath();ctx.moveTo(0,ty);ctx.lineTo(W,ty);ctx.stroke();ctx.setLineDash([]);
 // trigger position
 const x=st.hpos*W;ctx.beginPath();ctx.moveTo(x-6*dp,0);ctx.lineTo(x+6*dp,0);ctx.lineTo(x,8*dp);ctx.closePath();ctx.fill()}
function tri(x,y,dir,dp){const s=8*dp;ctx.beginPath();ctx.moveTo(x,y-s/1.6);ctx.lineTo(x+dir*s,y);ctx.lineTo(x,y+s/1.6);ctx.closePath();ctx.fill()}

function cursors(W,PH,dp){const out=$('curs');if(!curMode||!fr){out.textContent='';return}
 ctx.lineWidth=1*dp;ctx.setLineDash([6*dp,4*dp]);
 if(curMode===1){if(cur.tA===null){cur.tA=xt(W*.3,W);cur.tB=xt(W*.7,W)}
  [['tA','#4fc3f7'],['tB','#ff6fb5']].forEach(([k,c])=>{const x=tx(cur[k],W);ctx.strokeStyle=c;ctx.beginPath();ctx.moveTo(x,0);ctx.lineTo(x,PH);ctx.stroke()});
  const dt=cur.tB-cur.tA,va=valAt(cur.tA),vb=valAt(cur.tB);
  out.innerHTML=`A <i>${si(cur.tA,'s')}</i> (${si(va,'V',3)}) &nbsp; B <i>${si(cur.tB,'s')}</i> (${si(vb,'V',3)}) &nbsp; ΔT <i>${si(dt,'s')}</i> &nbsp; 1/ΔT <i>${si(dt?1/Math.abs(dt):NaN,'Hz')}</i> &nbsp; ΔV <i>${si(vb-va,'V',3)}</i>`}
 else{if(cur.vA===null){cur.vA=yv(PH*.3,PH);cur.vB=yv(PH*.7,PH)}
  [['vA','#4fc3f7'],['vB','#ff6fb5']].forEach(([k,c])=>{const y=vy(cur[k],PH);ctx.strokeStyle=c;ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(W,y);ctx.stroke()});
  out.innerHTML=`A <i>${si(cur.vA,'V')}</i> &nbsp; B <i>${si(cur.vB,'V')}</i> &nbsp; ΔV <i>${si(cur.vA-cur.vB,'V')}</i>`}
 ctx.setLineDash([])}
function valAt(t){if(!fr)return NaN;const i=t/fr.dt-fr.t0;const a=Math.floor(i);if(a<fr.n-fr.valid||a+1>=fr.n)return NaN;const f=i-a;return fr.v[a]*(1-f)+fr.v[a+1]*f}

// ---------- FFT
function fft(f){const m=f.valid;if(m<32)return null;let N=1;while(N<m)N<<=1;
 const re=new Float64Array(N),im=new Float64Array(N);const off=f.n-m;let mean=0;for(let i=0;i<m;i++)mean+=f.v[off+i];mean/=m;
 for(let i=0;i<m;i++){re[i]=(f.v[off+i]-mean)*(0.5-0.5*Math.cos(2*Math.PI*i/(m-1)))}
 for(let i=1,j=0;i<N;i++){let b=N>>1;for(;j&b;b>>=1)j^=b;j^=b;if(i<j){[re[i],re[j]]=[re[j],re[i]]}}
 for(let len=2;len<=N;len<<=1){const a=-2*Math.PI/len,wr=Math.cos(a),wi=Math.sin(a);for(let i=0;i<N;i+=len){let cr=1,ci=0;for(let k=0;k<len/2;k++){const p=i+k,q=p+len/2;
  const xr=re[q]*cr-im[q]*ci,xi=re[q]*ci+im[q]*cr;re[q]=re[p]-xr;im[q]=im[p]-xi;re[p]+=xr;im[p]+=xi;const t=cr*wr-ci*wi;ci=cr*wi+ci*wr;cr=t}}}
 const sc=4/m/Math.SQRT2,db=new Float32Array(N/2);let pk=1;
 for(let i=0;i<N/2;i++){db[i]=20*Math.log10(Math.hypot(re[i],im[i])*sc+1e-9);if(i>1&&db[i]>db[pk])pk=i}
 let pf=pk;if(pk>0&&pk<N/2-1){const a=db[pk-1],b=db[pk],c=db[pk+1];const den=a-2*b+c;if(den)pf=pk+0.5*(a-c)/den}
 return{db,df:1/(N*f.dt),peakF:pf/(N*f.dt),peakDb:db[pk]}}
function drawFFT(W,H,PH,dp){const y0=PH+1,h=H-PH-1;ctx.fillStyle='#05070a';ctx.fillRect(0,y0,W,h);
 ctx.strokeStyle='#26303a';ctx.lineWidth=1;ctx.beginPath();for(let i=1;i<10;i++){const x=Math.round(i*W/10)+.5;ctx.moveTo(x,y0);ctx.lineTo(x,H)}
 for(let i=1;i<5;i++){const y=Math.round(y0+i*h/5)+.5;ctx.moveTo(0,y);ctx.lineTo(W,y)}ctx.stroke();
 ctx.fillStyle='#7d8894';ctx.font=(10*dp)+'px ui-monospace,monospace';for(let i=0;i<5;i++)ctx.fillText((20-i*20)+'dBV',4*dp,y0+i*h/5+12*dp);
 if(!spec)return;const db=spec.db,n=db.length,top=20,rng=100;ctx.strokeStyle='#b388ff';ctx.lineWidth=1.2*dp;ctx.beginPath();
 for(let x=0;x<W;x++){const a=Math.floor(x*n/W),b=Math.max(a+1,Math.floor((x+1)*n/W));let m=-999;for(let i=a;i<b&&i<n;i++)m=Math.max(m,db[i]);
  const y=y0+(top-m)/rng*h;x?ctx.lineTo(x,Math.min(H,y)):ctx.moveTo(x,Math.min(H,y))}ctx.stroke();
 const span=n*spec.df;ctx.fillStyle='#b388ff';ctx.textAlign='right';ctx.fillText(`span 0-${si(span,'Hz',3)}  ${si(span/10,'Hz',3)}/div  peak ${si(spec.peakF,'Hz',5)} ${spec.peakDb.toFixed(1)} dBV`,W-6*dp,y0+12*dp);ctx.textAlign='left'}

// ---------- readouts
function overlay(){if(!st)return;const tl=lbl(TD[st.tdiv],'s'),vl=lbl(VD[st.vdiv],'V');
 $('ovl').innerHTML=`<b>CH1 ${vl}/div</b> ${st.cpl?'AC':'DC'} &nbsp; ${tl}/div &nbsp; <span class="t">T ${['A','N','S'][st.tmode]} ${st.tedge?'↓':'↑'} ${si(st.tlevel,'V',3)}</span>`;
 $('ovr').innerHTML=`${si(fr?1/fr.dt:0,'S/s',3)} &nbsp; ${fpsWin.length/2|0} fps`}
function updMeas(){if(!fr)return;const f=fr;const M=[['Freq',f.freq>0?si(f.freq,'Hz',6):'---'],['Period',f.freq>0?si(1/f.freq,'s',5):'---'],['Duty',f.freq>0?(f.duty*100).toFixed(1)+' %':'---'],
 ['Vpp',si(f.vmax-f.vmin,'V')],['Vmax',si(f.vmax,'V')],['Vmin',si(f.vmin,'V')],['Vavg',si(f.vavg,'V')],['Vrms',si(f.vrms,'V')],['AC rms',si(f.vac,'V')],['DC (gate)',si(f.dcMean,'V')]];
 $('meas').innerHTML=M.map(([a,b])=>`<div class="m"><span>${a}</span><b>${b}</b></div>`).join('')}
function on(id,b){$(id).classList.toggle('on',!!b)}
function setVal(id,v){const e=$(id);if(document.activeElement!==e)e.value=v}
function updUI(){const s=st;$('state').textContent=s.state;$('state').className='badge '+s.state;
 $('hdr').textContent=`${s.ip}  ADC ${si(s.fs,'S/s',4)}  heap ${(s.heap/1024)|0}k`;
 $('tdivL').textContent=lbl(TD[s.tdiv],'s');$('vdivL').textContent=lbl(VD[s.vdiv],'V');
 setVal('hpos',s.hpos);setVal('vpos',+s.vpos.toFixed(3));setVal('tlevel',+s.tlevel.toFixed(3));
 on('cDC',!s.cpl);on('cAC',s.cpl);[0,1,2].forEach(i=>on('tm'+i,s.tmode===i));on('te0',!s.tedge);on('te1',s.tedge);
 on('bRun',s.run);$('bRun').textContent=s.run?'Stop':'Run';
 $('bAuto').textContent=s.autoset?'Autoset…':'Autoset';
 on('gSq',s.gsq);$('gSq').textContent=s.gsq?'On':'Off';setVal('gSqHz',s.gsqHz);setVal('gSqDuty',s.gsqDuty);
 $('gSqAct').textContent=s.gsq?('actual '+si(s.gsqAct,'Hz',7)):'';
 on('gSin',s.gsin);$('gSin').textContent=s.gsin?'On':'Off';setVal('gSinHz',s.gsinHz);
 setVal('fe',['direct','bias','custom'][s.fe]);
 $('calInfo').textContent=`zero ${s.calZero.toFixed(1)} mV  gain ${s.calGain.toFixed(4)}\nADC cal: ${s.calSrc}\nADC ${s.fs.toFixed(1)} S/s (measured ${s.fsMeas.toFixed(0)}, corr ${s.rateCorr.toFixed(6)})\nrequested ${s.hwRate}${s.dup?' (each sample sent twice)':''}, SAR div ${s.sarDiv||'default'}, cycles ${s.sarCyc||'default'}\nswap ${s.swap} invert ${s.inv}  sampling CPU ${Math.round(s.load*100)}%  fw ${s.ver}`;
 need=true}

// ---------- controls
document.querySelectorAll('[data-c]').forEach(b=>b.onclick=()=>send(b.dataset.c));
$('bRun').onclick=()=>send(st&&st.run?'stop':'run');$('bSingle').onclick=()=>send('single');$('bAuto').onclick=()=>send('autoset');
$('hpos').oninput=e=>sendT('hpos','hpos '+e.target.value);
$('vpos').onchange=e=>send('vpos '+e.target.value);
$('tlevel').onchange=e=>send('tlevel '+e.target.value);
$('bCenter').onclick=()=>{if(fr)send('vpos '+(-(fr.vmax+fr.vmin)/2).toFixed(4))};
$('b50').onclick=()=>{if(fr)send('tlevel '+((fr.vmax+fr.vmin)/2).toFixed(4))};
$('bFFT').onclick=()=>{fftOn=!fftOn;on('bFFT',fftOn);if(fftOn&&fr)spec=fft(fr);need=true};
$('bPers').onclick=()=>{pers=!pers;on('bPers',pers);tctx.clearRect(0,0,tr.width,tr.height);need=true};
$('bDots').onclick=()=>{dots=!dots;on('bDots',dots);need=true};
[0,1,2].forEach(i=>$('cu'+i).onclick=()=>{curMode=i;[0,1,2].forEach(j=>on('cu'+j,j===i));cur={tA:null,tB:null,vA:null,vB:null};need=true});on('cu0',1);
$('gSq').onclick=()=>send('gen sq '+(st&&st.gsq?'off':'on'));
$('gSqHz').onchange=()=>send('gen sq '+$('gSqHz').value+' '+$('gSqDuty').value);
$('gSqDuty').onchange=()=>send('gen sq '+$('gSqHz').value+' '+$('gSqDuty').value);
$('gSin').onclick=()=>send('gen sine '+(st&&st.gsin?'off':'on'));
$('gSinHz').onchange=()=>send('gen sine '+$('gSinHz').value);
$('fe').onchange=e=>send('fe '+e.target.value);
$('bZero').onclick=()=>send('calzero');
$('bGain').onclick=()=>{const v=$('calV').value;if(v)send('calgain '+v)};
$('bSelf').onclick=()=>{log('self-test running…');send('selftest')};
const q=s=>'"'+s.replace(/\\/g,'\\\\').replace(/"/g,'\\"')+'"';
$('bJoin').onclick=()=>{const s=$('ssid').value;if(!s)return;send('wifi join '+q(s)+' '+q($('pass').value));log('joining '+s+'… reconnect to that network')};
$('bAP').onclick=()=>send('wifi ap');
$('bCmd').onclick=()=>{const c=$('cmd').value.trim();if(c){log('> '+c);send(c);$('cmd').value=''}};
$('cmd').onkeydown=e=>{if(e.key==='Enter')$('bCmd').click()};
$('bCSV').onclick=()=>{if(!fr)return;let s='time_s,volts\n';const off=fr.n-fr.valid;for(let i=off;i<fr.n;i++)s+=((fr.t0+i)*fr.dt).toExponential(6)+','+fr.v[i].toFixed(5)+'\n';dl(new Blob([s],{type:'text/csv'}),'scope.csv')};
$('bPNG').onclick=()=>cv.toBlob(b=>dl(b,'scope.png'));
function dl(b,n){const a=document.createElement('a');a.href=URL.createObjectURL(b);a.download=n;a.click();setTimeout(()=>URL.revokeObjectURL(a.href),2000)}

// ---------- canvas gestures
let drag=null;
cv.addEventListener('pointerdown',e=>{if(!st)return;const r=cv.getBoundingClientRect(),dp=cv.width/r.width;const x=(e.clientX-r.left)*dp,y=(e.clientY-r.top)*dp;const {W,PH}=dims();
 if(y>PH)return;cv.setPointerCapture(e.pointerId);
 if(curMode){const k=curMode===1?(Math.abs(x-tx(cur.tA,W))<Math.abs(x-tx(cur.tB,W))?'tA':'tB'):(Math.abs(y-vy(cur.vA,PH))<Math.abs(y-vy(cur.vB,PH))?'vA':'vB');
  if(x>W*.1&&x<W*.9&&y>PH*.08){drag={k};return}}
 if(x>W*.88)drag={k:'tl'};else if(x<W*.12)drag={k:'vp',y0:y,v0:st.vpos};else if(y<PH*.12)drag={k:'hp'};else drag=null;moveDrag(x,y)});
cv.addEventListener('pointermove',e=>{if(!drag)return;const r=cv.getBoundingClientRect(),dp=cv.width/r.width;moveDrag((e.clientX-r.left)*dp,(e.clientY-r.top)*dp)});
cv.addEventListener('pointerup',()=>drag=null);
function moveDrag(x,y){if(!drag)return;const {W,PH}=dims();
 if(drag.k==='tl'){st.tlevel=yv(y,PH);sendT('tl','tlevel '+st.tlevel.toFixed(4))}
 else if(drag.k==='vp'){st.vpos=drag.v0-(y-drag.y0)/(PH/8)*vdiv();sendT('vp','vpos '+st.vpos.toFixed(4))}
 else if(drag.k==='hp'){st.hpos=Math.max(.05,Math.min(.95,x/W));sendT('hp','hpos '+st.hpos.toFixed(3))}
 else if(drag.k[0]==='t')cur[drag.k]=xt(x,W);else if(drag.k[0]==='v')cur[drag.k]=yv(y,PH);need=true}
cv.addEventListener('wheel',e=>{e.preventDefault();const up=e.deltaY<0;send((e.shiftKey?'vdiv ':'tdiv ')+(up?'-':'+'))},{passive:false});

connect();resize();requestAnimationFrame(draw);setInterval(()=>{need=true},500);
</script></body></html>)rawliteral";
