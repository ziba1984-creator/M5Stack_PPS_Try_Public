#pragma once
#include <pgmspace.h>

static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PPS Control</title>
<style>
body{font-family:sans-serif;max-width:480px;margin:1em auto;padding:0 1em}
table{width:100%;border-collapse:collapse}td{padding:.3em;border-bottom:1px solid #ddd}
td.v{text-align:right;font-weight:bold}
input{width:6em;font-size:1.1em}button{font-size:1.1em;padding:.4em .8em;margin:.2em}
#err{color:#c00}
</style></head><body>
<h2>PPS Control</h2>
<div id="err"></div>
<table>
<tr><td>Output</td><td class="v" id="en">-</td></tr>
<tr><td>Mode</td><td class="v" id="mode">-</td></tr>
<tr><td>Vout</td><td class="v" id="vout">-</td></tr>
<tr><td>Iout</td><td class="v" id="iout">-</td></tr>
<tr><td>Vin</td><td class="v" id="vin">-</td></tr>
<tr><td>Temp</td><td class="v" id="temp">-</td></tr>
<tr><td>Set V / I</td><td class="v" id="set">-</td></tr>
<tr><td>Fan</td><td class="v" id="fan">-</td></tr>
<tr><td>Battery</td><td class="v" id="batt">-</td></tr>
</table>
<p>Voltage [V] <input id="v" type="number" step="0.1" min="0.5" max="30">
<button onclick="send('v',v.value)">Set</button></p>
<p>Current [A] <input id="i" type="number" step="0.1" min="0" max="5">
<button onclick="send('i',i.value)">Set</button></p>
<p><button onclick="send('en',1)">Output ON</button>
<button onclick="send('en',0)">Output OFF</button></p>
<script>
const $=id=>document.getElementById(id);
async function send(k,val){
  try{
    const r=await fetch('/api/set',{method:'POST',body:new URLSearchParams({[k]:val})});
    $('err').textContent=r.ok?'':'Error '+r.status+': '+await r.text();
  }catch(e){$('err').textContent='Error: request failed (result unknown)';}
}
async function poll(){
  try{
    const s=await (await fetch('/api/status')).json();
    $('en').textContent=s.halted?(s.output_unknown?'HALTED (OFF NOT confirmed)':'HALTED (OFF)'):
      !s.connected?(s.output_unknown?'PPS lost (output UNKNOWN)':'PPS not found'):
      (!s.valid?(s.verifying?'CHECKING...':(s.output_unknown?'UNKNOWN (output may be ON)':'UNKNOWN (stale)')):
      (s.enabled?'ON':'OFF'));
    $('mode').textContent=s.mode<=1?'CV':'CC';
    $('vout').textContent=s.vout.toFixed(2)+' V';
    $('iout').textContent=s.iout.toFixed(3)+' A';
    $('vin').textContent=s.vin.toFixed(2)+' V';
    $('temp').textContent=s.temp.toFixed(1)+' C';
    $('set').textContent=s.vset.toFixed(2)+' V / '+s.iset.toFixed(2)+' A';
    $('fan').textContent=s.fan<0?'not found':s.fan+' %';
    $('batt').textContent=(s.batt_level<0?'--':s.batt_level+'%')+' '+s.batt_mv+'mV'+(s.charging?' (charging)':'');
  }catch(e){$('err').textContent='Connection lost';}
  setTimeout(poll,500);
}
poll();
</script></body></html>
)HTML";
