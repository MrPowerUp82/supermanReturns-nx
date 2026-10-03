#include "sr_native_mirror.h"
#include "sr_native_ring.h"
#include <algorithm>
#include <limits>

namespace sr::native {
namespace {
constexpr uint32_t kPhysicalLimit = 0x20000000;
uint32_t ConstantBase(uint32_t value) {
  constexpr uint32_t bases[] = {0x4000,0x4800,0x4900,0x4908,0x2000};
  const uint32_t type = (value >> 16) & 0xff;
  return type < 5 ? bases[type] : UINT32_MAX;
}
uint32_t BE(std::span<const std::byte> bytes, size_t offset) {
  uint32_t value = 0;
  for (size_t i=0;i<4;++i) value=(value<<8)|std::to_integer<uint32_t>(bytes[offset+i]);
  return value;
}
bool PhysicalRange(uint32_t address, uint64_t bytes) {
  return !(address & 3) && address < kPhysicalLimit && bytes <= kPhysicalLimit-address;
}
}
void StateMirror::ResetSegment(uint64_t epoch) {
  epoch_=epoch; has_last_=false; last_bytes_.clear(); stamps_.clear();
}
bool StateMirror::Write(uint32_t index,uint32_t value) {
  if (index >= kRegisterCount) return false;
  registers_[index]=value; written_[index]=true;
  if (index>=0x4000 && index<0x4400) ++vs_version_;
  if (index>=0x4400 && index<0x4800) ++ps_version_;
  return true;
}
NativeResult StateMirror::Scan(const GuestMemory& memory,std::span<const std::byte> bytes,PacketSite site) {
  if (site.allocation_epoch!=epoch_ || !PhysicalRange(site.physical_address,bytes.size()) ||
      bytes.size()%4 || bytes.size()>4*1024*1024) return NativeResult::kInvalid;
  if (has_last_ && site==last_site_ && std::equal(bytes.begin(),bytes.end(),last_bytes_.begin(),last_bytes_.end())) {
    stamps_.clear(); return NativeResult::kComplete;
  }
  if (has_last_ && site.physical_address<next_address_) return NativeResult::kInvalid;
  // Transactional validation: an invalid tail or indirect never publishes earlier writes.
  StateMirror candidate=*this;
  candidate.stamps_.clear();
  std::vector<uint32_t> indirects;
  uint64_t budget=1024*1024;
  const auto result=candidate.ScanImpl(memory,bytes,site,indirects,budget);
  if (result!=NativeResult::kComplete) return result;
  candidate.last_site_=site; candidate.last_bytes_.assign(bytes.begin(),bytes.end());
  candidate.has_last_=true; candidate.next_address_=site.physical_address+uint32_t(bytes.size());
  *this=std::move(candidate);
  return NativeResult::kComplete;
}
NativeResult StateMirror::ScanImpl(const GuestMemory& memory,std::span<const std::byte> bytes,
                                  PacketSite site,std::vector<uint32_t>& indirects,uint64_t& budget) {
  Cursor cursor{bytes,0,uint32_t(bytes.size()/4),0};
  while (cursor.position<cursor.end) {
    PacketView packet;
    if (PeekPacket(cursor,packet)!=PacketResult::kConsumed) return NativeResult::kInvalid;
    const uint64_t size=1+packet.payload.size();
    if (size>budget) return NativeResult::kInvalid;
    budget-=size;
    stamps_.push_back({{site.allocation_epoch,site.physical_address+cursor.position*4},packet.header,packet.payload});
    const auto& p=packet.payload;
    const auto type=packet.header>>30;
    if (packet.header==0 || type==2) { CommitPacket(cursor,packet); continue; }
    if (type==0) {
      const uint32_t start=packet.header&0x7fff;
      for (uint32_t i=0;i<p.size();++i)
        if (!Write(start+((packet.header&0x8000)?0:i),p[i])) return NativeResult::kInvalid;
    } else if (type==1) {
      if (!Write(packet.header&0x7ff,p[0]) || !Write((packet.header>>11)&0x7ff,p[1])) return NativeResult::kInvalid;
    } else {
      const auto op=(packet.header>>8)&0x7f;
      switch(op) {
        case 0x2d: case 0x55: case 0x56: {
          const uint32_t base=op==0x2d?ConstantBase(p[0]):0;
          if (base==UINT32_MAX) return NativeResult::kInvalid;
          const uint32_t first=base+(p[0]&(op==0x2d?0x7ff:0xffff));
          for(uint32_t i=1;i<p.size();++i) if(!Write(first+i-1,p[i])) return NativeResult::kInvalid;
          break;
        }
        case 0x2f: {
          if(p.size()<3) return NativeResult::kInvalid;
          const auto address=p[0]&0x3fffffff, count=p[2]&0xfff, base=ConstantBase(p[1]);
          if(base==UINT32_MAX || !PhysicalRange(address,uint64_t(count)*4)) return NativeResult::kInvalid;
          std::vector<std::byte> data;
          if(count && !memory.CopyPhysical(address,count*4,data)) return NativeResult::kInvalid;
          for(uint32_t i=0;i<count;++i) if(!Write(base+(p[1]&0x7ff)+i,BE(data,i*4))) return NativeResult::kInvalid;
          break;
        }
        case 0x3f: case 0x37: {
          if(p.size()<2 || indirects.size()>=4) return NativeResult::kInvalid;
          const auto address=p[0], count=p[1]&0xfffff;
          if(!PhysicalRange(address,uint64_t(count)*4) || count>budget ||
             std::find(indirects.begin(),indirects.end(),address)!=indirects.end()) return NativeResult::kInvalid;
          std::vector<std::byte> data;
          if(!memory.CopyPhysical(address,count*4,data)) return NativeResult::kInvalid;
          indirects.push_back(address);
          const auto result=ScanImpl(memory,data,{site.allocation_epoch,address},indirects,budget);
          indirects.pop_back();
          if(result!=NativeResult::kComplete) return result;
          break;
        }
        case 0x27: { // IM_LOAD
          if(p.size()<2 || (p[0]&3)>1 || p[1]>>16) return NativeResult::kInvalid;
          const uint32_t count=p[1], address=p[0]&~3u;
          if(!PhysicalRange(address,uint64_t(count)*4)) return NativeResult::kInvalid;
          std::vector<std::byte> data;
          if(count && !memory.CopyPhysical(address,count*4,data)) return NativeResult::kInvalid;
          auto& code=code_[p[0]&3]; code.resize(count);
          for(uint32_t i=0;i<count;++i) code[i]=BE(data,i*4);
          break;
        }
        case 0x2b: { // IM_LOAD_IMMEDIATE
          if(p.size()<2 || p[0]>1 || p[1]>>16 || p[1]>p.size()-2) return NativeResult::kInvalid;
          code_[p[0]].assign(p.begin()+2,p.begin()+2+p[1]); break;
        }
        // Observable GPU/memory effects belong to RingExecutor, not this read-only observer.
        case 0x10: case 0x48: case 0x3b: case 0x54: case 0x3c: case 0x3e:
        case 0x3d: case 0x46: case 0x58: case 0x5a: case 0x5b: case 0x5c:
        case 0x22: case 0x36: case 0x64: case 0x50: case 0x51: case 0x60:
        case 0x61: case 0x62: case 0x63: case 0x5d: case 0x26: case 0x23: break;
        default: return NativeResult::kUnsupported;
      }
    }
    CommitPacket(cursor,packet);
  }
  return NativeResult::kComplete;
}
}  // namespace sr::native
