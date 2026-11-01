#include "audio_transport.h"
#include <cassert>
using Command=AudioTransport::Command;
void start(AudioTransport &t,uint32_t now){
 assert(t.poll(now)==Command::Check);t.ack(Command::Check,true,now+1);
 assert(t.poll(now+1)==Command::Start);t.audio(true);t.ack(Command::Start,true,now+2);
 assert(t.poll(now+3)==Command::None);
}
int main(){
 AudioTransport t;t.want(true);assert(t.poll(0)==Command::None);t.connection(true);start(t,10);
 for(unsigned n=0;n<5;n++){
  uint32_t now=100+n*100;
  t.want(false);assert(t.poll(now)==Command::Suspend);
  t.audio(false);t.ack(Command::Suspend,true,now+1);assert(t.poll(now+2)==Command::None);
  t.want(true);start(t,now+10);
 }
 // Resume arriving before the suspend acknowledgement must survive it.
 t.want(false);assert(t.poll(1000)==Command::Suspend);t.want(true);
 assert(t.poll(1001)==Command::None);t.ack(Command::Suspend,true,1002);start(t,1003);
 // Failed commands retry; missing acknowledgements time out.
 t.connection(true);assert(t.poll(2000)==Command::Check);t.ack(Command::Check,false,2001);
 assert(t.poll(2200)==Command::None);assert(t.poll(2301)==Command::Check);
 assert(t.poll(4802)==Command::Check);
 // Pause during readiness checking must not start the stream.
 t.want(false);t.ack(Command::Check,true,4803);assert(t.poll(4804)==Command::None);
 t.connection(false);t.want(true);assert(t.poll(6000)==Command::None);
 // A late suspend notification must not cancel an in-flight resume.
 AudioTransport late;late.connection(true);late.want(true);start(late,10);
 late.want(false);assert(late.poll(100)==Command::Suspend);
 late.want(true);late.ack(Command::Suspend,true,101);
 assert(late.poll(102)==Command::Check);
 late.audio(false);assert(late.poll(103)==Command::None);
 late.ack(Command::Check,true,104);assert(late.poll(105)==Command::Start);
 late.audio(false);assert(late.poll(106)==Command::None);
 late.ack(Command::Start,true,107);late.audio(true);assert(late.poll(108)==Command::None);
 // A late start notification must not send SUSPEND twice.
 late.want(false);assert(late.poll(200)==Command::Suspend);
 late.audio(true);assert(late.poll(201)==Command::None);
 late.ack(Command::Suspend,true,202);assert(late.poll(203)==Command::None);
}
