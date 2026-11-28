#include "ariston_api.h"
#include "generated_ca.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>
#include <time.h>
#include <stdlib.h>

namespace {
constexpr char BASE[] = "https://www.ariston-net.remotethermo.com";
constexpr size_t MAX_BODY = 48 * 1024;
constexpr int BAD_JSON = -1001, OFFLINE = -1002, CLOCK = -1003, BAD_DATA = -1004, BLOCKED = -1005, CANCELLED = -1006, AUTH_BLOCKED = -1007;
class BoundedBody : public Stream {
public:
    String data;
    size_t write(uint8_t b) override { return write(&b, 1); }
    size_t write(const uint8_t* buf, size_t n) override {
        if (n > MAX_BODY - data.length()) return 0;
        return data.concat(reinterpret_cast<const char*>(buf), n) ? n : 0;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
};
int code(JsonVariantConst v) {
    if (v.is<bool>()) return -1;
    if (v.is<int>()) { int n = v.as<int>(); return n >= 0 && n <= 6 ? n : -1; }
    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        if (s && s[0] >= '0' && s[0] <= '6' && !s[1]) return s[0]-'0';
    }
    return -1;
}
bool authError(int n) { return n == 401 || n == 403 || n == 405; }
bool safeId(const String& s) {
    if (s.isEmpty() || s.length() > 128) return false;
    for (unsigned i=0;i<s.length();++i) {
        char c=s[i];
        if (!((c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='-'||c=='_')) return false;
    }
    return true;
}
bool denied(const JsonDocument& d) {
    return (d["success"].is<bool>() && !d["success"].as<bool>()) ||
           (d["ok"].is<bool>() && !d["ok"].as<bool>());
}
const char* errorMessage(int n) {
    if (n == 429 || n == BLOCKED) return "Ariston: limite richieste, operazione annullata";
    if (n == CANCELLED) return "Evento superato: nessuna scrittura";
    if (n == AUTH_BLOCKED) return "Login rifiutato: pausa di 5 minuti";
    if (authError(n)) return "Ariston: autenticazione non riuscita";
    if (n == OFFLINE) return "Wi-Fi non disponibile";
    if (n == CLOCK) return "Orologio non sincronizzato: TLS non disponibile";
    if (n == BAD_DATA || n == BAD_JSON) return "Ariston: risposta inattesa o impianto non valido";
    return "Ariston: errore di comunicazione";
}
}

void AristonApi::trace(const char* stage,int status,int tlsError){
    snprintf(diagnostic_,sizeof(diagnostic_),"%s | codice %d | TLS %d | heap %lu byte",
             stage,status,tlsError,(unsigned long)ESP.getFreeHeap());
    Serial.println(diagnostic_);
}

int AristonApi::request(const char* method, const String& path, const JsonDocument* body, JsonDocument& out,
                         const std::function<bool()>* valid, bool* attempted) {
    out.clear();
    const char* stage=path.endsWith("/login")?"Login":
        path.endsWith("/lite")?"Lista impianti":
        path.indexOf("/features?")>=0?"Features":
        path.indexOf("/set?")>=0?"Scrittura PlantMode":"Lettura PlantMode";
    auto fail=[&](int n){trace(stage,n);return n;};
    if (valid && !(*valid)()) return fail(CANCELLED);
    if (cooldown_ && uint32_t(millis()-blockedAt_) < cooldown_) return fail(BLOCKED);
    if (WiFi.status()!=WL_CONNECTED) return fail(OFFLINE);
    if (time(nullptr)<1735689600) return fail(CLOCK);
    WiFiClientSecure tls;
    tls.setCACertBundle(ARISTON_CA_BUNDLE);
    tls.setHandshakeTimeout(10);
    HTTPClient http;
    if (!http.begin(tls, String(BASE)+path)) return fail(-1);
    http.setConnectTimeout(8000);
    http.setTimeout(10000);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setUserAgent("RestSharp/106.11.7.0");
    if (token_.length()) http.addHeader("ar.authToken", token_);
    const char* headers[]={"Retry-After"};
    http.collectHeaders(headers, 1);
    int status;
    if (body) {
        String payload;
        const size_t expected=measureJson(*body);
        if (!expected || expected>MAX_BODY || !payload.reserve(expected) ||
            serializeJson(*body,payload)!=expected || payload.length()!=expected) { http.end(); return fail(BAD_JSON); }
        http.addHeader("Content-Type","application/json");
        if (valid && !(*valid)()) { http.end(); return fail(CANCELLED); }
        if (attempted) *attempted=true;
        status=http.POST(payload);
    } else {
        if (valid && !(*valid)()) { http.end(); return fail(CANCELLED); }
        status=http.GET();
    }
    if (status==429) {
        if (throttles_<5) ++throttles_;
        uint32_t seconds = 300u << (throttles_-1);
        if (seconds>3600) seconds=3600;
        String retry=http.header("Retry-After");
        char* end=nullptr;
        unsigned long retrySeconds=strtoul(retry.c_str(),&end,10);
        if (retry.length() && end && !*end && retrySeconds>seconds)
            seconds=retrySeconds>86400?86400:retrySeconds;
        blockedAt_=millis(); cooldown_=seconds*1000u;
    }
    if (status<200 || status>=300) {
        char tlsMessage[128]{};
        int tlsError=status<0?tls.lastError(tlsMessage,sizeof(tlsMessage)):0;
        trace(stage,status,tlsError);
        http.end(); return status;
    }
    if (http.getSize()>int(MAX_BODY)) { http.end(); return fail(BAD_JSON); }
    BoundedBody response;
    const int written=http.writeToStream(&response);
    http.end();
    if (written<0 || size_t(written)!=response.data.length() ||
        deserializeJson(out,response.data) || out.overflowed()) return fail(BAD_JSON);
    trace(stage,status);
    throttles_=0; cooldown_=0;
    return 200;
}

int AristonApi::login(const std::function<bool()>& valid) {
    if (authBlocked_ && uint32_t(millis()-authBlockedAt_)<300000u) return AUTH_BLOCKED;
    authBlocked_=false;
    token_="";
    JsonDocument body, reply;
    body["usr"]=cfg_.username; body["pwd"]=cfg_.password;
    int r=request("POST","/api/v2/accounts/login",&body,reply,&valid);
    if (authError(r) || (r==200 && denied(reply))) {
        authBlocked_=true; authBlockedAt_=millis(); return 401;
    }
    if (r!=200) return r;
    const char* t=reply["token"].as<const char*>();
    if (!t || !*t || strlen(t)>8192) {trace("Login: token assente/non valido",BAD_DATA);return BAD_DATA;}
    token_=t;
    return 200;
}

int AristonApi::readMode(const String& plant,const JsonDocument& features,int& current,int target,bool& allowed,const std::function<bool()>* valid) {
    JsonDocument body, reply;
    body["useCache"]=false;
    auto item=body["items"].to<JsonArray>().add<JsonObject>();
    item["id"]="PlantMode"; item["zn"]=0;
    body["features"].set(features.as<JsonVariantConst>());
    if (body.overflowed()) return BAD_JSON;
    int r=request("POST","/api/v2/remote/dataItems/"+plant+"/get?umsys=si",&body,reply,valid);
    if (r!=200) return r;
    auto items=reply["items"].as<JsonArrayConst>();
    if (items.isNull()) {trace("Lettura PlantMode: items assente",BAD_DATA);return BAD_DATA;}
    bool found=false;
    for (JsonObjectConst row:items) {
        if (strcmp(row["id"] | "", "PlantMode") != 0) continue;
        JsonVariantConst zone=row["zone"];
        if (zone.isNull()) zone=row["zn"];
        if (code(zone)!=0) continue;
        if (found) {trace("Lettura PlantMode: duplicato",BAD_DATA);return BAD_DATA;}
        found=true;
        current=code(row["value"]);
        auto options=row["options"].as<JsonArrayConst>();
        if (current<0 || options.isNull() || !options.size()) {trace("Lettura PlantMode: valore/opzioni non validi",BAD_DATA);return BAD_DATA;}
        allowed=false;
        for (auto v:options) if (code(v)==target) allowed=true;
    }
    if(!found)trace("Lettura PlantMode: zona 0 assente",BAD_DATA);
    return found?200:BAD_DATA;
}

int AristonApi::prepare(String& plant,JsonDocument& features,int& current,int target,bool& allowed,const std::function<bool()>& valid) {
    if (!valid()) return CANCELLED;
    if (token_.isEmpty()) { int r=login(valid); if (r!=200) return r; }
    // Never auto-select among multiple plants. Check configured id against account list.
    JsonDocument list;
    int r=request("GET","/api/v2/remote/plants/lite",nullptr,list,&valid);
    if (r!=200) return r;
    auto arr=list.as<JsonArrayConst>();
    if (arr.isNull() || !arr.size()) {trace("Lista impianti: vuota/non valida",BAD_DATA);return BAD_DATA;}
    plant="";
    if (cfg_.plant.isEmpty() && arr.size()==1) plant=arr[0]["gwId"] | "";
    else if (!cfg_.plant.isEmpty()) {
        for (auto row:arr) if (cfg_.plant==String(row["gwId"] | "")) plant=cfg_.plant;
    }
    if (!safeId(plant)) {trace("Lista impianti: selezione non valida",BAD_DATA);return BAD_DATA;}
    list.clear();
    r=request("GET","/api/v2/remote/plants/"+plant+"/features?eagerMode=True",nullptr,features,&valid);
    if (r!=200) return r;
    if (!features.is<JsonObject>() || !features.size()) {trace("Features: risposta non valida",BAD_DATA);return BAD_DATA;}
    return readMode(plant,features,current,target,allowed,&valid);
}

AristonResult AristonApi::execute(int target,const std::function<bool()>& stillValid) {
    diagnostic_[0]=0;
    String plant;
    JsonDocument features;
    auto prepareState=[&]() -> Preparation {
        int current=-1; bool allowed=false;
        const bool hadToken=!token_.isEmpty();
        int r=prepare(plant,features,current,target,allowed,stillValid);
        // Only refresh a previously cached token, not a rejected fresh login.
        if (authError(r)) {
            token_="";
            if(hadToken && stillValid()) {
                features.clear();
                r=prepare(plant,features,current,target,allowed,stillValid);
                if(authError(r)) token_="";
            }
        }
        if(r!=200) return {false,errorMessage(r)};
        return {true,"",{current,int64_t(time(nullptr))},allowed};
    };
    auto write=[&](int current) -> PostResult {
        JsonDocument body,reply;
        auto item=body["items"].to<JsonArray>().add<JsonObject>();
        item["id"]="PlantMode"; item["zone"]=0; item["prevValue"]=current; item["value"]=target;
        body["features"].set(features.as<JsonVariantConst>());
        if(body.overflowed()) return {PostKind::NotSent,"Memoria insufficiente: nessuna scrittura"};
        bool attempted=false;
        int r=request("POST","/api/v2/remote/dataItems/"+plant+"/set?umsys=si",&body,reply,&stillValid,&attempted);
        if(!attempted) return {PostKind::NotSent,errorMessage(r)};
        if(authError(r)) token_="";
        if(r==429) return {PostKind::Rejected,"Ariston: scrittura rifiutata (429), pausa richieste",true};
        if((r>=400 && r<500 && r!=408) || (r==200 && denied(reply)))
            return {PostKind::Rejected,"Ariston ha rifiutato il comando",true};
        return {r==200?PostKind::Accepted:PostKind::Uncertain,"Esito da verificare",true};
    };
    auto readBack=[&]() -> Observation {
        int after=-1; bool ignored=false;
        // Readback is safe even if contact changed during the already dispatched POST.
        int r=readMode(plant,features,after,target,ignored);
        if(authError(r)) token_="";
        if(r!=200) return {};
        return {after,int64_t(time(nullptr))};
    };
    return controlFlow(target,stillValid,prepareState,write,readBack);
}
