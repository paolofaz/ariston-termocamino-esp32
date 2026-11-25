#include <Arduino.h>
#include <WiFi.h>
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
constexpr int PIN=27;
constexpr char VERSION[]="0.1.1-dev";
constexpr char REALM[]="Ariston Termocamino";
Preferences prefs;
WebServer server(80);
AristonConfig ariston;
String ssid, wifiPassword, adminPassword, csrf;
bool setupMode=false, storageOk=false, restartRequested=false;
uint32_t restartAt=0;
Contact contact;
std::atomic<bool> enabled{false}, stableClosed{false}, ready{false};
std::atomic<uint32_t> generation{0}, outstanding{0};
std::atomic<int> verified{-1};
QueueHandle_t queueHandle=nullptr;
bool previouslyConnected=false;
bool uploadAuthorized=false, uploadOk=false, uploadStarted=false;
String serialLine;
struct Event { uint32_t seq; int target; bool closed; };
struct Log { uint32_t seconds; const char* text; };
constexpr size_t LOG_COUNT=12;
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
    portENTER_CRITICAL(&logMux);
    logEntries[logNext]={millis()/1000,msg};
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
    ready=false;
}
bool pendingOrBusy(){return outstanding.load()!=0;}
bool validEvent(const Event& e) {
    if(generation.load()!=e.seq || WiFi.status()!=WL_CONNECTED) return false;
    if(e.target<0) return !enabled.load();
    return enabled.load() && ready.load() && stableClosed.load()==e.closed && rawClosed()==e.closed;
}
void cloudWorker(void*) {
    AristonApi api(ariston);
    Event e;
    for(;;) {
        if(xQueueReceive(queueHandle,&e,portMAX_DELAY)!=pdTRUE) continue;
        if(!validEvent(e)) logEvent("Evento annullato: stato o connessione cambiati");
        else {
            auto result=api.execute(e.target,[&](){return validEvent(e);});
            verified=result.verified;
            logEvent(result.message);
        }
        --outstanding;
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
    if(!contact.ready()) return;
    if(!ready.load()) {
        stableClosed=contact.closed();ready=true;
        logEvent("Contatto iniziale acquisito: nessun comando");
    }
    if(edge==Edge::None)return;
    stableClosed=contact.closed();
    uint32_t seq=++generation;
    int target=targetFor(edge,enabled.load());
    if(target<0){logEvent("Cambio contatto ignorato: stagione estiva");return;}
    if(WiFi.status()!=WL_CONNECTED || !enqueue({seq,target,contact.closed()})){
        logEvent("Cambio contatto perso: rete o worker non disponibile");return;
    }
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
bool authorized(){
    if(server.authenticate("admin",adminPassword.c_str()))return true;
    headers();server.requestAuthentication(DIGEST_AUTH,REALM,"Autenticazione richiesta");return false;
}
bool tokenValid(){return server.hasArg("csrf")&&server.arg("csrf")==csrf;}
bool postAuthorized(){
    if(!authorized())return false;
    if(!tokenValid()){sendText(403,"Richiesta non valida");return false;}
    if(restartRequested){sendText(409,"Riavvio in corso");return false;}
    return true;
}
void scheduleRestart(){restartRequested=true;restartAt=millis();}
bool loadSettings(){
    String json=prefs.getString("config","");
    JsonDocument d;
    if(json.isEmpty()||deserializeJson(d,json))return false;
    ssid=d["ssid"]|"";wifiPassword=d["wifi"]|"";
    ariston.username=d["username"]|"";ariston.password=d["password"]|"";
    ariston.plant=d["plant"]|"";adminPassword=d["admin"]|"";
    return ssid.length()&&ariston.username.length()&&ariston.password.length()&&adminPassword.length()>=12;
}
void setupPage(){
    String h=pageStart("Configurazione iniziale");
    h+=F("<section><p>Compila solo connesso direttamente alla rete Wi-Fi di configurazione della centralina. Scegli la stagione iniziale: in Inverno le variazioni del contatto inviano comandi reali. Nessun comando al riavvio.</p><form method='post' action='/setup'>");
    h+=hidden();
    h+=F("<label>Wi-Fi di casa<input name='ssid' maxlength='32' required></label><label>Password Wi-Fi<input name='wifi' type='password' maxlength='63'></label><label>Email Ariston NET<input name='username' maxlength='160' required></label><label>Password Ariston NET<input name='password' type='password' maxlength='160' required></label><label>ID impianto (vuoto solo se hai una caldaia)<input name='plant' maxlength='128'></label><label>Password pagina locale (almeno 12 caratteri)<input name='admin' type='password' minlength='12' maxlength='64' required></label><label>Stagione iniziale</label><select name='season' required style='font:inherit;padding:10px;margin-bottom:12px;width:100%'><option value='winter'>Inverno - automazione attiva</option><option value='summer'>Estate - automazione disabilitata</option></select><button class='primary'>Salva e riavvia</button></form></section>");
    sendHtml(h);
}
void saveSetup(){
    if(!setupMode||!tokenValid()||!storageOk||restartRequested){sendText(403,"Configurazione non disponibile");return;}
    String s=server.arg("ssid"),w=server.arg("wifi"),u=server.arg("username"),p=server.arg("password"),id=server.arg("plant"),a=server.arg("admin");
    if(!s.length()||s.length()>32||w.length()>63||!u.length()||u.length()>160||!p.length()||p.length()>160||id.length()>128||a.length()<12||a.length()>64){
        sendText(400,"Campi non validi");return;
    }
    String season=server.arg("season");
    if(season!="winter"&&season!="summer"){sendText(400,"Stagione non valida");return;}
    if(!prefs.putBool("winter",season=="winter")){sendText(500,"Errore memoria");return;}
    JsonDocument d;
    d["ssid"]=s;d["wifi"]=w;d["username"]=u;d["password"]=p;d["plant"]=id;d["admin"]=a;
    String payload;
    if(!serializeJson(d,payload)||prefs.putString("config",payload)!=payload.length()){
        sendText(500,"Errore salvataggio");return;
    }
    sendText(200,"Configurazione salvata. Riavvio in corso. Ricollega il telefono alla rete di casa.");
    scheduleRestart();
}
void dashboard(){
    if(!authorized())return;
    String h=pageStart("Ariston Termocamino");
    h+=F("<small>Centralina autonoma ESP32 | Firmware ");h+=VERSION;h+=F("</small><section><h2>Stagione automazione</h2>");
    h+=form("/season","mode","summer","Estate - disabilitata",!enabled.load());
    h+=form("/season","mode","winter","Inverno - abilitata",enabled.load());
    h+=F("<p><small>Il cambio stagione non modifica la caldaia. Agisce solo alla prossima variazione stabile del contatto.</small></p></section><section><h2>Stato</h2><dl><dt>Pompa</dt><dd>");
    h+=!ready.load()?"Acquisizione":stableClosed.load()?"Accesa":"Spenta";
    h+=F("</dd><dt>Contatto</dt><dd>");h+=!ready.load()?"Non noto":stableClosed.load()?"Chiuso":"Aperto";
    h+=F("</dd><dt>Ultima lettura Ariston</dt><dd>");h+=modeName(verified.load());
    h+=F("</dd><dt>Comunicazione</dt><dd>");h+=pendingOrBusy()?"In corso":"In attesa";
    h+=F("</dd><dt>Wi-Fi</dt><dd>");h+=String(WiFi.RSSI());h+=F(" dBm</dd></dl><a href='/'>Aggiorna stato</a></section><section><h2>Diagnostica</h2>");
    bool locked=enabled.load()||pendingOrBusy();
    h+=form("/test","action","read","Test connessione Ariston",false,locked);
    h+=F("<p><small>Test facoltativo di sola lettura, disponibile in stagione estiva e senza operazioni in corso.</small></p></section><section><h2>Ultimi eventi</h2><ul>");
    Log copy[LOG_COUNT];size_t n,next;
    portENTER_CRITICAL(&logMux);
    n=logSize;next=logNext;
    for(size_t i=0;i<LOG_COUNT;++i)copy[i]=logEntries[i];
    portEXIT_CRITICAL(&logMux);
    for(size_t i=0;i<n;++i){const Log& x=copy[(next+LOG_COUNT-1-i)%LOG_COUNT];h+="<li><small>+";h+=String(x.seconds);h+=" s</small> ";h+=x.text;h+="</li>";}
    h+=F("</ul></section><section><h2>Aggiornamento firmware</h2><p><small>Solo in stagione estiva. Caricare esclusivamente il firmware.bin compilato per questa centralina da una fonte fidata.</small></p>");
    if(!locked){h+="<form method='post' enctype='multipart/form-data' action='/update?csrf="+csrf+"'><input type='file' name='firmware' accept='.bin' required><button>Aggiorna firmware</button></form>";}
    else h+=F("<p>Disabilita l'automazione e attendi la fine delle operazioni.</p>");
    h+=F("</section><small>API Ariston non ufficiali. Non utilizzare per funzioni di sicurezza.</small>");
    sendHtml(h);
}
void changeSeason(){
    if(!postAuthorized())return;
    String mode=server.arg("mode");
    if(mode!="summer"&&mode!="winter"){sendText(400,"Stagione non valida");return;}
    bool next=mode=="winter";
    if(next&&!queueHandle){sendText(503,"Worker non disponibile");return;}
    if(next!=enabled.load()){
        enabled=false;baseline();
        if(!storageOk||!prefs.putBool("winter",next)){sendText(500,"Errore memoria: automazione disabilitata in RAM");return;}
        enabled=next;
        logEvent(next?"Stagione inverno: attendo nuova transizione":"Stagione estate: automazione disabilitata");
    }
    redirect();
}
void testCloud(){
    if(!postAuthorized())return;
    if(enabled.load()||pendingOrBusy()||WiFi.status()!=WL_CONNECTED){sendText(409,"Test non disponibile");return;}
    if(!enqueue({generation.load(),-1,false})){sendText(409,"Worker occupato");return;}
    logEvent("Test Ariston avviato (sola lettura)");
    redirect();
}
void uploadChunk(){
    HTTPUpload& u=server.upload();
    if(u.status==UPLOAD_FILE_START){
        uploadAuthorized=server.authenticate("admin",adminPassword.c_str())&&tokenValid()&&!enabled.load()&&!pendingOrBusy()&&!restartRequested;
        uploadOk=false;uploadStarted=false;
        if(uploadAuthorized){uploadStarted=Update.begin(UPDATE_SIZE_UNKNOWN,U_FLASH);uploadOk=uploadStarted;}
    }else if(u.status==UPLOAD_FILE_WRITE && uploadStarted && uploadOk){
        if(Update.write(u.buf,u.currentSize)!=u.currentSize){uploadOk=false;Update.abort();}
    }else if(u.status==UPLOAD_FILE_END && uploadStarted){
        uploadOk=uploadOk && u.totalSize>0 && Update.end(true);
        if(!uploadOk)Update.abort();
        uploadStarted=false;
    }else if(u.status==UPLOAD_FILE_ABORTED){
        if(uploadStarted)Update.abort();
        uploadStarted=false;uploadOk=false;uploadAuthorized=false;
    }
}
void uploadFinish(){
    if(!uploadAuthorized){sendText(403,"Aggiornamento non autorizzato");return;}
    if(!uploadOk){sendText(400,"Firmware non valido o aggiornamento fallito");return;}
    uploadOk=false;uploadAuthorized=false;
    logEvent("Firmware aggiornato: riavvio senza comandi");
    sendText(200,"Aggiornamento completato. Riavvio in corso.");scheduleRestart();
}
void serialCommands(){
    while(Serial.available()){
        char c=char(Serial.read());
        if(c=='\n'||c=='\r'){
            if(serialLine=="RESET_CONFIG"){
                enabled=false;baseline();
                if(storageOk && prefs.clear()){logEvent("Configurazione cancellata");scheduleRestart();}
                else logEvent("Errore cancellazione memoria");
            }
            serialLine="";
        }else if(serialLine.length()<64)serialLine+=c;
    }
}
}
void setup(){
    Serial.begin(115200);
    pinMode(PIN,INPUT_PULLUP);
    storageOk=prefs.begin("ariston",false);
    setupMode=!storageOk||!loadSettings();
    WiFi.persistent(false);
    if(setupMode){
        enabled=false;WiFi.mode(WIFI_AP);
        String apPassword=randomHex().substring(0,16);
        String apName="Ariston-Setup-"+String(uint32_t(ESP.getEfuseMac())&0xffff,HEX);
        if(!WiFi.softAP(apName.c_str(),apPassword.c_str()))Serial.println("Errore access point");
        csrf=randomHex();
        Serial.println("Configurazione: collegarsi alla rete Wi-Fi seguente");
        Serial.println(apName);Serial.println("Password AP: "+apPassword);Serial.println(WiFi.softAPIP());
        server.on("/",HTTP_GET,setupPage);
        server.on("/setup",HTTP_POST,saveSetup);
    }else{
        enabled=prefs.getBool("winter",false);
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.setHostname("ariston-termocamino");
        WiFi.begin(ssid.c_str(),wifiPassword.c_str());
        csrf=randomHex();
        configTime(0,0,"pool.ntp.org","time.cloudflare.com");
        queueHandle=xQueueCreate(4,sizeof(Event));
        if(!queueHandle||xTaskCreate(cloudWorker,"ariston",16384,nullptr,1,nullptr)!=pdPASS){
            if(queueHandle)vQueueDelete(queueHandle);
            queueHandle=nullptr;enabled=false;
            logEvent("Worker non disponibile: automazione disabilitata");
        }
        server.on("/",HTTP_GET,dashboard);
        server.on("/season",HTTP_POST,changeSeason);
        server.on("/test",HTTP_POST,testCloud);
        server.on("/update",HTTP_POST,uploadFinish,uploadChunk);
    }
    server.onNotFound([](){sendText(404,"Pagina non trovata");});
    server.begin();
    logEvent("Avvio: nessun comando Ariston");
}
void loop(){
    serialCommands();
    if(!setupMode){
        bool connected=WiFi.status()==WL_CONNECTED;
        if(connected!=previouslyConnected){
            previouslyConnected=connected;
            if(!connected)++generation;
            logEvent(connected?"Wi-Fi connesso":"Wi-Fi disconnesso");
            if(connected)Serial.println(WiFi.localIP());
        }
        if(!restartRequested)readContact();
    }
    server.handleClient();
    if(restartRequested && uint32_t(millis()-restartAt)>800 && !pendingOrBusy())ESP.restart();
    delay(10);
}
