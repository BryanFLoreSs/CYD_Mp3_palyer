#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "mp3_stream.h"
#include <string.h>

void Mp3Stream::reset(Reader reader, void *opaque) {
  mp3dec_init(&decoder); read=reader; context=opaque;
  buffered=0; samples=cursor=channels=rate=0; phase=consumedBytes=0;
  primed=finished=false;
}
bool Mp3Stream::source(int16_t &left, int16_t &right) {
  while(cursor>=samples) {
    buffered+=read(context,compressed+buffered,sizeof(compressed)-buffered);
    if(!buffered) return false;
    mp3dec_frame_info_t info{};
    samples=mp3dec_decode_frame(&decoder,compressed,int(buffered),decoded,&info);
    size_t used=info.frame_bytes;
    if(!used) { consumedBytes+=buffered; buffered=0; continue; }
    consumedBytes+=used; buffered-=used;
    memmove(compressed,compressed+used,buffered);
    cursor=0;
    if(!samples) continue;
    if(info.hz<8000 || info.hz>48000 || (info.channels!=1 && info.channels!=2))return false;
    channels=info.channels; rate=info.hz;
  }
  left=decoded[cursor*channels]; right=channels==1?left:decoded[cursor*channels+1];
  cursor++; return true;
}
bool Mp3Stream::next(int16_t &left, int16_t &right) {
  if(finished) return false;
  if(!primed) {
    if(!source(previousL,previousR)||!source(currentL,currentR)){finished=true;return false;}
    primed=true;
  }
  while(phase>=44100) {
    previousL=currentL;previousR=currentR;phase-=44100;
    if(!source(currentL,currentR)){finished=true;return false;}
  }
  left=previousL+(int64_t(currentL-previousL)*phase)/44100;
  right=previousR+(int64_t(currentR-previousR)*phase)/44100;
  phase+=rate;
  return true;
}
