#include "lego_dimensions_function.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

static sky::lego::Report next(sky::lego::ToyPadFunction& p) {
    sky::lego::Report r{};
    assert(p.peekInput(r));
    p.dropInput();
    return r;
}
static uint32_t readLE(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24); }
static void writeLE(uint8_t* p, uint32_t v) { p[0]=uint8_t(v); p[1]=uint8_t(v>>8); p[2]=uint8_t(v>>16); p[3]=uint8_t(v>>24); }
static void testTea(uint8_t* b, bool encrypt) {
    const uint8_t key[16]={0x55,0xFE,0xF6,0xB0,0x62,0xBF,0x0B,0x41,0xC9,0xB3,0x7C,0xB4,0x97,0x3E,0x29,0x7B};
    uint32_t k[4]={readLE(key),readLE(key+4),readLE(key+8),readLE(key+12)};
    uint32_t a=readLE(b), c=readLE(b+4);
    if(encrypt) { uint32_t sum=0; for(int i=0;i<32;++i){sum+=0x9e3779b9u;a+=(((c<<4)+k[0])^(c+sum)^((c>>5)+k[1]));c+=(((a<<4)+k[2])^(a+sum)^((a>>5)+k[3]));} }
    else { uint32_t sum=0xc6ef3720u; for(int i=0;i<32;++i){c-=(((a<<4)+k[2])^(a+sum)^((a>>5)+k[3]));a-=(((c<<4)+k[0])^(c+sum)^((c>>5)+k[1]));sum-=0x9e3779b9u;} }
    writeLE(b,a); writeLE(b+4,c);
}

static void request(sky::lego::ToyPadFunction& p, uint8_t cmd, uint8_t cid,
                    const uint8_t* payload, size_t payloadLen) {
    sky::lego::Report r{};
    r[0]=0x56; r[1]=uint8_t(payloadLen+2); r[2]=cmd; r[3]=cid;
    if(payloadLen) std::memcpy(r.data()+4,payload,payloadLen);
    r[2+r[1]]=sky::lego::checksum(r.data(),2+r[1]);
    p.onInterruptOut(r.data(),r.size());
}
static void initializeHost(sky::lego::ToyPadFunction& p) {
    sky::lego::Report r{};
    const uint8_t init[18]={0x55,0x0f,0xb0,0x01,0x28,0x63,0x29,0x20,0x4c,
                            0x45,0x47,0x4f,0x20,0x32,0x30,0x31,0x34,0xf7};
    std::memcpy(r.data(),init,sizeof init);
    p.onInterruptOut(r.data(),r.size());
}

int main(){
 using namespace sky; using namespace sky::lego;
 ToyPadFunction p;
 Report r{};
 assert(!p.peekInput(r)); // host-to-pad init must not be sent as an unsolicited IN report
 initializeHost(p);
 // v16 fix: the wake command gets a framed reply on HID IN (the game waits for it). Frame: 0x55, length,
 // command id, then the "(c) LEGO 2014" greeting.
 assert(p.hostActive());
 r=next(p);
 assert(r[0]==0x55 && r[1]==0x0e && r[2]==0x01 && std::memcmp(r.data()+3,"(c) LEGO 2014",13)==0);
 assert(!p.peekInput(r));
 TagProfile profile{};
 profile.uid={{4,1,2,3,4,5,6}}; profile.figureId=1; // Batman profile
 profile.memory[0]=0xa1; profile.memory[1]=0xb2; profile.memory[2]=0xc3; profile.memory[3]=0xd4;
 assert(p.placeProfile(0,profile)); assert(p.occupied(0));
 r=next(p);
 assert(r[0]==0x56 && r[2]==2 && r[4]==0 && r[5]==0 && r[6]==4); // left logical pad
 assert(r[13]==checksum(r.data(),13));
 assert(p.place(1,{{4,7,8,9,10,11,12}})); r=next(p);
 assert(r[0]==0x56 && r[2]==1 && r[4]==1 && r[5]==0); // centre, separate token index
 assert(p.place(5,profile.uid));
 r=next(p); assert(r[0]==0x56 && r[2]==2 && r[4]==0 && r[5]==1); // remove previous duplicate UID
 r=next(p); assert(r[0]==0x56 && r[2]==3 && r[4]==0 && r[5]==0); // right pad
 assert(!p.occupied(0) && p.occupied(5));
 assert(p.remove(5)); assert(!p.occupied(5)); r=next(p); assert(r[0]==0x56 && r[2]==3 && r[5]==1);

 // Read/write commands use the active token index and four-byte tag pages.
 assert(p.placeProfile(0,profile)); (void)next(p);
 const uint8_t readPayload[2]={0,0};
 request(p,0xd2,0x31,readPayload,sizeof(readPayload)); r=next(p);
 assert(r[0]==0x55 && r[1]==18 && r[2]==0x31 && r[3]==0);
 assert(r[4]==0xa1 && r[5]==0xb2 && r[6]==0xc3 && r[7]==0xd4);
 const uint8_t writePayload[6]={0,36,0x78,0x56,0x34,0x12};
 request(p,0xd3,0x32,writePayload,sizeof(writePayload)); r=next(p);
 assert(r[0]==0x55 && r[2]==0x32 && r[3]==0);
 TagProfile changed{}; assert(p.takeDirty(0,changed));
 assert(changed.memory[144]==0x78 && changed.memory[145]==0x56 && changed.memory[146]==0x34 && changed.memory[147]==0x12);
 assert(!p.takeDirty(0,changed));

 // The encrypted model query must identify a character and echo its configuration.
 uint8_t modelPayload[8]={0,0,0,0,0x12,0x34,0x56,0x78}; testTea(modelPayload,true);
 request(p,0xd4,0x33,modelPayload,sizeof(modelPayload)); r=next(p);
 assert(r[0]==0x55 && r[1]==10 && r[2]==0x33 && r[3]==0);
 uint8_t modelResponse[8]; std::memcpy(modelResponse,r.data()+4,8); testTea(modelResponse,false);
 assert(readLE(modelResponse)==1 && modelResponse[4]==0x12 && modelResponse[5]==0x34 && modelResponse[6]==0x56 && modelResponse[7]==0x78);

 // Quick-swap preserves the complete in-memory tag and moves it to the other pad.
 const auto movedUid = p.uid(0);
 assert(p.move(0,2));
 assert(!p.occupied(0) && p.occupied(2) && p.uid(2)==movedUid);
 r=next(p); assert(r[0]==0x56 && r[5]==1); // source removal event
 r=next(p); assert(r[0]==0x56 && r[5]==0); // destination placement event
 assert(!p.move(2,1)); // occupied destination is not allowed

 SetupPacket sp{}; sp.bmRequestType=0x81; sp.bRequest=0x06; sp.wValue=0x2200; sp.wIndex=0; sp.wLength=64;
 auto action=p.onSetup(sp); assert(action.kind==ControlAction::Kind::Send && action.data.size()==sky::lego::kReportDescriptorSize);
 p.onBusReset(); assert(!p.peekInput(r)); assert(!p.hostActive());
 initializeHost(p); assert(p.hostActive());
 r=next(p); assert(r[0]==0x55 && r[2]==0x01); // wake reply first
 r=next(p); assert(r[0]==0x56 && r[5]==0); // then the tags already on the pad
 std::puts("LEGO Dimensions backend tests: passed");
}
