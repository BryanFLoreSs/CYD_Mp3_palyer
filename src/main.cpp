#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include "cyd_bluetooth.h"
#include <vector>
#include <algorithm>
#include "config.h"
#include "touch_calibration.h"
#include <Preferences.h>
#include "mp3_stream.h"
#include <freertos/stream_buffer.h>

TFT_eSPI tft;
TFT_eSprite widgetCanvas(&tft);
bool widgetCanvasReady=false;
uint32_t uiClears=0,uiPushes=0;
bool forceFullFrame=true;
int renderedPage=-1,renderedTrack=-2,renderedFilter=-1,renderedOffset=-1,renderedLyricPage=-1;
String renderedNotice;
CydBluetooth bluetooth;
SemaphoreHandle_t mediaLock, pcmLock;
struct Track { String path, title; bool art, lyrics, favorite; };
std::vector<Track> tracks;
File audio;
Mp3Stream mp3;
StreamBufferHandle_t pcmQueue;
bool isMp3=false;
volatile bool sourceEOF=false;
uint32_t compressedPosition=0;
volatile uint32_t pcmBytes=0, pcmUnderruns=0;
volatile int pcmPeak=0;
size_t readMp3(void *,uint8_t *buffer,size_t size) { return audio.read(buffer,size); }

String btName, lyricText, notice, playbackError;
uint16_t *cover=nullptr;
bool coverReady=false;
uint32_t dataStart=0, dataSize=0, played=0;
volatile bool playing=false, ended=false;
int current=-1, page=0, filter=0, offset=0, lyricPage=0;
bool dirty=true, sdReady=false;
uint32_t lastPaint=0;
TouchCalibration touchMapping;
TouchPoint calibrationRaw[3];
const TouchPoint calibrationTargets[]={{24,24},{296,24},{24,216},{160,120}};
bool calibrating=false,calibrated=false,calibrationDirty=false;
bool calibrationCaptured=false,calibrationDown=false;
int calibrationStep=0;
uint32_t calibrationPressedAt=0;
String calibrationMessage;
void startCalibration() {
  forceFullFrame=true;
  calibrating=true;calibrationStep=0;calibrationCaptured=false;calibrationDown=false;
  calibrationMessage="Tap and hold each cross";calibrationDirty=true;
}
constexpr uint16_t BG=0x0863, PANEL=0x10A5, INK=0xEF7D, MUTED=0x8CB2, ACCENT=0x6FF4;
String base(const String &p) { return p.substring(0,p.lastIndexOf('.')); }
String shorten(String s, int n) { return s.length()>unsigned(n) ? s.substring(0,n-2)+".." : s; }
uint32_t u32(const uint8_t *b) { return b[0]|uint32_t(b[1])<<8|uint32_t(b[2])<<16|uint32_t(b[3])<<24; }
uint16_t u16(const uint8_t *b) { return b[0]|uint16_t(b[1])<<8; }

bool openWav(const String &path) {
  audio.close(); audio=SD.open(path); dataSize=0; played=0;
  uint8_t h[16];
  if(!audio || audio.read(h,12)!=12 || memcmp(h,"RIFF",4) || memcmp(h+8,"WAVE",4)) return false;
  bool valid=false;
  while(audio.available() && audio.position()+8<=audio.size()) {
    if(audio.read(h,8)!=8) break;
    uint32_t size=u32(h+4), start=audio.position();
    if(size>audio.size()-start) break;
    if(!memcmp(h,"fmt ",4)) {
      if(size<16 || audio.read(h,16)!=16) break;
      valid=u16(h)==1 && u16(h+2)==2 && u32(h+4)==44100 && u16(h+12)==4 && u16(h+14)==16;
    } else if(!memcmp(h,"data",4) && valid && size>=4) {
      dataStart=start; dataSize=size&~3U; return true;
    }
    if(!audio.seek(start+size+(size&1))) break;
  }
  audio.close(); return false;
}
bool openTrack(const String &path) {
  playbackError="";
  String lower=path; lower.toLowerCase();
  isMp3=lower.endsWith(".mp3"); sourceEOF=false; compressedPosition=0;
  xStreamBufferReset(pcmQueue);
  if(!isMp3)return openWav(path);
  audio.close(); audio=SD.open(path); played=0; dataStart=0; dataSize=0;
  if(!audio)return false;
  uint8_t tag[10];
  int headerBytes=audio.read(tag,10);
  if(headerBytes==10 && !memcmp(tag+4,"ftyp",4)) {
    playbackError="MP4 file; use a real MP3";audio.close();return false;
  }
  if(headerBytes==10 && !memcmp(tag,"ID3",3)) {
    uint32_t size=(uint32_t(tag[6]&127)<<21)|(uint32_t(tag[7]&127)<<14)|
      (uint32_t(tag[8]&127)<<7)|(tag[9]&127);
    dataStart=10+size+((tag[3]==4 && (tag[5]&16))?10:0);
    if(dataStart>=audio.size()){audio.close();return false;}
  }
  audio.seek(dataStart);dataSize=audio.size()-dataStart;
  mp3.reset(readMp3,nullptr);
  return dataSize>0;
}
void audioProducer(void *) {
  int16_t block[512*2];
  for(;;) {
    xSemaphoreTake(mediaLock,portMAX_DELAY);
    if(playing && audio && !sourceEOF && xStreamBufferSpacesAvailable(pcmQueue)>=sizeof(block)) {
      size_t bytes=0;
      if(isMp3) {
        int16_t l,r;
        while(bytes<sizeof(block) && mp3.next(l,r)){block[bytes/2]=l;block[bytes/2+1]=r;bytes+=4;}
        compressedPosition=mp3.consumed();
      } else {
        uint32_t remaining=dataStart+dataSize-audio.position();
        bytes=audio.read(reinterpret_cast<uint8_t*>(block),std::min(uint32_t(sizeof(block)),remaining));
        bytes&=~3U;
      }
      if(bytes)xStreamBufferSend(pcmQueue,block,bytes,0);
      if(bytes<sizeof(block))sourceEOF=true;
    }
    xSemaphoreGive(mediaLock);vTaskDelay(1);
  }
}
int32_t pcm(uint8_t *out,int32_t len) {
  if(!out || len<=0)return 0;
  memset(out,0,len);
  if(xSemaphoreTake(pcmLock,pdMS_TO_TICKS(8))!=pdTRUE)return len;
  if(playing) {
    size_t got=xStreamBufferReceive(pcmQueue,out,len&~3,0);played+=got;pcmBytes+=got;
    if(got<size_t(len))pcmUnderruns++;
    int peak=0;auto samples=reinterpret_cast<int16_t*>(out);
    for(size_t i=0;i<got/2;i++)peak=std::max(peak,std::abs(int(samples[i])));
    pcmPeak=peak;
    if(sourceEOF && xStreamBufferBytesAvailable(pcmQueue)==0){playing=false;ended=true;}
  }
  xSemaphoreGive(pcmLock);return len;
}
bool jpeg(int16_t x,int16_t y,uint16_t w,uint16_t h,uint16_t *pixels) {
  if(!cover || x<0 || y<0 || x+w>96 || y+h>96) return false;
  for(int row=0;row<h;row++) memcpy(cover+(y+row)*96+x,pixels+row*w,w*2);
  return true;
}
void loadLyrics() {
  lyricText="No lyrics available.\nAdd a matching .txt file."; lyricPage=0;
  if(current<0) return;
  File f=SD.open(base(tracks[current].path)+".txt");
  if(f) { lyricText=""; lyricText.reserve(8192); while(f.available() && lyricText.length()<8192) lyricText+=char(f.read()); f.close(); }
  if(lyricText.length()>8192) lyricText=lyricText.substring(0,8192);
}
void selectTrack(int index) {
  if(index<0 || index>=int(tracks.size())) return;
  xSemaphoreTake(mediaLock,portMAX_DELAY);
  xSemaphoreTake(pcmLock,portMAX_DELAY);
  playing=false; ended=false; current=index;
  bool ok=openTrack(tracks[index].path); loadLyrics();
  coverReady=false;
  if(tracks[index].art){if(!cover)cover=static_cast<uint16_t*>(calloc(96*96,sizeof(uint16_t)));}
  else {free(cover);cover=nullptr;}
  if(cover)memset(cover,0,96*96*sizeof(uint16_t));
  if(tracks[index].art && cover) {
    String path=base(tracks[index].path)+".jpg"; uint16_t w=0,h=0;
    if(TJpgDec.getSdJpgSize(&w,&h,path.c_str())==JDR_OK && w<=96 && h<=96)
      coverReady=TJpgDec.drawSdJpg(0,0,path.c_str())==JDR_OK;
  }
  notice=ok ? "" : (playbackError.length()?playbackError:String("Cannot open this audio file"));
  xSemaphoreGive(pcmLock);
  xSemaphoreGive(mediaLock);
  // Artwork is cached before audio starts, avoiding SD contention on redraw.
  dirty=true;
  playing=ok;
}
bool matches(const Track &t) { return filter==0 || (filter==1 && t.art) || (filter==2 && t.lyrics) || (filter==3 && t.favorite); }
std::vector<int> visible() { std::vector<int> v; for(unsigned i=0;i<tracks.size();i++) if(matches(tracks[i])) v.push_back(i); return v; }
void next(int delta) { if(tracks.empty()) return; auto v=visible(); if(v.empty()) return; auto it=std::find(v.begin(),v.end(),current); int n=it==v.end() ? (delta>0?-1:0) : it-v.begin(); selectTrack(v[(n+delta+int(v.size()))%v.size()]); }
void favorites() { xSemaphoreTake(mediaLock,portMAX_DELAY); File f=SD.open("/favorites.txt",FILE_WRITE); if(f) { for(auto &t:tracks) if(t.favorite) f.println(t.path); f.close(); } xSemaphoreGive(mediaLock); }
void scanDirectory(const String &folder,int depth) {
  File dir=SD.open(folder); if(!dir || !dir.isDirectory())return;
  for(File f=dir.openNextFile();f;f=dir.openNextFile()) {
    String p=f.name(); bool isDir=f.isDirectory(); f.close();
    if(!p.startsWith("/")) p=(folder=="/"?"/":folder+"/")+p;
    if(isDir && depth<3 && !p.startsWith("/.") && p!="/System Volume Information")scanDirectory(p,depth+1);
    String ext=p; ext.toLowerCase();
    if(!isDir && !p.substring(p.lastIndexOf('/')+1).startsWith(".") && (ext.endsWith(".wav") || ext.endsWith(".mp3")) && tracks.size()<200) {
      String b=base(p), title=b.substring(b.lastIndexOf('/')+1);
      tracks.push_back({p,title,SD.exists(b+".jpg"),SD.exists(b+".txt"),false});
    }
  }
}
void scan() {
  scanDirectory("/",0);
  std::sort(tracks.begin(),tracks.end(),[](const Track&a,const Track&b){return a.title.compareTo(b.title)<0;});
  File f=SD.open("/favorites.txt"); if(f) { while(f.available()) { String p=f.readStringUntil('\n'); p.trim(); for(auto &t:tracks) if(t.path==p)t.favorite=true; } f.close(); }
}
void text(const String &s,int x,int y,uint16_t color=INK,int font=2) { tft.setTextColor(color,BG); tft.drawString(s,x,y,font); }
void button(int x,int y,int w,const String &s,bool active=false) { tft.fillRoundRect(x,y,w,30,9,active?ACCENT:PANEL); tft.setTextColor(active?BG:INK); tft.drawCentreString(s,x+w/2,y+7,2); }
// 4-bit palette: one reusable 4.3 KB widget buffer, no full-screen framebuffer.
void pushWidget(int x,int y,int w,int h) {
  widgetCanvas.pushSprite(x,y,0,0,w,h);uiPushes++;
}
void smoothButton(int x,int y,int w,const String &label,bool active=false) {
  if(!widgetCanvasReady){button(x,y,w,label,active);return;}
  widgetCanvas.fillSprite(0);
  widgetCanvas.fillRoundRect(0,0,w,30,9,active?4:1);
  widgetCanvas.setTextColor(active?0:2,active?4:1);
  widgetCanvas.drawCentreString(label,w/2,7,2);pushWidget(x,y,w,30);
}
void drawBluetooth(bool force=false) {
  static int shown=-1;
  int status=bluetooth.status;if(!force && shown==status)return;
  const char *labels[]={"BT searching","BT connecting","BT connected","BT pair failed"};
  if(widgetCanvasReady){
    widgetCanvas.fillSprite(0);widgetCanvas.setTextColor(3,0);
    widgetCanvas.drawString(labels[status],0,0,1);pushWidget(199,9,121,12);
  } else {tft.fillRect(199,9,121,12,BG);text(labels[status],199,9,MUTED,1);}
  shown=status;
}
void drawVolume(bool force=false) {
  static int shown=-1;
  int volume=bluetooth.get_volume();if(!force && shown==volume)return;
  if(force){smoothButton(120,140,42,"-");smoothButton(254,140,50,"+");}
  String value=String(volume*100/127)+"%";
  if(widgetCanvasReady){widgetCanvas.fillSprite(0);widgetCanvas.setTextColor(4,0);widgetCanvas.drawString(value,0,0,1);pushWidget(187,151,44,12);}
  else {tft.fillRect(187,151,44,12,BG);text(value,187,151,ACCENT,1);}
  shown=volume;
}
void transport(bool force=false) {
  static int lastBar=-1,lastSecond=-1,lastDuration=-1,lastActive=-1,lastFavorite=-1;
  static bool lastMp3=false;
  uint32_t position,total,pcmPosition;bool active,mp3Track;
  xSemaphoreTake(mediaLock,portMAX_DELAY);
  position=isMp3?compressedPosition:played;total=dataSize;pcmPosition=played;active=playing;mp3Track=isMp3;
  xSemaphoreGive(mediaLock);
  int bar=total?uint64_t(288)*std::min(position,total)/total:0;
  unsigned second=pcmPosition/176400,duration=total/176400;
  if(force || lastBar!=bar || lastSecond!=int(second) || lastDuration!=int(duration) || lastMp3!=mp3Track) {
    char times[40];
    if(mp3Track)snprintf(times,sizeof(times),"%u:%02u / MP3",second/60,second%60);
    else snprintf(times,sizeof(times),"%u:%02u / %u:%02u",second/60,second%60,duration/60,duration%60);
    if(widgetCanvasReady) {
      widgetCanvas.fillSprite(0);widgetCanvas.fillRoundRect(0,0,288,4,2,1);
      if(bar)widgetCanvas.fillRect(0,0,bar,4,4);
      widgetCanvas.setTextColor(3,0);widgetCanvas.drawString(times,0,8,2);
      pushWidget(16,175,288,26);
    } else {
      tft.fillRect(16,175,288,26,BG);tft.fillRoundRect(16,175,288,4,2,PANEL);
      if(bar)tft.fillRect(16,175,bar,4,ACCENT);text(times,16,183,MUTED);
    }
    lastBar=bar;lastSecond=second;lastDuration=duration;lastMp3=mp3Track;
  }
  int favorite=current>=0 && tracks[current].favorite;
  if(force){smoothButton(16,207,55,"PREV");smoothButton(189,207,55,"NEXT");}
  if(force || lastActive!=int(active))smoothButton(80,207,100,active?"PAUSE":"PLAY",true);
  if(force || lastFavorite!=favorite)smoothButton(253,207,51,favorite?"FAV*":"FAV");
  lastActive=active;lastFavorite=favorite;
}
void refreshWidgets(bool force=false) {
  drawBluetooth(force);
  if(page==0){drawVolume(force);transport(force);}
}
void draw() {
  bool full=forceFullFrame || page!=renderedPage || current!=renderedTrack || notice!=renderedNotice ||
    (page==1 && (filter!=renderedFilter || offset!=renderedOffset)) ||
    (page==2 && lyricPage!=renderedLyricPage);
  if(!full){refreshWidgets();dirty=false;return;}
  forceFullFrame=false;renderedPage=page;renderedTrack=current;renderedNotice=notice;
  renderedFilter=filter;renderedOffset=offset;renderedLyricPage=lyricPage;
  tft.fillScreen(BG);uiClears++;text("CYD / MUSIC",12,7,ACCENT);
  drawBluetooth(true);
  button(8,30,94,"PLAYER",page==0); button(112,30,94,"PLAYLIST",page==1); button(216,30,96,"LYRICS",page==2);
  if(page==0) {
    tft.fillRoundRect(12,70,96,96,12,PANEL);
    if(coverReady) tft.pushImage(12,70,96,96,cover);
    else { text("CYD",37,103,ACCENT); text("MUSIC",29,123,MUTED); }
    text(current>=0?shorten(tracks[current].title,23):"Your music, here.",120,79);
    text("SD CARD / BLUETOOTH",120,108,MUTED,1);
    text(shorten(notice.length()?notice:String(tracks.size())+" tracks in library",29),120,130,MUTED,1);
    drawVolume(true);transport(true);
  } else if(page==1) {
    const char *filters[]={"ALL","ART","LYRICS","FAV"};
    for(int i=0;i<4;i++) button(8+i*78,65,72,filters[i],filter==i);
    auto v=visible(); if(v.empty()) text("No tracks in this filter",16,112,MUTED);
    for(int row=0;row<3 && offset+row<int(v.size());row++) {
      int idx=v[offset+row], y=103+row*32;
      tft.fillRoundRect(8,y,304,29,6,idx==current?0x1948:PANEL);
      text(shorten(tracks[idx].title,35),16,y+5,idx==current?ACCENT:INK);
    }
    button(8,207,90,"UP"); text(String(v.size())+" tracks",113,216,MUTED,1); button(222,207,90,"DOWN");
  } else {
    text(current>=0?shorten(tracks[current].title,39):"Select a track",12,68,ACCENT);
    // Fixed-width lines keep lyric pagination deterministic on the small display.
    std::vector<String> lines; String line;
    for(unsigned i=0;i<lyricText.length();i++) { char c=lyricText[i]; if(c=='\r')continue; if(c=='\n'||line.length()>=43){lines.push_back(line);line="";} if(c!='\n')line+=c; } if(line.length())lines.push_back(line);
    for(int r=0;r<6 && lyricPage*6+r<int(lines.size());r++)text(lines[lyricPage*6+r],12,94+r*16,INK,1);
    button(8,207,90,"UP"); button(222,207,90,"DOWN");
    text(String(lyricPage+1)+" / "+String(std::max(1,(int(lines.size())+5)/6)),134,216,MUTED,1);
  }
  dirty=false;
}
// Bit-banged XPT2046: the CYD has three separate SPI pin sets but only two SPI hosts.
uint16_t touchRead(uint8_t cmd) {
  digitalWrite(33,LOW);
  for(int b=7;b>=0;b--) { digitalWrite(32,(cmd>>b)&1);digitalWrite(25,HIGH);delayMicroseconds(2);digitalWrite(25,LOW); }
  uint16_t v=0; for(int b=0;b<16;b++){digitalWrite(25,HIGH);delayMicroseconds(2);v=(v<<1)|digitalRead(39);digitalWrite(25,LOW);}
  digitalWrite(33,HIGH); return (v>>3)&4095;
}
bool rawTouch(int &x,int &y) {
  if(digitalRead(36))return false;
  int xs[7],ys[7];
  for(int i=0;i<7;i++){xs[i]=touchRead(0xD0);ys[i]=touchRead(0x90);}
  touchRead(0x80);
  if(digitalRead(36))return false;
  std::sort(xs,xs+7);std::sort(ys,ys+7);x=xs[3];y=ys[3];
  return x>50 && x<4050 && y>50 && y<4050;
}
bool touch(int &x,int &y) {
  int a,b;if(!rawTouch(a,b))return false;
  auto p=touchMapping.map(a,b);
  x=constrain(int(lroundf(p.x)),0,319);y=constrain(int(lroundf(p.y)),0,239);
  return true;
}
void drawCalibration() {
  tft.fillScreen(BG);
  tft.setTextColor(ACCENT,BG);tft.drawCentreString("TOUCH CALIBRATION",160,68,2);
  tft.setTextColor(INK,BG);tft.drawCentreString(calibrationMessage,160,93,1);
  tft.drawCentreString(String("Target ")+String(calibrationStep+1)+" / 4",160,147,2);
  auto p=calibrationTargets[calibrationStep];
  tft.drawCircle(p.x,p.y,12,ACCENT);tft.drawFastHLine(p.x-18,p.y,37,ACCENT);
  tft.drawFastVLine(p.x,p.y-18,37,ACCENT);tft.fillCircle(p.x,p.y,2,INK);
  calibrationDirty=false;
}
void calibrationTick() {
  if(calibrationDirty)drawCalibration();
  int x,y;bool down=rawTouch(x,y);
  if(down && !calibrationDown)calibrationPressedAt=millis();
  if(!down)calibrationCaptured=false;
  if(down && !calibrationCaptured && millis()-calibrationPressedAt>=100) {
    calibrationCaptured=true;
    if(calibrationStep<3) {
      calibrationRaw[calibrationStep++]={float(x),float(y)};
      if(calibrationStep==3 && !touchMapping.fit(calibrationRaw,calibrationTargets)) {
        calibrationStep=0;calibrationMessage="Try again: tap cross centers";
      }
      calibrationDirty=true;
    } else {
      auto check=touchMapping.map(x,y);
      if(std::fabs(check.x-160)>18 || std::fabs(check.y-120)>18) {
        calibrationStep=0;calibrationMessage="Check missed. Try again.";calibrationDirty=true;
      } else {
        Preferences prefs;prefs.begin("cyd-touch",false);
        size_t saved=prefs.putBytes("mapping",&touchMapping,sizeof(touchMapping));prefs.end();
        if(saved!=sizeof(touchMapping)) {
          calibrationMessage="Save failed. Tap center again.";calibrationDirty=true;
        } else {
          calibrated=true;calibrating=false;dirty=true;
          Serial.println("Touch: calibration validated and saved");
        }
      }
    }
  }
  calibrationDown=down;
}
void setPlayerVolume(int level) {
  level=constrain(level,0,127);bluetooth.set_volume(level);
  if(bluetooth.is_connected()) {
    esp_err_t result=esp_avrc_ct_send_set_absolute_volume_cmd(1,uint8_t(level));
    Serial.printf("Volume request: %d, remote result %s\n",level,esp_err_to_name(result));
  }
  dirty=true;
}
void setPlaying(bool value) {
  xSemaphoreTake(pcmLock,portMAX_DELAY);
  playing=bool(audio) && value;
  bool requested=playing;
  xSemaphoreGive(pcmLock);
  bluetooth.wantPlayback(requested);dirty=true;
  Serial.printf("Player requested: %s\n",requested?"play":"pause");
}
void tap(int x,int y) {
  Serial.printf("Touch mapped: %d,%d\n",x,y);
  if(y>=30 && y<60) {page=x/107;dirty=true;return;}
  if(page==0 && y>=140 && y<170) {
    if(x>=120 && x<162)setPlayerVolume(bluetooth.get_volume()-8);
    else if(x>=254 && x<304)setPlayerVolume(bluetooth.get_volume()+8);
    return;
  }
  if(page==0 && y>=207) {
    if(x<75)next(-1); else if(x<185) { setPlaying(!playing); }
    else if(x<249)next(1); else if(current>=0){tracks[current].favorite=!tracks[current].favorite;favorites();} dirty=true;
  } else if(page==1) {
    auto v=visible();
    if(y>=65&&y<95){filter=std::min(3,x/78);offset=0;}
    else if(y>=103&&y<199){int row=(y-103)/32;if(offset+row<int(v.size())){selectTrack(v[offset+row]);page=0;}}
    else if(y>=207){if(x<100)offset=std::max(0,offset-3);else if(x>220&&offset+3<int(v.size()))offset+=3;} dirty=true;
  } else if(page==2 && y>=207) {
    if(x<100)lyricPage=std::max(0,lyricPage-1);
    else if(x>220){int lines=1,col=0;for(unsigned i=0;i<lyricText.length();i++){char c=lyricText[i];if(c=='\r')continue;if(c=='\n'||col>=43){lines++;col=0;}if(c!='\n')col++;}if((lyricPage+1)*6<lines)lyricPage++;} dirty=true;
  }
}
void setup() {
  Serial.begin(115200); mediaLock=xSemaphoreCreateMutex();pcmLock=xSemaphoreCreateMutex();
  pcmQueue=xStreamBufferCreate(12288,1);
  if(!mediaLock || !pcmLock || !pcmQueue){Serial.println("Audio allocation failed");while(true)delay(1000);}
  pinMode(21,OUTPUT);digitalWrite(21,HIGH); pinMode(0,INPUT_PULLUP);
  pinMode(33,OUTPUT);digitalWrite(33,HIGH);pinMode(25,OUTPUT);pinMode(32,OUTPUT);pinMode(39,INPUT);pinMode(36,INPUT);
  tft.init();tft.setRotation(1); tft.setSwapBytes(true);
  Preferences prefs;prefs.begin("cyd-touch",true);
  if(prefs.getBytesLength("mapping")==sizeof(touchMapping)) {
    prefs.getBytes("mapping",&touchMapping,sizeof(touchMapping));
    calibrated=std::isfinite(touchMapping.a)&&std::isfinite(touchMapping.b)&&
      std::isfinite(touchMapping.c)&&std::isfinite(touchMapping.d)&&std::isfinite(touchMapping.e)&&std::isfinite(touchMapping.f);
  }
  prefs.end();if(!calibrated)startCalibration();
  widgetCanvas.setColorDepth(4);
  widgetCanvasReady=widgetCanvas.createSprite(288,30)!=nullptr;
  if(widgetCanvasReady){
    widgetCanvas.setPaletteColor(0,BG);widgetCanvas.setPaletteColor(1,PANEL);
    widgetCanvas.setPaletteColor(2,INK);widgetCanvas.setPaletteColor(3,MUTED);widgetCanvas.setPaletteColor(4,ACCENT);
  }
  Serial.printf("UI widget buffer: %s\n",widgetCanvasReady?"ready (4-bit)":"direct drawing fallback");
  TJpgDec.setCallback(jpeg); TJpgDec.setJpgScale(1);
  SPI.begin(18,19,23,5);sdReady=SD.begin(5,SPI,20000000);
  btName=DEFAULT_BT_NAME;
  if(sdReady){scan();File f=SD.open("/bluetooth.txt");if(f){btName=f.readStringUntil('\n');btName.trim();f.close();} }
  else notice="SD missing: insert FAT32 card";
  Serial.printf("CYD Music: landscape %dx%d, SD %s, %u tracks, MP3 enabled\n",tft.width(),tft.height(),sdReady?"ready":"missing",unsigned(tracks.size()));
  if(!btName.length())btName=DEFAULT_BT_NAME;
  if(tracks.size())selectTrack(0);
  bluetooth.target=btName;Serial.printf("BT target: %s\n",btName.c_str());
  bluetooth.set_pin_code("0000",ESP_BT_PIN_TYPE_VARIABLE);
  bluetooth.set_volume(90);
  bluetooth.set_local_name("CYD Music");bluetooth.set_auto_reconnect(true);bluetooth.set_ssp_enabled(true);
  if(xTaskCreatePinnedToCore(audioProducer,"audio decode",24576,nullptr,2,nullptr,1)!=pdPASS){
    playing=false;notice="Audio task allocation failed";
  }
  // Arduino's default btStart enables dual-mode Bluetooth. A2DP only needs
  // Classic; start that controller explicitly so BLE cannot consume audio RAM.
  esp_err_t btResult=esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
  Serial.printf("BT release unused BLE memory: %s\n",esp_err_to_name(btResult));
  esp_bt_controller_config_t btConfig=BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  btConfig.mode=ESP_BT_MODE_CLASSIC_BT;
  btResult=esp_bt_controller_init(&btConfig);
  if(btResult==ESP_OK)btResult=esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
  if(btResult!=ESP_OK){Serial.printf("BT controller error: %s\n",esp_err_to_name(btResult));notice="Bluetooth startup failed";return;}
  bluetooth.set_on_audio_state_changed([](esp_a2d_audio_state_t state,void*){
    Serial.printf("BT audio state: %d\n",int(state));
  });
  bluetooth.set_data_callback(pcm);bluetooth.start(btName.c_str());
  esp_bt_io_cap_t capability=ESP_BT_IO_CAP_NONE;
  esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE,&capability,sizeof(capability));
  Serial.printf("BT started, free heap %u\n",ESP.getFreeHeap());
}
void playbackStatus() {
  xSemaphoreTake(mediaLock,portMAX_DELAY);
  Serial.printf("PLAYBACK active=%d calibration=%d queued=%u bytes=%u peak=%d underruns=%u EOF=%d source=%u/%u volume=%d audio=%d heap=%u uiClears=%u uiPushes=%u\n",
    int(playing),int(calibrating),unsigned(xStreamBufferBytesAvailable(pcmQueue)),unsigned(pcmBytes),int(pcmPeak),unsigned(pcmUnderruns),int(sourceEOF),unsigned(compressedPosition),unsigned(dataSize),bluetooth.get_volume(),int(bluetooth.get_audio_state()),ESP.getFreeHeap(),unsigned(uiClears),unsigned(uiPushes));
  xSemaphoreGive(mediaLock);
}
void loop() {
  static bool wasTouch=false,wasBoot=false,connected=false;
  static int lastBtStatus=-1;
  static uint32_t bootPressedAt=0;
  if(lastBtStatus!=bluetooth.status){lastBtStatus=bluetooth.status;dirty=true;}
  if(Serial.available()){
    char command=Serial.read();
    if(command=='p')bluetooth.pairAgain();else if(command=='t')startCalibration();
    else if(command=='s')playbackStatus();
    else if(command=='r'){if(current>=0)selectTrack(current);}
    else if(command=='0')setPlaying(false);else if(command=='1')setPlaying(true);
    else if(command=='+')setPlayerVolume(bluetooth.get_volume()+8);
  }
  static uint32_t lastStatus=0;
  if(bluetooth.is_connected() && millis()-lastStatus>5000){playbackStatus();lastStatus=millis();}
  bool conn=bluetooth.is_connected();if(conn!=connected){connected=conn;dirty=true;}
  bool boot=digitalRead(0)==LOW;
  if(boot&&!wasBoot)bootPressedAt=millis();
  if(!boot&&wasBoot){if(millis()-bootPressedAt>=1500)startCalibration();else bluetooth.pairAgain();}
  wasBoot=boot;
  bluetooth.wantPlayback(playing);
  if(calibrating){calibrationTick();wasTouch=true;delay(20);return;}
  int x,y;bool down=touch(x,y);if(down&&!wasTouch)tap(x,y);wasTouch=down;
  if(ended){ended=false;next(1);}
  if(dirty)draw();else if(millis()-lastPaint>250){refreshWidgets();lastPaint=millis();}
  delay(20);
}
