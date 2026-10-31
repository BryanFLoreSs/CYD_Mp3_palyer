#pragma once
#include <cmath>
struct TouchPoint { float x,y; };
struct TouchCalibration {
  float a=0,b=0,c=0,d=0,e=0,f=0;
  TouchPoint map(float x,float y) const {return {a*x+b*y+c,d*x+e*y+f};}
  bool fit(const TouchPoint *raw,const TouchPoint *screen) {
    float ux=raw[1].x-raw[0].x,uy=raw[1].y-raw[0].y;
    float vx=raw[2].x-raw[0].x,vy=raw[2].y-raw[0].y;
    float determinant=ux*vy-vx*uy;
    if(std::fabs(determinant)<10000)return false;
    a=((screen[1].x-screen[0].x)*vy-(screen[2].x-screen[0].x)*uy)/determinant;
    b=(ux*(screen[2].x-screen[0].x)-vx*(screen[1].x-screen[0].x))/determinant;
    c=screen[0].x-a*raw[0].x-b*raw[0].y;
    d=((screen[1].y-screen[0].y)*vy-(screen[2].y-screen[0].y)*uy)/determinant;
    e=(ux*(screen[2].y-screen[0].y)-vx*(screen[1].y-screen[0].y))/determinant;
    f=screen[0].y-d*raw[0].x-e*raw[0].y;
    return true;
  }
};
