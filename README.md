# Ariston Termocamino ESP32

**Versione sperimentale 0.1.1-dev. Non installare definitivamente prima del collaudo.**

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
uv tool install --python 3.12 --with certifi --with cryptography platformio
git clone --branch sviluppo https://github.com/paolofaz/ariston-termocamino-esp32.git
cd ariston-termocamino-esp32
pio run
pio run -t upload
pio device monitor
```

Se PlatformIO è già installato via uv, usare `uv tool install --force --python 3.12 --with certifi --with cryptography platformio`.
Il bundle TLS viene generato da certifi in compilazione. Nessuna modalità TLS insecure. Necessaria sincronizzazione NTP per autenticare il certificato. Il firmware OTA è in `.pio/build/esp32dev/firmware.bin`.

### Installazione da binario già compilato

Scaricare lo ZIP `ariston-termocamino-esp32-dev` dall'ultima esecuzione riuscita di [GitHub Actions](https://github.com/paolofaz/ariston-termocamino-esp32/actions). Le build con packaging factory includono due file:

- `ariston-termocamino-factory.bin`: immagine completa per prima installazione USB su ESP32 classico 4 MB, indirizzo `0x0` (bootloader, partizioni, inizializzazione OTA e applicazione).
- `firmware.bin`: applicazione per aggiornamento OTA dalla pagina web; non usare il factory per OTA.

Con esptool 5: `esptool --chip esp32 --port /dev/cu.usbserial-XXXX write-flash 0x0 ariston-termocamino-factory.bin`. Sostituire la porta con quella effettiva. Per esptool 4 il comando è `esptool.py --chip esp32 --port /dev/cu.usbserial-XXXX write_flash 0x0 ariston-termocamino-factory.bin`. Non usare il factory per aggiornamenti ordinari: riscrive anche le aree di configurazione. Il file factory non è ancora collaudato su hardware reale.

## Prima configurazione e prova

1. Collegare ESP32 via USB e aprire il monitor seriale a 115200.
2. Al primo avvio, leggere nome e password casuale della rete Ariston-Setup sul monitor seriale; connettersi direttamente a quella rete e aprire http://192.168.4.1.
3. Inserire Wi-Fi, credenziali Ariston NET, eventuale ID impianto (obbligatorio se più caldaie nell'account) password amministratore e stagione iniziale. Il firmware si riavvia senza inviare comandi.
4. Tornare alla rete di casa; trovare IP nel monitor seriale. Aprire http://IP e autenticarsi come admin con la password scelta.
5. In Inverno l’automazione è già operativa: non esiste una modalità simulazione né un consenso aggiuntivo. In Estate il pulsante Test connessione Ariston è disponibile facoltativamente (sola lettura).
6. Selezionare Inverno se non già scelto e attendere acquisizione iniziale. Con caldaia e impianto sorvegliati, aprire/chiudere un ponticello tra GPIO27 e GND per almeno 3 secondi: i comandi sono reali. Controllare l’app ufficiale e gli ultimi eventi. Verificare anche riavvio, cambio stagione e perdita Wi-Fi.

Per riconfigurare, usare il comando seriale `RESET_CONFIG` seguito da invio, poi ripetere il setup. Le password non vengono pubblicate nel repository; sono salvate nella NVS locale non cifrata. La pagina locale usa autenticazione HTTP Digest, non HTTPS: usare soltanto rete privata fidata, non esporre porte Internet. Le credenziali di configurazione transitano nella rete AP protetta da password.

## OTA sperimentale

In stagione Estate, senza operazioni in corso, caricare il solo firmware.bin dalla pagina locale. I due slot OTA conservano la NVS durante aggiornamenti normali. L'immagine non è firmata: usare esclusivamente build fidate dello stesso progetto. Verificare OTA su banco prima dell'uso reale. In caso di problemi recuperare via USB.

## Limiti

API cloud non ufficiali, non garantite; nessun interblocco di sicurezza. Se caldaia e termocamino necessitano di esclusione reciproca per sicurezza, questa va realizzata localmente e indipendentemente da Internet e dal firmware, con un tecnico qualificato. Un POST è preceduto da lettura, verifica delle opzioni, controllo dell'evento, e seguito da readback cloud. Un esito cloud non prova lo stato fisico del bruciatore. Nessun retry automatico dopo una scrittura incerta. Cooldown HTTP 429 adattivo; Retry-After numerico supportato. Nessuna credenziale reale nei test.

## Sviluppo

Branch sviluppo, nessun rilascio stabile. Test della logica di contatto su host: `c++ -std=c++17 -Iinclude test/contact_test.cpp -o /tmp/contact-test && /tmp/contact-test`. Il workflow GitHub Actions compila anche il firmware e conserva i binari come artefatti di sviluppo. La compatibilità API, TLS, RAM, OTA e hardware rimangono da verificare su ESP32 reale.
