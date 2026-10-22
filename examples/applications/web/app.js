/* SPDX-License-Identifier: MIT */
"use strict";
const $ = (id) => document.getElementById(id);
let kind = "media", state = {}, busy = false, frameURL = "", playbackRun = "", previous = null;
const rates = [];
async function api(path, data) {
  const response = await fetch(path, data === undefined ? {} : {method:"POST", headers:{"Content-Type":"application/json"}, body:JSON.stringify(data)});
  if (!response.ok) {
    let error = `Request failed (${response.status})`;
    try { error = (await response.json()).error || error; } catch (_) {}
    throw new Error(error);
  }
  return response.json();
}
function select(next) {
  if (busy) return;
  kind = next;
  document.querySelectorAll("nav button").forEach(b => {b.classList.toggle("selected", b.dataset.kind === kind); b.setAttribute("aria-pressed", String(b.dataset.kind === kind));});
  document.querySelectorAll("[data-section]").forEach(e => e.hidden = e.dataset.section !== kind);
  $("paths").hidden = $("path-hint").hidden = kind !== "media";
  $("viewer-title").textContent = kind === "media" ? "Receiver preview" : kind === "device" ? "Live · Sensor · Control" : "Recovery observations";
  $("placeholder-title").textContent = kind === "media" ? "Your received stream, live." : kind === "device" ? "Three delivery contracts. One device." : "Make loss visible.";
  $("placeholder-text").textContent = kind === "media" ? "Start a run to encode video, send it through SRT, and decode the received MPEG-TS." : kind === "device" ? "Video-sized frames and sensor samples flow to the operator. Commands return an acknowledgement after execution." : "Choose a recovery strategy and loss pattern. Inspect delivered data, retransmission and FEC counters below.";
  $("flow").textContent = kind === "media" ? "FFmpeg → UDP → SRT group → UDP → decoder" : "Sample sender → impairment relay → sample receiver";
  $("preview").hidden = $("playback").hidden = true; $("placeholder").hidden = false;
}
document.querySelectorAll("nav button").forEach(b => b.addEventListener("click", () => select(b.dataset.kind)));
$("controls").addEventListener("submit", async event => {
  event.preventDefault(); $("error").textContent = "";
  try {
    await api("/api/start", {kind, duration:Number($("duration").value), latency:Number($("latency").value),
      policy:$("policy").value, filter:$("filter").value, drop_every:Number($("drop-every").value), burst:Number($("burst").value), source:$("source").value});
    rates.length = 0; previous = null; playbackRun = ""; $("playback").pause(); $("playback").removeAttribute("src");
    $("playback").hidden = $("preview").hidden = true; $("placeholder").hidden = false;
    await poll();
  } catch(error) { $("error").textContent = error.message; }
});
$("stop").addEventListener("click", async () => {try {await api("/api/stop", {});} catch(e) {$("error").textContent=e.message;}});
document.querySelectorAll(".path-control").forEach(b => b.addEventListener("click", async () => {
  try {const index=Number(b.dataset.index); await api("/api/path", {index,paused:!state.paused?.[index]}); await poll();}
  catch(e) {$("error").textContent=e.message;}
}));
function metrics(events) {
  const stats=events.filter(e=>e.event==="stats"), received=stats.filter(e=>["receiver","operator"].includes(e.process));
  const latest=received.at(-1), tx=stats.filter(e=>["sender","device"].includes(e.process)).at(-1);
  const delivery=events.filter(e=>e.process==="receiver" && ["media_progress","media_summary"].includes(e.event)).at(-1);
  const receivedCount=kind==="media" && delivery ? delivery.datagrams : latest?.received;
  $("received-label").textContent=kind==="media" ? "Delivered datagrams" : "Received packets";
  $("received").textContent=receivedCount===undefined ? "—" : receivedCount.toLocaleString();
  const txPaths=stats.filter(e=>e.process==="sender" && e.channel==="path");
  $("retransmitted").textContent=txPaths.length ? txPaths.reduce((sum,e)=>sum+e.retransmitted,0).toLocaleString() : tx ? tx.retransmitted.toLocaleString() : "—";
  $("fec").textContent=latest ? latest.fec_supplied.toLocaleString() : "—";
  const pathRTT=stats.filter(e=>e.channel==="path").at(-1)||latest;
  $("rtt").textContent=pathRTT ? `${pathRTT.rtt_ms.toFixed(1)} ms` : "—";
  if (latest) {
    const now=performance.now(), count=receivedCount;
    if (previous && previous.socket===latest.socket && now>previous.time) rates.push(Math.max(0,(count-previous.count)*1000/(now-previous.time)));
    previous={count,time:now,socket:latest.socket};
    if(rates.length>100)rates.shift();
    const max=Math.max(1,...rates);
    $("trace").setAttribute("points", rates.map((v,i)=>`${i*800/Math.max(1,rates.length-1)},${100-v/max*90}`).join(" "));
  }
}
let polling=false;
async function poll() {
  if (polling) return;
  polling=true;
  try {
    state=await api("/api/state"); busy=["starting","running"].includes(state.state);
    $("connection").textContent="LOCAL · CONNECTED"; $("run-state").textContent=state.state.toUpperCase();
    $("start").disabled=busy || (kind==="media" && !state.capabilities.ffmpeg); $("stop").disabled=!busy;
    document.querySelectorAll("nav button, #controls input, #controls select").forEach(e=>e.disabled=busy);
    const fileOption=$("source").querySelector('[value="file"]'); if(fileOption)fileOption.disabled=!state.capabilities.file;
    if(state.error)$("error").textContent=state.error;
    $("decoded").textContent=`${state.decoded_frames} decoded preview frames`;
    $("media-status").textContent=state.state==="running" ? (kind==="media" ? "LIVE · SILENT PREVIEW" : "TRANSPORT ACTIVE") : state.state.toUpperCase();
    const events=state.events||[]; metrics(state.latest_events||events);
    $("events").textContent=events.length ? events.slice(-12).map(e=>JSON.stringify(e)).join("\n") : "Run a sample to see delivery, recovery, path and command events.";
    $("evidence-count").textContent=`${events.length} RECENT EVENTS`;
    $("output-path").textContent=state.directory ? `Local evidence: ${state.directory}` : "";
    for(let i=0;i<2;i++) {
      $( `path-${i}-state`).textContent=state.path_labels?.[i]||"Awaiting path snapshot";
      const button=document.querySelector(`.path-control[data-index="${i}"]`);
      button.disabled=state.state!=="running" || state.config?.kind!=="media";
      button.textContent=`${state.paused?.[i] ? "Resume" : "Pause"} relay ${i+1}`;
    }
    if(state.playback && kind==="media") {
      if(playbackRun!==state.directory) {$("playback").src=`/api/playback?run=${encodeURIComponent(state.directory)}`; playbackRun=state.directory;}
      $("playback").hidden=false; $("preview").hidden=$("placeholder").hidden=true; $("media-status").textContent="RECEIVED RECORDING · AUDIO AVAILABLE";
    } else if(state.decoded_frames && kind==="media" && state.config?.kind==="media") {
      const response=await fetch("/api/frame");
      if(response.status===200) {
        const next=URL.createObjectURL(await response.blob()); $("preview").src=next;
        if(frameURL)URL.revokeObjectURL(frameURL); frameURL=next;
        $("preview").hidden=false; $("placeholder").hidden=true;
      }
    }
  } catch(error) {$("connection").textContent="DISCONNECTED"; $("error").textContent=error.message;}
  finally {polling=false;}
}
async function initialize() {
  const token=location.hash.slice(1);
  if(token) {
    const response=await fetch("/api/session",{method:"POST",headers:{Authorization:`Bearer ${token}`},body:"{}"});
    if(response.ok)history.replaceState(null,"",location.pathname);
  }
  await poll(); setInterval(poll,500);
}
initialize();
