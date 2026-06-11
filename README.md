# Fari DMX

Controller DMX512 per ESP32 con pagina web di controllo. Il modello e' a due
livelli:

- un **faro** ha un nome, un indirizzo DMX e i canali del profilo (per questo
  faro: dimmer, R, G, B, bianco, ambra e gli effetti strobo/programmi — 9
  canali in tutto);
- un **gruppo** raccoglie piu' fari. Per ogni gruppo imposti i canali **una
  volta** e il valore va, tale e quale, a tutti i fari del gruppo. L'intensita'
  e' il canale dimmer del faro (CH1), quindi regola davvero la luminosita'.

I gruppi si **creano e si gestiscono dalla pagina web** (pulsante "Modifica"):
aggiungere, rinominare, eliminare gruppi e spostare i fari da un gruppo
all'altro. La configurazione (gruppi + assegnazione dei fari) viene salvata in
flash (NVS) e sopravvive allo spegnimento.

La pagina e' **responsive**: su telefono i gruppi sono in colonna; su tablet in
orizzontale si affiancano tutti su una riga e l'altezza dei fader **si adatta da
sola** per stare in una schermata senza scroll (anche con 5+ gruppi). In cima a
ogni gruppo c'e' una **palette di colori rapidi**, e in fondo i **pulsanti di
flash** e di **strobo** (uno per gruppo); in alto un **flash** e uno **strobo
globali** (tutti i gruppi) per i colpi di luce durante una serata. In alto c'e'
anche la barra delle **scene**: salvi interi look e li richiami con un tap, in
**dissolvenza** sul tempo che imposti, e li concateni in una **chase** (sequenza)
che scorre da sola a tempo (con tap-tempo).
Nessuna libreria esterna: basta il core ESP32 di Arduino.

## Collegamenti

Usa lo stadio d'uscita gia' presente sulla scheda (74AHCT125 alimentato a 5 V,
resistenze da 33 ohm in serie):

| ESP32  | Percorso                  | XLR        |
|--------|---------------------------|------------|
| GPIO18 | buffer 74AHCT125 + 33 ohm | 3 (Data+)  |
| GPIO23 | buffer 74AHCT125 + 33 ohm | 2 (Data-)  |
| GND    | diretto                   | 1 (GND)    |

GPIO23 emette la stessa TX della UART **invertita** (lo fa la GPIO matrix
dell'ESP32): le due uscite complementari 0-5 V emulano la coppia
differenziale RS-485.

**Limiti di questa uscita pseudo-differenziale:**

- cavi corti (indicativamente fino a ~10 m) e pochi fari;
- **niente** terminatore da 120 ohm a fine linea (caricherebbe troppo il buffer);
- il GND deve arrivare al pin 1 dell'XLR;
- solo trasmissione (niente RDM) e mai una console DMX collegata in parallelo.

Per un impianto serio basta aggiungere un MAX485 (~1 EUR): TX su `DI`,
`DE`+`RE` a 3V3, `A` al pin 3, `B` al pin 2 — il firmware resta identico.

## Caricamento

1. Arduino IDE 2 con il core ESP32 (gia' installato).
2. Apri `fari_dmx.ino`, scheda **ESP32 Dev Module**, seleziona la porta, carica.

## Configurazione (in testa allo sketch)

- `WIFI_SSID` / `WIFI_PASS`: rete di casa. Lasciando `WIFI_SSID` vuoto l'ESP32
  crea l'access point `FARI-DMX` (password `luciluci1`).
- `CHANNELS[]`: il **profilo del faro**, cioe' i canali che ogni faro ha, come
  offset dal suo indirizzo. Di default i 9 canali di questo faro: `{0,"Intensità",
  ...}` (dimmer), poi rosso, verde, blu, bianco, ambra, e infine strobo,
  programma e sfumatura. Per un faro diverso adatta questa tabella al suo
  manuale (numero, ordine e nome dei canali). Tutti i fari condividono il
  profilo. Il canale a offset 0 e' trattato come intensita' dal Blackout.
- `FIXTURES[]`: i **fari**. Ogni riga e' `{nome, indirizzo DMX}`; ogni faro
  occupa `N_CHANNELS` canali (qui 9, quindi indirizzi 1, 10, 19, 28, 37...).
  Metti sul display di ogni faro lo stesso indirizzo che scrivi qui **e
  selezionalo in modo 9 canali**.
- `FLASH[]`: i valori dei canali durante il **flash bianco** (stesso ordine di
  `CHANNELS[]`). Di default bianco pieno a piena intensita' e tutto il resto a
  zero; per un flash piu' caldo metti 255 anche su Ambra, ecc.
- `PALETTE[]`: i **colori rapidi** (i pallini in cima a ogni gruppo). Ogni voce
  e' `{pallino, {valori dei canali colore}}`. `COLOR_FIRST` e `COLOR_COUNT`
  dicono quali canali sono i colori (qui gli indici 1..5 = R, G, B, W, Ambra).
- `STROBE_CHANNEL` / `STROBE_VALUE`: il canale strobo del faro (indice in
  `CHANNELS[]`, qui 6) e la velocita' di lampeggio. `-1` per fari senza strobo.
- `GROUPS[]` e `FIXTURE_GROUP[]`: i gruppi del **primo avvio** e a quale gruppo
  va ogni faro. Dopo, gruppi e assegnazioni si gestiscono da web e vivono in
  flash; questi valori servono solo a "Ripristina default" (max 8 gruppi).
- `DMX_SWAP_POLARITY`: mettilo a `true` se i fari non rispondono — e' il
  classico caso di Data+ e Data- scambiati.
- `DMX_SLOTS`: slot trasmessi per frame (512 = massima compatibilita';
  abbassalo, es. 64, per un refresh piu' rapido).

## Uso

- **Access point**: collegati alla rete `FARI-DMX` e apri `http://192.168.4.1`
  (sui telefoni spesso la pagina si apre da sola come portale captive).
- **Rete di casa**: apri `http://fari.local` (iPhone/Mac) oppure l'IP stampato
  sul monitor seriale a 115200 baud.

La pagina mostra una scheda per gruppo: sotto il nome l'elenco dei fari del
gruppo, poi i fader di tutti i canali (intensita', i colori e gli effetti).
Spostando un canale lo cambi su **tutti i fari del gruppo** insieme. Il
pulsante **Blackout** azzera tutti i canali. Piu' telefoni/PC possono essere
collegati insieme: si sincronizzano da soli ogni 3 secondi.

Su tablet in orizzontale i gruppi si dispongono affiancati. Comodo da usare in
landscape durante una serata, con i flash a portata di pollice.

### Palette colori

In cima a ogni gruppo c'e' una fila di pallini colore: un tap imposta i canali
colore (R, G, B, W, Ambra) di tutti i fari del gruppo, **lasciando l'intensita'
com'e'**. Cambi atmosfera con un dito senza ritoccare la luminosita'. La lista
si modifica con `PALETTE[]` nello sketch.

### Scene

La barra in alto memorizza interi **look** (tutti i canali di tutti i gruppi):

- **+ Salva scena**: salva il look attuale con un nome (il nome si scrive in un
  campo dentro la pagina, niente finestrelle del browser; fino a 12 scene, in
  flash NVS, sopravvivono allo spegnimento);
- **tap su una scena**: la richiama in **dissolvenza** sul tempo impostato col
  cursore *Dissolvenza* (0 = a scatto). Il vecchio look sfuma sul nuovo;
- **&times;** su una scena: tocca due volte (il primo tocco chiede conferma
  diventando rosso) per eliminarla.

Le scene salvano i livelli, non i gruppi: se poi sposti i fari tra i gruppi, la
scena richiama comunque i valori sui gruppi con lo stesso nome interno.

### Chase (sequenza)

Sotto le scene costruisci una **sequenza**: con il menu *+ aggiungi* metti le
scene nell'ordine che vuoi (anche ripetute), e la **×** toglie un passo.

- **Avvia / Ferma chase**: quando e' attiva, la sequenza scorre da sola in loop,
  ogni scena in dissolvenza (usa il cursore *Dissolvenza*, limitato al tempo del
  passo);
- **Tap**: batti il tempo due o piu' volte e la velocita' (il *Tempo*) si
  adegua al tuo ritmo;
- la chase gira **nel firmware**, quindi continua anche se chiudi la pagina o
  spegni il tablet. La sequenza e' salvata in flash.

### Flash e strobo

Ogni gruppo ha in fondo due pulsanti, **Flash bianco** e **Strobo**, e in alto
c'e' un **Flash** globale (tutti i gruppi). Funzionano come i tasti "bump" delle
console: **tieni premuto** = effetto sui fari del gruppo, **rilascia** = torna
esattamente alla scena di prima (i fader non si toccano). Sono una
sovrascrittura momentanea dell'uscita, quindi non perdi i valori impostati.

- **Flash bianco**: tutti i LED a fondo (rosso+verde+blu+bianco a 255, massima
  luce). Cosa esce si decide con `FLASH[]`.
- **Strobo**: la stessa botta di luce ma lampeggiante, usando il canale strobo
  del faro (`STROBE_CHANNEL`/`STROBE_VALUE` nello sketch; metti
  `STROBE_CHANNEL = -1` per fari senza strobo, e il pulsante sparisce).
- In alto, **Flash** e **Strobo** globali fanno lo stesso ma su **tutti i
  gruppi insieme**.

### Gestire i gruppi e i fari

Il pulsante **Modifica** apre la modalita' gestione:

- rinomina un gruppo scrivendo nel suo campo nome;
- **+ Nuovo gruppo** ne crea uno nuovo (fino a 8);
- la **✕** rossa elimina un gruppo (tocca due volte per confermare; deve
  restarne almeno uno); i suoi fari passano automaticamente al primo gruppo;
- il menu a tendina accanto a ogni faro lo sposta in un altro gruppo (a fianco
  e' indicato il suo indirizzo DMX, es. `@5`);
- **Ripristina default** torna a gruppi e assegnazioni dello sketch.

Ogni modifica e' salvata subito in flash. Premi di nuovo **Fine** per tornare
ai controlli.

## Come funziona

- Frame DMX completo (break + start code + 512 slot) ogni 30 ms, ~33 Hz.
- A ogni frame, per ogni faro e ogni suo canale, il canale DMX
  `indirizzo faro + offset` riceve il valore del gruppo, tale e quale. La
  luminosita' la fa il faro col suo dimmer (CH1), non lo sketch.
- Il **flash** e' una sovrascrittura a livello di frame: mentre un gruppo
  lampeggia, ai suoi fari esce `FLASH[]` invece dei valori normali. Il pulsante
  rinfresca il flash ~ogni 250 ms mentre e' premuto e questo scade da solo dopo
  600 ms, quindi se il messaggio di rilascio si perde il bianco sparisce lo
  stesso (niente "flash incantato").
- Il richiamo di una **scena** interpola i livelli dal look attuale a quello
  salvato, frame per frame, sul tempo di dissolvenza. Mentre sfuma, la pagina
  aggiorna i fader piu' spesso (ogni ~200 ms) cosi' si vede il movimento.
- La **chase** avanza nel loop principale: ogni passo richiama la scena
  successiva della sequenza (con la sua dissolvenza). Tempo e dissolvenza
  arrivano dalla pagina; il tap-tempo e' calcolato dal browser sulla media
  delle ultime battute.
- Il break (108 us) e' generato abbassando temporaneamente la UART a
  83333 baud e scrivendo uno 0x00; i due bit di stop fanno da mark-after-break.
- La trasmissione avviene in interrupt (buffer TX da 1 KB), quindi il web
  server resta reattivo.
