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
constexpr int BAD_JSON = -1001, OFFLINE = -1002, CLOCK = -1003, BAD_DATA = -1004;
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
    if (n == 429) return "Ariston: limite richieste, operazione annullata";
    if (authError(n)) return "Ariston: autenticazione non riuscita";
    if (n == OFFLINE) return "Wi-Fi non disponibile";
    if (n == CLOCK) return "Orologio non sincronizzato: TLS non disponibile";
    if (n == BAD_DATA || n == BAD_JSON) return "Ariston: risposta inattesa o impianto non valido";
    return "Ariston: errore di comunicazione";
}
}

int AristonApi::request(const char* method, const String& path, const JsonDocument* body, JsonDocument& out) {
    out.clear();
    if (cooldown_ && uint32_t(millis()-blockedAt_) < cooldown_) return 429;
    if (WiFi.status()!=WL_CONNECTED) return OFFLINE;
    if (time(nullptr)<1735689600) return CLOCK;
    WiFiClientSecure tls;
    tls.setCACertBundle(ARISTON_CA_BUNDLE);
    tls.setHandshakeTimeout(10);
    HTTPClient http;
    if (!http.begin(tls, String(BASE)+path)) return -1;
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
        if (!serializeJson(*body, payload)) { http.end(); return BAD_JSON; }
        http.addHeader("Content-Type","application/json");
        status=http.POST(payload);
    } else status=http.GET();
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
    if (status<200 || status>=300) { http.end(); return status; }
    if (http.getSize()>int(MAX_BODY)) { http.end(); return BAD_JSON; }
    BoundedBody response;
    const int written=http.writeToStream(&response);
    http.end();
    if (written<0 || size_t(written)!=response.data.length() ||
        deserializeJson(out,response.data) || out.overflowed()) return BAD_JSON;
    throttles_=0; cooldown_=0;
    return 200;
}

int AristonApi::login() {
    token_="";
    JsonDocument body, reply;
    body["usr"]=cfg_.username; body["pwd"]=cfg_.password;
    int r=request("POST","/api/v2/accounts/login",&body,reply);
    if (r!=200) return r;
    if (denied(reply)) return 401;
    const char* t=reply["token"].as<const char*>();
    if (!t || !*t || strlen(t)>8192) return BAD_DATA;
    token_=t;
    return 200;
}

int AristonApi::readMode(const String& plant,const JsonDocument& features,int& current,int target,bool& allowed) {
    JsonDocument body, reply;
    body["useCache"]=false;
    auto item=body["items"].to<JsonArray>().add<JsonObject>();
    item["id"]="PlantMode"; item["zn"]=0;
    body["features"].set(features.as<JsonVariantConst>());
    if (body.overflowed()) return BAD_JSON;
    int r=request("POST","/api/v2/remote/dataItems/"+plant+"/get?umsys=si",&body,reply);
    if (r!=200) return r;
    auto items=reply["items"].as<JsonArrayConst>();
    if (items.isNull()) return BAD_DATA;
    bool found=false;
    for (JsonObjectConst row:items) {
        if (strcmp(row["id"] | "", "PlantMode") != 0) continue;
        JsonVariantConst zone=row["zone"];
        if (zone.isNull()) zone=row["zn"];
        if (code(zone)!=0) continue;
        if (found) return BAD_DATA;
        found=true;
        current=code(row["value"]);
        auto options=row["options"].as<JsonArrayConst>();
        if (current<0 || options.isNull() || !options.size()) return BAD_DATA;
        allowed=false;
        for (auto v:options) if (code(v)==target) allowed=true;
    }
    return found?200:BAD_DATA;
}

int AristonApi::prepare(String& plant,JsonDocument& features,int& current,int target,bool& allowed) {
    if (token_.isEmpty()) { int r=login(); if (r!=200) return r; }
    // Never auto-select among multiple plants. Check configured id against account list.
    JsonDocument list;
    int r=request("GET","/api/v2/remote/plants/lite",nullptr,list);
    if (r!=200) return r;
    auto arr=list.as<JsonArrayConst>();
    if (arr.isNull() || !arr.size()) return BAD_DATA;
    plant="";
    if (cfg_.plant.isEmpty() && arr.size()==1) plant=arr[0]["gwId"] | "";
    else if (!cfg_.plant.isEmpty()) {
        for (auto row:arr) if (cfg_.plant==String(row["gwId"] | "")) plant=cfg_.plant;
    }
    if (!safeId(plant)) return BAD_DATA;
    list.clear();
    r=request("GET","/api/v2/remote/plants/"+plant+"/features?eagerMode=True",nullptr,features);
    if (r!=200) return r;
    if (!features.is<JsonObject>() || !features.size()) return BAD_DATA;
    return readMode(plant,features,current,target,allowed);
}

AristonResult AristonApi::execute(int target,const std::function<bool()>& stillValid) {
    if (target!=-1 && target!=0 && target!=1) return {"Comando non valido"};
    if (!stillValid()) return {"Evento superato: nessuna scrittura"};
    String plant;
    JsonDocument features;
    int current=-1; bool allowed=false;
    int r=prepare(plant,features,current,target,allowed);
    // Refresh auth once during read-only preparation, never after a control POST.
    if (authError(r)) {
        token_=""; features.clear();
        if (!stillValid()) return {"Evento superato: nessuna scrittura"};
        r=prepare(plant,features,current,target,allowed);
    }
    if (r!=200) return {errorMessage(r)};
    if (target<0) return {"Test Ariston: lettura riuscita",current};
    if (current==target) return {"Modalita gia corretta: nessuna scrittura",current};
    // Deliberate safety policy: never turn a manually OFF/other-mode boiler back on.
    if (current!=0 && current!=1) return {"Modalita caldaia protetta: nessuna scrittura",current};
    if (!allowed) return {"Modalita richiesta non supportata",current};
    if (!stillValid()) return {"Evento superato: nessuna scrittura",current};
    JsonDocument body, reply;
    auto item=body["items"].to<JsonArray>().add<JsonObject>();
    item["id"]="PlantMode"; item["zone"]=0; item["prevValue"]=current; item["value"]=target;
    body["features"].set(features.as<JsonVariantConst>());
    if (body.overflowed()) return {"Memoria insufficiente: nessuna scrittura",current};
    if (!stillValid()) return {"Evento superato: nessuna scrittura",current};
    r=request("POST","/api/v2/remote/dataItems/"+plant+"/set?umsys=si",&body,reply);
    if (r!=200) return {"Scrittura fallita o incerta: verificare app Ariston"};
    if (denied(reply)) return {"Ariston ha rifiutato il comando"};
    body.clear(); reply.clear();
    int after=-1; bool ignored=false;
    r=readMode(plant,features,after,target,ignored);
    if (r==200 && after==target) return {"Comando confermato dal cloud",after};
    return {"Comando non confermato: verificare app Ariston"};
}
