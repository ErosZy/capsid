#include "host/ws_stream.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace ws=capsid::host::ws;
void require(bool value,const char *message) {if(!value){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
// Independent RFC wire builder, separate from the production encoder.
std::vector<uint8_t> frame(uint8_t opcode,const std::vector<uint8_t> &payload,bool fin=true) {
    std::vector<uint8_t> wire{uint8_t((fin?128:0)|opcode)};
    if(payload.size()<126) wire.push_back(uint8_t(128|payload.size()));
    else if(payload.size()<65536) {wire.push_back(254);wire.push_back(uint8_t(payload.size()>>8));wire.push_back(uint8_t(payload.size()));}
    else {wire.push_back(255);for(int shift=56;shift>=0;shift-=8)wire.push_back(uint8_t(uint64_t(payload.size())>>shift));}
    const uint8_t mask[4]={0x37,0xfa,0x21,0x3d};wire.insert(wire.end(),mask,mask+4);
    for(size_t i=0;i<payload.size();++i)wire.push_back(payload[i]^mask[i%4]);return wire;
}
std::vector<ws::StreamDecoder::Item> decode(ws::StreamDecoder &decoder,const std::vector<uint8_t> &wire,size_t chunk) {
    std::vector<ws::StreamDecoder::Item> items;
    size_t offset=0;
    while(offset<wire.size()) {
        const size_t count=std::min(chunk,wire.size()-offset);size_t used=0;ws::StreamDecoder::Item item;
        auto result=decoder.consume(wire.data()+offset,count,&used,65536,&item);
        require(result!=ws::StreamDecoder::Error,"legal RFC stream");
        require(used || result==ws::StreamDecoder::ItemReady,"decoder progress");offset+=used;
        if(result==ws::StreamDecoder::ItemReady)items.push_back(std::move(item));
    }
    return items;
}
void error(std::vector<uint8_t> wire,uint16_t expected,uint64_t maximum=1024*1024) {
    ws::StreamDecoder decoder(maximum);size_t offset=0;
    for(;;){size_t used=0;ws::StreamDecoder::Item item;auto result=decoder.consume(wire.data()+offset,wire.size()-offset,&used,65536,&item);offset+=used;
        if(result==ws::StreamDecoder::Error){require(decoder.error_code()==expected,"scoped RFC failure code");return;}
        require(offset<wire.size(),"invalid stream must be rejected");}
}
int main() {
    for(size_t chunk:{size_t(1),size_t(7),size_t(16384)}) {
        ws::StreamDecoder decoder(1024*1024);std::vector<uint8_t> payload(180000);
        for(size_t i=0;i<payload.size();++i)payload[i]=uint8_t(i*13);
        auto items=decode(decoder,frame(2,payload),chunk);std::vector<uint8_t> observed;
        for(size_t i=0;i<items.size();++i){require(items[i].payload.size()<=16384,"owned delivery chunk bound");require(items[i].start==(i==0)&&items[i].end==(i+1==items.size()),"one IPC start/end across big frame");observed.insert(observed.end(),items[i].payload.begin(),items[i].payload.end());}
        require(observed==payload,"masked 64-bit-length frame decoded byte for byte");
    }
    ws::StreamDecoder utf(1024);
    auto a=decode(utf,frame(1,{0xe7},false),1);auto ping=decode(utf,frame(9,{'p'}),1);
    auto b=decode(utf,frame(0,{0x95,0x8c}),1);
    require(a[0].start && !a[0].end && ping[0].opcode==9 && b.back().end && b.back().text,"UTF-8 spans frames with interleaved Ping");
    ws::StreamDecoder credit(1024);auto wire=frame(2,{1,2,3});size_t used=0;ws::StreamDecoder::Item item;
    require(credit.consume(wire.data(),wire.size(),&used,0,&item)==ws::StreamDecoder::NeedMore && used==6,"zero credit consumes header, allocates no payload");
    size_t offset=used;require(credit.consume(wire.data()+offset,wire.size()-offset,&used,1,&item)==ws::StreamDecoder::ItemReady&&item.payload==std::vector<uint8_t>{1}&&!item.end,"resume with bounded credit");
    // Malformed message text reaches the worker's lossy decoder unchanged,
    // including byte-at-a-time input. Close reasons still reject it below.
    for (const std::vector<uint8_t> payload : {
            std::vector<uint8_t>{0xff, 0xfe, 0xfd, 0x62}, {0xed, 0xa0, 0x80},
            {0xf4, 0x90, 0x80, 0x80}, {0xe2, 0x82}}) {
        ws::StreamDecoder malformed(1024);
        const auto parts = decode(malformed, frame(1, payload), 1);
        std::vector<uint8_t> observed;
        for (const auto &part : parts) {
            require(part.text, "malformed message retains text kind");
            observed.insert(observed.end(), part.payload.begin(), part.payload.end());
        }
        require(observed == payload && parts.front().start && parts.back().end,
                "malformed message bytes and boundaries reach the worker");
    }
    error(frame(0,{'x'}),1002);error(frame(8,{1}),1002);error(frame(8,{3,0xee}),1002);
    error(frame(8,{3,0xe8,0xff}),1007);error(frame(9,{},false),1002);error(frame(2,std::vector<uint8_t>(1025)),1009,1024);
    auto unmasked=frame(2,{1});unmasked[1]&=127;error(unmasked,1002);
    std::vector<uint8_t> server_wire{0x82,127,0,0,0,0,0,2,0xbf,0x20};
    std::vector<uint8_t> server_payload(180000);for(size_t i=0;i<server_payload.size();++i)server_payload[i]=uint8_t(i*17);
    server_wire.insert(server_wire.end(),server_payload.begin(),server_payload.end());
    ws::StreamDecoder client(1024*1024,ws::kClient);
    auto server_items=decode(client,server_wire,7);std::vector<uint8_t> server_observed;
    for(const auto &fragment:server_items)server_observed.insert(server_observed.end(),fragment.payload.begin(),fragment.payload.end());
    require(server_observed==server_payload,"outbound unmasked 64-bit-length stream preserves all bytes");
    ws::StreamDecoder invalid_server(1024,ws::kClient);auto masked=frame(1,{'x'});size_t rejected_bytes=0;ws::StreamDecoder::Item rejected;
    require(invalid_server.consume(masked.data(),masked.size(),&rejected_bytes,65536,&rejected)==ws::StreamDecoder::Error && invalid_server.error_code()==1002,"masked server frame is rejected before payload allocation");
    std::puts("incremental WebSocket: bounded chunks, mask, UTF-8, credit and Close PASS");
}
