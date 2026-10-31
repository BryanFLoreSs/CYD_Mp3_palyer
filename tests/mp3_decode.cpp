#include "mp3_stream.h"
#include <cstdio>
#include <cassert>
size_t readFile(void *f,uint8_t *b,size_t n){return fread(b,1,n,static_cast<FILE*>(f));}
int main(int argc,char **argv){
  assert(argc==3); FILE *in=fopen(argv[1],"rb"),*out=fopen(argv[2],"wb");assert(in&&out);
  Mp3Stream stream;stream.reset(readFile,in);
  int16_t l,r;unsigned frames=0;
  while(stream.next(l,r)){fwrite(&l,2,1,out);fwrite(&r,2,1,out);assert(++frames<44100*3600);}
  assert(frames>44100/2);fclose(in);fclose(out);
  printf("Decoded %u stereo frames at 44100 Hz\n",frames);
  // Reset must discard both the compressed reservoir and resampler state.
  in=fopen(argv[1],"rb");stream.reset(readFile,in);unsigned again=0;
  while(stream.next(l,r))again++;
  assert(again==frames);fclose(in);
}
