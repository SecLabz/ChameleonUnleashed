#include "rc522_tx.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    uint8_t data[16], out[367];
    for (unsigned length=1;length<=16;length++) {
        for (unsigned value=0;value<256;value++) {
            for (unsigned i=0;i<length;i++) data[i]=value^(i*37);
            memset(out,0xa5,sizeof(out));
            unsigned n=rc522_tx_encode(data,length,8,out,366);
            assert(n==23+length*10 && out[2*n]==0xa5);
            for (unsigned symbol=0;symbol<n;symbol++) {
                bool high;
                if (symbol<12) high=symbol>=10;
                else if(symbol<12+length*10) {
                    unsigned pos=(symbol-12)%10, byte=(symbol-12)/10;
                    high=pos==9 || (pos>0 && (data[byte]&(1u<<(pos-1))));
                } else high=symbol==n-1;
                assert(out[2*symbol]==0x50 && out[2*symbol+1]==(high?32:8));
            }
        }
    }
    memset(out,0xa5,sizeof(out));
    assert(!rc522_tx_encode(data,16,8,out,365) && out[0]==0xa5);
    assert(!rc522_tx_encode(data,17,8,out,sizeof(out)));
    assert(!rc522_tx_encode(data,0,8,out,sizeof(out)));
    assert(!rc522_tx_encode(NULL,1,8,out,sizeof(out)));
    assert(!rc522_tx_encode(data,1,8,NULL,sizeof(out)));
    puts("Type-B TX: all bytes, frame lengths, SOF/EOF, capacity and bounds passed");
}
