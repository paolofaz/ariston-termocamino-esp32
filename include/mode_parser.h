#pragma once
#include <ArduinoJson.h>
#include <cstring>

// Ariston returns enum codes as integers, integral JSON floats or decimal strings.
inline int modeCode(JsonVariantConst v) {
    if(v.is<bool>() || v.isNull())return -1;
    if(v.is<double>()){
        double n=v.as<double>();
        if(!(n>=0 && n<=6))return -1;
        int code=static_cast<int>(n);
        return n==code?code:-1;
    }
    if(v.is<const char*>()){
        const char* s=v.as<const char*>();
        if(s && s[0]>='0' && s[0]<='6' && !s[1])return s[0]-'0';
    }
    return -1;
}
struct PlantModeData {
    int current=-1;
    unsigned optionsMask=0;
    const char* error=nullptr;
    bool optionsPresent=false;
    bool allows(int target) const {return target>=0 && target<=6 && (optionsMask & (1u<<target));}
};
inline PlantModeData parsePlantMode(JsonVariantConst payload){
    PlantModeData result;
    auto items=payload["items"].as<JsonArrayConst>();
    if(items.isNull()){result.error="items assente";return result;}
    bool found=false;
    for(JsonObjectConst row:items){
        if(std::strcmp(row["id"]|"","PlantMode")!=0)continue;
        JsonVariantConst zone=row["zone"];
        if(zone.isNull())zone=row["zn"];
        if(modeCode(zone)!=0)continue;
        if(found){result.error="duplicato";return result;}
        found=true;
        result.current=modeCode(row["value"]);
        auto options=row["options"].as<JsonArrayConst>();
        result.optionsPresent=!options.isNull() && options.size()>0;
        for(auto v:options){int n=modeCode(v);if(n>=0)result.optionsMask|=1u<<n;}
    }
    if(!found)result.error="zona 0 assente";
    else if(result.current<0)result.error="valore non valido";
    return result;
}
