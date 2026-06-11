/*
  fari_dmx — controller DMX512 via web per ESP32
  ==============================================

  Hardware (uscita pseudo-differenziale, 74AHCT125 alimentato a 5 V):

    GPIO18 --> buffer 74AHCT125 --> 33 ohm --> XLR pin 3 (Data+)
    GPIO23 --> buffer 74AHCT125 --> 33 ohm --> XLR pin 2 (Data-)
    GND ------------------------------------> XLR pin 1

  GPIO23 porta la stessa TX della UART1 invertita dalla GPIO matrix:
  le due uscite complementari 0-5 V emulano la coppia RS-485 del DMX.
  Vale per cavi corti (~10 m) e pochi fari, SENZA terminatore da 120 ohm.

  Modello:
    - un FARO ha un nome, un indirizzo DMX e i canali del profilo CHANNELS[].
      Questo faro e' a 9 canali: dimmer + R,G,B,W,Ambra + strobo/programmi.
      I canali del faro stanno a indirizzo + offset.
    - un GRUPPO raccoglie piu' fari. Per ogni gruppo si impostano UNA volta i
      canali (intensita', colori, effetti) e il valore va a tutti i fari del
      gruppo, tale e quale (l'intensita' e' il canale dimmer del faro, CH1).
    - dalla pagina web (pulsante "Modifica") si creano/rinominano/eliminano
      i gruppi e si spostano i fari da un gruppo all'altro. La struttura
      (gruppi + assegnazione fari) e' salvata in flash (NVS) e sopravvive
      allo spegnimento; CHANNELS[]/FIXTURES[]/GROUPS[]/FIXTURE_GROUP[] sono
      solo i default.

  Nessuna libreria esterna: basta il core ESP32 di Arduino.
    - modalita' access point -> http://192.168.4.1 (o portale captive)
    - modalita' WiFi di casa -> http://fari.local (o IP sul monitor seriale)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"

// ------------------------- configurazione WiFi -------------------------
// Lascia WIFI_SSID vuoto per creare un access point autonomo.
const char *WIFI_SSID = "";
const char *WIFI_PASS = "";
const char *AP_SSID = "FARI-DMX";
const char *AP_PASS = "luciluci1";  // minimo 8 caratteri
const char *MDNS_NAME = "fari";     // http://fari.local

// ------------------------- configurazione DMX --------------------------
const int DMX_TX_PIN = 18;             // TX vera     -> Data+ (XLR pin 3)
const int DMX_TX_INV_PIN = 23;         // TX invertita -> Data- (XLR pin 2)
const bool DMX_SWAP_POLARITY = false;  // true se i fari non rispondono (D+/D- scambiati)
const int DMX_SLOTS = 512;             // slot per frame (1-512); meno slot = refresh piu' rapido
const uint32_t DMX_FRAME_MS = 30;      // un frame ogni 30 ms = ~33 aggiornamenti/s

// -------------------- profilo del faro (i canali) ----------------------
// I canali di OGNI faro, come offset dal suo indirizzo DMX. Layout di questo
// faro (9 canali, vedi il suo manuale):
//   CH1 dimmer, CH2 rosso, CH3 verde, CH4 blu, CH5 bianco, CH6 ambra,
//   CH7 strobo, CH8 programmi, CH9 sfumatura. Il faro va messo in modo 9 canali.
// Il primo canale (offset 0) e' trattato come intensita' dal pulsante Blackout.
// La colonna colore e' solo il pallino dello slider (grigio per i non-colori).
struct ChannelDef {
  uint8_t offset;     // offset dall'indirizzo DMX del faro
  const char *label;  // nome del canale (niente virgolette)
  const char *color;  // colore dello slider (#rrggbb)
};

const ChannelDef CHANNELS[] = {
  { 0, "Intensità", "#c7c7cc" },  // CH1 - dimmer generale
  { 1, "Rosso", "#ff453a" },      // CH2
  { 2, "Verde", "#32d74b" },      // CH3
  { 3, "Blu", "#0a84ff" },        // CH4
  { 4, "Bianco", "#f0f0f0" },     // CH5
  { 5, "Ambra", "#ff9f0a" },      // CH6
  { 6, "Strobo", "#c7c7cc" },     // CH7 - flicker + velocita'
  { 7, "Programma", "#c7c7cc" },  // CH8 - sequenza colori + velocita'
  { 8, "Sfumatura", "#c7c7cc" },  // CH9 - cambio graduale + velocita'
};
const size_t N_CHANNELS = sizeof(CHANNELS) / sizeof(CHANNELS[0]);

// Valori dei canali durante un "flash bianco" (stesso ordine di CHANNELS[]):
// bianco pieno a piena intensita', tutto il resto a zero. Personalizzabile
// (es. metti 255 anche su Ambra per un flash piu' caldo).
const uint8_t FLASH[N_CHANNELS] = { 255, 0, 0, 0, 255, 0, 0, 0, 0 };
// Mentre il pulsante e' premuto, il tablet rinfresca il flash ~ogni 250 ms;
// al rilascio il bianco sparisce entro questo tempo anche se il messaggio di
// rilascio si perde (rete di sicurezza contro il "flash incantato"). Vale
// anche per lo strobo.
const uint32_t FLASH_HOLD_MS = 600;

// Strobo momentaneo: mentre il pulsante e' premuto esce un flash bianco che
// pulsa, ottenuto col canale strobo del faro. STROBE_CHANNEL e' l'indice in
// CHANNELS[] del canale strobo (qui 6 = CH7); -1 per disattivarlo (e nascondere
// il pulsante). STROBE_VALUE e' la velocita' di lampeggio (0..255).
const int STROBE_CHANNEL = 6;
const uint8_t STROBE_VALUE = 200;

// --------------------- palette di colori rapidi ------------------------
// I canali colore sono contigui in CHANNELS[]: qui gli indici 1..5
// (rosso, verde, blu, bianco, ambra). COLOR_FIRST = primo indice colore,
// COLOR_COUNT = quanti canali colore consecutivi.
const uint8_t COLOR_FIRST = 1;
const uint8_t COLOR_COUNT = 5;

// Ogni voce della palette da' i valori dei COLOR_COUNT canali colore (qui
// R,G,B,W,Ambra). Un tap imposta SOLO questi canali del gruppo, l'intensita'
// resta com'e'. "swatch" e' il pallino mostrato nella pagina. Modificabile.
struct PaletteDef {
  const char *swatch;
  uint8_t v[COLOR_COUNT];
};

const PaletteDef PALETTE[] = {
  { "#ff453a", { 255, 0, 0, 0, 0 } },     // rosso
  { "#ff9f0a", { 0, 0, 0, 0, 255 } },     // ambra (LED ambra)
  { "#ffd60a", { 255, 160, 0, 0, 0 } },   // giallo
  { "#32d74b", { 0, 255, 0, 0, 0 } },     // verde
  { "#64d2ff", { 0, 200, 255, 0, 0 } },   // ciano
  { "#0a84ff", { 0, 0, 255, 0, 0 } },     // blu
  { "#bf5af2", { 160, 0, 255, 0, 0 } },   // viola
  { "#ff375f", { 255, 0, 150, 0, 0 } },   // magenta
  { "#ffffff", { 0, 0, 0, 255, 0 } },     // bianco (LED bianco)
  { "#ffcc88", { 120, 30, 0, 180, 60 } }, // bianco caldo
};
const size_t N_PALETTE = sizeof(PALETTE) / sizeof(PALETTE[0]);

// ----------------------------- i fari ----------------------------------
// Nome + indirizzo DMX di partenza. Ogni faro occupa N_CHANNELS canali con il
// profilo CHANNELS qui sopra (qui 9 canali, quindi indirizzi 1,10,19,28,37).
// Metti sul display di ogni faro lo stesso indirizzo che scrivi qui, in modo 9ch.
struct FixtureDef {
  const char *name;
  uint16_t address;
};

const FixtureDef FIXTURES[] = {
  { "Faro 1", 1 },
  { "Faro 2", 10 },
  { "Faro 3", 19 },
  { "Faro 4", 28 },
  { "Faro 5", 37 },
};
const size_t N_FIXTURES = sizeof(FIXTURES) / sizeof(FIXTURES[0]);

// ------------------------- gruppi (default) ----------------------------
// Gruppi del primo avvio: dopo si gestiscono da web (massimo MAX_GROUPS).
const char *GROUPS[] = { "Gruppo 1", "Gruppo 2" };
const size_t N_GROUPS = sizeof(GROUPS) / sizeof(GROUPS[0]);
const size_t MAX_GROUPS = 8;
const size_t MAX_SCENES = 12;  // quante scene si possono memorizzare

// Gruppo di default di ogni faro (1..N_GROUPS), nello stesso ordine di FIXTURES[].
const uint8_t FIXTURE_GROUP[N_FIXTURES] = { 1, 1, 1, 2, 2 };

// --------------------------- pagina web --------------------------------
static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="it"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#0f0f12">
<title>Fari DMX</title>
<style>
:root{color-scheme:dark}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0 auto;max-width:1300px;padding:16px;font-family:system-ui,-apple-system,sans-serif;background:#0f0f12;color:#ececf1}
header{display:flex;align-items:center;flex-wrap:wrap;gap:10px;margin:4px 0 18px}
h1{font-size:20px;margin:0;flex:1}
#st{width:10px;height:10px;border-radius:50%;background:#777;flex:none}
#st.ok{background:#32d74b}
#st.err{background:#ff453a}
button{background:#26262c;border:1px solid #3a3a42;color:#ececf1;border-radius:10px;padding:9px 13px;font-size:14px;cursor:pointer}
button:active{background:#3a3a42}
.flash{background:#ececf1;color:#16161a;border-color:#ececf1;font-weight:500;touch-action:none}
.flash:active{background:#fff;border-color:#fff}
.brow{display:flex;gap:10px;margin-top:12px}
.gflash,.gstrobe{flex:1;padding:13px;border-radius:10px;font-size:15px;font-weight:500;cursor:pointer;touch-action:none;user-select:none;-webkit-user-select:none}
.gflash{background:#ececf1;color:#16161a;border:1px solid #ececf1}
.gflash:active{background:#fff}
.gstrobe{background:#26262c;color:#ececf1;border:1px solid #5a5a64}
.gstrobe:active{background:#3a3a42}
#scenes:not(:empty){margin:0 0 16px}
.scrow{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-bottom:10px}
.scn{display:inline-flex;border:1px solid #3a3a42;border-radius:10px;overflow:hidden}
.scnr{background:#26262c;border:0;color:#ececf1;font-size:14px;padding:9px 13px;cursor:pointer}
.scnr:active{background:#3a3a42}
.scnx{background:#26262c;border:0;border-left:1px solid #3a3a42;color:#9a9aa3;font-size:13px;padding:9px 10px;cursor:pointer}
.scnx:active{background:#4a2a2a;color:#ff453a}
.scadd{padding:9px 13px}
.scfade{display:flex;align-items:center;gap:10px;font-size:14px;color:#9a9aa3;max-width:380px}
.scfade input{flex:1}
#app.view{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:14px;align-items:start}
#app.view .fix{margin-bottom:0}
#app.edit{max-width:560px;margin:0 auto}
.fix{background:#1a1a1f;border:1px solid #2a2a31;border-radius:14px;padding:14px 16px;margin-bottom:14px}
.fix h2{font-size:13px;font-weight:600;letter-spacing:.05em;text-transform:uppercase;color:#9a9aa3;margin:0 0 2px}
.cap{color:#8a8a93;font-size:13px;margin:0 0 8px}
.pal{display:flex;flex-wrap:wrap;gap:8px;margin:0 0 12px}
.sw{width:30px;height:30px;min-width:30px;padding:0;border-radius:50%;border:1px solid rgba(255,255,255,.18);cursor:pointer}
.sw:active{transform:scale(.9)}
.row{display:grid;grid-template-columns:100px 1fr 40px;gap:12px;align-items:center;padding:6px 0}
.master{border-bottom:1px solid #2a2a31;padding-bottom:10px;margin-bottom:6px}
.lbl{display:flex;align-items:center;gap:8px;font-size:14px;overflow:hidden;white-space:nowrap}
.dot{width:12px;height:12px;border-radius:50%;flex:none}
input[type=range]{width:100%;height:32px;margin:0}
.val{text-align:right;font-variant-numeric:tabular-nums;font-size:14px;color:#9a9aa3}
select{width:100%;background:#26262c;border:1px solid #3a3a42;color:#ececf1;border-radius:8px;padding:8px;font-size:14px}
.ghead{display:flex;gap:8px;margin-bottom:8px}
.gname{flex:1;min-width:0;background:#26262c;border:1px solid #3a3a42;color:#ececf1;border-radius:8px;padding:9px 10px;font-size:14px}
.gdel{color:#ff453a;border-color:#4a2a2a}
.ebar{display:flex;gap:10px;margin-top:2px}
.hint{color:#9a9aa3;font-size:13px;margin:2px 0 4px}
#msg{color:#9a9aa3;font-size:14px}
</style></head><body>
<header><h1>Fari DMX</h1><span id="st"></span><button id="fl" class="flash">Flash</button><button id="bo">Blackout</button><button id="ed">Modifica</button></header>
<div id="scenes"></div>
<div id="app"><p id="msg">Caricamento&hellip;</p></div>
<script>
"use strict";
const $=s=>document.querySelector(s);
const $$=s=>document.querySelectorAll(s);
const touched={},lastSent={},timers={};
let cfg=null,editing=false,fadeMs=2000,polling=null;
function ok(b){$("#st").className=b?"ok":"err"}
function esc(t){return String(t).replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;").replace(/"/g,"&quot;")}
function upd(id,v){const e=document.getElementById(id);if(e)e.textContent=v}
function keyOf(i){return i.dataset.g+"_"+i.dataset.c}
function valId(i){return "c"+i.dataset.g+"_"+i.dataset.c}
function fixOf(gid){return cfg.fixtures.filter(f=>f.g===gid)}
async function send(i,v){
 try{await fetch("/set?g="+i.dataset.g+"&c="+i.dataset.c+"&v="+v);ok(true)}catch(e){ok(false)}
}
function queue(i,v,force){
 const k=keyOf(i),now=Date.now();
 clearTimeout(timers[k]);
 if(force||!lastSent[k]||now-lastSent[k]>40){lastSent[k]=now;send(i,v)}
 else timers[k]=setTimeout(()=>{lastSent[k]=Date.now();send(i,v)},45);
}
function bindBump(btn,base){
 let hb=null;
 const on=()=>{if(hb)return;const beat=()=>fetch(base+"&on=1").then(()=>ok(true)).catch(()=>ok(false));beat();hb=setInterval(beat,250)};
 const off=()=>{if(hb){clearInterval(hb);hb=null}fetch(base+"&on=0").catch(()=>{})};
 btn.addEventListener("pointerdown",e=>{e.preventDefault();try{btn.setPointerCapture(e.pointerId)}catch(_){}on()});
 btn.addEventListener("pointerup",off);
 btn.addEventListener("pointercancel",off);
 btn.addEventListener("lostpointercapture",off);
}
function renderScenes(){
 const el=$("#scenes");
 if(editing||!cfg){el.innerHTML="";return}
 const list=cfg.scenes||[];
 const chips=list.map(s=>'<span class="scn"><button class="scnr" data-recall="'+s.i+'">'+esc(s.name)+
  '</button><button class="scnx" data-scdel="'+s.i+'" aria-label="elimina">&#10005;</button></span>').join("");
 el.innerHTML='<div class="scrow">'+chips+'<button class="scadd" id="scadd">+ Salva scena</button></div>'+
  '<label class="scfade">Dissolvenza <span id="fadev">'+(fadeMs/1000).toFixed(1)+'</span> s'+
  '<input type="range" id="fade" min="0" max="10000" step="100" value="'+fadeMs+'"></label>';
 $("#scadd").addEventListener("click",saveScene);
 for(const b of el.querySelectorAll("[data-recall]"))b.addEventListener("click",()=>recallScene(+b.dataset.recall));
 for(const b of el.querySelectorAll("[data-scdel]"))b.addEventListener("click",()=>{if(confirm("Eliminare la scena?"))op("/scenedel?i="+b.dataset.scdel)});
 const f=$("#fade");f.addEventListener("input",()=>{fadeMs=+f.value;$("#fadev").textContent=(fadeMs/1000).toFixed(1)});
}
async function recallScene(i){
 try{await fetch("/recall?i="+i+"&ms="+fadeMs);ok(true);poll()}catch(e){ok(false)}
}
async function saveScene(){
 const name=prompt("Nome della scena:","Scena "+(((cfg.scenes||[]).length)+1));
 if(name===null)return;
 await op("/scenesave?name="+encodeURIComponent(name));
}
function palHtml(gid){
 if(!cfg.palette||!cfg.palette.length)return "";
 return '<div class="pal">'+cfg.palette.map((sw,pi)=>
  '<button class="sw" data-g="'+gid+'" data-p="'+pi+'" style="background:'+sw+'" aria-label="colore '+(pi+1)+'"></button>').join("")+'</div>';
}
async function applyPalette(g,p){
 try{await fetch("/palette?g="+g+"&p="+p);const st=await(await fetch("/state")).json();applyState(st,false);ok(true)}catch(e){ok(false)}
}
function chanRow(gid,ch,ci){
 return '<div class="row'+(ci===0?' master':'')+'"><span class="lbl"><span class="dot" style="background:'+ch.color+'"></span>'+esc(ch.label)+
  '</span><input type="range" min="0" max="255" value="0" data-g="'+gid+'" data-c="'+ci+'" style="accent-color:'+ch.color+
  '"><span class="val" id="c'+gid+'_'+ci+'">0</span></div>';
}
function viewHtml(){
 return cfg.groups.map(g=>{
  const fx=fixOf(g.id).map(f=>esc(f.name)).join(", ")||"nessun faro";
  return '<div class="fix"><h2>'+esc(g.name)+'</h2><p class="cap">'+fx+'</p>'+
   palHtml(g.id)+
   cfg.channels.map((c,ci)=>chanRow(g.id,c,ci)).join("")+
   '<div class="brow"><button class="gflash" data-flash="'+g.id+'">Flash bianco</button>'+
   (cfg.hasStrobe?'<button class="gstrobe" data-strobe="'+g.id+'">Strobo</button>':'')+'</div></div>';
 }).join("");
}
function optsHtml(cur){
 return cfg.groups.map(g=>'<option value="'+g.id+'"'+(g.id===cur?" selected":"")+'>'+esc(g.name)+'</option>').join("");
}
function editHtml(){
 return cfg.groups.map(g=>{
  const rows=fixOf(g.id).map(f=>
   '<div class="row"><span class="lbl">'+esc(f.name)+'</span><select data-fx="'+f.i+'">'+optsHtml(g.id)+
   '</select><span class="val">@'+f.addr+'</span></div>').join("");
  return '<div class="fix"><div class="ghead"><input class="gname" maxlength="24" data-g="'+g.id+'" value="'+esc(g.name)+'">'+
   (cfg.groups.length>1?'<button class="gdel" data-g="'+g.id+'">&#10005;</button>':'')+'</div>'+
   (rows||'<p class="hint">Nessun faro in questo gruppo</p>')+'</div>';
 }).join("")+
 '<div class="ebar"><button id="gadd">+ Nuovo gruppo</button><button id="grst">Ripristina default</button></div>';
}
function applyState(st,guard){
 if(editing)return;
 for(const i of $$("input[type=range]")){
  const k=keyOf(i);
  if(guard&&Date.now()-(touched[k]||0)<1500)continue;
  const a=st.v[i.dataset.g];
  const v=a?a[+i.dataset.c]:undefined;
  if(v===undefined)continue;
  if(+i.value!==v){i.value=v;upd(valId(i),v)}
 }
}
function wire(){
 if(editing){
  for(const i of $$(".gname"))
   i.addEventListener("change",()=>op("/gren?g="+i.dataset.g+"&name="+encodeURIComponent(i.value)));
  for(const b of $$(".gdel"))
   b.addEventListener("click",()=>{if(confirm("Eliminare il gruppo? I suoi fari passano al primo gruppo."))op("/gdel?g="+b.dataset.g)});
  for(const s of $$("select[data-fx]"))
   s.addEventListener("change",()=>op("/assign?fx="+s.dataset.fx+"&g="+s.value));
  const ga=$("#gadd");if(ga)ga.addEventListener("click",()=>op("/gadd?name="+encodeURIComponent("Gruppo "+(cfg.groups.length+1))));
  const gr=$("#grst");if(gr)gr.addEventListener("click",()=>{if(confirm("Tornare a gruppi e assegnazioni di default?"))op("/greset")});
 }else{
  for(const i of $$("input[type=range]")){
   i.addEventListener("input",()=>{touched[keyOf(i)]=Date.now();upd(valId(i),+i.value);queue(i,+i.value)});
   i.addEventListener("change",()=>{touched[keyOf(i)]=Date.now();queue(i,+i.value,true)});
  }
  for(const b of $$("[data-flash]"))bindBump(b,"/flash?g="+b.dataset.flash);
  for(const b of $$("[data-strobe]"))bindBump(b,"/strobe?g="+b.dataset.strobe);
  for(const b of $$(".sw"))b.addEventListener("click",()=>applyPalette(b.dataset.g,b.dataset.p));
 }
}
async function refresh(){
 const c=await(await fetch("/config")).json();
 const st=await(await fetch("/state")).json();
 cfg=c;
 const app=$("#app");
 app.className=editing?"edit":"view";
 app.innerHTML=editing?editHtml():viewHtml();
 renderScenes();
 wire();applyState(st,false);
 $("#ed").textContent=editing?"Fine":"Modifica";
}
async function op(url){
 try{const r=await fetch(url);if(!r.ok)alert(await r.text());ok(true)}catch(e){ok(false)}
 try{await refresh()}catch(e){ok(false)}
}
async function init(){
 try{await refresh();ok(true)}
 catch(e){const a=$("#app");a.className="";a.innerHTML='<p id="msg">Connessione fallita, riprovo&hellip;</p>';ok(false);setTimeout(init,2000)}
}
$("#bo").addEventListener("click",async()=>{
 try{
  await fetch("/all?v=0");ok(true);
  for(const i of $$("input[data-c]")){i.value=0;upd(valId(i),0);touched[keyOf(i)]=Date.now()}
 }catch(e){ok(false)}
});
$("#ed").addEventListener("click",()=>{editing=!editing;refresh().catch(()=>ok(false))});
bindBump($("#fl"),"/flash?g=0");
async function poll(){
 clearTimeout(polling);
 let next=3000;
 if(!editing){
  try{const st=await(await fetch("/state")).json();ok(true);applyState(st,true);if(st.fading)next=200}catch(e){ok(false)}
 }
 polling=setTimeout(poll,next);
}
poll();
init();
</script></body></html>)HTML";

// --------------------------- stato runtime ------------------------------
struct GroupDef {
  uint8_t id;
  String name;
};

GroupDef groups[MAX_GROUPS];
size_t nGroups = 0;
uint8_t fixGroup[N_FIXTURES];                      // id del gruppo di ogni faro
uint8_t groupVal[MAX_GROUPS + 1][N_CHANNELS];      // [id gruppo][canale] = 0..255
uint32_t flashUntil[MAX_GROUPS + 1] = { 0 };       // millis() fino a cui il gruppo lampeggia bianco
uint32_t strobeUntil[MAX_GROUPS + 1] = { 0 };      // millis() fino a cui il gruppo fa strobo
uint8_t dmxData[DMX_SLOTS + 1] = { 0 };            // buffer trasmesso, [0] = start code

// scene: ogni scena salva i valori di tutti i canali di tutti i gruppi
struct Scene {
  bool used;
  char name[24];
  uint8_t v[MAX_GROUPS + 1][N_CHANNELS];
};
Scene scenes[MAX_SCENES];                          // azzerato all'avvio (tutte non usate)

// motore di dissolvenza per il richiamo delle scene
bool fading = false;
uint32_t fadeStart = 0, fadeDur = 0;
uint8_t fadeFrom[MAX_GROUPS + 1][N_CHANNELS];
uint8_t fadeTo[MAX_GROUPS + 1][N_CHANNELS];
Preferences prefs;
WebServer server(80);
DNSServer dnsServer;
bool apMode = false;
uint32_t lastFrameAt = 0;
HardwareSerial &DMX = Serial1;

// --------------------- gruppi: helper e persistenza ---------------------
int groupIndexById(uint8_t id) {
  for (size_t i = 0; i < nGroups; i++)
    if (groups[i].id == id) return (int)i;
  return -1;
}

int fixtureIndexByAddress(uint16_t addr) {
  for (size_t i = 0; i < N_FIXTURES; i++)
    if (FIXTURES[i].address == addr) return (int)i;
  return -1;
}

// niente virgolette/caratteri di controllo nei nomi (finiscono in JSON e in NVS)
String cleanName(const String &raw) {
  String out;
  for (size_t i = 0; i < raw.length() && out.length() < 24; i++) {
    char c = raw[i];
    if (c == '"' || c == '\\' || (uint8_t)c < 0x20) continue;
    out += c;
  }
  out.trim();
  if (out.length() == 0) out = "Gruppo";
  return out;
}

void applyDefaultConfig() {
  nGroups = N_GROUPS < MAX_GROUPS ? N_GROUPS : MAX_GROUPS;
  for (size_t i = 0; i < nGroups; i++) {
    groups[i].id = (uint8_t)(i + 1);
    groups[i].name = GROUPS[i];
  }
  for (size_t i = 0; i < N_FIXTURES; i++) {
    uint8_t g = FIXTURE_GROUP[i];
    fixGroup[i] = (groupIndexById(g) >= 0) ? g : groups[0].id;
  }
}

void resetLevels() {
  for (size_t g = 0; g <= MAX_GROUPS; g++) {
    flashUntil[g] = 0;
    strobeUntil[g] = 0;
    for (size_t c = 0; c < N_CHANNELS; c++) groupVal[g][c] = 0;
  }
}

void saveConfig() {
  String g, a;
  for (size_t i = 0; i < nGroups; i++) {
    g += String(groups[i].id);
    g += '\t';
    g += groups[i].name;
    g += '\n';
  }
  for (size_t i = 0; i < N_FIXTURES; i++) {
    a += String(FIXTURES[i].address);  // chiave stabile: l'indirizzo del faro
    a += '\t';
    a += String(fixGroup[i]);
    a += '\n';
  }
  prefs.begin("faridmx", false);
  prefs.putString("groups", g);
  prefs.putString("assign", a);
  prefs.end();
}

void loadConfig() {
  applyDefaultConfig();
  prefs.begin("faridmx", true);
  String g = prefs.getString("groups", "");
  String a = prefs.getString("assign", "");
  prefs.end();

  if (g.length() > 0) {
    GroupDef tmp[MAX_GROUPS];
    size_t tn = 0;
    int pos = 0;
    while (tn < MAX_GROUPS) {
      int nl = g.indexOf('\n', pos);
      if (nl < 0) break;
      String line = g.substring(pos, nl);
      pos = nl + 1;
      int tab = line.indexOf('\t');
      if (tab < 0) continue;
      int id = line.substring(0, tab).toInt();
      if (id < 1 || id > (int)MAX_GROUPS) continue;
      bool dup = false;
      for (size_t i = 0; i < tn; i++)
        if (tmp[i].id == (uint8_t)id) dup = true;
      if (dup) continue;
      tmp[tn].id = (uint8_t)id;
      tmp[tn].name = cleanName(line.substring(tab + 1));
      tn++;
    }
    if (tn > 0) {
      nGroups = tn;
      for (size_t i = 0; i < tn; i++) groups[i] = tmp[i];
    }
  }

  if (a.length() > 0) {
    int pos = 0;
    while (true) {
      int nl = a.indexOf('\n', pos);
      if (nl < 0) break;
      String line = a.substring(pos, nl);
      pos = nl + 1;
      int tab = line.indexOf('\t');
      if (tab < 0) continue;
      int addr = line.substring(0, tab).toInt();
      int gid = line.substring(tab + 1).toInt();
      int fx = fixtureIndexByAddress((uint16_t)addr);
      if (fx >= 0 && groupIndexById((uint8_t)gid) >= 0) fixGroup[fx] = (uint8_t)gid;
    }
  }

  // rete di sicurezza: ogni faro deve puntare a un gruppo esistente
  for (size_t i = 0; i < N_FIXTURES; i++)
    if (groupIndexById(fixGroup[i]) < 0) fixGroup[i] = groups[0].id;
}

// ------------------------------- scene ----------------------------------
void saveScenesNVS() {
  prefs.begin("faridmx", false);
  prefs.putBytes("scenes", scenes, sizeof(scenes));
  prefs.end();
}

void loadScenesNVS() {
  prefs.begin("faridmx", true);
  size_t len = prefs.getBytesLength("scenes");
  if (len == sizeof(scenes)) prefs.getBytes("scenes", scenes, sizeof(scenes));  // solo se il formato combacia
  prefs.end();
}

// avvia la dissolvenza dal look attuale verso la scena idx, in ms millisecondi
void recallScene(int idx, uint32_t ms) {
  for (size_t i = 0; i < nGroups; i++) {
    uint8_t id = groups[i].id;
    for (size_t c = 0; c < N_CHANNELS; c++) {
      fadeFrom[id][c] = groupVal[id][c];
      fadeTo[id][c] = scenes[idx].v[id][c];
    }
  }
  if (ms == 0) {
    for (size_t i = 0; i < nGroups; i++) {
      uint8_t id = groups[i].id;
      for (size_t c = 0; c < N_CHANNELS; c++) groupVal[id][c] = fadeTo[id][c];
    }
    fading = false;
  } else {
    fadeStart = millis();
    fadeDur = ms;
    fading = true;
  }
}

// avanza la dissolvenza: scrive in groupVal i valori interpolati from->to
void fadeUpdate() {
  if (!fading) return;
  uint32_t el = millis() - fadeStart;
  bool done = el >= fadeDur;
  for (size_t i = 0; i < nGroups; i++) {
    uint8_t id = groups[i].id;
    for (size_t c = 0; c < N_CHANNELS; c++) {
      if (done) {
        groupVal[id][c] = fadeTo[id][c];
      } else {
        int from = fadeFrom[id][c], to = fadeTo[id][c];
        groupVal[id][c] = (uint8_t)(from + (int32_t)(to - from) * (int32_t)el / (int32_t)fadeDur);
      }
    }
  }
  if (done) fading = false;
}

// ------------------------------- DMX ------------------------------------
void dmxBegin() {
  pinMode(DMX_TX_PIN, OUTPUT);
  digitalWrite(DMX_TX_PIN, DMX_SWAP_POLARITY ? LOW : HIGH);
  pinMode(DMX_TX_INV_PIN, OUTPUT);
  digitalWrite(DMX_TX_INV_PIN, DMX_SWAP_POLARITY ? HIGH : LOW);

  DMX.setTxBufferSize(1024);  // scritture non bloccanti: il frame esce in interrupt
  DMX.begin(250000, SERIAL_8N2, -1, DMX_TX_PIN);

  // GPIO matrix: la stessa TX su due pin, uno dritto e uno invertito
  esp_rom_gpio_connect_out_signal(DMX_TX_PIN, U1TXD_OUT_IDX, DMX_SWAP_POLARITY, false);
  esp_rom_gpio_connect_out_signal(DMX_TX_INV_PIN, U1TXD_OUT_IDX, !DMX_SWAP_POLARITY, false);
}

void dmxRender() {
  // ogni faro prende i canali del suo gruppo; se il gruppo sta lampeggiando
  // (flash) esce il bianco pieno FLASH[]; con lo strobo esce lo stesso bianco
  // ma col canale strobo attivo. Il flash, se entrambi premuti, ha la priorita'.
  uint32_t now = millis();
  for (size_t f = 0; f < N_FIXTURES; f++) {
    uint8_t id = fixGroup[f];
    if (groupIndexById(id) < 0) continue;
    bool flashing = flashUntil[id] != 0 && (int32_t)(flashUntil[id] - now) > 0;
    bool strobing = strobeUntil[id] != 0 && (int32_t)(strobeUntil[id] - now) > 0;
    for (size_t c = 0; c < N_CHANNELS; c++) {
      uint16_t addr = FIXTURES[f].address + CHANNELS[c].offset;
      if (addr < 1 || addr > DMX_SLOTS) continue;
      uint8_t val = groupVal[id][c];
      if (strobing) {
        val = FLASH[c];
        if ((int)c == STROBE_CHANNEL) val = STROBE_VALUE;
      }
      if (flashing) val = FLASH[c];
      dmxData[addr] = val;
    }
  }
}

void dmxSendFrame() {
  fadeUpdate();  // se una scena sta sfumando, aggiorna i livelli prima di costruire il frame
  dmxRender();
  DMX.flush();                // attende che il frame precedente sia uscito tutto
  DMX.updateBaudRate(83333);  // BREAK: uno 0x00 a 83.3 kbaud = 108 us di linea bassa
  DMX.write((uint8_t)0);
  DMX.flush();                // i 2 bit di stop fanno da mark-after-break (24 us)
  DMX.updateBaudRate(250000);
  DMX.write(dmxData, DMX_SLOTS + 1);  // start code + slot
}

// ------------------------------ web -------------------------------------
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleSet() {
  int g = server.arg("g").toInt();
  int c = server.arg("c").toInt();
  int v = server.arg("v").toInt();
  if (g < 1 || g > (int)MAX_GROUPS || groupIndexById((uint8_t)g) < 0
      || c < 0 || c >= (int)N_CHANNELS || v < 0 || v > 255) {
    server.send(400, "text/plain", "parametri non validi");
    return;
  }
  groupVal[g][c] = (uint8_t)v;
  server.send(200, "text/plain", "ok");
}

void handleAll() {
  int v = server.arg("v").toInt();
  if (v < 0 || v > 255) {
    server.send(400, "text/plain", "parametri non validi");
    return;
  }
  for (size_t i = 0; i < nGroups; i++)
    for (size_t c = 0; c < N_CHANNELS; c++) groupVal[groups[i].id][c] = (uint8_t)v;
  server.send(200, "text/plain", "ok");
}

// flash bianco momentaneo: g=0 tutti i gruppi, on=1 premuto / on=0 rilasciato.
// Il pulsante rinfresca on=1 mentre e' premuto; il flash scade da solo dopo
// FLASH_HOLD_MS dall'ultimo messaggio, quindi non resta mai "incantato".
void handleFlash() {
  int g = server.arg("g").toInt();
  bool on = server.arg("on").toInt() != 0;
  uint32_t until = on ? millis() + FLASH_HOLD_MS : 0;
  if (g == 0) {
    for (size_t i = 0; i < nGroups; i++) flashUntil[groups[i].id] = until;
  } else if (g >= 1 && g <= (int)MAX_GROUPS && groupIndexById((uint8_t)g) >= 0) {
    flashUntil[g] = until;
  } else {
    server.send(400, "text/plain", "parametri non validi");
    return;
  }
  server.send(200, "text/plain", "ok");
}

// applica un colore della palette al gruppo: imposta solo i canali colore
void handlePalette() {
  int g = server.arg("g").toInt();
  int p = server.arg("p").toInt();
  if (g < 1 || g > (int)MAX_GROUPS || groupIndexById((uint8_t)g) < 0 || p < 0 || p >= (int)N_PALETTE) {
    server.send(400, "text/plain", "parametri non validi");
    return;
  }
  for (size_t k = 0; k < COLOR_COUNT; k++) groupVal[g][COLOR_FIRST + k] = PALETTE[p].v[k];
  server.send(200, "text/plain", "ok");
}

// richiama una scena con dissolvenza ms (0 = a scatto)
void handleRecall() {
  int i = server.arg("i").toInt();
  uint32_t ms = server.hasArg("ms") ? (uint32_t)server.arg("ms").toInt() : 2000;
  if (i < 0 || i >= (int)MAX_SCENES || !scenes[i].used) {
    server.send(400, "text/plain", "scena inesistente");
    return;
  }
  if (ms > 60000) ms = 60000;  // limite di sicurezza
  recallScene(i, ms);
  server.send(200, "text/plain", "ok");
}

// salva il look attuale in una scena (slot ?i= esplicito, o il primo libero)
void handleSceneSave() {
  int slot = server.hasArg("i") ? server.arg("i").toInt() : -1;
  if (slot < 0) {
    for (size_t k = 0; k < MAX_SCENES; k++)
      if (!scenes[k].used) { slot = (int)k; break; }
  }
  if (slot < 0 || slot >= (int)MAX_SCENES) {
    server.send(400, "text/plain", "memoria scene piena");
    return;
  }
  scenes[slot].used = true;
  cleanName(server.arg("name")).toCharArray(scenes[slot].name, sizeof(scenes[slot].name));
  memset(scenes[slot].v, 0, sizeof(scenes[slot].v));
  for (size_t i = 0; i < nGroups; i++) {
    uint8_t id = groups[i].id;
    for (size_t c = 0; c < N_CHANNELS; c++) scenes[slot].v[id][c] = groupVal[id][c];
  }
  saveScenesNVS();
  server.send(200, "text/plain", "ok");
}

void handleSceneDel() {
  int i = server.arg("i").toInt();
  if (i < 0 || i >= (int)MAX_SCENES || !scenes[i].used) {
    server.send(400, "text/plain", "scena inesistente");
    return;
  }
  scenes[i].used = false;
  scenes[i].name[0] = '\0';
  saveScenesNVS();
  server.send(200, "text/plain", "ok");
}

// strobo momentaneo, stessa meccanica del flash (g=0 = tutti i gruppi)
void handleStrobe() {
  int g = server.arg("g").toInt();
  bool on = server.arg("on").toInt() != 0;
  uint32_t until = on ? millis() + FLASH_HOLD_MS : 0;
  if (g == 0) {
    for (size_t i = 0; i < nGroups; i++) strobeUntil[groups[i].id] = until;
  } else if (g >= 1 && g <= (int)MAX_GROUPS && groupIndexById((uint8_t)g) >= 0) {
    strobeUntil[g] = until;
  } else {
    server.send(400, "text/plain", "parametri non validi");
    return;
  }
  server.send(200, "text/plain", "ok");
}

void handleGroupAdd() {
  if (nGroups >= MAX_GROUPS) {
    server.send(400, "text/plain", "troppi gruppi (max 8)");
    return;
  }
  uint8_t id = 0;
  for (uint8_t cand = 1; cand <= MAX_GROUPS; cand++)
    if (groupIndexById(cand) < 0) { id = cand; break; }
  groups[nGroups].id = id;
  groups[nGroups].name = cleanName(server.arg("name"));
  nGroups++;
  flashUntil[id] = 0;
  strobeUntil[id] = 0;
  for (size_t c = 0; c < N_CHANNELS; c++) groupVal[id][c] = 0;
  saveConfig();
  server.send(200, "text/plain", "ok");
}

void handleGroupRename() {
  int g = server.arg("g").toInt();
  int idx = (g >= 1 && g <= (int)MAX_GROUPS) ? groupIndexById((uint8_t)g) : -1;
  if (idx < 0) {
    server.send(400, "text/plain", "gruppo inesistente");
    return;
  }
  groups[idx].name = cleanName(server.arg("name"));
  saveConfig();
  server.send(200, "text/plain", "ok");
}

void handleGroupDelete() {
  int g = server.arg("g").toInt();
  int idx = (g >= 1 && g <= (int)MAX_GROUPS) ? groupIndexById((uint8_t)g) : -1;
  if (idx < 0) {
    server.send(400, "text/plain", "gruppo inesistente");
    return;
  }
  if (nGroups <= 1) {
    server.send(400, "text/plain", "serve almeno un gruppo");
    return;
  }
  for (size_t i = idx; i + 1 < nGroups; i++) groups[i] = groups[i + 1];
  nGroups--;
  flashUntil[g] = 0;
  strobeUntil[g] = 0;
  uint8_t fallback = groups[0].id;
  for (size_t i = 0; i < N_FIXTURES; i++)
    if (fixGroup[i] == (uint8_t)g) fixGroup[i] = fallback;
  saveConfig();
  server.send(200, "text/plain", "ok");
}

void handleAssign() {
  int fx = server.arg("fx").toInt();
  int g = server.arg("g").toInt();
  if (fx < 0 || fx >= (int)N_FIXTURES || g < 1 || g > (int)MAX_GROUPS || groupIndexById((uint8_t)g) < 0) {
    server.send(400, "text/plain", "parametri non validi");
    return;
  }
  fixGroup[fx] = (uint8_t)g;
  saveConfig();
  server.send(200, "text/plain", "ok");
}

void handleGroupReset() {
  applyDefaultConfig();
  resetLevels();
  saveConfig();
  server.send(200, "text/plain", "ok");
}

void handleState() {
  String j = "{\"v\":{";
  for (size_t i = 0; i < nGroups; i++) {
    if (i) j += ',';
    j += '"';
    j += String(groups[i].id);
    j += "\":[";
    for (size_t c = 0; c < N_CHANNELS; c++) {
      if (c) j += ',';
      j += String(groupVal[groups[i].id][c]);
    }
    j += ']';
  }
  j += "},\"fading\":";
  j += fading ? "true" : "false";
  j += "}";
  server.send(200, "application/json", j);
}

void handleConfig() {
  String j = "{\"channels\":[";
  for (size_t c = 0; c < N_CHANNELS; c++) {
    if (c) j += ',';
    j += "{\"label\":\"";
    j += CHANNELS[c].label;
    j += "\",\"color\":\"";
    j += CHANNELS[c].color;
    j += "\"}";
  }
  j += "],\"groups\":[";
  for (size_t i = 0; i < nGroups; i++) {
    if (i) j += ',';
    j += "{\"id\":";
    j += String(groups[i].id);
    j += ",\"name\":\"";
    j += groups[i].name;
    j += "\"}";
  }
  j += "],\"fixtures\":[";
  for (size_t i = 0; i < N_FIXTURES; i++) {
    if (i) j += ',';
    j += "{\"i\":";
    j += String(i);
    j += ",\"name\":\"";
    j += FIXTURES[i].name;
    j += "\",\"addr\":";
    j += String(FIXTURES[i].address);
    j += ",\"g\":";
    j += String(fixGroup[i]);
    j += "}";
  }
  j += "],\"hasStrobe\":";
  j += (STROBE_CHANNEL >= 0 && STROBE_CHANNEL < (int)N_CHANNELS) ? "true" : "false";
  j += ",\"palette\":[";
  for (size_t i = 0; i < N_PALETTE; i++) {
    if (i) j += ',';
    j += '"';
    j += PALETTE[i].swatch;
    j += '"';
  }
  j += "],\"scenes\":[";
  bool firstScene = true;
  for (size_t i = 0; i < MAX_SCENES; i++) {
    if (!scenes[i].used) continue;
    if (!firstScene) j += ',';
    firstScene = false;
    j += "{\"i\":";
    j += String(i);
    j += ",\"name\":\"";
    j += scenes[i].name;
    j += "\"}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleNotFound() {
  if (apMode) {  // portale captive: qualunque URL porta alla pagina
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "non trovato");
  }
}

// ------------------------------ WiFi ------------------------------------
void wifiBegin() {
  if (strlen(WIFI_SSID) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // meno latenza sugli slider
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("Connessione a \"%s\"", WIFI_SSID);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
      delay(250);
      Serial.print('.');
    }
    Serial.println();
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connesso, pagina su http://");
    Serial.println(WiFi.localIP());
  } else {
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    dnsServer.start(53, "*", WiFi.softAPIP());
    Serial.printf("Access point \"%s\" (password \"%s\"), pagina su http://%s\n",
                  AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
  }
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS attivo: http://%s.local\n", MDNS_NAME);
  }
}

// -------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nfari_dmx - avvio");

  resetLevels();
  loadConfig();
  loadScenesNVS();

  dmxBegin();
  wifiBegin();

  server.on("/", handleRoot);
  server.on("/set", handleSet);
  server.on("/all", handleAll);
  server.on("/flash", handleFlash);
  server.on("/strobe", handleStrobe);
  server.on("/palette", handlePalette);
  server.on("/recall", handleRecall);
  server.on("/scenesave", handleSceneSave);
  server.on("/scenedel", handleSceneDel);
  server.on("/gadd", handleGroupAdd);
  server.on("/gren", handleGroupRename);
  server.on("/gdel", handleGroupDelete);
  server.on("/assign", handleAssign);
  server.on("/greset", handleGroupReset);
  server.on("/state", handleState);
  server.on("/config", handleConfig);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.printf("DMX attivo: %d slot ogni %u ms su GPIO%d (D+) e GPIO%d (D-); %u fari da %u canali, %u gruppi\n",
                DMX_SLOTS, DMX_FRAME_MS, DMX_TX_PIN, DMX_TX_INV_PIN,
                (unsigned)N_FIXTURES, (unsigned)N_CHANNELS, (unsigned)nGroups);
}

void loop() {
  if (apMode) dnsServer.processNextRequest();
  server.handleClient();
  if (millis() - lastFrameAt >= DMX_FRAME_MS) {
    lastFrameAt = millis();
    dmxSendFrame();
  }
}
