#include "mode_parser.h"
#include <cassert>
PlantModeData parse(const char* json){
    JsonDocument doc;
    assert(!deserializeJson(doc,json));
    return parsePlantMode(doc.as<JsonVariantConst>());
}
int main(){
    auto a=parse(R"({"items":[{"id":"PlantMode","zone":0,"value":1.0,"options":[0.0,1.0,5.0]}]})");
    assert(!a.error && a.current==1 && a.allows(0) && a.allows(1) && a.allows(5) && !a.allows(2));
    a=parse(R"({"items":[{"id":"PlantMode","zn":"0","value":"1","options":["0","1"]}]})");
    assert(!a.error && a.current==1 && a.allows(0));
    a=parse(R"({"items":[{"id":"PlantMode","zone":0,"value":0}]})");
    assert(!a.error && a.current==0 && !a.optionsPresent && !a.allows(1));
    a=parse(R"({"items":[{"id":"PlantMode","zone":0,"value":0,"options":[true,0.5,{},7,null]}]})");
    assert(!a.error && !a.allows(0) && !a.allows(1));
    assert(parse(R"({"items":[{"id":"PlantMode","zone":0,"value":true}]})").error);
    assert(parse(R"({"items":[{"id":"PlantMode","zone":0,"value":1.5}]})").error);
    assert(parse(R"({"items":[{"id":"PlantMode","zone":0,"value":7}]})").error);
    assert(parse(R"({"items":[{"id":"PlantMode","zone":1,"value":1}]})").error);
    assert(parse(R"({"items":[{"id":"PlantMode","zone":0,"value":1},{"id":"PlantMode","zone":0,"value":0}]})").error);
}
