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
body{margin:0 auto;max-width:560px;padding:16px;font-family:system-ui,-apple-system,sans-serif;background:#0f0f12;color:#ececf1}
header{display:flex;align-items:center;gap:10px;margin:4px 0 18px}
h1{font-size:20px;margin:0;flex:1}
#st{width:10px;height:10px;border-radius:50%;background:#777;flex:none}
#st.ok{background:#32d74b}
#st.err{background:#ff453a}
button{background:#26262c;border:1px solid #3a3a42;color:#ececf1;border-radius:10px;padding:9px 13px;font-size:14px;cursor:pointer}
button:active{background:#3a3a42}
.fix{background:#1a1a1f;border:1px solid #2a2a31;border-radius:14px;padding:14px 16px;margin-bottom:14px}
.fix h2{font-size:13px;font-weight:600;letter-spacing:.05em;text-transform:uppercase;color:#9a9aa3;margin:0 0 2px}
.cap{color:#8a8a93;font-size:13px;margin:0 0 8px}
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
<header><h1>Fari DMX</h1><span id="st"></span><button id="bo">Blackout</button><button id="ed">Modifica</button></header>
<div id="app"><p id="msg">Caricamento&hellip;</p></div>
<script>
"use strict";
const $=s=>document.querySelector(s);
const $$=s=>document.querySelectorAll(s);
const touched={},lastSent={},timers={};
let cfg=null,editing=false;
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
function chanRow(gid,ch,ci){
 return '<div class="row'+(ci===0?' master':'')+'"><span class="lbl"><span class="dot" style="background:'+ch.color+'"></span>'+esc(ch.label)+
  '</span><input type="range" min="0" max="255" value="0" data-g="'+gid+'" data-c="'+ci+'" style="accent-color:'+ch.color+
  '"><span class="val" id="c'+gid+'_'+ci+'">0</span></div>';
}
function viewHtml(){
 return cfg.groups.map(g=>{
  const fx=fixOf(g.id).map(f=>esc(f.name)).join(", ")||"nessun faro";
  return '<div class="fix"><h2>'+esc(g.name)+'</h2><p class="cap">'+fx+'</p>'+
   cfg.channels.map((c,ci)=>chanRow(g.id,c,ci)).join("")+'</div>';
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
 }
}
async function refresh(){
 const c=await(await fetch("/config")).json();
 const st=await(await fetch("/state")).json();
 cfg=c;
 $("#app").innerHTML=editing?editHtml():viewHtml();
 wire();applyState(st,false);
 $("#ed").textContent=editing?"Fine":"Modifica";
}
async function op(url){
 try{const r=await fetch(url);if(!r.ok)alert(await r.text());ok(true)}catch(e){ok(false)}
 try{await refresh()}catch(e){ok(false)}
}
async function init(){
 try{await refresh();ok(true)}
 catch(e){$("#app").innerHTML='<p id="msg">Connessione fallita, riprovo&hellip;</p>';ok(false);setTimeout(init,2000)}
}
$("#bo").addEventListener("click",async()=>{
 try{
  await fetch("/all?v=0");ok(true);
  for(const i of $$("input[data-c]")){i.value=0;upd(valId(i),0);touched[keyOf(i)]=Date.now()}
 }catch(e){ok(false)}
});
$("#ed").addEventListener("click",()=>{editing=!editing;refresh().catch(()=>ok(false))});
setInterval(async()=>{
 if(editing)return;
 try{const st=await(await fetch("/state")).json();ok(true);applyState(st,true)}catch(e){ok(false)}
},3000);
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
uint8_t dmxData[DMX_SLOTS + 1] = { 0 };            // buffer trasmesso, [0] = start code
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
  for (size_t g = 0; g <= MAX_GROUPS; g++)
    for (size_t c = 0; c < N_CHANNELS; c++) groupVal[g][c] = 0;
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
  // ogni faro prende, tale e quale, i canali del suo gruppo
  for (size_t f = 0; f < N_FIXTURES; f++) {
    uint8_t id = fixGroup[f];
    if (groupIndexById(id) < 0) continue;
    for (size_t c = 0; c < N_CHANNELS; c++) {
      uint16_t addr = FIXTURES[f].address + CHANNELS[c].offset;
      if (addr >= 1 && addr <= DMX_SLOTS) dmxData[addr] = groupVal[id][c];
    }
  }
}

void dmxSendFrame() {
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
  j += "}}";
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

  dmxBegin();
  wifiBegin();

  server.on("/", handleRoot);
  server.on("/set", handleSet);
  server.on("/all", handleAll);
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
