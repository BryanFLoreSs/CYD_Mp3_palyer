#pragma once
#include <BluetoothA2DPSource.h>
#include "audio_transport.h"

// Accept the explicitly configured name from either GAP name response format.
// Some earbuds omit an EIR name or do not advertise an audio class correctly.
class CydBluetooth : public BluetoothA2DPSource {
 public:
  String target;
  void wantPlayback(bool wanted) {
    uint32_t now=millis();
    bool changed=requestedPlayback!=wanted;requestedPlayback=wanted;
    if(is_connected() && (changed || uint32_t(now-lastMediaKick)>250)) {
      lastMediaKick=now;a2d_app_heart_beat(nullptr);
    }
  }
  volatile int status=0; // searching, connecting, connected, authentication failed
  void pairAgain() {
    esp_bd_addr_t address;
    memcpy(address,*get_last_peer_address(),sizeof(address));
    bool nonzero=false;for(auto b:address)nonzero|=b!=0;
    if(nonzero)esp_bt_gap_remove_bond_device(address);
    clean_last_connection();
    Serial.println("BT: restarting with fresh discovery");
    delay(250);ESP.restart();
  }
 protected:
  volatile bool requestedPlayback=false;
  uint32_t lastMediaKick=0;
  AudioTransport transport;
  using MediaCommand=AudioTransport::Command;
  void driveMedia() {
    transport.want(requestedPlayback);
    auto command=transport.poll(millis());
    if(command==MediaCommand::None)return;
    auto sdk=command==MediaCommand::Check?ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY:
      command==MediaCommand::Start?ESP_A2D_MEDIA_CTRL_START:ESP_A2D_MEDIA_CTRL_SUSPEND;
    esp_err_t result=esp_a2d_media_ctrl(sdk);
    Serial.printf("BT media request: %d (%s)\n",int(sdk),esp_err_to_name(result));
    if(result!=ESP_OK)transport.ack(command,false,millis());
  }
  void process_user_state_callbacks(uint16_t event,void *raw) override {
    BluetoothA2DPSource::process_user_state_callbacks(event,raw);
    auto param=static_cast<esp_a2d_cb_param_t*>(raw);
    if(event==ESP_A2D_CONNECTION_STATE_EVT) {
      transport.connection(param->conn_stat.state==ESP_A2D_CONNECTION_STATE_CONNECTED);
      driveMedia();
    } else if(event==ESP_A2D_AUDIO_STATE_EVT) {
      transport.audio(param->audio_stat.state==ESP_A2D_AUDIO_STATE_STARTED);
      driveMedia();
    }
  }
  void bt_app_av_media_proc(uint16_t event,void *raw) override {
    if(event==ESP_A2D_MEDIA_CTRL_ACK_EVT) {
      auto param=static_cast<esp_a2d_cb_param_t*>(raw);
      auto sdk=param->media_ctrl_stat.cmd;
      auto command=sdk==ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY?MediaCommand::Check:
        sdk==ESP_A2D_MEDIA_CTRL_START?MediaCommand::Start:
        sdk==ESP_A2D_MEDIA_CTRL_SUSPEND?MediaCommand::Suspend:MediaCommand::None;
      Serial.printf("BT media acknowledgement: %d status %d\n",int(sdk),int(param->media_ctrl_stat.status));
      transport.ack(command,param->media_ctrl_stat.status==ESP_A2D_MEDIA_CTRL_ACK_SUCCESS,millis());
    }
    driveMedia();
  }
  void app_gap_callback(esp_bt_gap_cb_event_t event,esp_bt_gap_cb_param_t *param) override {
    if(event==ESP_BT_GAP_DISC_RES_EVT) {
      if(s_a2d_state!=APP_AV_STATE_DISCOVERING)return;
      char name[ESP_BT_GAP_MAX_BDNAME_LEN+1]={0};
      int rssi=0;
      for(int i=0;i<param->disc_res.num_prop;i++) {
        auto &p=param->disc_res.prop[i];
        if(p.type==ESP_BT_GAP_DEV_PROP_BDNAME) {
          size_t n=std::min(size_t(p.len),sizeof(name)-1);memcpy(name,p.val,n);name[n]=0;
        } else if(p.type==ESP_BT_GAP_DEV_PROP_EIR && !name[0]) {
          uint8_t len=0;
          auto bytes=esp_bt_gap_resolve_eir_data(static_cast<uint8_t*>(p.val),ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME,&len);
          if(!bytes)bytes=esp_bt_gap_resolve_eir_data(static_cast<uint8_t*>(p.val),ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME,&len);
          if(bytes){size_t n=std::min(size_t(len),sizeof(name)-1);memcpy(name,bytes,n);name[n]=0;}
        } else if(p.type==ESP_BT_GAP_DEV_PROP_RSSI)rssi=*static_cast<int8_t*>(p.val);
      }
      auto &addr=param->disc_res.bda;
      Serial.printf("BT found: '%s' %02X:%02X:%02X:%02X:%02X:%02X RSSI %d\n",name,addr[0],addr[1],addr[2],addr[3],addr[4],addr[5],rssi);
      String found(name);found.trim();
      if(found.equalsIgnoreCase(target)) {
        memcpy(s_peer_bdname,name,sizeof(name));memcpy(peer_bd_addr,addr,ESP_BD_ADDR_LEN);
        set_last_connection(addr);s_a2d_state=APP_AV_STATE_DISCOVERED;status=1;
        Serial.println("BT: target found; connecting");esp_bt_gap_cancel_discovery();
      }
      return;
    }
    if(event==ESP_BT_GAP_AUTH_CMPL_EVT) {
      Serial.printf("BT authentication: %s (status %d)\n",param->auth_cmpl.stat==ESP_BT_STATUS_SUCCESS?"success":"failed",int(param->auth_cmpl.stat));
      if(param->auth_cmpl.stat!=ESP_BT_STATUS_SUCCESS)status=3;
    }
    if(event==ESP_BT_GAP_PIN_REQ_EVT)Serial.println("BT: legacy PIN requested (using configured PIN 0000)");
    BluetoothA2DPSource::app_gap_callback(event,param);
  }
  void app_a2d_callback(esp_a2d_cb_event_t event,esp_a2d_cb_param_t *param) override {
    if(event==ESP_A2D_CONNECTION_STATE_EVT) {
      auto state=param->conn_stat.state;
      Serial.printf("BT A2DP state: %d, disconnect reason: %d\n",int(state),int(param->conn_stat.disc_rsn));
      if(state==ESP_A2D_CONNECTION_STATE_CONNECTED)status=2;
      else if(state==ESP_A2D_CONNECTION_STATE_CONNECTING)status=1;
      else if(state==ESP_A2D_CONNECTION_STATE_DISCONNECTED && status!=3)status=0;
    }
    BluetoothA2DPSource::app_a2d_callback(event,param);
  }
};
