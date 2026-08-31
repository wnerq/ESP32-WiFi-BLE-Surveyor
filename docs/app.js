(function(){
  const body=document.body;
  const savedView=localStorage.getItem('demo-view')||'standard';
  body.dataset.view=savedView;
  document.querySelectorAll('[data-view]').forEach(btn=>{
    btn.classList.toggle('active',btn.dataset.view===savedView);
    btn.addEventListener('click',()=>{
      body.dataset.view=btn.dataset.view;
      localStorage.setItem('demo-view',btn.dataset.view);
      document.querySelectorAll('[data-view]').forEach(b=>b.classList.toggle('active',b===btn));
    });
  });
  const theme=localStorage.getItem('demo-theme')||'system';
  const sel=document.querySelector('[data-theme-select]');
  function applyTheme(v){
    const actual=v==='system'?(matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light'):v;
    document.documentElement.dataset.theme=actual;
  }
  if(sel){sel.value=theme;sel.addEventListener('change',()=>{localStorage.setItem('demo-theme',sel.value);applyTheme(sel.value)});} applyTheme(theme);
  document.querySelectorAll('[data-demo-action]').forEach(el=>el.addEventListener('click',e=>{e.preventDefault();alert('Static demo: '+el.dataset.demoAction+'. No ESP32 is connected.')}));
  document.querySelectorAll('th[data-sort]').forEach(th=>th.addEventListener('click',()=>{
    const table=th.closest('table'),body=table.tBodies[0],idx=[...th.parentNode.children].indexOf(th),rows=[...body.rows],asc=th.dataset.dir!=='asc';
    rows.sort((a,b)=>{const av=a.cells[idx]?.dataset.value??a.cells[idx]?.textContent.trim()??'',bv=b.cells[idx]?.dataset.value??b.cells[idx]?.textContent.trim()??'';const an=Number(av),bn=Number(bv);return (Number.isFinite(an)&&Number.isFinite(bn)?an-bn:av.localeCompare(bv))*(asc?1:-1)}).forEach(r=>body.appendChild(r)); th.dataset.dir=asc?'asc':'desc';
  }));
  document.querySelectorAll('canvas[data-points]').forEach(canvas=>{
    const pts=canvas.dataset.points.split(',').map(Number),dpr=devicePixelRatio||1,w=canvas.clientWidth||720,h=280;canvas.width=w*dpr;canvas.height=h*dpr;const c=canvas.getContext('2d');c.scale(dpr,dpr);c.clearRect(0,0,w,h);c.strokeStyle=getComputedStyle(document.documentElement).getPropertyValue('--line');c.fillStyle=getComputedStyle(document.documentElement).getPropertyValue('--muted');c.font='12px Arial';const l=48,r=16,t=18,b=34,pw=w-l-r,ph=h-t-b;[-30,-50,-70,-90].forEach(v=>{const y=t+(-30-v)/70*ph;c.beginPath();c.moveTo(l,y);c.lineTo(w-r,y);c.stroke();c.fillText(v+' dBm',2,y+4)});c.strokeStyle=getComputedStyle(document.documentElement).getPropertyValue('--link');c.lineWidth=2;c.beginPath();pts.forEach((v,i)=>{const x=l+(pts.length===1?0:i/(pts.length-1))*pw,y=t+(-30-v)/70*ph;i?c.lineTo(x,y):c.moveTo(x,y)});c.stroke();
  });
  const toggle=document.querySelector('[data-ble-toggle]');
  if(toggle)toggle.addEventListener('click',e=>{e.preventDefault();document.querySelector('[data-ble-disabled]').classList.add('state-hidden');document.querySelector('[data-ble-enabled]').classList.remove('state-hidden');});
})();
