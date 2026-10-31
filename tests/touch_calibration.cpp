#include "touch_calibration.h"
#include <cassert>
#include <initializer_list>
int main(){
 TouchPoint screen[]={{24,24},{296,24},{24,216}};
 for(int swap=0;swap<2;swap++)for(int flipX=0;flipX<2;flipX++)for(int flipY=0;flipY<2;flipY++){
  auto raw=[&](TouchPoint p){float x=200+p.x*10,y=250+p.y*14;if(flipX)x=4095-x;if(flipY)y=4095-y;return swap?TouchPoint{y,x}:TouchPoint{x,y};};
  TouchPoint points[]={raw(screen[0]),raw(screen[1]),raw(screen[2])};
  TouchCalibration mapping;assert(mapping.fit(points,screen));
  for(auto p:{TouchPoint{160,120},TouchPoint{16,220},TouchPoint{300,40}}){auto r=raw(p);auto result=mapping.map(r.x,r.y);assert(std::fabs(result.x-p.x)<.01&&std::fabs(result.y-p.y)<.01);}
 }
 TouchPoint invalid[]={{100,100},{100,100},{100,100}};TouchCalibration bad;assert(!bad.fit(invalid,screen));
}
