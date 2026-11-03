// The built-in website. It is served when sdmc:/switch/Toypad-Emu/web/index.html does not exist;
// put your own index.html (plus any .css/.js/.png it uses) in that folder to replace it.
#include "web_api.h"

namespace sky {

const char* const kWebPage = R"WEB(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Toypad-Emu</title>
<style>
:root{--bg:#10131c;--panel:#1a2030;--hi:#252d42;--text:#e8ecf6;--dim:#8a95b0;--accent:#4aa3ff;--green:#3ecf8e;--amber:#f2b53c;--red:#ef5b5b;color-scheme:dark}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{display:flex;flex-wrap:wrap;gap:10px;align-items:center;padding:12px 16px;background:var(--panel);position:sticky;top:0;z-index:5}
h1{font-size:18px;margin:0 auto 0 0}
.pill{padding:4px 10px;border-radius:99px;background:var(--hi);color:var(--dim);font-size:13px;white-space:nowrap}
.pill.ok{color:var(--green)}.pill.warn{color:var(--amber)}.pill.bad{color:var(--red)}
main{display:grid;gap:14px;padding:14px;max-width:1200px;margin:0 auto}
@media(min-width:900px){main{grid-template-columns:1fr 1fr;align-items:start}.wide{grid-column:1/-1}}
section{background:var(--panel);border-radius:14px;padding:14px}
h2{font-size:15px;margin:0 0 10px;display:flex;align-items:center;gap:8px}
h2 small{margin-left:auto;color:var(--dim);font-weight:400}
button,select,input[type=search]{font:inherit;color:var(--text);background:var(--hi);border:1px solid #33405c;border-radius:9px;padding:7px 12px}
button{cursor:pointer}button:hover{border-color:var(--accent)}button:disabled{opacity:.5;cursor:default}
button.on{background:var(--accent);color:#06101f;border-color:var(--accent)}
.modes{display:flex;gap:8px;flex-wrap:wrap}.modes button{flex:1;min-width:120px}
.bar{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:10px}.bar input{flex:1;min-width:140px}
.grid{display:grid;gap:8px;grid-template-columns:repeat(var(--cols,4),1fr)}
.slot{min-height:78px;border:2px dashed #33405c;border-radius:12px;padding:8px;background:#141a28;position:relative;cursor:pointer;overflow:hidden}
.slot.full{border-style:solid;border-color:#2f7a5c;background:var(--hi)}
.slot.sel{border-color:var(--accent)}.slot.dirty{border-color:var(--amber)}
.slot .l{font-size:11px;color:var(--dim)}.slot .n{font-size:13px;margin-top:4px;word-break:break-word}.slot .u{font-size:11px;color:var(--dim)}
.slot .x{position:absolute;right:6px;top:4px;font-size:11px;color:var(--amber)}
.lego{display:flex;gap:12px;justify-content:center;flex-wrap:wrap}.lego>div{flex:1;min-width:200px;max-width:260px}.lego h3{margin:0 0 6px;font-size:12px;color:var(--dim);text-align:center;font-weight:500}
.lego .grid{grid-template-columns:1fr 1fr}.lego .grid .slot:last-child:nth-child(odd){grid-column:1/-1}
.list{max-height:62vh;overflow:auto;border-radius:10px}
.row{display:flex;align-items:center;gap:8px;padding:7px 8px;border-bottom:1px solid #202840}
.row:hover{background:#1f2740}.row .nm{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;cursor:pointer}
.row .tag{font-size:11px;color:var(--dim)}.row .on{font-size:11px;color:var(--green)}
.star{background:none;border:0;padding:2px 6px;font-size:18px;color:#53607e}.star.f{color:var(--amber)}
.drop{border:2px dashed #33405c;border-radius:12px;padding:12px;text-align:center;color:var(--dim);margin-top:10px}.drop.over{border-color:var(--accent);color:var(--text)}
#toast{position:fixed;left:50%;bottom:18px;transform:translateX(-50%);background:#000c;border:1px solid #33405c;padding:9px 16px;border-radius:10px;max-width:90vw;opacity:0;transition:opacity .2s;pointer-events:none;z-index:9}
#toast.show{opacity:1}#toast.err{border-color:var(--red)}
.hint{color:var(--dim);font-size:12px;margin-top:8px}
</style>
</head>
<body>
<header>
  <h1 id="title">Toypad-Emu</h1>
  <span class="pill" id="game">Game: waiting</span>
  <span class="pill" id="usb">connecting...</span>
</header>
<main>
  <section class="wide"><h2>Device</h2>
    <div class="modes" id="modes"></div>
    <div class="hint">Switching drops USB for a moment, so the console sees an unplug and a plug-in.</div>
  </section>
  <section><h2 id="slotsTitle">On the portal <small id="slotsCount"></small></h2>
    <div id="slots"></div>
    <div class="bar" style="margin:10px 0 0"><button id="clear">Clear all</button><button id="replug">Re-plug USB</button></div>
    <div class="hint" id="slotsHint"></div>
  </section>
  <section><h2>Library <small id="libCount"></small></h2>
    <div class="bar">
      <input type="search" id="q" placeholder="Search figures">
      <select id="game2"></select>
      <select id="type"><option value="all">All types</option><option value="c">Characters</option><option value="v">Vehicles</option></select>
      <button id="favOnly" title="Favorites only">&#9733; only</button>
    </div>
    <div class="list" id="list"></div>
    <div class="drop" id="drop">Drop figure dumps here, or <button id="pick">choose files</button><input type="file" id="file" multiple hidden>
      <div class="hint" id="sizeHint"></div></div>
  </section>
</main>
<div id="toast"></div>
<script>
'use strict';
const $=s=>document.querySelector(s);
function h(tag,attrs,...kids){const e=document.createElement(tag);for(const k in(attrs||{})){const v=attrs[k];if(k==='class')e.className=v;else if(k.startsWith('on'))e.addEventListener(k.slice(2),v);else if(v!==false&&v!=null)e.setAttribute(k,v===true?'':v)}for(const c of kids.flat())if(c!=null)e.append(c.nodeType?c:document.createTextNode(c));return e}
let S=null,F=null,filesSeen=-1,target=null,moveFrom=null,lastSeq=-1,shown=300,tt=0;
const MODES=[['skylanders','Skylanders'],['infinity','Disney Infinity'],['lego','LEGO Dimensions']];
function toast(m,ok){if(!m)return;const t=$('#toast');t.textContent=m;t.className='show'+(ok===false?' err':'');clearTimeout(tt);tt=setTimeout(()=>t.className='',3500)}
async function api(op,params,body){
  const qs=new URLSearchParams(params||{}).toString();
  try{const r=await fetch('/api/'+op+(qs?'?'+qs:''),{method:'POST',headers:{'X-Requested-With':'Toypad-Emu'},body});
    const j=await r.json();toast(j.message,j.ok);poll();return j}
  catch(e){toast('Could not reach the Switch',false);return{ok:false}}
}
async function poll(){
  try{S=await(await fetch('/api/state')).json();
    if(S.ready===false){$('#usb').textContent='starting...';return}
    if(S.filesVersion!==filesSeen){F=await(await fetch('/api/files')).json();filesSeen=S.filesVersion;shown=300;fillGames()}
    if(S.msgSeq!==lastSeq){if(lastSeq>=0)toast(S.message);lastSeq=S.msgSeq}
    render()}
  catch(e){const u=$('#usb');u.textContent='Switch not reachable';u.className='pill bad'}
}
const pretty=p=>{let s=p.split('/').pop().replace(/\.[^.]*$/,'');s=s.replace(/^\d+_/,'').replace(/_/g,' ');return s};
function fillGames(){const g=$('#game2');const cur=g.value;g.replaceChildren(h('option',{value:''},'All games'),...(F.games||[]).map(x=>h('option',{value:x},x)));g.value=[...g.options].some(o=>o.value===cur)?cur:'';g.hidden=!(F.games||[]).length;$('#type').hidden=S.mode!=='lego';$('#favOnly').hidden=false}
function render(){
  $('#title').textContent=S.title;
  const gm=$('#game');gm.textContent=S.gameConnected?'Game: connected':'Game: waiting';gm.className='pill'+(S.gameConnected?' ok':'');
  const u=$('#usb');u.textContent=S.usb.label;u.className='pill '+({ok:'ok',bad:'bad',warn:'warn'}[S.usb.level]||'');
  const m=$('#modes');if(m.dataset.mode!==S.mode||!m.children.length){m.dataset.mode=S.mode;m.replaceChildren(...MODES.map(([k,n])=>h('button',{class:k===S.mode?'on':'',onclick:()=>{if(k!==S.mode)api('mode',{mode:k})}},n)))}
  $('#slotsTitle').firstChild.textContent=S.slotsTitle+' ';
  const used=S.slots.filter(s=>s.present).length;$('#slotsCount').textContent=used+' / '+S.slots.length+' used';
  $('#slotsHint').textContent=S.mode==='skylanders'?'Click a figure to take it off.':'Click an empty position to choose where the next figure goes. Click a figure to take it off'+(S.mode==='lego'?'; use \u21c4 to move it.':'.');
  const card=s=>h('div',{class:'slot'+(s.present?' full':'')+(s.dirty?' dirty':'')+((target===s.i||moveFrom===s.i)?' sel':''),onclick:()=>slotClick(s)},
    h('div',{class:'l'},s.label),s.dirty?h('div',{class:'x'},'unsaved'):null,
    s.present?[h('div',{class:'n'},s.name||pretty(s.file||'')),h('div',{class:'u'},s.uid),S.mode==='lego'?h('button',{class:'star',title:'Move',onclick:e=>{e.stopPropagation();moveFrom=moveFrom===s.i?null:s.i;render()}},'\u21c4'):null]:h('div',{class:'n',style:'color:var(--dim)'},'empty'));
  const box=$('#slots');
  if(S.layout==='lego'){const by=i=>S.slots[i];const grp=(t,ids)=>h('div',null,h('h3',null,t),h('div',{class:'grid'},ids.map(i=>card(by(i)))));
    box.replaceChildren(h('div',{class:'lego'},grp('LEFT PAD',[0,3,4]),grp('CENTER PAD',[1]),grp('RIGHT PAD',[2,5,6])))}
  else box.replaceChildren(h('div',{class:'grid',style:'--cols:'+S.cols},S.slots.map(card)));
  $('#sizeHint').textContent='Files must be exactly '+S.figureSize+' bytes.';
  $('#type').hidden=S.mode!=='lego';
  renderList()}
function slotClick(s){
  if(moveFrom!=null){if(!s.present&&s.i!==moveFrom){api('move',{from:moveFrom,to:s.i});moveFrom=null;return}moveFrom=null;render();return}
  if(s.present){api('remove',{slot:s.i});return}
  target=target===s.i?null:s.i;render()}
function renderList(){
  if(!F)return;const q=$('#q').value.trim().toLowerCase(),g=$('#game2').value,ty=$('#type').value,fo=$('#favOnly').classList.contains('on');
  const placed=new Set(S.slots.filter(s=>s.present).map(s=>s.file));
  const rows=F.files.filter(f=>(!q||f.n.toLowerCase().includes(q)||f.p.toLowerCase().includes(q))&&(!g||(f.g||'')===g)&&(ty==='all'||(ty==='v')===!!f.v)&&(!fo||f.f));
  $('#libCount').textContent=rows.length+' of '+F.files.length;
  const list=$('#list');
  if(!F.files.length){list.replaceChildren(h('div',{class:'hint',style:'padding:10px'},'No figure files yet. Upload some below or copy them to '+S.dir));return}
  list.replaceChildren(...rows.slice(0,shown).map(f=>h('div',{class:'row'},
    h('button',{class:'star'+(f.f?' f':''),title:'Favorite',onclick:()=>api('favorite',{file:f.p})},f.f?'\u2605':'\u2606'),
    h('div',{class:'nm',title:f.p,onclick:()=>place(f)},pretty(f.n)),
    f.g?h('span',{class:'tag'},f.g):null,placed.has(f.p)?h('span',{class:'on'},'on '+S.deviceName):null,
    h('button',{onclick:()=>place(f)},'Place'))),
    rows.length>shown?h('div',{class:'row'},h('button',{onclick:()=>{shown+=300;renderList()}},'Show more')):null)}
async function place(f){const p={file:f.p};if(target!=null&&S.mode!=='skylanders')p.slot=target;const j=await api('load',p);if(j.ok)target=null}
async function upload(files){
  let okN=0;for(const f of files){
    if(f.size!==S.figureSize){toast(f.name+': must be '+S.figureSize+' bytes',false);continue}
    const j=await api('upload',{name:f.name},f);if(j.ok)okN++}
  if(okN)await api('rescan')}
$('#q').addEventListener('input',()=>{shown=300;renderList()});
for(const id of['game2','type'])$('#'+id).addEventListener('change',()=>{shown=300;renderList()});
$('#favOnly').addEventListener('click',e=>{e.currentTarget.classList.toggle('on');shown=300;renderList()});
$('#clear').addEventListener('click',()=>{if(confirm('Take everything off?'))api('clear')});
$('#replug').addEventListener('click',()=>api('replug'));
$('#pick').addEventListener('click',()=>$('#file').click());
$('#file').addEventListener('change',e=>{upload([...e.target.files]);e.target.value=''});
const dz=$('#drop');
dz.addEventListener('dragover',e=>{e.preventDefault();dz.classList.add('over')});
dz.addEventListener('dragleave',()=>dz.classList.remove('over'));
dz.addEventListener('drop',e=>{e.preventDefault();dz.classList.remove('over');upload([...e.dataTransfer.files])});
poll();setInterval(poll,1500);
</script>
</body>
</html>
)WEB";

}  // namespace sky
