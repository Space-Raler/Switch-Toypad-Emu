#include "lego_dimensions_function.h"
#include <algorithm>
#include <cstring>

namespace sky::lego {
const uint8_t kReportDescriptor[kReportDescriptorSize] = {
  0x06,0x00,0xFF, 0x09,0x01, 0xA1,0x01,
  0x19,0x01,0x29,0x20, 0x15,0x00, 0x26,0xFF,0x00,
  0x75,0x08, 0x95,0x20, 0x81,0x00,
  0x19,0x01,0x29,0x20, 0x91,0x00, 0xC0
};
namespace {
constexpr uint8_t kTypeMask=0x60, kStandard=0x00, kClass=0x20, kRecipMask=0x1F, kInterface=0x01;
constexpr uint8_t kGetStatus=0x00, kGetDescriptor=0x06, kGetInterface=0x0A, kSetInterface=0x0B;
constexpr uint8_t kGetIdle=0x02, kGetProtocol=0x03, kSetReport=0x09, kSetIdle=0x0A, kSetProtocol=0x0B;
constexpr uint8_t kCmdWake=0xB0, kCmdSeed=0xB1, kCmdChallenge=0xB3, kCmdRead=0xD2, kCmdWrite=0xD3, kCmdModel=0xD4;
constexpr uint32_t kTeaDelta=0x9E3779B9u;
constexpr uint8_t kTeaKey[16]={0x55,0xFE,0xF6,0xB0,0x62,0xBF,0x0B,0x41,0xC9,0xB3,0x7C,0xB4,0x97,0x3E,0x29,0x7B};
struct Burtle { uint32_t a=0, b=0, c=0, d=0; };
Burtle burtle_{};
std::vector<uint8_t> hidDesc(){ return {9,0x21,0x11,0x01,0,1,0x22,uint8_t(kReportDescriptorSize),0}; }
std::vector<uint8_t> ep(uint8_t a,uint8_t i){return {7,5,a,3,32,0,i};}
ControlAction send(const std::vector<uint8_t>& v,uint16_t n){ControlAction a;a.kind=ControlAction::Kind::Send;a.data=v;if(a.data.size()>n)a.data.resize(n);return a;}
ControlAction ack(){ControlAction a;a.kind=ControlAction::Kind::Ack;return a;}
uint32_t readLE32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
uint32_t readBE32(const uint8_t* p){return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|uint32_t(p[3]);}
void writeLE32(uint8_t* p,uint32_t v){p[0]=uint8_t(v);p[1]=uint8_t(v>>8);p[2]=uint8_t(v>>16);p[3]=uint8_t(v>>24);}
void writeBE32(uint8_t* p,uint32_t v){p[0]=uint8_t(v>>24);p[1]=uint8_t(v>>16);p[2]=uint8_t(v>>8);p[3]=uint8_t(v);}
void tea(uint8_t* block,bool encrypt){
 uint32_t v0=readLE32(block),v1=readLE32(block+4),k[4];
 for(int i=0;i<4;++i)k[i]=readLE32(kTeaKey+i*4);
 if(encrypt){uint32_t sum=0;for(int i=0;i<32;++i){sum+=kTeaDelta;v0+=(((v1<<4)+k[0])^(v1+sum)^((v1>>5)+k[1]));v1+=(((v0<<4)+k[2])^(v0+sum)^((v0>>5)+k[3]));}}
 else {uint32_t sum=0xC6EF3720u;for(int i=0;i<32;++i){v1-=(((v0<<4)+k[2])^(v0+sum)^((v0>>5)+k[3]));v0-=(((v1<<4)+k[0])^(v1+sum)^((v1>>5)+k[1]));sum-=kTeaDelta;}}
 writeLE32(block,v0);writeLE32(block+4,v1);
}
uint32_t rot(uint32_t v,unsigned n){return (v<<n)|(v>>(32-n));}
void seedBurtle(uint32_t seed){burtle_.a=0xf1ea5eedu;burtle_.b=burtle_.c=burtle_.d=seed;for(int i=0;i<42;++i){uint32_t e=burtle_.a-rot(burtle_.b,21);burtle_.a=burtle_.b^rot(burtle_.c,19);burtle_.b=burtle_.c+rot(burtle_.d,6);burtle_.c=burtle_.d+e;burtle_.d=e+burtle_.a;}}
uint32_t nextBurtle(){uint32_t e=burtle_.a-rot(burtle_.b,21);burtle_.a=burtle_.b^rot(burtle_.c,19);burtle_.b=burtle_.c+rot(burtle_.d,6);burtle_.c=burtle_.d+e;burtle_.d=e+burtle_.a;return burtle_.d;}
uint8_t logicalPad(size_t p){if(p==1)return 1;if(p==0||p==3||p==4)return 2;return 3;}
uint8_t allocateTokenIndex(const std::array<bool,kPositionCount>& occupied,const std::array<uint8_t,kPositionCount>& indices){
 for(uint8_t candidate=0;candidate<kPositionCount;++candidate){bool used=false;for(size_t i=0;i<kPositionCount;++i)if(occupied[i]&&indices[i]==candidate){used=true;break;}if(!used)return candidate;}return 0xff;
}
}
std::vector<std::vector<uint8_t>> interfaceDescriptors(uint8_t n, Speed s,uint8_t interval,uint8_t outOverride){
 const uint8_t iv=interval?interval:(s==Speed::Full?1:4); std::vector<std::vector<uint8_t>> v;
 v.push_back({9,4,n,0,2,3,0,0,0}); v.push_back(hidDesc());
 const std::vector<uint8_t> comp={6,0x30,0,0,32,0};
 v.push_back(ep(endpointInAddress(n),iv)); if(s==Speed::Super)v.push_back(comp);
 v.push_back(ep(outOverride?outOverride:endpointOutAddress(n),iv)); if(s==Speed::Super)v.push_back(comp);
 return v;
}
ToyPadFunction::ToyPadFunction(){ onBusReset(); }
void ToyPadFunction::queue(const Report& r){ if(out_.size()<64)out_.push_back(r); }
void ToyPadFunction::queueResponse(uint8_t cid,const uint8_t* payload,size_t len){
 if(len>28)return;
 Report r{};r[0]=0x55;r[1]=uint8_t(len+1);r[2]=cid;if(len&&payload)std::copy(payload,payload+len,r.begin()+3);
 r[2+r[1]]=checksum(r.data(),2+r[1]);queue(r);
}
ControlAction ToyPadFunction::onSetup(const SetupPacket& sp){
 pendingSetReport_=false; if((sp.bmRequestType&kRecipMask)!=kInterface)return {};
 const uint8_t type=sp.bmRequestType&kTypeMask;
 if(type==kStandard){
  if(sp.bRequest==kGetDescriptor&&sp.deviceToHost()){
   const uint8_t dt=uint8_t(sp.wValue>>8);
   if(dt==0x22)return send(std::vector<uint8_t>(kReportDescriptor,kReportDescriptor+kReportDescriptorSize),sp.wLength);
   if(dt==0x21)return send(hidDesc(),sp.wLength);
  }
  if(sp.bRequest==kGetStatus&&sp.deviceToHost())return send({0,0},sp.wLength);
  if(sp.bRequest==kGetInterface&&sp.deviceToHost())return send({0},sp.wLength);
  if(sp.bRequest==kSetInterface&&!sp.deviceToHost()&&sp.wValue==0)return ack();
 }
 if(type==kClass){
  switch(sp.bRequest){case kSetReport:{if(!sp.wLength)return ack();ControlAction a;a.kind=ControlAction::Kind::Receive;a.rxLen=std::min<size_t>(sp.wLength,0x1000);pendingSetReport_=true;return a;}
   case kGetIdle:return send({0},sp.wLength);case kGetProtocol:return send({1},sp.wLength);case kSetIdle:case kSetProtocol:return ack();default:break;}
 }
 return {};
}
void ToyPadFunction::onControlOut(const uint8_t* d,size_t n){if(!pendingSetReport_)return;pendingSetReport_=false;if(n==33&&d[0]==0){++d;--n;}handleCommand(d,n);}
void ToyPadFunction::onInterruptOut(const uint8_t* d,size_t n){handleCommand(d,n);}
void ToyPadFunction::handleCommand(const uint8_t* d,size_t n){
 if(!d||n<3)return;
 // The game sends command frames on HID OUT. In particular, the initial
 // 0x55 0x0f 0xb0 wake request needs a framed response on HID IN. Do not
 // mistake it for an initialization marker or require it to use the same
 // message type as the tag reports sent in the opposite direction.
 if(d[0]==0x55||d[0]==0x56)handleFrame(d,n);
}
void ToyPadFunction::handleFrame(const uint8_t* d,size_t n){
 const size_t payloadLen=d[1];if(payloadLen<2||payloadLen>28||n<payloadLen+3)return;
 if(checksum(d,payloadLen+2)!=d[payloadLen+2])return;
 const uint8_t cmd=d[2],cid=d[3];const uint8_t* p=d+4;const size_t plen=payloadLen-2;
 uint8_t out[28]{};size_t outLen=0;
 switch(cmd){
  case kCmdWake:{const uint8_t wake[13]={0x28,0x63,0x29,0x20,0x4c,0x45,0x47,0x4f,0x20,0x32,0x30,0x31,0x34};std::copy(std::begin(wake),std::end(wake),out);outLen=sizeof(wake);break;}
  case kCmdRead:{
   if(plen<2) return;
   const uint8_t index=p[0],page=p[1]; out[0]=0;
   const size_t pos=page*4u;for(size_t i=0;i<kPositionCount;++i)if(occupied_[i]&&tokenIndices_[i]==index){if(pos<profiles_[i].memory.size()){const size_t count=std::min<size_t>(16,profiles_[i].memory.size()-pos);std::copy(profiles_[i].memory.begin()+pos,profiles_[i].memory.begin()+pos+count,out+1);}break;}
   outLen=17;break;
  }
  case kCmdWrite:{
   if(plen<6) return;
   const uint8_t index=p[0],page=p[1]; const size_t pos=page*4u;
   if(page<45){for(size_t i=0;i<kPositionCount;++i)if(occupied_[i]&&tokenIndices_[i]==index){std::copy(p+2,p+6,profiles_[i].memory.begin()+pos);dirty_[i]=true;break;}}
   out[0]=0;outLen=1;break;
  }
  case kCmdModel:{
   if(plen<8) return;
   uint8_t block[8]; std::copy(p,p+8,block); tea(block,false); const uint8_t index=block[0]; const uint32_t conf=readBE32(block+4);
   for(size_t i=0;i<kPositionCount;++i)if(occupied_[i]&&tokenIndices_[i]==index){
    if(profiles_[i].vehicle){out[0]=0xf9;outLen=1;}
    else if(profiles_[i].figureId){uint8_t model[8]{}; writeLE32(model,profiles_[i].figureId); writeBE32(model+4,conf); tea(model,true); out[0]=0; std::copy(model,model+8,out+1); outLen=9;}
    else {out[0]=0xf2;outLen=1;}break;
   }
   if(!outLen){out[0]=0xf2;outLen=1;}break;
  }
  case kCmdSeed:{
   if(plen<8) return;
   uint8_t block[8]; std::copy(p,p+8,block); tea(block,false); seedBurtle(readLE32(block)); const uint32_t conf=readBE32(block+4); std::fill(out,out+8,0); writeBE32(out,conf); tea(out,true); outLen=8; break;
  }
  case kCmdChallenge:{
   if(plen<8) return;
   uint8_t block[8]; std::copy(p,p+8,block); tea(block,false); const uint32_t conf=readBE32(block); std::fill(out,out+8,0); writeLE32(out,nextBurtle()); writeBE32(out+4,conf); tea(out,true); outLen=8; break;
  }
  case 0xc0: case 0xc1: case 0xc2: case 0xc3: case 0xc4: case 0xc6: case 0xc7: case 0xc8: case 0xd0: case 0xe1: case 0xe5: case 0xff:
   // Supported LED/status commands return an empty ACK response.
   outLen=0;break;
  default:return;
 }
 queueResponse(cid,out,outLen);
 if(cmd==kCmdWake&&!hostInitialized_){
  // Queue the wake reply first, then announce tags already on the virtual pad.
  hostInitialized_=true;
  queuePresentTags();
 }
}
bool ToyPadFunction::place(size_t p,const std::array<uint8_t,7>& id){TagProfile profile{};profile.uid=id;return placeProfile(p,profile);}
bool ToyPadFunction::placeProfile(size_t p,const TagProfile& profile){
 if(p>=kPositionCount)return false;
 for(size_t i=0;i<kPositionCount;++i)if(i!=p&&occupied_[i]&&uids_[i]==profile.uid)remove(i);
 if(occupied_[p]&&uids_[p]==profile.uid&&profiles_[p].figureId==profile.figureId&&profiles_[p].vehicle==profile.vehicle)return true;
 if(occupied_[p])remove(p);
 const uint8_t index=allocateTokenIndex(occupied_,tokenIndices_);if(index==0xff)return false;
 profiles_[p]=profile;uids_[p]=profile.uid;occupied_[p]=true;tokenIndices_[p]=index;dirty_[p]=false;
 if(hostInitialized_) queue(tokenReport(static_cast<Pad>(logicalPad(p)),index,true,profile.uid));
 return true;
}
bool ToyPadFunction::remove(size_t p){
 if(p>=kPositionCount||!occupied_[p])return false;
 if(hostInitialized_) queue(tokenReport(static_cast<Pad>(logicalPad(p)),tokenIndices_[p],false,uids_[p]));
 occupied_[p]=false;uids_[p].fill(0);profiles_[p]=TagProfile{};tokenIndices_[p]=0;dirty_[p]=false;return true;
}
bool ToyPadFunction::move(size_t from,size_t to){
 if(from>=kPositionCount||to>=kPositionCount||from==to||!occupied_[from]||occupied_[to])return false;
 TagProfile profile=profiles_[from];
 const bool wasDirty=dirty_[from];
 remove(from);
 if(!placeProfile(to,profile))return false;
 dirty_[to]=wasDirty;
 return true;
}
bool ToyPadFunction::takeDirty(size_t p,TagProfile& profile){if(p>=kPositionCount||!occupied_[p]||!dirty_[p])return false;profile=profiles_[p];dirty_[p]=false;return true;}
void ToyPadFunction::markDirty(size_t p){if(p<kPositionCount&&occupied_[p])dirty_[p]=true;}
bool ToyPadFunction::occupied(size_t p)const{return p<kPositionCount&&occupied_[p];}
bool ToyPadFunction::dirty(size_t p)const{return p<kPositionCount&&dirty_[p];}
const std::array<uint8_t,7>& ToyPadFunction::uid(size_t p)const{static const std::array<uint8_t,7> empty{};return p<kPositionCount?uids_[p]:empty;}
bool ToyPadFunction::peekInput(Report& r)const{if(out_.empty())return false;r=out_.front();return true;}
void ToyPadFunction::dropInput(){if(!out_.empty())out_.pop_front();}
void ToyPadFunction::queuePresentTags(){for(size_t i=0;i<kPositionCount;++i)if(occupied_[i])queue(tokenReport(static_cast<Pad>(logicalPad(i)),tokenIndices_[i],true,uids_[i]));}
void ToyPadFunction::onBusReset(){out_.clear();pendingSetReport_=false;hostInitialized_=false;}
} // namespace sky::lego
