// Execute the firmware's actual terminal script with a small DOM/HTTP test double.
// This verifies behavior; it is not a browser rendering or on-device transport test.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const source = fs.readFileSync(new URL('../src/main.cpp', import.meta.url), 'utf8');
const html = source.split('R"TERMINAL(')[1].split(')TERMINAL"')[0];
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const elements = new Map();
for (const id of ['terminal-output','terminal-status','terminal-pause','terminal-scroll',
  'terminal-filter','live-updates-toggle','terminal-clear','terminal-download',
  'terminal-command','terminal-send','terminal-command-status','terminal-hide-input','terminal-command-form']) {
  elements.set(id, {textContent:'',value:'',checked:true,scrollTop:0,scrollHeight:42,disabled:false,focus(){}});
}
const get = id => elements.get(id);
const document = {hidden:false,getElementById:get,createElement:()=>({click(){}})};
const timers = new Map();
let timerId=0,requests=[],responses=[],requestOptions=[],download;
const context = {
  document,TextDecoder,TextEncoder,AbortController,Blob,
  URL:{createObjectURL(blob){download=blob;return 'blob:test';},revokeObjectURL(){}},
  setTimeout(fn,delay){timers.set(++timerId,{fn,delay});return timerId;},
  clearTimeout(id){timers.delete(id);},
  async fetch(url,options){requests.push(url);requestOptions.push(options);const next=responses.shift();if(next instanceof Error)throw next;assert.ok(next,'Unexpected fetch');return next;},
};
function response(text, {boot='100',next='10',dropped='0',more='0',status=200}={}) {
  const bytes=typeof text==='string'?new TextEncoder().encode(text):text;
  return {ok:status===200,status,headers:new Map([
    ['X-Terminal-Boot',boot],['X-Terminal-Next',next],['X-Terminal-Dropped',dropped],['X-Terminal-More',more],
  ]),async arrayBuffer(){return bytes.buffer.slice(bytes.byteOffset,bytes.byteOffset+bytes.byteLength);}};
}
const settle=()=>new Promise(resolve=>setImmediate(resolve));
async function step(){
  const task=[...timers.entries()].sort((a,b)=>a[1].delay-b[1].delay)[0];
  assert.ok(task,'No scheduled task');timers.delete(task[0]);task[1].fn();await settle();
}

responses.push(response('BOOT ready\nINFRA connected\n<script>literal</script>\n'));
vm.runInNewContext(script, context);await settle();
assert.match(get('terminal-output').textContent,/<script>literal<\/script>/);
assert.equal(requests.length,1);
assert.equal(timers.size,1,'Only the next poll should remain scheduled');
get('terminal-filter').value='infra';get('terminal-filter').oninput();
assert.equal(get('terminal-output').textContent,'INFRA connected');
get('terminal-filter').value='';get('terminal-filter').oninput();
get('terminal-pause').onclick();await step();assert.equal(requests.length,1);
get('terminal-pause').onclick();
responses.push(response('new output\n',{next:'20',dropped:'1'}));await step();
assert.match(requests.at(-1),/cursor=10&boot=100$/);
assert.match(get('terminal-output').textContent,/Earlier output no longer retained/);
responses.push(response('rebooted\n',{boot:'200',next:'9'}));await step();
assert.match(get('terminal-output').textContent,/Device restarted/);
responses.push(new Error('network down'));await step();
assert.match(get('terminal-status').textContent,/Disconnected.*retrying/);
responses.push(response(new Uint8Array([0xc3]),{boot:'200',next:'10',more:'1'}));await step();
assert.match(requests.at(-1),/cursor=9&boot=200$/);
responses.push(response(new Uint8Array([0xa9,10]),{boot:'200',next:'12'}));await step();
assert.ok(get('terminal-output').textContent.endsWith('é\n'),'UTF-8 survives byte chunk boundaries');
get('terminal-clear').onclick();assert.equal(get('terminal-output').textContent,'');
responses.push(response('x'.repeat(70000),{boot:'200',next:'70012'}));await step();
assert.equal(get('terminal-output').textContent.length,65536);
get('terminal-download').onclick();assert.equal((await download.text()).length,65536);
document.hidden=true;const count=requests.length;await step();await step();assert.equal(requests.length,count);
document.hidden=false;get('live-updates-toggle').checked=false;await step();
assert.match(get('terminal-status').textContent,/Live updates are off/);
assert.equal(requests.length,count);
console.log('PASS: terminal polling, pause/resume, filtering, literal markup, gap/restart recovery, UTF-8, error retry, clear/download and memory bound');

// Command entry uses POST, clears input, resumes output and never retries a command.
get('terminal-command').value='wifi on';responses.push(response('',{status:202}));
await get('terminal-command-form').onsubmit({preventDefault(){}});
assert.equal(requests.at(-1),'/api/terminal/command');
assert.equal(requestOptions.at(-1).method,'POST');
assert.equal(requestOptions.at(-1).body,'wifi on');
assert.equal(requestOptions.at(-1).headers['X-Terminal-Command'],'1');
assert.equal(get('terminal-command').value,'');
assert.equal(get('live-updates-toggle').checked,true);
assert.match(get('terminal-command-status').textContent,/Queued/);
const beforeLong=requests.length;
get('terminal-command').value='\u00e9'.repeat(100);
await get('terminal-command-form').onsubmit({preventDefault(){}});
assert.equal(requests.length,beforeLong);assert.match(get('terminal-command-status').textContent,/192 bytes/);
get('terminal-hide-input').checked=true;get('terminal-hide-input').onchange();
assert.equal(get('terminal-command').type,'password');
get('terminal-command').value='appass private123';responses.push(new Error('offline'));
await get('terminal-command-form').onsubmit({preventDefault(){}});
assert.match(get('terminal-command-status').textContent,/could not be confirmed/);
assert.equal(get('terminal-command').value,'');
assert.ok(!get('terminal-output').textContent.includes('private123'));
assert.equal(requests.length,beforeLong+1); // no automatic command retry
console.log('PASS: terminal command POST, byte limit, hidden input, acknowledgment and ambiguous delivery');
