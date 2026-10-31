#pragma once
#include <stdint.h>

// Called only from the Bluetooth event task. UI requests are copied in by it.
class AudioTransport {
 public:
  enum class Command { None, Check, Start, Suspend };
  enum class Phase { Idle, Checking, Starting, Started, Suspending };
  void connection(bool value) {
    connected=value;phase=Phase::Idle;ready=false;retryAt=0;
  }
  void want(bool value){wanted=value;}
  void audio(bool started) {
    phase=started?Phase::Started:Phase::Idle;ready=false;
  }
  Command poll(uint32_t now) {
    if(!connected)return Command::None;
    if(phase==Phase::Checking || phase==Phase::Starting || phase==Phase::Suspending) {
      if(uint32_t(now-pendingAt)<2500)return Command::None;
      phase=phase==Phase::Suspending?Phase::Started:Phase::Idle;ready=false;
    }
    if(int32_t(now-retryAt)<0)return Command::None;
    if(wanted && phase==Phase::Idle) {
      pendingAt=now;
      if(ready){phase=Phase::Starting;return Command::Start;}
      phase=Phase::Checking;return Command::Check;
    }
    if(!wanted && phase==Phase::Started) {
      phase=Phase::Suspending;pendingAt=now;return Command::Suspend;
    }
    return Command::None;
  }
  void ack(Command command,bool success,uint32_t now) {
    if(command==Command::Check && phase==Phase::Checking) {
      phase=Phase::Idle;ready=success;
    } else if(command==Command::Start && phase==Phase::Starting) {
      phase=success?Phase::Started:Phase::Idle;ready=false;
    } else if(command==Command::Suspend && phase==Phase::Suspending) {
      phase=success?Phase::Idle:Phase::Started;ready=false;
    } else return; // stale acknowledgements must not undo newer audio events
    retryAt=success?now:now+300;
  }
 private:
  bool wanted=false,connected=false,ready=false;
  Phase phase=Phase::Idle;
  uint32_t pendingAt=0,retryAt=0;
};
