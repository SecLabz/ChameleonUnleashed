#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#define BYTE_SWAP(x) (((uint8_t)(x)>>4)|((uint8_t)(x)<<4))
#define NO_ACCESS 0x07
static struct { uint8_t acs[3]; } trailer, *m_tag_trailer_info = &trailer;
/* ACCESS CONDITION FUNCTION */

int main(void) {
    unsigned errors=0, checks=0;
    uint8_t groups[256]; unsigned block=0;
    for (unsigned sector=0; sector<32; sector++)
        for (unsigned group=0; group<4; group++) groups[block++]=group;
    for (unsigned sector=32; sector<40; sector++) {
        for (unsigned group=0; group<3; group++)
            for (unsigned member=0; member<5; member++) groups[block++]=group;
        groups[block++]=3;
    }
    assert(block==256);
    for (unsigned configuration=0; configuration<4096; configuration++) {
        uint8_t conditions[4], c1=0, c2=0, c3=0;
        for (unsigned group=0; group<4; group++) {
            conditions[group]=(configuration>>(3*group))&7;
            c1|=(conditions[group]&1)<<group;
            c2|=((conditions[group]>>1)&1)<<group;
            c3|=((conditions[group]>>2)&1)<<group;
        }
        trailer.acs[0]=((~c2&15)<<4)|(~c1&15);
        trailer.acs[1]=(c1<<4)|(~c3&15);
        trailer.acs[2]=(c3<<4)|c2;
        for (block=0; block<256; block++) {
            uint8_t actual=GetAccessCondition(block);
            if (actual!=conditions[groups[block]]) errors++;
            checks++;
        }
        for (unsigned bit=0; bit<24; bit++) {
            trailer.acs[bit/8]^=1u<<(bit%8);
            assert(GetAccessCondition(128)==NO_ACCESS);
            trailer.acs[bit/8]^=1u<<(bit%8);
        }
    }
    printf("%u valid access-condition checks; %u mismatches; invalid encodings rejected\n",checks,errors);
    return errors?1:0;
}
