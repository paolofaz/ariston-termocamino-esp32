# Ariston Termocamino ESP32

[![Release](https://img.shields.io/github/v/release/paolofaz/ariston-termocamino-esp32)](https://github.com/paolofaz/ariston-termocamino-esp32/releases/latest)
[![Build ESP32](https://github.com/paolofaz/ariston-termocamino-esp32/actions/workflows/build.yml/badge.svg)](https://github.com/paolofaz/ariston-termocamino-esp32/actions/workflows/build.yml)

Centralina autonoma per gestire la modalità Estate/Inverno di una caldaia Ariston NET in base allo stato della pompa del termocamino. Progettata per ESP32-WROOM-32 con flash da 4 MB e collaudata per la lettura e il cambio modalità su una caldaia Ariston con Cube S.

Non richiede Home Assistant, Raspberry Pi, MQTT o altri dispositivi intermedi. Utilizza le API cloud non ufficiali Ariston NET, come il progetto [ha-ariston-cube](https://github.com/paolofaz/ha-ariston-cube). Nessuna affiliazione con Ariston.

## Funzionalità

- Cambio automatico Estate/Inverno alla variazione stabile del contatto della pompa.
- Lettura dello stato Ariston all'avvio e ogni ora, senza modificare la modalità della caldaia.
- Dashboard web locale senza password, con stato, diagnostica e ultimi eventi.
- Configurazione Wi-Fi e Ariston direttamente da browser.
- Verifica dello stato e riallineamento manuale.
- Aggiornamenti firmware OTA senza perdita delle impostazioni.
- Access point Wi-Fi di recupero in caso di perdita della rete domestica.

## Come funziona

Sono disponibili due varianti dello stesso firmware: **Normale** e **Invertita**. Scegliere in base allo stato effettivo del contatto quando la pompa è in funzione.

| Evento | Normale | Invertita |
|---|---|---|
| Contatto aperto → chiuso | Estate (0), pompa ON | Inverno (1), pompa OFF |
| Contatto chiuso → aperto | Inverno (1), pompa OFF | Estate (0), pompa ON |
| Accensione o riavvio | Sola lettura | Sola lettura |
| Riconnessione Wi-Fi | Nuova acquisizione, nessun recupero comandi | Nuova acquisizione, nessun recupero comandi |
| Aggiornamento automatico | Sola lettura ogni ora | Sola lettura ogni ora |

La dashboard indica la variante installata; lo stato fisico Aperto/Chiuso resta invariato, mentre lo stato dedotto della pompa e la modalità richiesta seguono la variante. Anche il riallineamento manuale utilizza la stessa logica. Per passare da una variante all'altra basta un aggiornamento OTA: non viene inviato un comando al riavvio.

Le variazioni del contatto devono rimanere stabili per almeno 3 secondi. Prima di ogni cambio modalità vengono letti lo stato e le opzioni disponibili; dopo il comando viene verificato il risultato sul cloud. Se la caldaia è già nella modalità richiesta, non viene inviata alcuna scrittura. La caldaia impostata manualmente su Spenta o su una modalità diversa da Estate/Inverno non viene riattivata automaticamente.

L'automazione è attiva quando il dispositivo è alimentato, configurato e operativo. Durante l'estate è sufficiente scollegare l'alimentazione USB della centralina: questo non modifica la modalità della caldaia.

### Aggiornamento automatico dello stato

La prima lettura viene eseguita dopo la connessione Wi-Fi, la sincronizzazione NTP e l'acquisizione del contatto. Ogni lettura riuscita, anche manuale o conseguente a un comando, fa ripartire l'intervallo di un'ora. In caso di errore viene programmata una nuova lettura dopo 5 minuti, nel rispetto dei limiti delle API.

Il polling è esclusivamente in lettura: non riallinea automaticamente la caldaia. Viene eseguito quando il contatto è stabile e il worker è libero. Una variazione del contatto invalida le letture automatiche superate; una richiesta HTTP già in corso può ritardare il comando successivo.

## Hardware e collegamento

- ESP32 classico con modulo ESP32-WROOM-32 e flash da 4 MB.
- Alimentazione USB 5 V.
- Contatto pulito del relè della pompa, con logica normale o invertita.

| Contatto relè | ESP32 |
|---|---|
| NO oppure NC (secondo il relè e la variante scelta) | GPIO27 |
| COM | GND |

L'ingresso GPIO27 utilizza il pull-up interno a 3,3 V. Per collegamenti brevi e poco disturbati non è necessaria una resistenza esterna.

**Non applicare tensione esterna al GPIO.** Il contatto deve essere realmente isolato, a potenziale libero e adatto a piccoli segnali. Non collegare direttamente il circuito della bobina o della pompa. Cavi lunghi o vicini alla rete elettrica possono richiedere un ingresso isolato.

## Installazione

Scaricare i file dalla [release più recente](https://github.com/paolofaz/ariston-termocamino-esp32/releases/latest).

| Utilizzo | Normale | Invertita |
|---|---|---|
| Prima installazione USB, 0x0 | `ariston-termocamino-factory.bin` | `ariston-termocamino-factory-inverted.bin` |
| Aggiornamento OTA | `firmware.bin` | `firmware-inverted.bin` |

`SHA256SUMS.txt` contiene i checksum di tutti i binari.

### Prima installazione tramite USB

Collegare l'ESP32 al computer. Su macOS individuare la porta seriale con:

```bash
ls /dev/cu.*
```

Con esptool 5, dalla cartella contenente il file BIN:

```bash
esptool --chip esp32 --port /dev/cu.usbserial-XXXX write-flash 0x0 ariston-termocamino-factory.bin
```

Per la variante invertita sostituire il nome del BIN con `ariston-termocamino-factory-inverted.bin`. Sostituire la porta con quella effettiva. Con esptool 4 utilizzare `esptool.py` e `write_flash`.

Il file factory include bootloader, tabella delle partizioni, inizializzazione OTA e applicazione. **Non utilizzarlo per aggiornamenti ordinari:** può sovrascrivere le aree di configurazione. Il file factory resta da verificare su hardware reale.

### Prima configurazione

1. Aprire il monitor seriale a 115200 baud.
2. Leggere il nome e la password dell'access point `Ariston-Setup-xxxx`. Conservare la password per il recupero.
3. Collegarsi a questa rete Wi-Fi e aprire http://192.168.4.1.
4. Inserire SSID e password Wi-Fi, email e password Ariston NET, ed eventualmente l'ID impianto (necessario se l'account contiene più caldaie).
5. Salvare e attendere il riavvio.
6. Ricollegarsi alla rete domestica e aprire http://ariston-termocamino.local oppure l'indirizzo IP mostrato sul monitor seriale.

Non è richiesta una password di accesso alla dashboard. L'automazione si attiva automaticamente e il primo aggiornamento dello stato non invia comandi di cambio modalità.

### Aggiornamento OTA

Scaricare dalla sezione Releases il file OTA della variante desiderata: `firmware.bin` (normale) o `firmware-inverted.bin` (invertita). Aprire **Aggiornamento firmware** nella dashboard, attendere che non ci siano operazioni in corso e caricare il file, preferibilmente a pompa ferma. Le credenziali Wi-Fi e Ariston vengono conservate.

Durante il caricamento il campionamento del contatto è sospeso; al termine viene acquisito nuovamente senza recuperare gli eventi persi. Non utilizzare il file factory via OTA. In caso di problemi, con la tabella delle partizioni di questo progetto è possibile ripristinare il solo firmware via USB, senza cancellare la flash, scrivendo entrambi gli slot:

```bash
esptool --chip esp32 --port /dev/cu.usbserial-XXXX write-flash 0x10000 firmware.bin 0x1f0000 firmware.bin
```

Per il recupero USB della variante invertita sostituire entrambi i riferimenti a `firmware.bin` nel comando precedente con `firmware-inverted.bin`.

## Dashboard web

La dashboard identifica la variante Normale o Invertita e mostra lo stato della pompa e del contatto, l'ultima modalità osservata sul cloud Ariston con data e ora, la modalità richiesta dal contatto, la connessione Wi-Fi e lo stato delle comunicazioni.

- **Verifica stato Ariston:** lettura manuale, senza cambio modalità.
- **Riallinea caldaia:** singolo comando esplicito, solo se necessario e consentito dalla modalità corrente.
- **Aggiornamento automatico:** stato del polling e tempo indicativo alla prossima lettura.
- **Diagnostica:** fase API, codice HTTP, codice TLS, heap libero e minimo, stack residuo del worker e contatori.
- **Ultimi eventi:** 32 eventi conservati in RAM con orario italiano o uptime quando l'ora non è sincronizzata.
- **Modifica configurazione:** aggiornamento di Wi-Fi, credenziali Ariston e ID impianto. I campi vuoti conservano i valori, salvo le opzioni esplicite per rete aperta e rimozione dell'ID.

La lettura Ariston è un'osservazione storica, non una conferma in tempo reale. Eventuali disallineamenti o comandi non confermati vengono segnalati; l'avviso viene conservato in memoria anche dopo un riavvio. Non vengono memorizzati comandi da ripetere automaticamente.

## Recupero e risoluzione dei problemi

### Wi-Fi non disponibile

Dopo 5 minuti senza Wi-Fi si attiva l'access point WPA2 `Ariston-Recovery-xxxx`. Per recuperare la password, utilizzare il monitor seriale a 115200 baud e inviare:

```text
SHOW_RECOVERY
```

Collegarsi alla rete di recupero, aprire http://192.168.4.1 e modificare la configurazione. L'access point si spegne quando la rete domestica torna disponibile. In modalità AP+STA, scansioni e cambi di canale possono interrompere temporaneamente la connessione di recupero.

### Errore Ariston

Un HTTP 404 durante il login può indicare credenziali errate. Verificare email e password dalla pagina di configurazione. I login rifiutati comportano una pausa di 5 minuti. Le risposte HTTP 429 attivano un cooldown adattivo che rispetta anche il valore numerico di Retry-After.

Il monitor seriale registra le fasi delle richieste senza mostrare credenziali, token, URL completi o corpi delle risposte.

### Ripristino della configurazione

A dispositivo già avviato, tenere premuto BOOT per 10 secondi e rilasciarlo, oppure inviare `RESET_CONFIG` via seriale. La configurazione viene cancellata, ma la password dell'access point di recupero rimane disponibile. Ricollegarsi alla rete `Ariston-Setup-xxxx` e ripetere la configurazione.

## Compilazione da sorgente (macOS)

```bash
uv tool install --python 3.12 --with certifi==2026.11.20 --with cryptography==50.0.2 platformio==6.1.18
git clone https://github.com/paolofaz/ariston-termocamino-esp32.git
cd ariston-termocamino-esp32
pio run -e esp32dev
pio run -e esp32dev -t upload
pio device monitor
```

Se PlatformIO è già installato tramite uv, utilizzare `uv tool install --force` con gli stessi argomenti. Il bundle delle CA TLS viene generato durante la compilazione. Per compilare o caricare la variante invertita usare `-e esp32dev-inverted` al posto di `-e esp32dev`. Il firmware OTA è in `.pio/build/esp32dev/firmware.bin` oppure `.pio/build/esp32dev-inverted/firmware.bin` (rinominato nella release).

GitHub Actions esegue i test della logica di contatto, del flusso di controllo, del parser PlantMode e della temporizzazione del polling, quindi compila e prepara i binari di entrambe le varianti. La mappatura del contatto è verificata dai test in entrambe le polarità. Per pubblicare una nuova versione, aggiornare `VERSION` e avviare Build ESP32 con l'opzione `publish_release`, oppure includere `[release]` nel messaggio del commit su main. Le release esistenti non vengono sovrascritte.

## Sicurezza e limiti

- Utilizzare soltanto una rete domestica fidata: dashboard, configurazione e OTA sono accessibili senza autenticazione dalla sottorete locale. Non esporre il dispositivo a Internet.
- Le credenziali sono conservate nella NVS non cifrata; l'access point è protetto con WPA2. Le API Ariston utilizzano HTTPS con verifica dei certificati; la dashboard locale usa HTTP.
- Le API cloud sono non ufficiali e possono cambiare. Il tempo di esecuzione dipende dalla rete e da Ariston; il cambio modalità comprende lettura preliminare, invio e verifica.
- Una scrittura incerta non viene ripetuta automaticamente. Gli eventi durante assenza di alimentazione o rete possono essere persi. Una richiesta già trasmessa non è annullabile.
- Il firmware non è real-time e non costituisce un interblocco di sicurezza. Eventuali esclusioni necessarie tra caldaia e termocamino devono essere realizzate localmente da un tecnico qualificato, indipendentemente da Internet.
- L'OTA non prevede verifica crittografica della firma o rollback automatico. Utilizzare esclusivamente binari fidati del progetto.

Lettura e transizioni Estate/Inverno sono state verificate su hardware reale. La variante invertita, il polling automatico, il recupero Wi-Fi e il funzionamento prolungato richiedono ulteriore verifica sul dispositivo.
