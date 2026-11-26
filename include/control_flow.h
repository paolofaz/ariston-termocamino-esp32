#pragma once
#include <stdint.h>

enum class Outcome { ReadOk, Already, Confirmed, Rejected, NotSent, Uncertain, Protected, Cancelled };
struct Observation { int mode=-1; int64_t at=0; };
struct AristonResult {
    Outcome outcome;
    const char* message;
    Observation observation{};
    bool attempted=false;
};
struct Preparation { bool ok; const char* message; Observation observation{}; bool allowed=false; };
enum class PostKind { Accepted, Rejected, NotSent, Uncertain };
struct PostResult { PostKind kind; const char* message; bool attempted=false; };
inline bool satisfied(Outcome o) { return o==Outcome::Already || o==Outcome::Confirmed; }
inline bool mismatch(int observed,int desired) { return observed>=0 && desired>=0 && observed!=desired; }

// Actual production control sequence, also exercised with fake transport on host.
template<class Valid,class Prepare,class Write,class Read>
AristonResult controlFlow(int target,Valid valid,Prepare prepare,Write write,Read read) {
    if(target < -1 || target > 1) return {Outcome::NotSent,"Comando non valido"};
    if(!valid()) return {Outcome::Cancelled,"Evento superato: nessuna scrittura"};
    auto p=prepare();
    if(!p.ok) return {Outcome::NotSent,p.message};
    auto old=p.observation;
    if(old.mode<0 || old.mode>6) return {Outcome::NotSent,"Stato non valido"};
    if(target<0) return {Outcome::ReadOk,"Lettura Ariston riuscita",old};
    if(!valid()) return {Outcome::Cancelled,"Evento superato: nessuna scrittura",old};
    if(old.mode==target) return {Outcome::Already,"Modalita gia corretta: nessuna scrittura",old};
    if(old.mode!=0 && old.mode!=1) return {Outcome::Protected,"Modalita caldaia protetta: nessuna scrittura",old};
    if(!p.allowed) return {Outcome::NotSent,"Modalita richiesta non supportata",old};
    auto post=write(old.mode);
    if(post.kind==PostKind::NotSent) return {Outcome::NotSent,post.message,old,post.attempted};
    if(post.kind==PostKind::Rejected) return {Outcome::Rejected,post.message,old,post.attempted};
    // Even when transport fails, read only; never repeat a control POST.
    auto after=read();
    if(after.mode==target) return {Outcome::Confirmed,"Modalita confermata dal cloud",after,post.attempted};
    return {Outcome::Uncertain,"Comando non confermato: verificare o riallineare",
            after.mode>=0?after:old,post.attempted};
}
