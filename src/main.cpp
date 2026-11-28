#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_heap_caps.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_system.h>
#include <atomic>
#include <time.h>
#include "automation.h"
#include "ariston_api.h"

namespace {
constexpr int PIN=27, BOOT_PIN=0;
constexpr uint32_t RECOVERY_DELAY=300000, BOOT_HOLD=10000;
constexpr char HOST[]="ariston-termocamino";
constexpr char VERSION[]="0.1.5-dev";
Preferences prefs;
WebServer server(80);
AristonConfig ariston;
String ssid, wifiPassword, csrf, recoveryPassword;
bool legacyAdminPresent=false;
bool recoveryActive=false, mdnsActive=false, recoveryStored=false;
uint32_t offlineSince=0, bootSince=0, baselineSince=0;
bool bootArmed=false, bootPressed=false, noisyLogged=false;
bool issue=false;
const char* lastStatus="Nessun comando dall avvio";
Observation verified{};
uint32_t attemptedCount=0, confirmedCount=0, errorCount=0;
std::atomic<uint32_t> workerStackMin{0};
bool setupMode=false, storageOk=false, restartRequested=false;
uint32_t restartAt=0;
Contact contact;
std::atomic<bool> operational{false}, stableClosed{false}, ready{false};
std::atomic<uint32_t> generation{0}, outstanding{0};
QueueHandle_t queueHandle=nullptr, resultQueue=nullptr;
bool previouslyConnected=false;
bool uploadAuthorized=false, uploadOk=false, uploadStarted=false, uploadHadBaseline=false, uploadPreviousClosed=false;
String serialLine;
struct Event { uint32_t seq; int target; bool closed; };
struct Completed { Event event; AristonResult result; };
struct Log { uint32_t seconds; time_t epoch; const char* text; };
constexpr size_t LOG_COUNT=32;
Log logEntries[LOG_COUNT]{};
size_t logNext=0, logSize=0;
portMUX_TYPE logMux=portMUX_INITIALIZER_UNLOCKED;

String randomHex() {
    char buf[33];
    snprintf(buf,sizeof(buf),"%08lx%08lx%08lx%08lx",(unsigned long)esp_random(),
             (unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random());
    return String(buf);
}
void logEvent(const char* msg) {
    time_t now=time(nullptr);
    portENTER_CRITICAL(&logMux);
    logEntries[logNext]={millis()/1000,now>=1735689600?now:0,msg};
    logNext=(logNext+1)%LOG_COUNT;
    if(logSize<LOG_COUNT) ++logSize;
    portEXIT_CRITICAL(&logMux);
    Serial.println(msg);
}
const char* modeName(int n) {
    switch(n){case 0:return "Estate";case 1:return "Inverno";case 5:return "Spenta";default:return "Non nota / altra";}
}
bool rawClosed(){return digitalRead(PIN)==LOW;}
void baseline() {
    ++generation;
    contact=Contact(3000);
    ready=false;baselineSince=millis();noisyLogged=false;
}
bool pendingOrBusy(){return outstanding.load()!=0;}
bool validEvent(const Event& e) {
    if(generation.load()!=e.seq || WiFi.status()!=WL_CONNECTED) return false;
    if(e.target<0) return true;
    return operational.load() && ready.load() && stableClosed.load()==e.closed && rawClosed()==e.closed;
}
void setIssue(bool next){
    if(issue==next)return;
    issue=next;
    if(storageOk&&!restartRequested&&!prefs.putBool("issue",next))
        logEvent("Errore salvataggio avviso in memoria");
}
void cloudWorker(void*) {
    AristonApi api(ariston);
    Event e;
    for(;;) {
        if(xQueueReceive(queueHandle,&e,portMAX_DELAY)!=pdTRUE) continue;
        auto result=api.execute(e.target,[&](){return validEvent(e);});
        workerStackMin=uxTaskGetStackHighWaterMark(nullptr);
        Completed completed{e,result};
        xQueueSend(resultQueue,&completed,portMAX_DELAY);
    }
}
void collectResults(){
    if(!resultQueue)return;
    Completed c;
    while(xQueueReceive(resultQueue,&c,0)==pdTRUE){
        --outstanding;
        const auto& r=c.result;
        if(r.observation.mode>=0)verified=r.observation;
        if(r.attempted)++attemptedCount;
        if(r.outcome==Outcome::Confirmed)++confirmedCount;
        if(r.outcome==Outcome::Rejected||r.outcome==Outcome::NotSent||r.outcome==Outcome::Uncertain)++errorCount;
        logEvent(r.message);
        if(c.event.seq!=generation.load()||restartRequested)continue;
        lastStatus=r.message;
        if(c.event.target>=0) setIssue(!satisfied(r.outcome));
        else if(r.outcome==Outcome::ReadOk && operational.load() && ready.load() &&
                r.observation.mode==(stableClosed.load()?0:1)) setIssue(false);
    }
}
bool enqueue(Event e){
    if(!queueHandle)return false;
    ++outstanding;
    if(xQueueSend(queueHandle,&e,0)==pdTRUE)return true;
    --outstanding;
    return false;
}
void readContact() {
    Edge edge=contact.sample(rawClosed(),millis());
    if(!contact.ready()) {
        if(!noisyLogged && uint32_t(millis()-baselineSince)>=15000){noisyLogged=true;logEvent("Contatto non stabile: acquisizione prolungata");}
        return;
    }
    if(!ready.load()) {
        stableClosed=contact.closed();ready=true;
        logEvent("Contatto iniziale acquisito: nessun comando");
    }
    if(edge==Edge::None)return;
    stableClosed=contact.closed();
    uint32_t seq=++generation;
    int target=targetFor(edge,operational.load());
    if(target<0){logEvent("Cambio contatto: automazione non disponibile");return;}
    setIssue(true); // An interrupted/pending event remains visible after reboot, never replayed.
    if(WiFi.status()!=WL_CONNECTED || !enqueue({seq,target,contact.closed()})){
        lastStatus="Cambio contatto perso: rete o worker non disponibile";++errorCount;
        logEvent(lastStatus);return;
    }
    lastStatus="Comando in corso";
    logEvent(target==0?"Pompa accesa: evento ESTATE":"Pompa spenta: evento INVERNO");
}
String pageStart(const char* title) {
    String h=F("<!doctype html><html lang='it'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>");
    h+=title;
    h+=F("</title><style>body{font:16px system-ui;background:#f3f5f7;color:#162338;margin:0}main{max-width:640px;margin:auto;padding:20px}section{background:white;border:1px solid #dce1e8;border-radius:12px;padding:18px;margin:12px 0}h1{font-size:26px;margin:12px 0}h2{font-size:18px;margin-top:0}p{line-height:1.45}small{color:#536174}button{padding:12px 16px;border:1px solid #8190a4;border-radius:8px;background:white;color:#162338;font:inherit;cursor:pointer;margin:4px 4px 4px 0}button.primary{background:#215a91;color:white;border-color:#215a91}input{box-sizing:border-box;width:100%;padding:10px;border:1px solid #8190a4;border-radius:6px;margin:5px 0 12px;font:inherit}label{display:block}dl{display:grid;grid-template-columns:1fr 1fr;gap:8px}dd{margin:0;font-weight:600}li{margin:8px 0}a{color:#215a91}button:disabled{opacity:.5}</style><main><h1>");
    h+=title;h+=F("</h1>");return h;
}
String hidden(){return "<input type='hidden' name='csrf' value='"+csrf+"'>";}
String form(const char* path,const char* name,const char* value,const char* label,bool selected=false,bool disabled=false){
    String h="<form style='display:inline-block' method='post' action='";h+=path;h+="'>";h+=hidden();
    h+="<button name='";h+=name;h+="' value='";h+=value;h+="'";
    if(selected)h+=" class='primary'";
    if(disabled)h+=" disabled";
    h+=">";h+=label;h+="</button></form>";return h;
}
void headers(){
    server.sendHeader("Cache-Control","no-store");
    server.sendHeader("X-Frame-Options","DENY");
    server.sendHeader("X-Content-Type-Options","nosniff");
    server.sendHeader("Content-Security-Policy","default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'");
}
void sendHtml(String h){headers();server.send(200,"text/html; charset=utf-8",h+"</main></html>");}
void sendText(int status,const char* msg){headers();server.send(status,"text/plain; charset=utf-8",msg);}
void redirect(){headers();server.sendHeader("Location","/");server.send(303,"text/plain","");}
// Prevent DNS rebinding from unrelated sites reaching the unauthenticated LAN UI.
bool localHost(){
    String host=server.hostHeader();host.toLowerCase();
    if(host.endsWith(":80"))host.remove(host.length()-3);
    IPAddress peer=server.client().remoteIP();
    IPAddress sta=WiFi.localIP(), mask=WiFi.subnetMask();
    if(WiFi.status()==WL_CONNECTED && uint32_t(mask)!=0 &&
       (uint32_t(peer)&uint32_t(mask))==(uint32_t(sta)&uint32_t(mask)) &&
       (host==sta.toString()||host=="ariston-termocamino.local"||host==HOST))return true;
    IPAddress ap=WiFi.softAPIP();
    return (setupMode||recoveryActive)&&host==ap.toString()&&
           peer[0]==ap[0]&&peer[1]==ap[1]&&peer[2]==ap[2];
}
bool requireLocal(){
    if(localHost())return true;
    sendText(403,"Usare l'indirizzo IP locale o ariston-termocamino.local");return false;
}
bool tokenValid(){return server.hasArg("csrf")&&server.arg("csrf")==csrf;}
bool postAuthorized(){
    if(!requireLocal())return false;
    if(!tokenValid()){sendText(403,"Richiesta non valida");return false;}
    if(restartRequested){sendText(409,"Riavvio in corso");return false;}
    return true;
}
void scheduleRestart(){restartRequested=true;restartAt=millis();operational=false;++generation;}
bool loadSettings(){
    String json=prefs.getString("config","");
    JsonDocument d;
    if(json.isEmpty()||deserializeJson(d,json))return false;
    ssid=d["ssid"]|"";wifiPassword=d["wifi"]|"";
    ariston.username=d["username"]|"";ariston.password=d["password"]|"";
    ariston.plant=d["plant"]|"";legacyAdminPresent=d.containsKey("admin");
    return ssid.length()&&ariston.username.length()&&ariston.password.length();
}
String timeLabel(time_t epoch,uint32_t seconds=0){
    if(epoch>=1735689600){
        struct tm local{};
        char buf[32];
        if(localtime_r(&epoch,&local)&&strftime(buf,sizeof(buf),"%d/%m/%Y %H:%M:%S",&local))return String(buf);
    }
    return "+"+String(seconds)+" s (ora non sincronizzata)";
}
bool validWifi(const String& s,const String& w){return s.length()>0&&s.length()<=32&&(w.isEmpty()||(w.length()>=8&&w.length()<=63));}
bool storeConfig(const String& s,const String& w,const String& u,const String& p,const String& id){
    JsonDocument d;
    d["ssid"]=s;d["wifi"]=w;d["username"]=u;d["password"]=p;d["plant"]=id;
    String payload;
    return !d.overflowed()&&serializeJson(d,payload)>0&&prefs.putString("config",payload)==payload.length();
}
void setupPage(){
    if(!requireLocal())return;
    String h=pageStart("Configurazione iniziale");
    h+=F("<section><p>Compila solo connesso direttamente alla rete Wi-Fi di configurazione della centralina. L automazione e attiva quando il dispositivo e alimentato e configurato. Nessun comando al riavvio.</p><form method='post' action='/setup'>");
    h+=hidden();
    h+=F("<label>Wi-Fi di casa<input name='ssid' maxlength='32' required></label><label>Password Wi-Fi<input name='wifi' type='password' maxlength='63'></label><label>Email Ariston NET<input name='username' maxlength='160' required></label><label>Password Ariston NET<input name='password' type='password' maxlength='160' required></label><label>ID impianto (vuoto solo se hai una caldaia)<input name='plant' maxlength='128'></label><button class='primary'>Salva e riavvia</button></form></section>");
    sendHtml(h);
}
void saveSetup(){
    if(!requireLocal())return;
    if(!setupMode||!tokenValid()||!storageOk||restartRequested){sendText(403,"Configurazione non disponibile");return;}
    String s=server.arg("ssid"),w=server.arg("wifi"),u=server.arg("username"),p=server.arg("password"),id=server.arg("plant");
    if(!validWifi(s,w)||!u.length()||u.length()>160||!p.length()||p.length()>160||id.length()>128){
        sendText(400,"Campi non validi");return;
    }
    if(!prefs.putBool("issue",false)||!storeConfig(s,w,u,p,id)){
        sendText(500,"Errore salvataggio");return;
    }
    sendText(200,"Configurazione salvata. Riavvio in corso. Ricollega il telefono alla rete di casa.");
    scheduleRestart();
}
void configurationPage(){
    if(!requireLocal())return;
    String h=pageStart("Configurazione");
    h+=F("<section><p>Lascia vuoti i campi da conservare. Per cambiare Wi-Fi senza password seleziona rete aperta. Modifica credenziali solo su una rete privata fidata.</p><form method='post' action='/config'>");
    h+=hidden();
    h+=F("<label>Nuovo nome Wi-Fi<input name='ssid' maxlength='32'></label><label>Nuova password Wi-Fi<input name='wifi' type='password' maxlength='63'></label><label><input style='width:auto' type='checkbox' name='open' value='1'> Rete aperta (cancella password Wi-Fi)</label><label>Nuova email Ariston NET<input name='username' maxlength='160'></label><label>Nuova password Ariston NET<input name='password' type='password' maxlength='160'></label><label>Nuovo ID impianto<input name='plant' maxlength='128'></label><label><input style='width:auto' type='checkbox' name='autoPlant' value='1'> Rimuovi ID (un solo impianto)</label><button class='primary'>Salva e riavvia</button></form><p><a href='/'>Indietro</a></p></section>");
    if(recoveryStored){
        h+=F("<section><h2>Wi-Fi di recupero</h2><p>Rete: Ariston-Recovery-");h+=String(uint32_t(ESP.getEfuseMac())&0xffff,HEX);
        h+=F("<br>Indirizzo: 192.168.4.1. Password di recupero disponibile via USB (comando seriale SHOW_RECOVERY).</p></section>");
    }
    sendHtml(h);
}
void saveConfiguration(){
    if(!postAuthorized())return;
    if(pendingOrBusy()||!storageOk){sendText(409,"Operazione in corso o memoria non disponibile");return;}
    String s=server.arg("ssid"),w=server.arg("wifi"),u=server.arg("username"),p=server.arg("password"),id=server.arg("plant");
    if(s.isEmpty())s=ssid;
    if(server.hasArg("open"))w="";
    else if(w.isEmpty()){
        if(s!=ssid){sendText(400,"Indicare la password della nuova rete o selezionare rete aperta");return;}
        w=wifiPassword;
    }
    if(u.isEmpty())u=ariston.username;
    if(p.isEmpty()){
        if(u!=ariston.username){sendText(400,"Indicare la password del nuovo account");return;}
        p=ariston.password;
    }
    if(server.hasArg("autoPlant"))id="";
    else if(id.isEmpty())id=ariston.plant;
    if(!validWifi(s,w)||!u.length()||u.length()>160||!p.length()||p.length()>160||id.length()>128){
        sendText(400,"Campi non validi");return;
    }
    if(!storeConfig(s,w,u,p,id)){sendText(500,"Errore salvataggio");return;}
    sendText(200,"Configurazione salvata. Riavvio senza invio di comandi.");
    scheduleRestart();
}
void dashboard(){
    if(!requireLocal())return;
    String h=pageStart("Ariston Termocamino");
    h+=F("<small>Centralina autonoma ESP32 | Firmware ");h+=VERSION;h+=F("</small><section><h2>Stato</h2><dl><dt>Automazione</dt><dd>");
    h+=operational.load()?"Attiva":"Non disponibile";
    h+=F("</dd><dt>Pompa</dt><dd>");
    h+=!ready.load()?"Acquisizione":stableClosed.load()?"Accesa":"Spenta";
    h+=F("</dd><dt>Contatto</dt><dd>");h+=!ready.load()?"Non noto":stableClosed.load()?"Chiuso":"Aperto";
    h+=F("</dd><dt>Ultima lettura Ariston</dt><dd>");h+=modeName(verified.mode);
    h+=F("</dd><dt>Data ultima lettura</dt><dd>");h+=verified.mode>=0?timeLabel(time_t(verified.at)):"Mai in questo avvio";
    h+=F("</dd><dt>Richiesta dal contatto</dt><dd>");h+=operational.load()&&ready.load()?modeName(stableClosed.load()?0:1):"Non attiva / non nota";
    h+=F("</dd><dt>Comunicazione</dt><dd>");h+=pendingOrBusy()?"In corso":"In attesa";
    h+=F("</dd><dt>Wi-Fi</dt><dd>");
    if(WiFi.status()==WL_CONNECTED){h+=String(WiFi.RSSI());h+=" dBm";}else h+="Non connesso";
    h+=F("</dd></dl><p><small>La lettura Ariston e storica, non in tempo reale.</small></p><p><strong>Ultimo esito: </strong>");h+=lastStatus;h+="</p>";
    bool differs=operational.load()&&ready.load()&&mismatch(verified.mode,stableClosed.load()?0:1);
    if(issue||differs){
        h+=F("<p style='color:#9a4700'><strong>Possibile disallineamento</strong><br>Un evento non e stato confermato oppure l ultima lettura differisce dal contatto. Verificare lo stato prima di riallineare.</p>");
    }else if(operational.load()&&verified.mode<0)h+=F("<p><small>Stato caldaia non verificato in questo avvio.</small></p>");
    bool busy=pendingOrBusy(), online=WiFi.status()==WL_CONNECTED;
    h+=form("/test","action","read","Verifica stato Ariston",false,busy||!online);
    h+=form("/align","action","align","Riallinea caldaia",false,busy||!online||!operational.load()||!ready.load()||rawClosed()!=stableClosed.load());
    h+=F("<p><small>Riallinea invia un comando esplicito se necessario. Non riattiva una caldaia Spenta.</small></p><a href='/'>Aggiorna pagina</a></section><section><h2>Diagnostica</h2>");
    h+=F("<dl><dt>RAM libera</dt><dd>");h+=String(ESP.getFreeHeap());h+=F(" byte</dd><dt>RAM minima</dt><dd>");h+=String(ESP.getMinFreeHeap());
    h+=F(" byte</dd><dt>Blocco libero maggiore</dt><dd>");h+=String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));h+=F(" byte</dd><dt>Stack worker minimo</dt><dd>");
    if(workerStackMin.load())h+=String(workerStackMin.load());else h+="Non misurato";
    h+=F("</dd><dt>Uptime</dt><dd>");h+=String(millis()/1000);h+=F(" s</dd><dt>POST tentati</dt><dd>");h+=String(attemptedCount);
    h+=F("</dd><dt>Comandi confermati</dt><dd>");h+=String(confirmedCount);h+=F("</dd><dt>Errori operazioni</dt><dd>");h+=String(errorCount);h+=F("</dd></dl></section><section><h2>Rete e configurazione</h2>");
    h+=F("<p><a href='/config'>Modifica configurazione Wi-Fi e Ariston</a></p><p>Nome locale: ariston-termocamino.local</p>");
    if(recoveryStored){h+=F("<p>Wi-Fi di recupero dopo 5 minuti senza rete: <strong>Ariston-Recovery-");h+=String(uint32_t(ESP.getEfuseMac())&0xffff,HEX);h+=F("</strong><br>Indirizzo: 192.168.4.1. Password disponibile via USB (SHOW_RECOVERY).</p>");}
    else h+=F("<p>Wi-Fi recupero non disponibile: errore memoria.</p>");
    h+=F("</section><section><h2>Ultimi eventi</h2><ul>");
    Log copy[LOG_COUNT];size_t n,next;
    portENTER_CRITICAL(&logMux);
    n=logSize;next=logNext;
    for(size_t i=0;i<LOG_COUNT;++i)copy[i]=logEntries[i];
    portEXIT_CRITICAL(&logMux);
    for(size_t i=0;i<n;++i){const Log& x=copy[(next+LOG_COUNT-1-i)%LOG_COUNT];h+="<li><small>";h+=timeLabel(x.epoch,x.seconds);h+="</small> ";h+=x.text;h+="</li>";}
    h+=F("</ul></section><section><h2>Aggiornamento firmware</h2><p><a href='/update'>Apri aggiornamento firmware</a></p></section><small>API Ariston non ufficiali. Non utilizzare per funzioni di sicurezza.</small>");
    sendHtml(h);
}
void firmwarePage(){
    if(!requireLocal())return;
    String h=pageStart("Aggiornamento firmware");
    h+=F("<section><p>Aggiornare quando non ci sono operazioni Ariston in corso, preferibilmente a pompa ferma. Caricare esclusivamente il firmware.bin compilato per questa centralina da una fonte fidata.</p>");
    if(!pendingOrBusy()&&!restartRequested){
        h+="<form method='post' enctype='multipart/form-data' action='/update?csrf="+csrf+"'><input type='file' name='firmware' accept='.bin' required><button>Aggiorna firmware</button></form>";
    }else h+=F("<p>Attendi la fine delle operazioni in corso.</p>");
    h+=F("<p><a href='/'>Indietro</a></p></section>");sendHtml(h);
}
void testCloud(){
    if(!postAuthorized())return;
    if(pendingOrBusy()||WiFi.status()!=WL_CONNECTED){sendText(409,"Test non disponibile");return;}
    if(!enqueue({generation.load(),-1,false})){sendText(409,"Worker occupato");return;}
    logEvent("Test Ariston avviato (sola lettura)");
    redirect();
}
void alignCloud(){
    if(!postAuthorized())return;
    if(!operational.load()||!ready.load()||pendingOrBusy()||WiFi.status()!=WL_CONNECTED||rawClosed()!=stableClosed.load()){
        sendText(409,"Riallineamento non disponibile");return;
    }
    bool closed=stableClosed.load();uint32_t seq=++generation;
    setIssue(true);
    if(!enqueue({seq,closed?0:1,closed})){lastStatus="Riallineamento non avviato";sendText(503,lastStatus);return;}
    lastStatus="Riallineamento manuale in corso";logEvent(lastStatus);redirect();
}
void uploadChunk(){
    HTTPUpload& u=server.upload();
    if(u.status==UPLOAD_FILE_START){
        uploadAuthorized=localHost()&&tokenValid()&&!pendingOrBusy()&&!restartRequested;
        uploadOk=false;uploadStarted=false;
        if(uploadAuthorized){
            uploadStarted=Update.begin(UPDATE_SIZE_UNKNOWN,U_FLASH);uploadOk=uploadStarted;
            if(uploadStarted){
                // WebServer handles the upload synchronously: no cloud job is pending.
                // Reacquire contact after the upload, never replay a transition during it.
                uploadHadBaseline=ready.load();uploadPreviousClosed=stableClosed.load();
                if(operational.load() && (!uploadHadBaseline || rawClosed()!=uploadPreviousClosed))setIssue(true);
                baseline();
                logEvent("Aggiornamento firmware: acquisizione contatto sospesa");
            }
        }
    }else if(u.status==UPLOAD_FILE_WRITE && uploadStarted && uploadOk){
        if(Update.write(u.buf,u.currentSize)!=u.currentSize){uploadOk=false;Update.abort();}
    }else if(u.status==UPLOAD_FILE_END && uploadStarted){
        uploadOk=uploadOk && u.totalSize>0 && Update.end(true);
        if(!uploadOk)Update.abort();
        if(operational.load() && uploadHadBaseline && rawClosed()!=uploadPreviousClosed)setIssue(true);
        uploadStarted=false;
    }else if(u.status==UPLOAD_FILE_ABORTED){
        if(uploadStarted){Update.abort();if(operational.load() && uploadHadBaseline && rawClosed()!=uploadPreviousClosed)setIssue(true);}
        uploadStarted=false;uploadOk=false;uploadAuthorized=false;
    }
}
void uploadFinish(){
    if(!uploadAuthorized){sendText(403,"Aggiornamento non autorizzato");return;}
    if(!uploadOk){uploadAuthorized=false;logEvent("Aggiornamento fallito: nuova acquisizione contatto");sendText(400,"Firmware non valido o aggiornamento fallito");return;}
    uploadOk=false;uploadAuthorized=false;
    logEvent("Firmware aggiornato: riavvio senza comandi");
    sendText(200,"Aggiornamento completato. Riavvio in corso.");scheduleRestart();
}
void resetConfig(){
    if(restartRequested)return;
    operational=false;baseline();
    // Preserve the recovery Wi-Fi password so BOOT reset needs no serial cable.
    if(storageOk && (!prefs.isKey("config")||prefs.remove("config"))){
        if(prefs.isKey("winter"))prefs.remove("winter");
        if(prefs.isKey("issue"))prefs.remove("issue");
        logEvent("Configurazione cancellata");scheduleRestart();
    }else logEvent("Errore cancellazione configurazione");
}
void bootButton(){
    bool down=digitalRead(BOOT_PIN)==LOW;
    if(!down){bootArmed=true;bootPressed=false;return;}
    if(!bootArmed||restartRequested)return;
    if(!bootPressed){bootPressed=true;bootSince=millis();}
    else if(uint32_t(millis()-bootSince)>=BOOT_HOLD){bootArmed=false;resetConfig();}
}
void serialCommands(){
    while(Serial.available()){
        char c=char(Serial.read());
        if(c=='\n'||c=='\r'){
            if(serialLine=="RESET_CONFIG"){
                resetConfig();
            }else if(serialLine=="SHOW_RECOVERY"){
                Serial.println("Password AP: "+recoveryPassword);
            }
            serialLine="";
        }else if(serialLine.length()<64)serialLine+=c;
    }
}
}
void setup(){
    Serial.begin(115200);
    pinMode(PIN,INPUT_PULLUP);
    pinMode(BOOT_PIN,INPUT_PULLUP);
    baselineSince=millis();
    setenv("TZ","CET-1CEST,M3.5.0,M10.5.0/3",1);tzset();
    storageOk=prefs.begin("ariston",false);
    setupMode=!storageOk||!loadSettings();
    if(!setupMode&&legacyAdminPresent){
        if(storeConfig(ssid,wifiPassword,ariston.username,ariston.password,ariston.plant))
            logEvent("Vecchia password manutenzione rimossa");
        else logEvent("Migrazione configurazione non salvata: password manutenzione comunque ignorata");
    }
    WiFi.persistent(false);
    WiFi.mode(setupMode?WIFI_AP:WIFI_STA); // enable radio before generating secrets
    if(storageOk)recoveryPassword=prefs.getString("recover","");
    if(recoveryPassword.length()==16)recoveryStored=true;
    else {recoveryPassword=randomHex().substring(0,16);recoveryStored=storageOk&&prefs.putString("recover",recoveryPassword)==16;}
    issue=storageOk&&prefs.getBool("issue",false);
    if(issue)lastStatus="Evento precedente non verificato (memoria persistente)";
    if(setupMode){
        operational=false;
        String apPassword=recoveryPassword;
        String apName="Ariston-Setup-"+String(uint32_t(ESP.getEfuseMac())&0xffff,HEX);
        if(!WiFi.softAP(apName.c_str(),apPassword.c_str()))Serial.println("Errore access point");
        csrf=randomHex();
        Serial.println("Configurazione: collegarsi alla rete Wi-Fi seguente");
        Serial.println(apName);Serial.println("Password AP: "+apPassword);Serial.println(WiFi.softAPIP());
        server.on("/",HTTP_GET,setupPage);
        server.on("/setup",HTTP_POST,saveSetup);
    }else{
        // Migration: the old seasonal setting no longer gates automation.
        if(prefs.isKey("winter"))prefs.remove("winter");
        WiFi.setAutoReconnect(true);
        WiFi.setHostname(HOST);
        offlineSince=millis();
        WiFi.begin(ssid.c_str(),wifiPassword.c_str());
        csrf=randomHex();
        configTzTime("CET-1CEST,M3.5.0,M10.5.0/3","pool.ntp.org","time.cloudflare.com");
        queueHandle=xQueueCreate(4,sizeof(Event));
        resultQueue=xQueueCreate(4,sizeof(Completed));
        if(!queueHandle||!resultQueue||xTaskCreate(cloudWorker,"ariston",16384,nullptr,1,nullptr)!=pdPASS){
            if(queueHandle)vQueueDelete(queueHandle);
            if(resultQueue)vQueueDelete(resultQueue);
            queueHandle=nullptr;resultQueue=nullptr;operational=false;
            logEvent("Worker non disponibile: automazione non disponibile");
        }else operational=true;
        server.on("/",HTTP_GET,dashboard);
        server.on("/test",HTTP_POST,testCloud);
        server.on("/align",HTTP_POST,alignCloud);
        server.on("/config",HTTP_GET,configurationPage);
        server.on("/config",HTTP_POST,saveConfiguration);
        server.on("/update",HTTP_GET,firmwarePage);
        server.on("/update",HTTP_POST,uploadFinish,uploadChunk);
    }
    server.onNotFound([](){sendText(404,"Pagina non trovata");});
    server.begin();
    logEvent("Avvio: nessun comando Ariston");
}
void loop(){
    serialCommands();bootButton();collectResults();
    if(!setupMode){
        bool connected=WiFi.status()==WL_CONNECTED;
        if(connected!=previouslyConnected){
            previouslyConnected=connected;
            if(!connected){
                ++generation;offlineSince=millis();
                if(issue&&pendingOrBusy())lastStatus="Connessione persa durante operazione: verificare Ariston";
                if(mdnsActive){MDNS.end();mdnsActive=false;}
            }
            logEvent(connected?"Wi-Fi connesso":"Wi-Fi disconnesso");
            if(connected){
                if(recoveryActive){WiFi.softAPdisconnect(true);WiFi.mode(WIFI_STA);recoveryActive=false;}
                // Do not turn a pre-reconnect candidate into a delayed write.
                if(ready.load()&&rawClosed()!=stableClosed.load()&&operational.load()){
                    setIssue(true);lastStatus="Possibile cambio contatto durante assenza rete";logEvent(lastStatus);
                }
                baseline();
                mdnsActive=MDNS.begin(HOST);
                if(mdnsActive)MDNS.addService("http","tcp",80);
                else logEvent("mDNS non disponibile: usare IP");
                Serial.println(WiFi.localIP());
            }
        }
        if(!restartRequested&&!connected&&!recoveryActive&&recoveryStored&&uint32_t(millis()-offlineSince)>=RECOVERY_DELAY){
            offlineSince=millis();
            WiFi.mode(WIFI_AP_STA);
            String name="Ariston-Recovery-"+String(uint32_t(ESP.getEfuseMac())&0xffff,HEX);
            recoveryActive=WiFi.softAP(name.c_str(),recoveryPassword.c_str());
            logEvent(recoveryActive?"Access point di recupero attivo":"Errore access point di recupero");
            if(recoveryActive){Serial.println(name);Serial.println(WiFi.softAPIP());}
        }
        if(!restartRequested)readContact();
    }
    server.handleClient();
    if(restartRequested && uint32_t(millis()-restartAt)>800 && !pendingOrBusy() && digitalRead(BOOT_PIN)==HIGH)ESP.restart();
    delay(10);
}
