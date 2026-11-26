# Ariston Termocamino ESP32

**Versione sperimentale 0.1.2-dev. Non installare definitivamente prima del collaudo.**

Centralina autonoma per ESP32-WROOM-32 (4 MB), sviluppata per una caldaia Ariston NET con Cube S e un termocamino. Nessun Home Assistant, Raspberry o broker MQTT necessario. Utilizza le API cloud non ufficiali già utilizzate da [ha-ariston-cube](https://github.com/paolofaz/ha-ariston-cube). Nessuna affiliazione con Ariston.

## Collegamento

Contatto pulito, normalmente aperto (NO), del relè della pompa: NO al GPIO27, COM a GND. L'ingresso usa INPUT_PULLUP a 3,3 V; non occorre una resistenza esterna per collegamenti brevi e in ambiente poco disturbato. **Mai collegare tensione esterna al GPIO, né il circuito della bobina o della pompa.** Verificare che il contatto sia realmente isolato e a potenziale libero e adatto a piccoli segnali; cavi lunghi o vicini alla rete elettrica possono richiedere un ingresso isolato. Alimentare l'ESP32 via USB 5 V.

| Evento stabile (3 s) | Stagione Inverno | Stagione Estate |
|---|---|---|
| Aperto → chiuso (pompa ON) | PlantMode=0 | Ignorato |
| Chiuso → aperto (pompa OFF) | PlantMode=1 | Ignorato |
| Accensione / riavvio / cambio stagione | Nessuna scrittura | Nessuna scrittura |
| Riconnessione Wi-Fi | Nessun recupero eventi persi | Nessuna scrittura |

La selezione stagione è un consenso all'automazione, non un comando diretto. La caldaia impostata manualmente su Spenta o altra modalità diversa da 0/1 non viene riattivata automaticamente. Non viene eseguito alcun polling cloud periodico. Le variazioni durante assenza alimentazione o rete possono essere perse intenzionalmente. Un nuovo evento invalida i precedenti non ancora inviati; un comando già trasmesso non è annullabile. In caso di esito incerto non si ripete la scrittura.

## Compilazione Mac

```bash
uv tool install --python 3.12 --with certifi==2026.11.20 --with cryptography==50.0.2 platformio==6.1.18
git clone --branch sviluppo https://github.com/paolofaz/ariston-termocamino-esp32.git
cd ariston-termocamino-esp32
pio run
pio run -t upload
pio device monitor
```

Se PlatformIO è già installato via uv, usare `uv tool install --force --python 3.12 --with certifi==2026.11.20 --with cryptography==50.0.2 platformio==6.1.18`.
Il bundle TLS viene generato da certifi in compilazione. Nessuna modalità TLS insecure. Necessaria sincronizzazione NTP per autenticare il certificato. Il firmware OTA è in `.pio/build/esp32dev/firmware.bin`.

### Installazione da binario già compilato

Scaricare lo ZIP `ariston-termocamino-esp32-dev` dall'ultima esecuzione riuscita di [GitHub Actions](https://github.com/paolofaz/ariston-termocamino-esp32/actions). Le build con packaging factory includono due file:

- `ariston-termocamino-factory.bin`: immagine completa per prima installazione USB su ESP32 classico 4 MB, indirizzo `0x0` (bootloader, partizioni, inizializzazione OTA e applicazione).
- `firmware.bin`: applicazione per aggiornamento OTA dalla pagina web; non usare il factory per OTA.

Con esptool 5: `esptool --chip esp32 --port /dev/cu.usbserial-XXXX write-flash 0x0 ariston-termocamino-factory.bin`. Sostituire la porta con quella effettiva. Per esptool 4 il comando è `esptool.py --chip esp32 --port /dev/cu.usbserial-XXXX write_flash 0x0 ariston-termocamino-factory.bin`. Non usare il factory per aggiornamenti ordinari: riscrive anche le aree di configurazione. Il file factory non è ancora collaudato su hardware reale.

## Prima configurazione e prova

1. Collegare ESP32 via USB e aprire il monitor seriale a 115200.
2. Al primo avvio, leggere nome e password casuale della rete Ariston-Setup sul monitor seriale (conservare la password per il recupero); connettersi direttamente a quella rete e aprire http://192.168.4.1.
3. Inserire Wi-Fi, credenziali Ariston NET, eventuale ID impianto (obbligatorio se più caldaie nell'account) password amministratore e stagione iniziale. Il firmware si riavvia senza inviare comandi.
4. Tornare alla rete di casa; aprire http://ariston-termocamino.local oppure http://IP (IP nel monitor seriale) e autenticarsi come admin con la password scelta.
5. In Inverno l’automazione è già operativa: non esiste una modalità simulazione né un consenso aggiuntivo. Il pulsante Verifica stato Ariston è disponibile in entrambe le stagioni (sola lettura).
6. Selezionare Inverno se non già scelto e attendere acquisizione iniziale. Con caldaia e impianto sorvegliati, aprire/chiudere un ponticello tra GPIO27 e GND per almeno 3 secondi: i comandi sono reali. Controllare l’app ufficiale e gli ultimi eventi. Verificare anche riavvio, cambio stagione e perdita Wi-Fi.

## Recupero e diagnostica

- In Inverno il pulsante **Riallinea caldaia** avvia una singola operazione esplicita verso la modalità richiesta dal contatto stabile. Rispetta il limite delle modalità 0/1, non scrive se la caldaia è già allineata e non riattiva una caldaia spenta. Non esiste un riallineamento automatico.
- Un evento in corso, perso o non confermato lascia un avviso persistente in NVS. Non vengono salvati comandi da eseguire al boot. Una successiva operazione riuscita o una lettura esplicita corrispondente al contatto in Inverno può cancellare l'avviso. L'ultima osservazione Ariston è mantenuta in RAM anche quando una chiamata fallisce; è storica, con data, e non prova lo stato attuale della caldaia. Dopo riavvio la lettura è sconosciuta fino alla prossima operazione.
- Dopo un esito POST incerto si tenta una lettura, se il servizio è raggiungibile e non in cooldown; mai una seconda scrittura. Un rifiuto 429 non viene ritentato. Un login rifiutato impone una pausa di 5 minuti in RAM; un token precedentemente ottenuto ma scaduto può essere rinnovato una volta. Nessun polling cloud periodico.
- Gli ultimi 32 eventi sono mantenuti in RAM con ora locale italiana NTP (oppure uptime finché l'orologio non è sincronizzato). La dashboard mostra heap libero, minimo e blocco maggiore, stack minimo residuo del worker e contatori dall'avvio. Non è previsto un servizio push.
- Se il Wi-Fi manca per 5 minuti, si avvia un access point WPA2 `Ariston-Recovery-xxxx`, con password casuale persistente mostrata nella dashboard autenticata. Conservare questa password. Collegarsi direttamente e aprire http://192.168.4.1; autenticarsi come admin e selezionare Modifica configurazione. L'AP si spegne quando il Wi-Fi di casa torna disponibile; non vengono recuperati gli eventi persi. Alla riconnessione si acquisisce una nuova baseline senza scrivere e si segnalano eventuali cambi non ancora stabilizzati.
- Dalla pagina Modifica configurazione si possono aggiornare Wi-Fi, account Ariston, ID impianto e password amministratore senza cancellare la stagione. Il salvataggio riavvia il dispositivo senza inviare comandi. Campi lasciati vuoti conservano i valori, salvo le opzioni esplicite per rete aperta e rimozione ID impianto.
- Per un reset completo della configurazione, tenere premuto BOOT per 10 secondi e poi rilasciarlo **a dispositivo già avviato** (non durante l'accensione), oppure inviare `RESET_CONFIG` via seriale. La configurazione viene cancellata, ma la password AP di recupero viene conservata: ricollegarsi ad `Ariston-Setup-xxxx` e ripetere la configurazione. In fase di avvio la pressione di BOOT può entrare nella modalità flash del chip. Le password non vengono pubblicate nel repository; sono salvate nella NVS locale non cifrata. La pagina locale usa autenticazione HTTP Digest, non HTTPS: usare soltanto rete privata fidata, non esporre porte Internet. Le credenziali di configurazione transitano nella rete AP protetta da password.

## OTA sperimentale

In stagione Estate, senza operazioni in corso, caricare il solo firmware.bin dalla pagina locale. I due slot OTA conservano la NVS durante aggiornamenti normali. L'immagine non è firmata: usare esclusivamente build fidate dello stesso progetto. Verificare OTA su banco prima dell'uso reale. In caso di problemi recuperare via USB.

## Limiti

API cloud non ufficiali, non garantite; nessun interblocco di sicurezza. Se caldaia e termocamino necessitano di esclusione reciproca per sicurezza, questa va realizzata localmente e indipendentemente da Internet e dal firmware, con un tecnico qualificato. Un POST è preceduto da lettura, verifica delle opzioni, controllo dell'evento, e seguito da readback cloud. Un esito cloud non prova lo stato fisico del bruciatore. Nessun retry automatico dopo una scrittura incerta. Cooldown HTTP 429 adattivo; Retry-After numerico supportato. Il firmware non è real-time: una richiesta web locale lenta può ritardare il campionamento del GPIO. L'OTA non ha rollback automatico né verifica crittografica della firma; recupero USB se necessario. Nessuna credenziale reale nei test.

## Sviluppo

Branch sviluppo, nessun rilascio stabile. Test della logica di contatto su host: `c++ -std=c++17 -Iinclude test/contact_test.cpp -o /tmp/contact-test && /tmp/contact-test`. Test del flusso di controllo reale con trasporto fittizio: `c++ -std=c++17 -Iinclude test/control_flow_test.cpp -o /tmp/control-flow-test && /tmp/control-flow-test`. Il workflow GitHub Actions esegue entrambi e compila il firmware. Le dipendenze del generatore del bundle TLS sono fissate alle versioni risolte dalla build del 25 novembre 2026; aggiornare deliberatamente le CA e le librerie nel tempo. Si mantiene il bundle Mozilla completo per non dipendere da una singola CA Ariston. La compatibilità API, TLS, RAM, OTA e hardware rimangono da verificare su ESP32 reale.
