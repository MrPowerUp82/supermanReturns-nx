#include "sr_container.h"
#include <cassert>
#include <cstdio>
static void Put(std::vector<uint8_t>& b,size_t at,uint32_t value) {
  for(size_t i=0;i<4;++i) b[at+i]=uint8_t(value>>(24-i*8));
}
int main() {
  std::vector<uint8_t> container(96);
  Put(container,0,0x102a1101); Put(container,4,72); Put(container,8,24);
  Put(container,24,36); Put(container,36,0); Put(container,40,24);
  Put(container,72,0x1001); Put(container,76,0x2000); // EXECE, one ALU at block 1.
  const auto flow=sr::Validate(container);
  assert(flow.instrucciones==1 && flow.bytes==12);
  Put(container,16,71);
  bool rejected=false;
  try { (void)sr::Validate(container); } catch(const std::runtime_error&) { rejected=true; }
  assert(rejected);
  std::puts("containers: absent CTAB accepted; nonzero invalid CTAB rejected");
}
