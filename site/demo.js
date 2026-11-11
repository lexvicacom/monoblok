(function(){
  const $=(id)=>document.getElementById(id);
  const statusEl=$('demo-status');
  const logEl=$('sse-log');
  if(!statusEl||!logEl)return;

  let cancelSubs=[];
  const graph=$('sse-graph');
  const readings=[];
  let graphFrame;
  const colors=['#d9ed94','#a7d7b8','#efcb8a','#a9c8ed','#e6b8cf','#b7b3ef'];
  const outputTabs=[...document.querySelectorAll('[data-output-view]')];
  function selectOutput(view){
    outputTabs.forEach(tab=>{
      const selected=tab.dataset.outputView===view;
      tab.setAttribute('aria-selected',String(selected));
      tab.tabIndex=selected?0:-1;
      $(`output-${tab.dataset.outputView}`).hidden=!selected;
    });
  }
  outputTabs.forEach((tab,index)=>{
    tab.addEventListener('click',()=>selectOutput(tab.dataset.outputView));
    tab.addEventListener('keydown',event=>{
      if(!['ArrowLeft','ArrowRight','Home','End'].includes(event.key))return;
      event.preventDefault();
      const next=event.key==='Home'?0:event.key==='End'?outputTabs.length-1:
        (index+(event.key==='ArrowRight'?1:-1)+outputTabs.length)%outputTabs.length;
      outputTabs[next].focus();
      selectOutput(outputTabs[next].dataset.outputView);
    });
  });
  function svgNode(tag,attributes,text){
    const node=document.createElementNS('http://www.w3.org/2000/svg',tag);
    Object.entries(attributes).forEach(([key,value])=>node.setAttribute(key,value));
    if(text!==undefined)node.textContent=text;
    graph.append(node);
    return node;
  }
  function drawGraph(){
    graphFrame=undefined;
    graph.replaceChildren();
    $('graph-legend').replaceChildren();
    $('graph-empty').hidden=readings.length>0;
    if(!readings.length)return;
    let low=Math.min(...readings.map(r=>r.value));
    let high=Math.max(...readings.map(r=>r.value));
    if(low===high){const padding=Math.max(Math.abs(low)*0.05,1);low-=padding;high+=padding;}
    // Normalize first to keep finite numeric payloads from overflowing the range.
    const magnitude=Math.max(Math.abs(low),Math.abs(high),1);
    const {width,height}=graph.getBoundingClientRect();
    const plotWidth=width||440, plotHeight=height||240;
    graph.setAttribute('viewBox',`0 0 ${plotWidth} ${plotHeight}`);
    const right=plotWidth-12, bottom=plotHeight-16;
    const y=value=>bottom-(bottom-12)*((value/magnitude-low/magnitude)/(high/magnitude-low/magnitude));
    const x=index=>48+(right-48)*index/Math.max(readings.length-1,1);
    for(let i=0;i<3;i++){
      const value=low*(1-i/2)+high*(i/2);
      svgNode('path',{d:`M48 ${y(value)}H${right}`,class:'graph-grid'});
      svgNode('text',{x:42,y:y(value)+4,'text-anchor':'end',class:'graph-axis'},Number(value.toPrecision(4)).toString());
    }
    const keys=[...new Set(readings.map(r=>r.key))];
    keys.forEach((key,series)=>{
      const color=colors[series%colors.length];
      const points=readings.map((r,index)=>({...r,index})).filter(r=>r.key===key);
      svgNode('path',{d:points.map((r,i)=>`${i?'L':'M'}${x(r.index)} ${y(r.value)}`).join(' '),stroke:color,class:'graph-line','stroke-dasharray':points[0].kind==='pub'?'5 4':'none'});
      // Keep a single reading visible until there is a line to draw.
      if(points.length===1)points.forEach(r=>{
        const dot=svgNode('circle',{cx:x(r.index),cy:y(r.value),r:3,fill:color});
        const title=document.createElementNS('http://www.w3.org/2000/svg','title');
        title.textContent=`${key}: ${r.value}`;
        dot.append(title);
      });
      const label=document.createElement('span');
      label.textContent=key;
      label.style.setProperty('--series-color',color);
      $('graph-legend').append(label);
    });
  }
  new ResizeObserver(()=>{
    if(graphFrame===undefined)graphFrame=requestAnimationFrame(drawGraph);
  }).observe(graph);
  function recordReading(kind,message){
    if(!['pub','msg','latest'].includes(kind)||!message||typeof message!=='object')return;
    const payload=String(message.payload??'').trim();
    const value=payload==='hot'?1:payload==='cool'?0:payload===''?NaN:Number(payload);
    if(!Number.isFinite(value))return;
    const key=`${kind==='pub'?'Sent':'Received'} · ${message.subject}`;
    readings.push({key,kind,value});
    if(readings.length>120)readings.shift();
    if(graphFrame===undefined)graphFrame=requestAnimationFrame(drawGraph);
  }
  function clearOutput(){
    logEl.textContent='';
    readings.length=0;
    drawGraph();
    $('latest-value').textContent='Read a subject’s most recent cached value.';
  }

  function setStatus(text,kind='',detail=''){
    statusEl.textContent=text;
    statusEl.dataset.kind=kind;
    statusEl.title=detail||text;
    statusEl.setAttribute('aria-label',detail||text);
  }

  function appendLog(kind,value){
    recordReading(kind,value);
    const stamp=new Date().toLocaleTimeString();
    const body=typeof value==='string'?value:JSON.stringify(value);
    const marker=kind==='pub'?'->':kind==='msg'?'<-':kind==='latest'?'<=':'!!';
    const line=document.createElement('div');
    line.className=`demo-log-line is-${kind}`;
    line.textContent=`[${stamp}] ${marker} ${kind} ${body}`;
    logEl.appendChild(line);
    while(logEl.textContent.length>24000&&logEl.firstChild){
      logEl.removeChild(logEl.firstChild);
    }
    logEl.scrollTop=logEl.scrollHeight;
    requestAnimationFrame(()=>{logEl.scrollTop=logEl.scrollHeight});
  }

  function client(){
    if(typeof MonoblokHttpClient!=='function'){
      throw new Error('http-sse-client.js did not load');
    }
    return new MonoblokHttpClient();
  }

  function stopSub(updateStatus=true){
    cancelSubs.forEach(cancel=>cancel());
    cancelSubs=[];
    $('sse-sub-button').disabled=false;
    $('sse-stop-button').disabled=true;
    if(updateStatus)setStatus('Stopped');
  }

  async function publishPayload(payload){
    const subject=$('sse-pub-subject').value.trim();
    $('sse-pub-button').disabled=true;
    document.querySelectorAll('[data-demo-payload], [data-demo-example]').forEach((button)=>{button.disabled=true});
    try{
      appendLog('pub',{subject,payload});
      await client().publish(subject,payload);
      setStatus('Published','ok',`Published to ${subject}`);
    }catch(err){
      appendLog('error',err.message);
      setStatus('Error','bad',err.message);
    }finally{
      $('sse-pub-button').disabled=false;
      document.querySelectorAll('[data-demo-payload], [data-demo-example]').forEach((button)=>{button.disabled=false});
    }
  }

  async function readLatest(){
    const subject=$('sse-latest-subject').value.trim();
    $('sse-latest-button').disabled=true;
    try{
      const payload=await client().latest(subject);
      appendLog('latest',{subject,payload});
      $('latest-value').textContent=`${subject}\n${payload}`;
      setStatus('Latest','ok',`Read latest for ${subject}`);
    }catch(err){
      $('latest-value').textContent=err.message;
      appendLog('error',err.message);
      setStatus('Error','bad',err.message);
    }finally{
      $('sse-latest-button').disabled=false;
    }
  }

  const exampleTabs=[...document.querySelectorAll('[data-demo-example]')];
  function selectExample(key){
    if($('sse-pub-button').disabled)return;
    const example=patchbayExamples[key];
    stopSub(false);
    clearOutput();
    exampleTabs.forEach(tab=>{
      const selected=tab.dataset.demoExample===key;
      tab.setAttribute('aria-selected',String(selected));
      tab.tabIndex=selected?0:-1;
    });
    $('demo-example-panel').setAttribute('aria-labelledby',`demo-tab-${key}`);
    renderPatchbayCode($('demo-example-code'),example.code.replace('"sensors.*"','"demo.sensors.*"'));
    $('demo-example-title').textContent=example.title;
    $('demo-example-description').textContent=example.description;
    const subject='demo.sensors.temp';
    const suffix=key==='clean'?'stable':key==='smooth'?'smoothed':'alert';
    $('sse-pub-subject').value=subject;
    $('sse-subject').value=key==='alert'?`${subject}.alert, ${subject}.ok`:`${subject}.${suffix}`;
    $('sse-latest-subject').value=`${subject}.${suffix}`;
    $('demo-sub-help').textContent=key==='alert'
      ?'Subscribe to both subjects, then send 27 first and follow the sample readings in order.'
      :'Subscribe first, then send the sample readings in order.';
    const values=key==='alert'?['27',...example.input]:example.input;
    $('sse-payload').value=values[0];
    $('demo-samples').replaceChildren(...values.map(value=>{
      const button=document.createElement('button');
      button.type='button';
      button.className='demo-button secondary';
      button.dataset.demoPayload=value;
      button.textContent=value;
      return button;
    }));
    if(location.protocol==='file:')setStatus('HTTP required','bad','Open this page through the monoblok HTTP server to connect');
    else setStatus('Ready');
  }
  exampleTabs.forEach((tab,index)=>{
    tab.addEventListener('click',()=>selectExample(tab.dataset.demoExample));
    tab.addEventListener('keydown',event=>{
      if($('sse-pub-button').disabled)return;
      let next;
      if(event.key==='ArrowRight')next=(index+1)%exampleTabs.length;
      if(event.key==='ArrowLeft')next=(index+exampleTabs.length-1)%exampleTabs.length;
      if(event.key==='Home')next=0;
      if(event.key==='End')next=exampleTabs.length-1;
      if(next===undefined)return;
      event.preventDefault();
      exampleTabs[next].focus();
      selectExample(exampleTabs[next].dataset.demoExample);
    });
  });
  const requestedExample=new URLSearchParams(location.search).get('example');
  const isLive=Object.hasOwn(patchbayExamples,requestedExample)&&patchbayExamples[requestedExample].live!==false;
  selectExample(isLive?requestedExample:'clean');

  if(location.protocol==='file:'){
    $('demo-connection-note').hidden=false;
    setStatus('HTTP required','bad','Open this page through the monoblok HTTP server to connect');
  }else{
    setStatus('Ready');
  }

  $('sse-clear-log').addEventListener('click',()=>{
    clearOutput();
  });
  $('sse-stop-button').addEventListener('click',()=>stopSub());

  $('sse-sub-form').addEventListener('submit',(event)=>{
    event.preventDefault();
    stopSub(false);
    const subjects=$('sse-subject').value.split(',').map(value=>value.trim()).filter(Boolean);
    try{
      if(!subjects.length)throw new Error('Enter a subject to subscribe to');
      subjects.forEach(subject=>{
        cancelSubs.push(client().subscribe(subject,msg=>appendLog('msg',msg),{
          onError:err=>{
            stopSub(false);
            appendLog('error',err.message);
            setStatus('Error','bad',err.message);
          },
        }));
      });
      $('sse-sub-button').disabled=true;
      $('sse-stop-button').disabled=false;
      setStatus('Subscribed','ok',`Subscribed to ${subjects.join(', ')}`);
    }catch(err){
      stopSub(false);
      appendLog('error',err.message);
      setStatus('Error','bad',err.message);
    }
  });

  $('sse-pub-form').addEventListener('submit',(event)=>{
    event.preventDefault();
    publishPayload($('sse-payload').value);
  });
  $('sse-latest-form').addEventListener('submit',(event)=>{
    event.preventDefault();
    readLatest();
  });

  $('demo-samples').addEventListener('click',event=>{
    const button=event.target.closest('[data-demo-payload]');
    if(!button||button.disabled)return;
    const payload=button.dataset.demoPayload;
    $('sse-payload').value=payload;
    publishPayload(payload);
  });
})();
