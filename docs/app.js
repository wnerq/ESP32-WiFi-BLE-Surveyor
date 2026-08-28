(function(){
  const saved=localStorage.getItem('demo-theme')||'system';
  function appliedTheme(v){
    if(v==='system') return matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light';
    return v;
  }
  document.documentElement.dataset.theme=appliedTheme(saved);

  addEventListener('DOMContentLoaded',()=>{
    document.querySelectorAll('[data-theme-select]').forEach(sel=>{
      sel.value=saved;
      sel.addEventListener('change',()=>{
        localStorage.setItem('demo-theme',sel.value);
        document.documentElement.dataset.theme=appliedTheme(sel.value);
        drawAllCharts();
      });
    });

    document.querySelectorAll('th[data-sort]').forEach(th=>{
      th.addEventListener('click',()=>sortTable(th.closest('table'), th.cellIndex));
    });

    document.querySelectorAll('[data-demo-action]').forEach(el=>{
      el.addEventListener('click',e=>{
        e.preventDefault();
        showToast(el.dataset.demoAction || 'Demo action');
      });
    });

    drawAllCharts();
  });

  function sortTable(table,col){
    const body=table.tBodies[0], rows=[...body.rows];
    const asc=table.dataset.sortCol!=col || table.dataset.sortDir!=='asc';
    rows.sort((a,b)=>{
      let x=a.cells[col].dataset.value ?? a.cells[col].innerText;
      let y=b.cells[col].dataset.value ?? b.cells[col].innerText;
      const nx=parseFloat(x), ny=parseFloat(y);
      if(!Number.isNaN(nx)&&!Number.isNaN(ny)) return asc?nx-ny:ny-nx;
      return asc?x.localeCompare(y):y.localeCompare(x);
    });
    rows.forEach(r=>body.appendChild(r));
    table.dataset.sortCol=col; table.dataset.sortDir=asc?'asc':'desc';
  }

  function showToast(msg){
    let t=document.getElementById('demo-toast');
    if(!t){ t=document.createElement('div'); t.id='demo-toast';
      Object.assign(t.style,{position:'fixed',right:'18px',bottom:'18px',padding:'12px 16px',
        borderRadius:'7px',background:'#20262e',color:'#fff',zIndex:9999,boxShadow:'0 3px 12px #0005'});
      document.body.appendChild(t);
    }
    t.textContent=msg+' — simulated demo only';
    t.style.display='block'; clearTimeout(window.__toast);
    window.__toast=setTimeout(()=>t.style.display='none',2200);
  }

  function drawChart(canvas){
    const raw=canvas.dataset.points; if(!raw)return;
    const pts=raw.split(',').map(Number), ctx=canvas.getContext('2d');
    const ratio=devicePixelRatio||1, w=canvas.clientWidth, h=280;
    canvas.width=w*ratio; canvas.height=h*ratio; ctx.scale(ratio,ratio);
    const dark=document.documentElement.dataset.theme==='dark';
    const fg=dark?'#dce5ee':'#27313b', grid=dark?'#3a4652':'#d8dee5', line=dark?'#79b5ff':'#1666c5';
    ctx.clearRect(0,0,w,h); ctx.font='12px system-ui'; ctx.fillStyle=fg;
    const L=52,R=18,T=18,B=35, pw=w-L-R, ph=h-T-B;
    const ymin=-100,ymax=-30;
    [-100,-90,-80,-70,-60,-50,-40,-30].forEach(v=>{
      const y=T+(ymax-v)/(ymax-ymin)*ph;
      ctx.strokeStyle=grid; ctx.beginPath();ctx.moveTo(L,y);ctx.lineTo(w-R,y);ctx.stroke();
      ctx.fillStyle=fg;ctx.fillText(v+' dBm',4,y+4);
    });
    ctx.strokeStyle=line;ctx.lineWidth=2;ctx.beginPath();
    pts.forEach((v,i)=>{
      const x=L+(pts.length===1?0: i/(pts.length-1))*pw;
      const y=T+(ymax-v)/(ymax-ymin)*ph;
      if(i===0)ctx.moveTo(x,y); else ctx.lineTo(x,y);
    });ctx.stroke();
    pts.forEach((v,i)=>{
      const x=L+(pts.length===1?0: i/(pts.length-1))*pw;
      const y=T+(ymax-v)/(ymax-ymin)*ph;
      ctx.fillStyle=line;ctx.beginPath();ctx.arc(x,y,3,0,Math.PI*2);ctx.fill();
    });
    ctx.fillStyle=fg;ctx.fillText('Older scans',L,h-10);ctx.fillText('Newest',Math.max(L,w-R-45),h-10);
  }
  function drawAllCharts(){ document.querySelectorAll('canvas[data-points]').forEach(drawChart); }
  addEventListener('resize',()=>{ clearTimeout(window.__rs);window.__rs=setTimeout(drawAllCharts,120); });
})();
