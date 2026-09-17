// Run the actual firmware layout script against a small DOM model.
import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const source=fs.readFileSync(new URL('../src/main.cpp',import.meta.url),'utf8');
const script=source.match(/R"LAYOUT\(<script>([\s\S]*?)<\/script>\)LAYOUT"/)[1];
class Node {
  constructor(tag='div',text='') {this.tag=tag;this.text=text;this.nodes=[];this.dataset={};this.listeners={};this.attributes={};this.className='';this.id='';this.hidden=false;}
  get children(){return this.nodes.filter(n=>n.tag!=='#comment');}
  get textContent(){return this.text+this.nodes.map(n=>n.textContent).join('');}
  set textContent(value){this.text=value;this.nodes=[];}
  get classList(){const self=this;return {
    contains(c){return self.className.split(' ').includes(c);},
    add(...cs){self.className=[...new Set([...self.className.split(' ').filter(Boolean),...cs])].join(' ');},
    remove(...cs){self.className=self.className.split(' ').filter(c=>!cs.includes(c)).join(' ');}
  };}
  insertBefore(node,ref){node.remove();const i=ref?this.nodes.indexOf(ref):this.nodes.length;assert.ok(i>=0);this.nodes.splice(i,0,node);node.parent=this;return node;}
  append(...nodes){nodes.forEach(n=>this.insertBefore(n,null));}
  appendChild(node){this.append(node);return node;}
  insertAdjacentElement(position,node){assert.equal(position,'afterend');this.parent.insertBefore(node,this.parent.nodes[this.parent.nodes.indexOf(this)+1]||null);}
  prepend(node){this.insertBefore(node,this.nodes[0]);}
  remove(){if(this.parent){this.parent.nodes.splice(this.parent.nodes.indexOf(this),1);this.parent=null;}}
  cloneNode(deep){const n=new Node(this.tag,this.text);n.className=this.className;if(deep)this.nodes.forEach(c=>n.append(c.cloneNode(true)));return n;}
  querySelectorAll(selector){
    if(selector===':scope > h2')return this.children.filter(c=>c.tag==='h2');
    const matches=n=>selector.split(',').some(s=>s.trim().startsWith('.')?n.classList.contains(s.trim().slice(1)):n.tag===s.trim());
    return this.children.flatMap(c=>[...(matches(c)?[c]:[]),...c.querySelectorAll(selector)]);
  }
  querySelector(selector){return this.querySelectorAll(selector)[0]||null;}
  addEventListener(type,fn){(this.listeners[type]??=[]).push(fn);}
  dispatch(type,values={}){const e={button:0,pointerId:1,preventDefault(){this.prevented=true;},...values};for(const fn of this.listeners[type]||[])fn(e);return e;}
  setAttribute(k,v){this.attributes[k]=v;}
  focus(){this.focused=true;}
  setPointerCapture(){}
  scrollIntoView(){}
  getClientRects(){return this.hidden?[]:[this.getBoundingClientRect()];}
  getBoundingClientRect(){const index=this.parent.children.filter(c=>c.classList.contains('card')&&!c.hidden).indexOf(this);return {top:index*200,bottom:index*200+180,height:180};}
}
function page({path='/scan',saved=null,blocked=false}={}) {
  const root=new Node();root.className='container';
  const header=new Node('h1','Survey'),footer=new Node('footer','Footer');root.append(header);
  const cards=['Status','History','Observed Networks','Diagnostics'].map((title,i)=>{
    const c=new Node();c.className='card';c.id=['status','history','observed','diagnostics'][i];
    const h=new Node('h2',title);c.append(h);root.append(c);return c;
  });root.append(footer);
  const document=new Node();document.readyState='complete';document.querySelector=()=>root;
  document.createElement=tag=>new Node(tag);document.createComment=text=>new Node('#comment',text);
  const values=new Map();const key='esp32-card-layout-v1:'+(path==='/'?'/scan':path);
  if(saved!==null)values.set(key,saved);
  let observer,requests=0;
  const window=new Node();window.scrollBy=()=>{};
  vm.runInNewContext(script,{document,window,location:{pathname:path},innerHeight:800,
    localStorage:{getItem(k){if(blocked)throw Error();return values.get(k)||null;},setItem(k,v){if(blocked)throw Error();values.set(k,v);},removeItem(k){if(blocked)throw Error();values.delete(k);}},
    MutationObserver:class {constructor(fn){observer=fn;}observe(){}},
    requestAnimationFrame:()=>1,cancelAnimationFrame(){},fetch:()=>{requests++;return Promise.resolve();}
  });
  const order=()=>root.children.filter(n=>n.classList.contains('card')).map(n=>n.id);
  const handle=i=>cards[i].querySelector('.card-move');
  return {root,cards,document,window,values,key,order,handle,refresh:()=>observer(),requests:()=>requests,
    reset:root.querySelector('.layout-controls').querySelector('button'),status:root.querySelector('.layout-status'),header,footer};
}
const p=page();
assert.deepEqual(p.order(),['status','history','observed','diagnostics']);
p.handle(0).dispatch('keydown',{key:'ArrowDown'});
assert.deepEqual(p.order(),['history','status','observed','diagnostics']);
assert.deepEqual(JSON.parse(p.values.get(p.key)),p.order());
p.handle(0).dispatch('keydown',{key:'End'});assert.equal(p.order().at(-1),'status');
p.handle(0).dispatch('keydown',{key:'Home'});assert.equal(p.order()[0],'status');
// Hidden developer cards retain their slots and keyboard navigation skips them.
p.cards[1].hidden=true;p.handle(0).dispatch('keydown',{key:'ArrowDown'});
assert.deepEqual(p.order(),['history','observed','status','diagnostics']);
p.cards[1].hidden=false;
// Pointer gestures (mouse or touch) only begin on a handle; canceled drags don't save.
const before=p.order();p.handle(0).dispatch('pointerdown',{clientY:420,pointerType:'touch'});
p.document.dispatch('pointermove',{clientY:10,pointerType:'touch'});
p.document.dispatch('keydown',{key:'Escape'});assert.deepEqual(p.order(),before);
p.handle(0).dispatch('pointerdown',{clientY:420,pointerType:'touch'});
p.document.dispatch('pointermove',{clientY:10,pointerType:'touch'});
p.document.dispatch('pointerup');assert.equal(p.order()[0],'status');
assert.equal(p.cards[0].listeners.pointerdown,undefined);
// Replacing a live fragment restores exactly one handle without changing order.
const h=p.cards[2].querySelector('h2');h.remove();p.cards[2].prepend(new Node('h2','Observed Networks'));
p.refresh();p.refresh();assert.equal(p.cards[2].querySelectorAll('.card-move').length,1);
assert.equal(p.order()[0],'status');assert.equal(p.root.children[0],p.header);assert.equal(p.root.children.at(-1),p.footer);
p.reset.dispatch('click');assert.deepEqual(p.order(),['status','history','observed','diagnostics']);assert.equal(p.values.has(p.key),false);
const stored=JSON.stringify(['observed','history','status','diagnostics']);
assert.deepEqual(page({path:'/',saved:stored}).order(),['observed','history','status','diagnostics']);
assert.equal(page({path:'/settings'}).key,'esp32-card-layout-v1:/settings');
assert.deepEqual(page({saved:'["missing","history","history",9]'}).order(),['history','status','observed','diagnostics']);
assert.equal(page({saved:'bad json'}).order()[0],'status');
const blocked=page({blocked:true});blocked.handle(0).dispatch('keydown',{key:'End'});
assert.equal(blocked.order().at(-1),'status');assert.match(blocked.status.textContent,/unavailable/);
const terminal=page({path:'/terminal'});terminal.handle(0).dispatch('keydown',{key:'End'});assert.equal(terminal.requests(),0);
console.log('PASS: keyboard/touch ordering, cancellation, hidden cards, persistence, reset, live fragments and storage failures');

const helpScript=source.slice(source.indexOf('String contextHelpScript()')).match(/<script>([\s\S]*?)<\/script>/)[1];
for(const path of ['/help','/settings']){
  const document=new Node();document.readyState='complete';document.body=document;
  document.createElement=tag=>new Node(tag);
  const memory=new Node();memory.className='card';memory.append(new Node('h2','Developer Memory'));
  const history=new Node();history.className='card';history.append(new Node('h2','History'));
  const unknown=new Node();unknown.className='card';unknown.append(new Node('h2','Unlisted feature'));
  document.append(memory,history,unknown);
  vm.runInNewContext(helpScript,{document,location:{pathname:path},MutationObserver:class {observe(){}}});
  assert.equal(memory.querySelectorAll('.card-help-developer').length,0);
  assert.equal(memory.querySelectorAll('.card-help-standard').length,path==='/help'?0:1);
  assert.equal(history.querySelectorAll('.card-help-developer').length,1,'Keep distinct developer guidance');
  assert.equal(unknown.querySelectorAll('.card-help-developer').length,0,'No generic developer filler');
  assert.equal(memory.querySelectorAll('.card-help-link').length,1);
}
console.log('PASS: no duplicate help summaries or developer filler; distinct technical detail retained');
