/*
  ============================================================
  SIMON - ESP32-C6 NATIVE ZIGBEE
  RFID + PRODUCTION + DEVICE INFO + HEARTBEAT + LCD I2C
  Firmware: SIMON-8.4.0-XIAO-EXT-ANT-C6
  ============================================================

  HARDWARE:
    Photo Sensor NPN : D1 / GPIO1, active LOW
    RDM6300 TX       : D7 / GPIO17
    Active Buzzer    : D2 / GPIO2

    LCD I2C:
      SDA            : D4 / GPIO22
      SCL            : D5 / GPIO23
      Address        : 0x27
      Size           : 16x2

  SENSOR TYPE:
    1 = Photosensor
    2 = Limit Switch
    3 = Button

  ZIGBEE:
    EP10 = WORK TIME
    EP11 = IDLE TIME
    EP12 = TOTAL STITCH (TRIGGER INSERT)
    EP13 = SIMON CUSTOM CLUSTER
    EP14 = HANDLING TIME

  CUSTOM COMMAND:
    0x01 = AUTH REQUEST
    0x02 = DEVICE BOOT
    0x03 = HEARTBEAT
    0x04 = MACHINE STATE

  CUSTOM ATTRIBUTE:
    0x0002 = NIK
    0x0003 = LOGIN STATUS
    0x0004 = OPERATOR NAME
    0x0005 = PROCESS NAME
    0x0006 = BRANCH

  SENSOR:
    LOW < 2ms      = noise
    LOW 2..199ms   = stitch
    LOW >= 200ms   = trim

  MACHINE STATE:
    Setelah TRIM                 = IDLE
    Stitch pertama setelah TRIM = WORK
    >=2 detik tanpa stitch      = HANDLING
    Stitch kembali              = WORK
    TRIM                         = kirim cycle lalu kembali IDLE

  BUZZER:
    login    = 1 beep saat trim
    no login = 2 beep saat trim
                                                                                                                                                                                                                         
  Arduino Setting:
    Board            : XIAO ESP32C6
    Zigbee Mode      : Zigbee ED
    Partition Scheme : Zigbee 4MB with spiffs
    Erase All Flash  : Disabled
  ============================================================
*/

#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#ifndef ZIGBEE_MODE_ED
#error "Tools -> Zigbee Mode harus Zigbee ED"
#endif

#include "Zigbee.h"
#include "esp_zigbee_cluster.h"
#include "zcl/esp_zigbee_zcl_command.h"


// ============================================================
// HARDWARE
// ============================================================

// Seeed Studio XIAO ESP32-C6 mapping
// D1 = GPIO1  : Photo sensor input
// D2 = GPIO2  : Buzzer output
// D7 = GPIO17 : RDM6300 RX
//
// GPIO3 and GPIO14 are RESERVED for the onboard RF switch.
// Do not use them for external I/O when external antenna is enabled.
#define SENSOR_PIN                 1
#define BUZZER_PIN                 2

#define RFID_RX_PIN                17
#define RFID_BAUD                  9600


// ============================================================
// XIAO ESP32-C6 EXTERNAL ANTENNA RF SWITCH
// ============================================================
//
// Seeed Studio XIAO ESP32-C6:
// GPIO3  LOW  = enable RF switch control
// GPIO14 HIGH = select external U.FL/I-PEX antenna
//
// IMPORTANT:
// GPIO14 must NOT be used as the photo sensor input.
//
#define XIAO_RF_SWITCH_ENABLE_PIN   3
#define XIAO_RF_ANT_SELECT_PIN      14


// ============================================================
// LCD
// ============================================================

// XIAO native I2C pins
// D4 = GPIO22 SDA
// D5 = GPIO23 SCL
#define LCD_SDA_PIN                22
#define LCD_SCL_PIN                23

#define LCD_I2C_ADDRESS            0x27

#define LCD_COLUMNS                16
#define LCD_ROWS                   2

#define LCD_REFRESH_MS             300UL
#define LCD_PAGE_TIME_MS           3000UL
#define LCD_TRIM_HOLD_MS           2500UL


LiquidCrystal_I2C lcd(
  LCD_I2C_ADDRESS,
  LCD_COLUMNS,
  LCD_ROWS
);


// ============================================================
// RFID
// ============================================================

HardwareSerial RFIDSerial(1);

#define RFID_REMOVE_TIMEOUT_MS     1000UL


// ============================================================
// NVS
// ============================================================

Preferences preferences;

const char *PREF_NAMESPACE =
  "simon";


// ============================================================
// SENSOR
// ============================================================

#define MIN_STITCH_PULSE_MS        2UL
#define TRIM_DETECT_MS             200UL
#define TRIM_HOLD_IDLE_MS          2000UL

// Setelah TRIM cukup HIGH stabil singkat.
// 300 ms terlalu lama untuk mesin jahit berkecepatan tinggi.
#define TRIM_RELEASE_DEBOUNCE_MS     5UL

#define HANDLING_CONFIRM_MS         2000UL

#define DEBUG_PULSE_TIMING         false


// ============================================================
// BUZZER
// ============================================================

#define BUZZER_ON_MS               100UL
#define BUZZER_OFF_MS              120UL


// ============================================================
// AUTH
// ============================================================

#define AUTH_TIMEOUT_MS            10000UL

#define AUTH_LOGIN                 1
#define AUTH_LOGOUT                0


// ============================================================
// DEVICE INFORMATION
// ============================================================

#define FIRMWARE_VERSION           "SIMON-8.4.0-XIAO-EXT-ANT-C6"

/*
  1 = Photosensor
  2 = Limit switch
  3 = Button
*/
#define SENSOR_TYPE                1

#define COORDINATOR_ID             "SLZB-06@10.5.2.100"

#define HEARTBEAT_INTERVAL_MS      10000UL

// Device baru yang belum pernah join:
// setelah waktu ini LCD memberi petunjuk Permit Join.
// Zigbee stack tetap mencoba network steering otomatis.
#define ZIGBEE_NEW_DEVICE_HINT_MS   15000UL

// Interval log status ketika belum connected.
#define ZIGBEE_WAIT_LOG_MS          10000UL


// ============================================================
// ZIGBEE
// ============================================================

#define ZIGBEE_CHANNEL             11

#define EP_WORK                    10
#define EP_IDLE                    11
#define EP_STITCH                  12
#define EP_AUTH                    13
#define EP_HANDLING                14

#define SIMON_CLUSTER_ID           0xFC10


// ============================================================
// ZIGBEE ATTRIBUTES
// ============================================================

#define ATTR_NIK                   0x0002
#define ATTR_LOGIN_STATUS          0x0003
#define ATTR_OPERATOR_NAME         0x0004
#define ATTR_PROCESS_NAME          0x0005
#define ATTR_BRANCH                0x0006


// ============================================================
// ZIGBEE COMMANDS
// ============================================================

#define CMD_AUTH_REQUEST           0x01
#define CMD_DEVICE_BOOT            0x02
#define CMD_HEARTBEAT              0x03
#define CMD_MACHINE_STATE          0x04


// ============================================================
// PRODUCTION ENDPOINTS
// ============================================================

ZigbeeAnalog zbWork(
  EP_WORK
);

ZigbeeAnalog zbIdle(
  EP_IDLE
);

ZigbeeAnalog zbStitch(
  EP_STITCH
);

ZigbeeAnalog zbHandling(
  EP_HANDLING
);


// ============================================================
// CUSTOM SIMON ZIGBEE ENDPOINT
// ============================================================

class ZigbeeSimonAuth :
  public ZigbeeEP {

public:

  ZigbeeSimonAuth(
    uint8_t endpoint
  )
    : ZigbeeEP(endpoint) {

    _device_id =
      (esp_zb_ha_standard_devices_t)
      ESP_ZB_HA_CUSTOM_ATTR_DEVICE_ID;


    // ========================================================
    // CLUSTER LIST
    // ========================================================

    _cluster_list =
      esp_zb_zcl_cluster_list_create();


    // ========================================================
    // BASIC CLUSTER
    // ========================================================

    esp_zb_cluster_list_add_basic_cluster(
      _cluster_list,
      esp_zb_basic_cluster_create(NULL),
      ESP_ZB_ZCL_CLUSTER_SERVER_ROLE
    );


    // ========================================================
    // IDENTIFY CLUSTER
    // ========================================================

    esp_zb_cluster_list_add_identify_cluster(
      _cluster_list,
      esp_zb_identify_cluster_create(NULL),
      ESP_ZB_ZCL_CLUSTER_SERVER_ROLE
    );


    // ========================================================
    // CUSTOM CLUSTER
    // ========================================================

    esp_zb_attribute_list_t *customCluster =
      esp_zb_zcl_attr_list_create(
        SIMON_CLUSTER_ID
      );


    // ========================================================
    // NIK
    // ========================================================

    ESP_ERROR_CHECK(

      esp_zb_custom_cluster_add_custom_attr(

        customCluster,

        ATTR_NIK,

        ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,

        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,

        _nikAttr
      )
    );


    // ========================================================
    // LOGIN STATUS
    // ========================================================

    ESP_ERROR_CHECK(

      esp_zb_custom_cluster_add_custom_attr(

        customCluster,

        ATTR_LOGIN_STATUS,

        ESP_ZB_ZCL_ATTR_TYPE_U8,

        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,

        &_loginStatusAttr
      )
    );


    // ========================================================
    // OPERATOR NAME
    // ========================================================

    ESP_ERROR_CHECK(

      esp_zb_custom_cluster_add_custom_attr(

        customCluster,

        ATTR_OPERATOR_NAME,

        ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,

        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,

        _operatorNameAttr
      )
    );


    // ========================================================
    // PROCESS NAME
    // ========================================================

    ESP_ERROR_CHECK(

      esp_zb_custom_cluster_add_custom_attr(

        customCluster,

        ATTR_PROCESS_NAME,

        ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,

        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,

        _processNameAttr
      )
    );


    // ========================================================
    // BRANCH
    // ========================================================

    ESP_ERROR_CHECK(

      esp_zb_custom_cluster_add_custom_attr(

        customCluster,

        ATTR_BRANCH,

        ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,

        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,

        _branchAttr
      )
    );


    ESP_ERROR_CHECK(

      esp_zb_cluster_list_add_custom_cluster(

        _cluster_list,

        customCluster,

        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE
      )
    );


    // ========================================================
    // ENDPOINT CONFIG
    // ========================================================

    _ep_config = {

      .endpoint =
        _endpoint,

      .app_profile_id =
        ESP_ZB_AF_HA_PROFILE_ID,

      .app_device_id =
        ESP_ZB_HA_CUSTOM_ATTR_DEVICE_ID,

      .app_device_version =
        0
    };
  }


  // ==========================================================
  // SEND AUTH REQUEST
  // ==========================================================

  bool sendAuthRequest(
    const char *rfid,
    uint8_t action
  ) {

    if (
      !rfid ||
      rfid[0] == '\0'
    ) {

      return false;
    }


    if (
      !Zigbee.connected()
    ) {

      Serial.println(
        "[AUTH] ZIGBEE NOT CONNECTED"
      );

      return false;
    }


    char authText[32];


    snprintf(
      authText,
      sizeof(authText),
      "%u|%s",
      action,
      rfid
    );


    size_t len =
      strlen(authText);


    if (
      len > 30
    ) {

      return false;
    }


    uint8_t zclString[32] =
      {0};


    zclString[0] =
      (uint8_t)len;


    memcpy(
      &zclString[1],
      authText,
      len
    );


    esp_zb_zcl_custom_cluster_cmd_req_t cmd =
      {};


    cmd.zcl_basic_cmd.dst_addr_u.addr_short =
      0x0000;


    cmd.zcl_basic_cmd.dst_endpoint =
      1;


    cmd.zcl_basic_cmd.src_endpoint =
      _endpoint;


    cmd.address_mode =
      ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;


    cmd.profile_id =
      ESP_ZB_AF_HA_PROFILE_ID;


    cmd.cluster_id =
      SIMON_CLUSTER_ID;


    cmd.direction =
      ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;


    cmd.manuf_specific =
      0;


    cmd.manuf_code =
      0;


    cmd.dis_default_resp =
      1;


    cmd.custom_cmd_id =
      CMD_AUTH_REQUEST;


    cmd.data.type =
      ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING;


    cmd.data.size =
      0;


    cmd.data.value =
      zclString;


    esp_zb_lock_acquire(
      portMAX_DELAY
    );


    uint8_t tsn =
      esp_zb_zcl_custom_cluster_cmd_req(
        &cmd
      );


    esp_zb_lock_release();


    Serial.print(
      "[AUTH] SENT : "
    );

    Serial.println(
      authText
    );


    Serial.print(
      "[AUTH] TSN  : "
    );

    Serial.println(
      tsn
    );


    return true;
  }


  // ==========================================================
  // SEND GENERIC TEXT COMMAND
  // ==========================================================

  bool sendTextCommand(
    uint8_t commandId,
    const char *payload,
    const char *label
  ) {

    if (
      !payload ||
      payload[0] == '\0'
    ) {

      return false;
    }


    if (
      !Zigbee.connected()
    ) {

      return false;
    }


    size_t len =
      strlen(payload);


    if (
      len > 240
    ) {

      Serial.print(
        "["
      );

      Serial.print(
        label
      );

      Serial.println(
        "] PAYLOAD TOO LONG"
      );

      return false;
    }


    uint8_t zclString[242] =
      {0};


    zclString[0] =
      (uint8_t)len;


    memcpy(
      &zclString[1],
      payload,
      len
    );


    esp_zb_zcl_custom_cluster_cmd_req_t cmd =
      {};


    cmd.zcl_basic_cmd.dst_addr_u.addr_short =
      0x0000;


    cmd.zcl_basic_cmd.dst_endpoint =
      1;


    cmd.zcl_basic_cmd.src_endpoint =
      _endpoint;


    cmd.address_mode =
      ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;


    cmd.profile_id =
      ESP_ZB_AF_HA_PROFILE_ID;


    cmd.cluster_id =
      SIMON_CLUSTER_ID;


    cmd.direction =
      ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;


    cmd.manuf_specific =
      0;


    cmd.manuf_code =
      0;


    cmd.dis_default_resp =
      1;


    cmd.custom_cmd_id =
      commandId;


    cmd.data.type =
      ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING;


    cmd.data.size =
      0;


    cmd.data.value =
      zclString;


    esp_zb_lock_acquire(
      portMAX_DELAY
    );


    uint8_t tsn =
      esp_zb_zcl_custom_cluster_cmd_req(
        &cmd
      );


    esp_zb_lock_release();


    Serial.print(
      "["
    );

    Serial.print(
      label
    );

    Serial.print(
      "] SENT : "
    );

    Serial.println(
      payload
    );


    Serial.print(
      "["
    );

    Serial.print(
      label
    );

    Serial.print(
      "] TSN : "
    );

    Serial.println(
      tsn
    );


    return true;
  }


  // ==========================================================
  // RESPONSE
  // ==========================================================

  bool responseReady() const {

    return _responseReady;
  }


  uint8_t receivedStatus() const {

    return _receivedStatus;
  }


  String receivedNik() const {

    return _receivedNik;
  }


  String receivedOperatorName() const {

    return _receivedOperatorName;
  }


  String receivedProcessName() const {

    return _receivedProcessName;
  }


  String receivedBranch() const {

    return _receivedBranch;
  }


  void clearResponse() {

    _receivedNik =
      "";

    _receivedOperatorName =
      "";

    _receivedProcessName =
      "";

    _receivedBranch =
      "";

    _receivedStatus =
      0;

    _responseReady =
      false;
  }


protected:

  // ==========================================================
  // ATTRIBUTE CALLBACK
  // ==========================================================

  void zbAttributeSet(
    const esp_zb_zcl_set_attr_value_message_t *message
  ) override {

    if (
      !message
    ) {

      return;
    }


    if (
      message->info.cluster !=
      SIMON_CLUSTER_ID
    ) {

      return;
    }


    if (
      message->attribute.data.value ==
      nullptr
    ) {

      return;
    }


    // ========================================================
    // NIK
    // ========================================================

    if (
      message->attribute.id ==
        ATTR_NIK &&

      message->attribute.data.type ==
        ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING
    ) {

      _receivedNik =
        decodeZclString(
          message->attribute.data.value
        );


      Serial.print(
        "[AUTH RX] NIK = "
      );

      Serial.println(
        _receivedNik
      );


      return;
    }


    // ========================================================
    // NAME
    // ========================================================

    if (
      message->attribute.id ==
        ATTR_OPERATOR_NAME &&

      message->attribute.data.type ==
        ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING
    ) {

      _receivedOperatorName =
        decodeZclString(
          message->attribute.data.value
        );


      Serial.print(
        "[AUTH RX] NAME = "
      );

      Serial.println(
        _receivedOperatorName
      );


      return;
    }


    // ========================================================
    // PROCESS NAME
    // ========================================================

    if (
      message->attribute.id ==
        ATTR_PROCESS_NAME &&

      message->attribute.data.type ==
        ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING
    ) {

      _receivedProcessName =
        decodeZclString(
          message->attribute.data.value
        );

      Serial.print(
        "[AUTH RX] PROCESS = "
      );

      Serial.println(
        _receivedProcessName
      );

      return;
    }


    // ========================================================
    // BRANCH
    // ========================================================

    if (
      message->attribute.id ==
        ATTR_BRANCH &&

      message->attribute.data.type ==
        ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING
    ) {

      _receivedBranch =
        decodeZclString(
          message->attribute.data.value
        );

      Serial.print(
        "[AUTH RX] BRANCH = "
      );

      Serial.println(
        _receivedBranch
      );

      return;
    }


    // ========================================================
    // STATUS
    // ========================================================

    if (
      message->attribute.id ==
        ATTR_LOGIN_STATUS &&

      message->attribute.data.type ==
        ESP_ZB_ZCL_ATTR_TYPE_U8
    ) {

      _receivedStatus =
        *((uint8_t *)
          message->attribute.data.value);


      _responseReady =
        true;


      Serial.print(
        "[AUTH RX] STATUS = "
      );

      Serial.println(
        _receivedStatus
      );


      return;
    }
  }


private:

  uint8_t _nikAttr[65] =
    {0};


  uint8_t _loginStatusAttr =
    0;


  uint8_t _operatorNameAttr[65] =
    {0};


  uint8_t _processNameAttr[65] =
    {0};


  uint8_t _branchAttr[65] =
    {0};


  volatile bool _responseReady =
    false;


  volatile uint8_t _receivedStatus =
    0;


  String _receivedNik =
    "";


  String _receivedOperatorName =
    "";


  String _receivedProcessName =
    "";


  String _receivedBranch =
    "";


  // ==========================================================
  // ZCL STRING DECODER
  // ==========================================================

  String decodeZclString(
    const void *value
  ) {

    if (
      !value
    ) {

      return "";
    }


    const uint8_t *raw =
      (const uint8_t *)value;


    uint8_t len =
      raw[0];


    if (
      len > 64
    ) {

      len =
        64;
    }


    char buffer[65];


    memcpy(
      buffer,
      &raw[1],
      len
    );


    buffer[len] =
      '\0';


    return String(
      buffer
    );
  }
};


ZigbeeSimonAuth zbAuth(
  EP_AUTH
);


// ============================================================
// DEVICE STATE
// ============================================================

bool deviceBootSent =
  false;


// ============================================================
// ZIGBEE REGISTRATION / AUTO COMMISSION STATE
// ============================================================

// Disimpan di Preferences setelah alat pertama kali sukses join.
// Tujuannya membedakan:
// - alat baru   : JOINING / ERR PERMIT JOIN
// - alat lama   : REJOINING bila coordinator sementara hilang
bool zigbeeRegistered =
  false;


bool zigbeeWasConnected =
  false;


bool zigbeePermitHint =
  false;


uint32_t zigbeeStartMs =
  0;


uint32_t lastZigbeeWaitLogMs =
  0;


uint32_t lastHeartbeatMs =
  0;


// ============================================================
// LCD STATE
// ============================================================

uint32_t lastLcdRefreshMs =
  0;


uint32_t lcdPageStartedMs =
  0;


uint8_t lcdPage =
  0;


uint32_t lastTrimWorkSec =
  0;


uint32_t lastTrimIdleSec =
  0;


uint32_t lastTrimHandlingSec =
  0;


uint32_t lastTrimStitch =
  0;


uint32_t lastTrimLcdUntil =
  0;


// ============================================================
// OPERATOR / AUTH
// ============================================================

bool operatorLoggedIn =
  false;


bool authPending =
  false;


uint8_t pendingAction =
  0;


uint32_t authStartedMs =
  0;


String pendingRFID =
  "";


String currentRFID =
  "";


String currentNIK =
  "";


String currentOperatorName =
  "";


String currentProcessName =
  "";


String currentBranch =
  "";


bool pendingSwitch =
  false;


// ============================================================
// BUZZER STATE
// ============================================================

bool buzzerOn =
  false;


uint8_t buzzerBeepsRemaining =
  0;


uint32_t buzzerChangedMs =
  0;


// ============================================================
// RFID STATE
// ============================================================

bool rfidFrameActive =
  false;


char rfidFrame[13] =
  {0};


uint8_t rfidFrameIndex =
  0;


bool rfidReadyForTap =
  true;


uint32_t lastRFIDFrameMs =
  0;


// ============================================================
// MACHINE STATE
// ============================================================

enum MachineState {

  MACHINE_IDLE,

  MACHINE_WORK,

  MACHINE_HANDLING
};


MachineState machineState =
  MACHINE_IDLE;


uint32_t stateStartMs =
  0;


uint32_t lastStitchMs =
  0;


uint64_t workAccumMs =
  0;


uint64_t idleAccumMs =
  0;


uint64_t handlingAccumMs =
  0;


uint32_t totalStitch =
  0;


// ============================================================
// SENSOR STATE
// ============================================================

bool previousSensorActive =
  false;


bool pulseActive =
  false;


bool trimLatched =
  false;


bool trimRearmRequired =
  false;


bool trimHoldIdleNotified =
  false;


bool postTrimReleaseReady =
  false;


bool postTrimPulseActive =
  false;


uint32_t trimHoldStartMs =
  0;


uint32_t sensorReleaseStartMs =
  0;


uint32_t postTrimPulseStartMs =
  0;


uint32_t pulseStartMs =
  0;


// ============================================================
// ZIGBEE REGISTRATION NVS
// ============================================================

void restoreZigbeeRegistration() {

  zigbeeRegistered =
    preferences.getBool(
      "zb_registered",
      false
    );


  Serial.print(
    "[ZIGBEE] REGISTERED NVS : "
  );


  Serial.println(
    zigbeeRegistered
      ? "YES"
      : "NO"
  );
}


void saveZigbeeRegistration() {

  if (
    zigbeeRegistered
  ) {

    return;
  }


  zigbeeRegistered =
    true;


  preferences.putBool(
    "zb_registered",
    true
  );


  Serial.println(
    "[ZIGBEE] FIRST JOIN SUCCESS"
  );


  Serial.println(
    "[ZIGBEE] REGISTERED SAVED TO NVS"
  );


  Serial.println(
    "[ZIGBEE] Z2M AUTO INTERVIEW / CONFIGURE EXPECTED"
  );
}


// ============================================================
// ZIGBEE CONNECTION MANAGER
// ============================================================

void updateZigbeeConnectionState() {

  uint32_t now =
    millis();


  bool connected =
    Zigbee.connected();


  // ----------------------------------------------------------
  // JUST CONNECTED / REJOINED
  // ----------------------------------------------------------

  if (
    connected &&
    !zigbeeWasConnected
  ) {

    zigbeeWasConnected =
      true;


    zigbeePermitHint =
      false;


    if (
      !zigbeeRegistered
    ) {

      saveZigbeeRegistration();

    } else {

      Serial.println(
        "[ZIGBEE] REJOIN SUCCESS"
      );
    }


    Serial.println(
      "[ZIGBEE] CONNECTED"
    );


    if (
      !deviceBootSent
    ) {

      Serial.println(
        "[ZIGBEE] WAIT Z2M INTERVIEW / AUTO BIND EP10/11/12/14"
      );
    }


    // Supaya boot info dikirim ulang setelah reconnect.
    deviceBootSent =
      false;


    // Heartbeat berikutnya boleh langsung berjalan.
    lastHeartbeatMs =
      0;


    return;
  }


  // ----------------------------------------------------------
  // CONNECTION LOST
  // ----------------------------------------------------------

  if (
    !connected &&
    zigbeeWasConnected
  ) {

    zigbeeWasConnected =
      false;


    deviceBootSent =
      false;


    Serial.println(
      "[ZIGBEE] CONNECTION LOST"
    );


    if (
      zigbeeRegistered
    ) {

      Serial.println(
        "[ZIGBEE] AUTO REJOINING..."
      );
    }


    return;
  }


  // ----------------------------------------------------------
  // NEW DEVICE - STILL NOT JOINED
  // ----------------------------------------------------------

  if (
    !connected &&
    !zigbeeRegistered
  ) {

    if (
      !zigbeePermitHint &&
      now -
      zigbeeStartMs >=
      ZIGBEE_NEW_DEVICE_HINT_MS
    ) {

      zigbeePermitHint =
        true;


      Serial.println(
        "[ZIGBEE] NEW DEVICE NOT JOINED"
      );


      Serial.println(
        "[ZIGBEE] JOIN FAILED - CHECK PERMIT JOIN / COORDINATOR"
      );


      Serial.println(
        "[ZIGBEE] SCAN RANGE : CHANNEL 11-26"
      );


      Serial.println(
        "[ZIGBEE] NETWORK STEERING RETRIES AUTOMATICALLY"
      );
    }


    if (
      now -
      lastZigbeeWaitLogMs >=
      ZIGBEE_WAIT_LOG_MS
    ) {

      lastZigbeeWaitLogMs =
        now;


      Serial.println(
        "[ZIGBEE] WAITING FOR PERMIT JOIN..."
      );
    }
  }
}


// ============================================================
// SAVE SESSION
// ============================================================

void saveOperatorSession() {

  preferences.putBool(
    "logged",
    operatorLoggedIn
  );


  preferences.putString(
    "rfid",
    currentRFID
  );


  preferences.putString(
    "nik",
    currentNIK
  );


  preferences.putString(
    "name",
    currentOperatorName
  );


  preferences.putString(
    "process",
    currentProcessName
  );


  preferences.putString(
    "branch",
    currentBranch
  );
}


// ============================================================
// CLEAR SESSION
// ============================================================

void clearOperatorSession() {

  preferences.putBool(
    "logged",
    false
  );


  preferences.remove(
    "rfid"
  );


  preferences.remove(
    "nik"
  );


  preferences.remove(
    "name"
  );


  preferences.remove(
    "process"
  );


  preferences.remove(
    "branch"
  );
}


// ============================================================
// RESTORE SESSION
// ============================================================

void restoreOperatorSession() {

  bool savedLogged =
    preferences.getBool(
      "logged",
      false
    );


  if (
    !savedLogged
  ) {

    operatorLoggedIn =
      false;

    currentRFID =
      "";

    currentNIK =
      "";

    currentOperatorName =
      "";

    currentProcessName =
      "";

    currentBranch =
      "";

    return;
  }


  String savedRFID =
    preferences.getString(
      "rfid",
      ""
    );


  String savedNIK =
    preferences.getString(
      "nik",
      ""
    );


  String savedName =
    preferences.getString(
      "name",
      ""
    );


  String savedProcess =
    preferences.getString(
      "process",
      ""
    );


  String savedBranch =
    preferences.getString(
      "branch",
      ""
    );


  if (
    savedRFID.length() == 0 ||
    savedNIK.length() == 0
  ) {

    operatorLoggedIn =
      false;

    currentRFID =
      "";

    currentNIK =
      "";

    currentOperatorName =
      "";

    currentProcessName =
      "";

    currentBranch =
      "";


    clearOperatorSession();


    return;
  }


  operatorLoggedIn =
    true;


  currentRFID =
    savedRFID;


  currentNIK =
    savedNIK;


  currentOperatorName =
    savedName;


  currentProcessName =
    savedProcess;


  currentBranch =
    savedBranch;
}


// ============================================================
// BUZZER
// ============================================================

void buzzerWrite(
  bool on
) {

  digitalWrite(
    BUZZER_PIN,
    on
      ? HIGH
      : LOW
  );
}


void startTrimBuzzer() {

  buzzerBeepsRemaining =
    operatorLoggedIn
      ? 1
      : 2;


  buzzerOn =
    true;


  buzzerChangedMs =
    millis();


  buzzerWrite(
    true
  );
}


void updateBuzzer() {

  if (
    buzzerOn
  ) {

    if (
      millis() -
      buzzerChangedMs >=
      BUZZER_ON_MS
    ) {

      buzzerWrite(
        false
      );


      buzzerOn =
        false;


      buzzerChangedMs =
        millis();


      if (
        buzzerBeepsRemaining > 0
      ) {

        buzzerBeepsRemaining--;
      }
    }


    return;
  }


  if (
    buzzerBeepsRemaining > 0 &&

    millis() -
      buzzerChangedMs >=
      BUZZER_OFF_MS
  ) {

    buzzerWrite(
      true
    );


    buzzerOn =
      true;


    buzzerChangedMs =
      millis();
  }
}


// ============================================================
// SENSOR
// ============================================================

bool sensorActive() {

  return (
    digitalRead(
      SENSOR_PIN
    ) ==
    LOW
  );
}


// ============================================================
// MACHINE STATE TEXT
// ============================================================

const char *machineStateText() {

  if (
    machineState ==
    MACHINE_WORK
  ) {

    return "work";
  }


  if (
    machineState ==
    MACHINE_HANDLING
  ) {

    return "handling";
  }


  return "idle";
}


// ============================================================
// DEVICE BOOT
// ============================================================

bool sendDeviceBootInfo() {

  if (
    !Zigbee.connected()
  ) {

    return false;
  }


  char payload[241];


  snprintf(
    payload,
    sizeof(payload),

    "%s|%u|%s|%s|%u|%.16s|%.32s|%.48s|%.48s|%.24s",

    FIRMWARE_VERSION,

    (unsigned int)
      SENSOR_TYPE,

    COORDINATOR_ID,

    machineStateText(),

    operatorLoggedIn
      ? 1
      : 0,

    currentRFID.c_str(),

    currentNIK.c_str(),

    currentOperatorName.c_str(),

    currentProcessName.c_str(),

    currentBranch.c_str()
  );


  bool sent =
    zbAuth.sendTextCommand(

      CMD_DEVICE_BOOT,

      payload,

      "BOOT"
    );


  if (
    sent
  ) {

    deviceBootSent =
      true;


    lastHeartbeatMs =
      millis();
  }


  return sent;
}


// ============================================================
// HEARTBEAT
// ============================================================

void sendHeartbeatIfDue() {

  if (
    !deviceBootSent ||
    !Zigbee.connected()
  ) {

    return;
  }


  uint32_t now =
    millis();


  if (
    now -
    lastHeartbeatMs <
    HEARTBEAT_INTERVAL_MS
  ) {

    return;
  }


  char payload[96];


  snprintf(
    payload,
    sizeof(payload),

    "%lu|%s|%u|%s",

    (unsigned long)(
      now /
      1000UL
    ),

    machineStateText(),

    operatorLoggedIn
      ? 1
      : 0,

    currentRFID.c_str()
  );


  if (
    zbAuth.sendTextCommand(

      CMD_HEARTBEAT,

      payload,

      "HEARTBEAT"
    )
  ) {

    lastHeartbeatMs =
      now;
  }
}


// ============================================================
// SEND MACHINE STATE
// ============================================================

void sendMachineStateUpdate() {

  if (
    !deviceBootSent ||
    !Zigbee.connected()
  ) {

    return;
  }


  zbAuth.sendTextCommand(

    CMD_MACHINE_STATE,

    machineStateText(),

    "STATE"
  );
}


// ============================================================
// PRODUCTION RESET
// ============================================================

void resetProductionCycle(
  uint32_t now
) {

  totalStitch =
    0;


  workAccumMs =
    0;


  idleAccumMs =
    0;


  handlingAccumMs =
    0;


  machineState =
    MACHINE_IDLE;


  stateStartMs =
    now;


  lastStitchMs =
    0;
}


void resetSensorTracking() {

  previousSensorActive =
    sensorActive();


  pulseActive =
    false;


  trimLatched =
    false;


  trimRearmRequired =
    false;


  trimHoldIdleNotified =
    false;


  postTrimReleaseReady =
    false;


  postTrimPulseActive =
    false;


  trimHoldStartMs =
    0;


  sensorReleaseStartMs =
    0;


  postTrimPulseStartMs =
    0;


  pulseStartMs =
    0;
}


// ============================================================
// MACHINE STATE CHANGE
// ============================================================

void changeMachineState(
  MachineState newState,
  uint32_t eventTime
) {

  if (
    newState ==
    machineState
  ) {

    return;
  }


  uint32_t duration =
    eventTime -
    stateStartMs;


  if (
    machineState ==
    MACHINE_WORK
  ) {

    workAccumMs +=
      duration;

  } else if (
    machineState ==
    MACHINE_HANDLING
  ) {

    handlingAccumMs +=
      duration;

  } else {

    idleAccumMs +=
      duration;
  }


  machineState =
    newState;


  stateStartMs =
    eventTime;


  sendMachineStateUpdate();
}


// ============================================================
// WORK -> HANDLING
// ============================================================

void updateMachineState() {

  if (
    machineState !=
    MACHINE_WORK
  ) {

    return;
  }


  if (
    lastStitchMs ==
    0
  ) {

    return;
  }


  uint32_t now =
    millis();


  if (
    now -
    lastStitchMs >=
    HANDLING_CONFIRM_MS
  ) {

    // Konfirmasi menunggu 2 detik, tetapi HANDLING dihitung
    // mulai dari stitch terakhir agar gap tidak masuk WORK.
    changeMachineState(
      MACHINE_HANDLING,
      lastStitchMs
    );
  }
}


// ============================================================
// WORK / IDLE / HANDLING
// ============================================================

uint64_t getWorkMs(
  uint32_t now
) {

  uint64_t result =
    workAccumMs;


  if (
    machineState ==
    MACHINE_WORK
  ) {

    result +=
      now -
      stateStartMs;
  }


  return result;
}


uint64_t getIdleMs(
  uint32_t now
) {

  uint64_t result =
    idleAccumMs;


  if (
    machineState ==
    MACHINE_IDLE
  ) {

    result +=
      now -
      stateStartMs;
  }


  return result;
}


uint64_t getHandlingMs(
  uint32_t now
) {

  uint64_t result =
    handlingAccumMs;


  if (
    machineState ==
    MACHINE_HANDLING
  ) {

    result +=
      now -
      stateStartMs;
  }


  return result;
}


uint32_t msToSeconds(
  uint64_t valueMs
) {

  return (uint32_t)(

    (valueMs + 500ULL) /

    1000ULL
  );
}


// ============================================================
// LCD WRITE LINE
// ============================================================

void lcdWriteLine(
  uint8_t row,
  const String &text
) {

  char line[
    LCD_COLUMNS + 1
  ];


  memset(
    line,
    ' ',
    LCD_COLUMNS
  );


  line[
    LCD_COLUMNS
  ] =
    '\0';


  size_t len =
    text.length();


  if (
    len >
    LCD_COLUMNS
  ) {

    len =
      LCD_COLUMNS;
  }


  for (
    size_t i = 0;
    i < len;
    i++
  ) {

    line[i] =
      text[i];
  }


  lcd.setCursor(
    0,
    row
  );


  lcd.print(
    line
  );
}


// ============================================================
// LCD OPERATOR TEXT
// ============================================================

String lcdOperatorText() {

  if (
    !operatorLoggedIn
  ) {

    return "NO OPERATOR";
  }


  if (
    currentOperatorName.length() > 0
  ) {

    return currentOperatorName;
  }


  if (
    currentNIK.length() > 0
  ) {

    return currentNIK;
  }


  return currentRFID;
}


// ============================================================
// LCD
// ============================================================

void updateLCD() {

  uint32_t now =
    millis();


  if (
    now -
    lastLcdRefreshMs <
    LCD_REFRESH_MS
  ) {

    return;
  }


  lastLcdRefreshMs =
    now;


  // ==========================================================
  // ZIGBEE COMMISSIONING / REJOIN STATUS
  // ==========================================================

  if (
    !Zigbee.connected()
  ) {

    if (
      !zigbeeRegistered &&
      zigbeePermitHint
    ) {

      lcdWriteLine(
        0,
        "ERR PERMIT JOIN"
      );


      lcdWriteLine(
        1,
        "ENABLE Z2M JOIN"
      );


      return;
    }


    if (
      !zigbeeRegistered
    ) {

      lcdWriteLine(
        0,
        "ZB JOINING..."
      );


      lcdWriteLine(
        1,
        "PERMIT JOIN ON"
      );


      return;
    }


    lcdWriteLine(
      0,
      "ZB REJOINING..."
    );


    lcdWriteLine(
      1,
      "PROD ACTIVE"
    );


    return;
  }


  if (authPending) {

    if (pendingAction == AUTH_LOGOUT) {
      lcdWriteLine(0, "LOGOUT...");
    } else if (pendingSwitch) {
      lcdWriteLine(0, "SWITCH USER...");
    } else {
      lcdWriteLine(0, "LOGIN...");
    }

    lcdWriteLine(1, pendingRFID);
    return;
  }


  if ((int32_t)(lastTrimLcdUntil - now) > 0) {

    char row0[17];
    char row1[17];

    snprintf(row0, sizeof(row0), "W:%lu I:%lu H:%lu",
      (unsigned long)lastTrimWorkSec,
      (unsigned long)lastTrimIdleSec,
      (unsigned long)lastTrimHandlingSec);

    snprintf(row1, sizeof(row1), "ST:%lu TRIM",
      (unsigned long)lastTrimStitch);

    lcdWriteLine(0, String(row0));
    lcdWriteLine(1, String(row1));
    return;
  }


  if (now - lcdPageStartedMs >= LCD_PAGE_TIME_MS) {
    lcdPage++;
    if (lcdPage >= 4) lcdPage = 0;
    lcdPageStartedMs = now;
  }


  uint32_t workSec = msToSeconds(getWorkMs(now));
  uint32_t idleSec = msToSeconds(getIdleMs(now));
  uint32_t handlingSec = msToSeconds(getHandlingMs(now));


  if (lcdPage == 0) {
    lcdWriteLine(0, operatorLoggedIn ? lcdOperatorText() : "NO OPERATOR");
    lcdWriteLine(1, operatorLoggedIn ? currentProcessName : "TAP RFID CARD");
    return;
  }


  if (lcdPage == 1) {
    String stateText = "STATE:";
    stateText += machineStateText();

    char row1[17];
    snprintf(row1, sizeof(row1), "W:%lu I:%lu H:%lu",
      (unsigned long)workSec,
      (unsigned long)idleSec,
      (unsigned long)handlingSec);

    lcdWriteLine(0, stateText);
    lcdWriteLine(1, String(row1));
    return;
  }


  if (lcdPage == 2) {
    char row0[17];
    snprintf(row0, sizeof(row0), "STITCH:%lu", (unsigned long)totalStitch);

    String row1 = "RFID:";
    row1 += operatorLoggedIn ? currentRFID : "-";

    lcdWriteLine(0, String(row0));
    lcdWriteLine(1, row1);
    return;
  }


  String row0 = Zigbee.connected() ? "ZB:OK " : "ZB:OFF ";
  row0 += machineStateText();

  String row1 = "NIK:";
  row1 += operatorLoggedIn ? currentNIK : "-";

  lcdWriteLine(0, row0);
  lcdWriteLine(1, row1);
}


// ============================================================
// REGISTER STITCH
// ============================================================

void registerStitch(
  uint32_t stitchTime
) {

  // Stitch pertama setelah TRIM mengakhiri IDLE
  // dan memulai WORK kembali.
  if (
    machineState == MACHINE_IDLE ||
    machineState == MACHINE_HANDLING
  ) {

    changeMachineState(
      MACHINE_WORK,
      stitchTime
    );
  }


  lastStitchMs =
    stitchTime;


  totalStitch++;
}


// ============================================================
// SEND ANALOG
// ============================================================

void sendAnalog(
  ZigbeeAnalog &endpoint,
  uint8_t sourceEndpoint,
  uint32_t value
) {

  if (
    !Zigbee.connected()
  ) {

    Serial.print(
      "[PRODUCTION EP"
    );

    Serial.print(
      sourceEndpoint
    );

    Serial.println(
      "] ZIGBEE NOT CONNECTED"
    );

    return;
  }


  // ==========================================================
  // 1. UPDATE LOCAL ANALOG INPUT ATTRIBUTE
  // ==========================================================

  bool setOk =
    endpoint.setAnalogInput(
      (float)value
    );


  if (
    !setOk
  ) {

    Serial.print(
      "[PRODUCTION EP"
    );

    Serial.print(
      sourceEndpoint
    );

    Serial.println(
      "] SET ANALOG FAILED"
    );

    return;
  }


  // ==========================================================
  // 2. SEND REPORT DIRECTLY TO COORDINATOR
  //
  // Tidak lagi bergantung pada binding table.
  //
  // Destination:
  //   Coordinator short address = 0x0000
  //   Coordinator endpoint      = 1
  //
  // Source endpoint:
  //   EP10 = WORK
  //   EP11 = IDLE
  //   EP12 = STITCH (trigger terakhir)
  //   EP14 = HANDLING
  //
  // simon.mjs TIDAK perlu diubah karena Z2M tetap menerima
  // genAnalogInput / attributeReport / presentValue.
  // ==========================================================

  esp_zb_zcl_report_attr_cmd_t reportCmd = {};

  reportCmd.zcl_basic_cmd.dst_addr_u.addr_short =
    0x0000;

  reportCmd.zcl_basic_cmd.dst_endpoint =
    1;

  reportCmd.zcl_basic_cmd.src_endpoint =
    sourceEndpoint;

  reportCmd.address_mode =
    ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;

  reportCmd.clusterID =
    ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT;

  reportCmd.attributeID =
    ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID;

  reportCmd.direction =
    ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;

  esp_zb_lock_acquire(
    portMAX_DELAY
  );

  esp_err_t reportStatus =
    esp_zb_zcl_report_attr_cmd_req(
      &reportCmd
    );

  esp_zb_lock_release();


  Serial.print(
    "[PRODUCTION EP"
  );

  Serial.print(
    sourceEndpoint
  );

  Serial.print(
    "] VALUE="
  );

  Serial.print(
    value
  );

  Serial.print(
    " STATUS=0x"
  );

  Serial.println(
    (uint32_t)reportStatus,
    HEX
  );
}


// ============================================================
// SEND PRODUCTION
// ============================================================

void sendProductionCycle(
  uint32_t workSec,
  uint32_t idleSec,
  uint32_t handlingSec,
  uint32_t stitch
) {

  if (workSec == 0 && idleSec == 0 && handlingSec == 0) {
    Serial.println("[ZIGBEE] SKIP: EMPTY CYCLE");
    return;
  }

  if (!Zigbee.connected()) {
    Serial.println("[ZIGBEE] NOT CONNECTED");
    return;
  }

  sendAnalog(zbWork, EP_WORK, workSec);
  delay(20);

  sendAnalog(zbIdle, EP_IDLE, idleSec);
  delay(20);

  sendAnalog(zbHandling, EP_HANDLING, handlingSec);
  delay(20);

  // EP12 selalu terakhir = trigger INSERT Node-RED.
  sendAnalog(zbStitch, EP_STITCH, stitch);
}


// ============================================================
// REGISTER TRIM
// ============================================================

void registerTrim() {

  uint32_t now = millis();

  updateMachineState();

  // Trim menutup WORK atau HANDLING.
  if (machineState != MACHINE_IDLE) {
    changeMachineState(MACHINE_IDLE, now);
  }

  uint32_t workSec = msToSeconds(getWorkMs(now));
  uint32_t idleSec = msToSeconds(getIdleMs(now));
  uint32_t handlingSec = msToSeconds(getHandlingMs(now));
  uint32_t stitchSnapshot = totalStitch;

  lastTrimWorkSec = workSec;
  lastTrimIdleSec = idleSec;
  lastTrimHandlingSec = handlingSec;
  lastTrimStitch = stitchSnapshot;
  lastTrimLcdUntil = millis() + LCD_TRIM_HOLD_MS;

  startTrimBuzzer();

  Serial.println();
  Serial.println("====================================");
  Serial.println("TRIM");
  Serial.print("RFID     : "); Serial.println(currentRFID);
  Serial.print("NIK      : "); Serial.println(currentNIK);
  Serial.print("NAME     : "); Serial.println(currentOperatorName);
  Serial.print("PROCESS  : "); Serial.println(currentProcessName);
  Serial.print("BRANCH   : "); Serial.println(currentBranch);
  Serial.print("WORK     : "); Serial.print(workSec); Serial.println(" sec");
  Serial.print("IDLE     : "); Serial.print(idleSec); Serial.println(" sec");
  Serial.print("HANDLING : "); Serial.print(handlingSec); Serial.println(" sec");
  Serial.print("STITCH   : "); Serial.println(stitchSnapshot);
  Serial.println("====================================");

  sendProductionCycle(workSec, idleSec, handlingSec, stitchSnapshot);

  // Cycle baru setelah trim selalu IDLE.
  resetProductionCycle(now);

  // ==========================================================
  // TRIM FAST RE-ARM
  //
  // Setelah satu trim terdeteksi, trim berikutnya tetap dikunci.
  // Cycle baru tetap IDLE. Sensor hanya perlu HIGH stabil singkat,
  // lalu pulse pendek pertama akan langsung dianggap STITCH/WORK.
  // Pulse panjang selama fase ini tetap diabaikan sebagai TRIM HOLD.
  // ==========================================================

  trimLatched =
    true;

  trimRearmRequired =
    true;


  // Mulai penguncian TRIM HOLD.
  // Jika sensor tetap aktif >2 detik, tidak ada TRIM baru.
  // Machine sudah IDLE sehingga idle_time terus bertambah.
  trimHoldStartMs =
    now;

  trimHoldIdleNotified =
    false;


  postTrimReleaseReady =
    false;


  postTrimPulseActive =
    false;


  sensorReleaseStartMs =
    0;


  postTrimPulseStartMs =
    0;


  pulseActive =
    false;
}


// ============================================================
// SEND PARTIAL CYCLE BEFORE LOGOUT
// ============================================================

bool sendCurrentCycleBeforeLogout() {

  uint32_t now = millis();

  updateMachineState();

  if (machineState != MACHINE_IDLE) {
    changeMachineState(MACHINE_IDLE, now);
  }

  uint32_t workSec = msToSeconds(getWorkMs(now));
  uint32_t idleSec = msToSeconds(getIdleMs(now));
  uint32_t handlingSec = msToSeconds(getHandlingMs(now));
  uint32_t stitchSnapshot = totalStitch;

  if (workSec == 0 && idleSec == 0 && handlingSec == 0) {
    return false;
  }

  Serial.println();
  Serial.println("====================================");
  Serial.println("LOGOUT - SEND CURRENT CYCLE");
  Serial.print("WORK     : "); Serial.println(workSec);
  Serial.print("IDLE     : "); Serial.println(idleSec);
  Serial.print("HANDLING : "); Serial.println(handlingSec);
  Serial.print("STITCH   : "); Serial.println(stitchSnapshot);
  Serial.println("====================================");

  sendProductionCycle(workSec, idleSec, handlingSec, stitchSnapshot);

  resetProductionCycle(now);
  resetSensorTracking();

  return true;
}


// ============================================================
// SENSOR PROCESS
// ============================================================

void processSensor() {

  bool active =
    sensorActive();


  uint32_t now =
    millis();


  // ==========================================================
  // POST-TRIM FAST RE-ARM
  // ==========================================================
  //
  // Setelah TRIM:
  // - machine tetap IDLE
  // - LOW panjang tidak boleh menjadi TRIM kedua
  // - HIGH stabil 5ms menandakan sensor sudah release
  // - pulse pendek pertama setelah release langsung menjadi STITCH
  //   dan mengubah IDLE -> WORK
  // ==========================================================

  if (
    trimRearmRequired
  ) {

    // ----------------------------------------------------------
    // SENSOR HIGH
    // ----------------------------------------------------------
    if (
      !active
    ) {

      if (
        sensorReleaseStartMs == 0
      ) {

        sensorReleaseStartMs =
          now;
      }


      // Kandidat pulse post-trim selesai (LOW -> HIGH).
      if (
        previousSensorActive &&
        postTrimPulseActive
      ) {

        uint32_t postTrimPulseMs =
          now -
          postTrimPulseStartMs;


        postTrimPulseActive =
          false;


        // Pulse pendek valid = stitch pertama cycle baru.
        if (
          postTrimPulseMs >=
            MIN_STITCH_PULSE_MS &&

          postTrimPulseMs <
            TRIM_DETECT_MS
        ) {

          trimRearmRequired =
            false;

          trimLatched =
            false;

          trimHoldIdleNotified =
            false;

          postTrimReleaseReady =
            false;

          trimHoldStartMs =
            0;

          sensorReleaseStartMs =
            0;

          postTrimPulseStartMs =
            0;

          pulseActive =
            false;

          pulseStartMs =
            0;


          Serial.print(
            "[FIRST STITCH AFTER TRIM] "
          );

          Serial.print(
            postTrimPulseMs
          );

          Serial.println(
            " ms -> WORK"
          );


          registerStitch(
            now
          );


          previousSensorActive =
            active;


          return;
        }
      }


      if (
        now -
        sensorReleaseStartMs >=
        TRIM_RELEASE_DEBOUNCE_MS
      ) {

        postTrimReleaseReady =
          true;
      }


      previousSensorActive =
        active;


      return;
    }


    // ----------------------------------------------------------
    // SENSOR LOW
    // ----------------------------------------------------------

    sensorReleaseStartMs =
      0;


    // HIGH -> LOW sesudah sensor release = kandidat stitch.
    if (
      postTrimReleaseReady &&
      !previousSensorActive &&
      !postTrimPulseActive
    ) {

      postTrimPulseActive =
        true;

      postTrimPulseStartMs =
        now;
    }


    // Jika LOW menjadi panjang, bukan stitch.
    // Tetap IDLE dan tunggu release berikutnya.
    if (
      postTrimPulseActive &&
      now -
      postTrimPulseStartMs >=
      TRIM_DETECT_MS
    ) {

      postTrimPulseActive =
        false;

      postTrimReleaseReady =
        false;

      postTrimPulseStartMs =
        0;


      if (
        DEBUG_PULSE_TIMING
      ) {

        Serial.println(
          "[POST-TRIM LONG LOW] KEEP IDLE"
        );
      }
    }


    if (
      !trimHoldIdleNotified &&
      trimHoldStartMs != 0 &&
      now -
      trimHoldStartMs >=
      TRIM_HOLD_IDLE_MS
    ) {

      trimHoldIdleNotified =
        true;


      Serial.println(
        "[TRIM HOLD >2S] IDLE CONTINUE UNTIL FIRST STITCH"
      );
    }


    previousSensorActive =
      active;


    return;
  }


  // ==========================================================
  // HIGH -> LOW
  // ==========================================================

  if (
    active &&
    !previousSensorActive
  ) {

    pulseActive =
      true;


    pulseStartMs =
      now;


    trimLatched =
      false;
  }


  // ==========================================================
  // LONG LOW = TRIM
  // ==========================================================

  if (
    active &&
    pulseActive &&
    !trimLatched &&

    (
      now -
      pulseStartMs >=
      TRIM_DETECT_MS
    )
  ) {

    trimLatched =
      true;


    if (
      DEBUG_PULSE_TIMING
    ) {

      Serial.print(
        "[PULSE] TRIM LOW >= "
      );


      Serial.print(
        TRIM_DETECT_MS
      );


      Serial.println(
        " ms"
      );
    }


    registerTrim();


    // registerTrim() mengaktifkan trimRearmRequired.
    // Stop proses sensor pada loop ini agar event yang sama
    // tidak diterjemahkan lagi sebagai stitch / release.
    previousSensorActive =
      active;


    return;
  }


  // ==========================================================
  // LOW -> HIGH
  // ==========================================================

  if (
    !active &&
    previousSensorActive
  ) {

    if (
      pulseActive &&
      !trimLatched
    ) {

      uint32_t pulseMs =
        now -
        pulseStartMs;


      if (
        pulseMs >=
          MIN_STITCH_PULSE_MS &&

        pulseMs <
          TRIM_DETECT_MS
      ) {

        if (
          DEBUG_PULSE_TIMING
        ) {

          Serial.print(
            "[PULSE] STITCH LOW = "
          );


          Serial.print(
            pulseMs
          );


          Serial.println(
            " ms"
          );
        }


        registerStitch(
          now
        );
      }
    }


    pulseActive =
      false;


    trimLatched =
      false;
  }


  previousSensorActive =
    active;
}

// ============================================================
// RFID HEX HELPERS
// ============================================================

int hexNibble(
  char c
) {

  if (
    c >= '0' &&
    c <= '9'
  ) {

    return c - '0';
  }


  if (
    c >= 'A' &&
    c <= 'F'
  ) {

    return c - 'A' + 10;
  }


  if (
    c >= 'a' &&
    c <= 'f'
  ) {

    return c - 'a' + 10;
  }


  return -1;
}


bool parseHexByte(
  char high,
  char low,
  uint8_t &value
) {

  int h =
    hexNibble(
      high
    );


  int l =
    hexNibble(
      low
    );


  if (
    h < 0 ||
    l < 0
  ) {

    return false;
  }


  value =
    (uint8_t)(

      (h << 4) |

      l
    );


  return true;
}


// ============================================================
// RFID DECODE
// ============================================================

bool decodeRFIDFrame(
  const char *frame,
  char *rfidOut,
  size_t rfidOutSize
) {

  if (
    !frame ||
    !rfidOut ||
    rfidOutSize < 11
  ) {

    return false;
  }


  // ==========================================================
  // VALIDATE HEX
  // ==========================================================

  for (
    uint8_t i = 0;
    i < 12;
    i++
  ) {

    if (
      hexNibble(
        frame[i]
      ) < 0
    ) {

      return false;
    }
  }


  // ==========================================================
  // CHECKSUM
  // ==========================================================

  uint8_t checksum =
    0;


  for (
    uint8_t i = 0;
    i < 10;
    i += 2
  ) {

    uint8_t value;


    if (
      !parseHexByte(

        frame[i],

        frame[i + 1],

        value
      )
    ) {

      return false;
    }


    checksum ^=
      value;
  }


  uint8_t receivedChecksum;


  if (
    !parseHexByte(

      frame[10],

      frame[11],

      receivedChecksum
    )
  ) {

    return false;
  }


  if (
    checksum !=
    receivedChecksum
  ) {

    return false;
  }


  // ==========================================================
  // TAG
  // ==========================================================

  char tagHex[9];


  memcpy(
    tagHex,
    &frame[2],
    8
  );


  tagHex[8] =
    '\0';


  uint32_t cardDecimal =
    strtoul(
      tagHex,
      nullptr,
      16
    );


  snprintf(

    rfidOut,

    rfidOutSize,

    "%010lu",

    (unsigned long)
      cardDecimal
  );


  return true;
}


// ============================================================
// START AUTH
// ============================================================

void startAuth(
  const String &rfid,
  uint8_t action
) {

  if (
    authPending
  ) {

    return;
  }


  zbAuth.clearResponse();


  pendingRFID =
    rfid;


  pendingAction =
    action;


  pendingSwitch =
    (
      action == AUTH_LOGIN &&
      operatorLoggedIn &&
      rfid != currentRFID
    );


  authPending =
    true;


  authStartedMs =
    millis();


  Serial.println();


  Serial.println(
    "===================================="
  );


  if (
    action ==
    AUTH_LOGIN
  ) {

    Serial.println(
      "LOGIN REQUEST"
    );

  } else {

    Serial.println(
      "LOGOUT REQUEST"
    );
  }


  Serial.print(
    "RFID : "
  );


  Serial.println(
    rfid
  );


  if (
    !zbAuth.sendAuthRequest(

      rfid.c_str(),

      action
    )
  ) {

    Serial.println(
      "AUTH SEND FAILED"
    );


    authPending =
      false;


    pendingRFID =
      "";


    pendingSwitch =
      false;


    Serial.println(
      "===================================="
    );


    return;
  }


  Serial.println(
    "WAITING NODE-RED..."
  );


  Serial.println(
    "===================================="
  );
}


// ============================================================
// RFID TAP
// ============================================================

void handleRFIDTap(
  const String &rfid
) {

  // ==========================================================
  // AUTH BUSY
  // ==========================================================

  if (
    authPending
  ) {

    Serial.println();


    Serial.println(
      "===================================="
    );


    Serial.println(
      "AUTH BUSY"
    );


    Serial.println(
      "===================================="
    );


    return;
  }


  // ==========================================================
  // BELUM LOGIN
  // ==========================================================

  if (
    !operatorLoggedIn
  ) {

    startAuth(
      rfid,
      AUTH_LOGIN
    );


    return;
  }


  // ==========================================================
  // CARD SAMA = LOGOUT
  // ==========================================================

  if (
    rfid ==
    currentRFID
  ) {

    bool cycleSent =
      sendCurrentCycleBeforeLogout();


    if (cycleSent) {

      // Beri waktu report produksi masuk sebelum server
      // mengosongkan session operator saat logout.
      delay(300);
    }


    startAuth(
      rfid,
      AUTH_LOGOUT
    );


    return;
  }


  // ==========================================================
  // CARD BERBEDA = SWITCH
  // ==========================================================

  Serial.println();


  Serial.println(
    "===================================="
  );


  Serial.println(
    "OPERATOR SWITCH REQUEST"
  );


  Serial.print(
    "FROM RFID : "
  );


  Serial.println(
    currentRFID
  );


  Serial.print(
    "TO RFID   : "
  );


  Serial.println(
    rfid
  );


  Serial.println(
    "===================================="
  );


  startAuth(
    rfid,
    AUTH_LOGIN
  );
}


// ============================================================
// RFID PROCESS
// ============================================================

void processRFID() {

  while (
    RFIDSerial.available()
  ) {

    uint8_t b =
      RFIDSerial.read();


    // ========================================================
    // STX
    // ========================================================

    if (
      b == 0x02
    ) {

      rfidFrameActive =
        true;


      rfidFrameIndex =
        0;


      continue;
    }


    if (
      !rfidFrameActive
    ) {

      continue;
    }


    // ========================================================
    // ETX
    // ========================================================

    if (
      b == 0x03
    ) {

      if (
        rfidFrameIndex ==
        12
      ) {

        char rfid[11];


        if (
          decodeRFIDFrame(

            rfidFrame,

            rfid,

            sizeof(rfid)
          )
        ) {

          lastRFIDFrameMs =
            millis();


          if (
            rfidReadyForTap
          ) {

            rfidReadyForTap =
              false;


            handleRFIDTap(
              String(rfid)
            );
          }
        }
      }


      rfidFrameActive =
        false;


      rfidFrameIndex =
        0;


      continue;
    }


    // ========================================================
    // DATA
    // ========================================================

    if (
      rfidFrameIndex <
      12
    ) {

      rfidFrame[
        rfidFrameIndex++
      ] =
        (char)b;

    } else {

      rfidFrameActive =
        false;


      rfidFrameIndex =
        0;
    }
  }


  // ==========================================================
  // CARD REMOVED
  // ==========================================================

  if (
    !rfidReadyForTap &&

    lastRFIDFrameMs != 0 &&

    (
      millis() -
      lastRFIDFrameMs >=
      RFID_REMOVE_TIMEOUT_MS
    )
  ) {

    rfidReadyForTap =
      true;


    lastRFIDFrameMs =
      0;
  }
}


// ============================================================
// PROCESS AUTH RESPONSE
// ============================================================

void processAuthResponse() {

  // ==========================================================
  // RESPONSE READY
  // ==========================================================

  if (
    zbAuth.responseReady()
  ) {

    String nik =
      zbAuth.receivedNik();


    String operatorName =
      zbAuth.receivedOperatorName();


    String processName =
      zbAuth.receivedProcessName();


    String branch =
      zbAuth.receivedBranch();


    uint8_t status =
      zbAuth.receivedStatus();


    zbAuth.clearResponse();


    if (
      !authPending
    ) {

      Serial.println(
        "[AUTH] UNEXPECTED RESPONSE"
      );


      return;
    }


    // ========================================================
    // LOGIN RESPONSE
    // ========================================================

    if (
      pendingAction ==
      AUTH_LOGIN
    ) {

      // ======================================================
      // SUCCESS
      // ======================================================

      if (
        status == 1 &&
        nik.length() > 0
      ) {

        bool wasSwitch =
          pendingSwitch;


        String previousRFID =
          currentRFID;


        operatorLoggedIn =
          true;


        currentRFID =
          pendingRFID;


        currentNIK =
          nik;


        currentOperatorName =
          operatorName;


        currentProcessName =
          processName;


        currentBranch =
          branch;


        // ====================================================
        // SAVE SESSION
        // ====================================================

        saveOperatorSession();


        uint32_t now =
          millis();


        /*
          Login/switch memulai cycle baru.

          Data operator lama tidak tercampur
          dengan operator baru.
        */
        resetProductionCycle(
          now
        );


        resetSensorTracking();


        // ====================================================
        // SWITCH SUCCESS
        // ====================================================

        if (
          wasSwitch
        ) {

          Serial.println();


          Serial.println(
            "===================================="
          );


          Serial.println(
            "OPERATOR SWITCH SUCCESS"
          );


          Serial.print(
            "FROM RFID : "
          );


          Serial.println(
            previousRFID
          );


          Serial.print(
            "TO RFID   : "
          );


          Serial.println(
            currentRFID
          );


          Serial.println(
            "===================================="
          );
        }


        // ====================================================
        // LOGIN SUCCESS
        // ====================================================

        Serial.println();


        Serial.println(
          "===================================="
        );


        Serial.println(
          "LOGIN SUCCESS"
        );


        Serial.print(
          "RFID   : "
        );


        Serial.println(
          currentRFID
        );


        Serial.print(
          "NIK    : "
        );


        Serial.println(
          currentNIK
        );


        Serial.print(
          "NAME   : "
        );


        Serial.println(
          currentOperatorName
        );


        Serial.print(
          "PROCESS: "
        );

        Serial.println(
          currentProcessName
        );


        Serial.print(
          "BRANCH : "
        );

        Serial.println(
          currentBranch
        );


        Serial.print(
          "STATUS : "
        );


        Serial.println(
          status
        );


        Serial.println(
          "===================================="
        );
      }


      // ======================================================
      // LOGIN FAILED
      // ======================================================

      else {

        if (
          pendingSwitch
        ) {

          /*
            Switch gagal.

            Operator lama tetap login.
          */

          Serial.println();


          Serial.println(
            "===================================="
          );


          Serial.println(
            "OPERATOR SWITCH FAILED"
          );


          Serial.print(
            "RFID NEW : "
          );


          Serial.println(
            pendingRFID
          );


          Serial.print(
            "KEEP RFID: "
          );


          Serial.println(
            currentRFID
          );


          Serial.println(
            "CURRENT OPERATOR CONTINUES"
          );


          Serial.println(
            "===================================="
          );

        } else {

          operatorLoggedIn =
            false;


          currentRFID =
            "";


          currentNIK =
            "";


          currentOperatorName =
            "";

          currentProcessName =
            "";

          currentBranch =
            "";


          clearOperatorSession();


          resetProductionCycle(
            millis()
          );


          resetSensorTracking();


          Serial.println();


          Serial.println(
            "===================================="
          );


          Serial.println(
            "LOGIN FAILED"
          );


          Serial.print(
            "RFID   : "
          );


          Serial.println(
            pendingRFID
          );


          Serial.print(
            "STATUS : "
          );


          Serial.println(
            status
          );


          Serial.println(
            "RFID NOT REGISTERED"
          );


          Serial.println(
            "===================================="
          );
        }
      }
    }


    // ========================================================
    // LOGOUT RESPONSE
    // ========================================================

    else if (
      pendingAction ==
      AUTH_LOGOUT
    ) {

      if (
        status == 0
      ) {

        String logoutRFID =
          currentRFID;


        String logoutNIK =
          currentNIK;


        String logoutName =
          currentOperatorName;


        String logoutProcess =
          currentProcessName;


        String logoutBranch =
          currentBranch;


        operatorLoggedIn =
          false;


        currentRFID =
          "";


        currentNIK =
          "";


        currentOperatorName =
          "";

        currentProcessName =
          "";

        currentBranch =
          "";


        clearOperatorSession();


        /*
          Cycle sudah dikirim sebelum request logout dimulai.
          Reset ini hanya memastikan session berikutnya bersih.
        */
        resetProductionCycle(
          millis()
        );


        resetSensorTracking();


        Serial.println();


        Serial.println(
          "===================================="
        );


        Serial.println(
          "LOGOUT SUCCESS"
        );


        Serial.print(
          "RFID   : "
        );


        Serial.println(
          logoutRFID
        );


        Serial.print(
          "NIK    : "
        );


        Serial.println(
          logoutNIK
        );


        Serial.print(
          "NAME   : "
        );


        Serial.println(
          logoutName
        );


        Serial.print(
          "PROCESS: "
        );

        Serial.println(
          logoutProcess
        );


        Serial.print(
          "BRANCH : "
        );

        Serial.println(
          logoutBranch
        );


        Serial.print(
          "STATUS : "
        );


        Serial.println(
          status
        );


        Serial.println(
          "===================================="
        );

      } else {

        Serial.println();


        Serial.println(
          "===================================="
        );


        Serial.println(
          "LOGOUT FAILED"
        );


        Serial.print(
          "RFID   : "
        );


        Serial.println(
          currentRFID
        );


        Serial.print(
          "STATUS : "
        );


        Serial.println(
          status
        );


        Serial.println(
          "===================================="
        );
      }
    }


    // ========================================================
    // RESET TRANSACTION
    // ========================================================

    authPending =
      false;


    pendingAction =
      0;


    pendingRFID =
      "";


    pendingSwitch =
      false;


    return;
  }


  // ==========================================================
  // AUTH TIMEOUT
  // ==========================================================

  if (
    authPending &&

    (
      millis() -
      authStartedMs >=
      AUTH_TIMEOUT_MS
    )
  ) {

    Serial.println();


    Serial.println(
      "===================================="
    );


    Serial.println(
      "AUTH TIMEOUT"
    );


    Serial.print(
      "RFID : "
    );


    Serial.println(
      pendingRFID
    );


    Serial.println(
      "NO RESPONSE FROM NODE-RED"
    );


    Serial.println(
      "===================================="
    );


    authPending =
      false;


    pendingAction =
      0;


    pendingRFID =
      "";


    pendingSwitch =
      false;


    zbAuth.clearResponse();
  }
}


// ============================================================
// CONFIG ANALOG ENDPOINT
// ============================================================

void configureAnalogEndpoint(
  ZigbeeAnalog &endpoint,
  const char *description,
  float maxValue
) {

  endpoint.addAnalogInput();


  endpoint.setAnalogInputApplication(
    ESP_ZB_ZCL_AI_COUNT_UNITLESS_COUNT
  );


  endpoint.setAnalogInputDescription(
    description
  );


  endpoint.setAnalogInputResolution(
    1
  );


  endpoint.setAnalogInputMinMax(
    0,
    maxValue
  );


  endpoint.setManufacturerAndModel(
    "GGI Robotics",
    "SIMON"
  );


  Zigbee.addEndpoint(
    &endpoint
  );
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  // ==========================================================
  // XIAO ESP32-C6 - SELECT EXTERNAL ANTENNA
  // ==========================================================
  //
  // Official XIAO ESP32-C6 RF switch:
  //   GPIO3  LOW  -> RF switch control enabled
  //   GPIO14 HIGH -> external U.FL/I-PEX antenna selected
  //
  // Configure this BEFORE Zigbee stack starts.
  // ==========================================================

  pinMode(
    XIAO_RF_SWITCH_ENABLE_PIN,
    OUTPUT
  );

  digitalWrite(
    XIAO_RF_SWITCH_ENABLE_PIN,
    LOW
  );

  delay(
    100
  );

  pinMode(
    XIAO_RF_ANT_SELECT_PIN,
    OUTPUT
  );

  digitalWrite(
    XIAO_RF_ANT_SELECT_PIN,
    HIGH
  );

  delay(
    100
  );


  // ==========================================================
  // LCD - INIT PERTAMA
  // ==========================================================

  delay(100);

  Wire.setPins(
    LCD_SDA_PIN,
    LCD_SCL_PIN
  );

  lcd.init();

  Wire.setClock(100000);

  lcd.backlight();
  lcd.clear();

  lcdWriteLine(0, "SIMON STARTING");
  lcdWriteLine(1, "PLEASE WAIT...");
  lcdPageStartedMs = millis();


  // ==========================================================
  // SERIAL
  // ==========================================================

  Serial.begin(115200);
  delay(100);

  Serial.println();


  Serial.println(
    "===================================="
  );


  Serial.println(
    "SIMON START"
  );


  Serial.println(
    "ESP32-C6"
  );


  Serial.println(
    "BOARD        : SEEED STUDIO XIAO ESP32-C6"
  );


  Serial.println(
    "ANTENNA      : EXTERNAL U.FL/I-PEX"
  );


  Serial.println(
    "RF SWITCH    : GPIO3 LOW / GPIO14 HIGH"
  );


  Serial.print(
    "PHOTO SENSOR : GPIO"
  );


  Serial.println(
    SENSOR_PIN
  );


  Serial.print(
    "RDM6300 RX   : GPIO"
  );


  Serial.println(
    RFID_RX_PIN
  );


  Serial.print(
    "RDM6300 BAUD : "
  );


  Serial.println(
    RFID_BAUD
  );


  Serial.print(
    "BUZZER       : GPIO"
  );


  Serial.println(
    BUZZER_PIN
  );


  Serial.print(
    "LCD SDA      : GPIO"
  );


  Serial.println(
    LCD_SDA_PIN
  );


  Serial.print(
    "LCD SCL      : GPIO"
  );


  Serial.println(
    LCD_SCL_PIN
  );


  Serial.print(
    "LCD ADDRESS  : 0x"
  );


  Serial.println(
    LCD_I2C_ADDRESS,
    HEX
  );


  Serial.print(
    "TRIM HOLD    : >= "
  );


  Serial.print(
    TRIM_DETECT_MS
  );


  Serial.println(
    " ms"
  );


  Serial.println(
    "PRODUCTION   : ALWAYS ACTIVE"
  );


  Serial.print(
    "FIRMWARE     : "
  );


  Serial.println(
    FIRMWARE_VERSION
  );


  Serial.print(
    "SENSOR TYPE  : "
  );


  Serial.println(
    SENSOR_TYPE
  );


  Serial.print(
    "COORDINATOR  : "
  );


  Serial.println(
    COORDINATOR_ID
  );


  Serial.print(
    "HEARTBEAT    : "
  );


  Serial.print(
    HEARTBEAT_INTERVAL_MS /
    1000UL
  );


  Serial.println(
    " sec"
  );


  Serial.println(
    "PROD REPORT  : DIRECT TO COORDINATOR EP1"
  );


  // ==========================================================
  // NVS
  // ==========================================================

  preferences.begin(
    PREF_NAMESPACE,
    false
  );


  restoreZigbeeRegistration();


  restoreOperatorSession();

  // ==========================================================
  // BUZZER
  // ==========================================================

  pinMode(
    BUZZER_PIN,
    OUTPUT
  );


  buzzerWrite(
    false
  );


  // ==========================================================
  // SENSOR
  // ==========================================================

  pinMode(
    SENSOR_PIN,
    INPUT
  );


  previousSensorActive =
    sensorActive();


  // ==========================================================
  // RFID UART
  // ==========================================================

  RFIDSerial.begin(

    RFID_BAUD,

    SERIAL_8N1,

    RFID_RX_PIN,

    -1
  );


  // ==========================================================
  // PRODUCTION ENDPOINT
  // ==========================================================

  configureAnalogEndpoint(

    zbWork,

    "DURATION_TIME_WORK",

    86400
  );


  configureAnalogEndpoint(

    zbIdle,

    "DURATION_TIME_IDLE",

    86400
  );


  configureAnalogEndpoint(

    zbStitch,

    "TOTAL_STITCH",

    1000000
  );


  configureAnalogEndpoint(

    zbHandling,

    "DURATION_TIME_HANDLING",

    86400
  );


  // ==========================================================
  // AUTH / DEVICE ENDPOINT
  // ==========================================================

  zbAuth.setManufacturerAndModel(

    "GGI Robotics",

    "SIMON"
  );


  Zigbee.addEndpoint(
    &zbAuth
  );


  // ==========================================================
  // ZIGBEE CHANNEL
  // ==========================================================

  // ==========================================================
  // ZIGBEE COMMISSIONING CHANNEL
  // ==========================================================
  //
  // Device BARU:
  // scan seluruh channel Zigbee 11-26 agar commissioning tidak
  // gagal hanya karena coordinator ternyata tidak berada di CH11.
  //
  // Device SUDAH REGISTERED:
  // tetap gunakan CH11 sebagai primary target. Network/channel
  // hasil join sebelumnya juga tersimpan di NVRAM Zigbee.
  // ==========================================================

  if (
    zigbeeRegistered
  ) {

    Zigbee.setPrimaryChannelMask(
      1UL <<
      ZIGBEE_CHANNEL
    );


    Serial.print(
      "[ZIGBEE] REJOIN PRIMARY CHANNEL : "
    );


    Serial.println(
      ZIGBEE_CHANNEL
    );

  } else {

    Zigbee.setPrimaryChannelMask(
      ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK
    );


    Serial.println(
      "[ZIGBEE] NEW DEVICE SCAN : CHANNEL 11-26"
    );
  }


  // Scan duration maksimum untuk commissioning device baru.
  Zigbee.setScanDuration(
    4
  );


  // Aktifkan debug internal Zigbee.
  Zigbee.setDebugMode(
    true
  );


  /*
    Device harus selalu bisa menerima
    auth response.
  */
  Zigbee.setRxOnWhenIdle(
    true
  );


  lcdWriteLine(0, "ZIGBEE STARTING");
  lcdWriteLine(1, "PLEASE WAIT...");

  Serial.println(
    "[ZIGBEE] STARTING..."
  );


  zigbeeStartMs =
    millis();


  lastZigbeeWaitLogMs =
    zigbeeStartMs;


  // ==========================================================
  // START ZIGBEE
  // ==========================================================
  //
  // ALAT BELUM REGISTERED:
  //   erase_nvs = true
  //   -> hapus network Zigbee lama
  //   -> device benar-benar factory-new
  //   -> network steering / join baru
  //
  // ALAT SUDAH REGISTERED:
  //   erase_nvs = false
  //   -> pertahankan network Zigbee
  //   -> AUTO REJOIN walaupun Permit Join OFF
  // ==========================================================

  bool eraseZigbeeNetworkNvs =
    !zigbeeRegistered;


  // Registered device normally rejoins from saved Zigbee NVRAM.
  // If that state is stale/corrupt or coordinator network changed,
  // do not block for the full default 30 seconds.
  if (
    zigbeeRegistered
  ) {

    Zigbee.setTimeout(
      10000
    );


    Serial.println(
      "[ZIGBEE] REJOIN TIMEOUT : 10 SEC"
    );
  }


  Serial.print(
    "[ZIGBEE] ERASE NETWORK NVS : "
  );


  Serial.println(
    eraseZigbeeNetworkNvs
      ? "YES - NEW DEVICE"
      : "NO - REGISTERED DEVICE"
  );


  if (
    !Zigbee.begin(
      ZIGBEE_END_DEVICE,
      eraseZigbeeNetworkNvs
    )
  ) {

    Serial.println(
      "[ZIGBEE] STACK START FAILED"
    );


    // ========================================================
    // SELF-RECOVERY
    // ========================================================
    //
    // Kasus:
    //   zb_registered = YES
    // tetapi Zigbee NVRAM/network state tidak bisa rejoin.
    //
    // Solusi:
    //   hapus flag aplikasi saja, reboot, lalu boot berikutnya
    //   menjalankan erase_nvs=true dan commissioning fresh.
    // ========================================================

    if (
      zigbeeRegistered
    ) {

      Serial.println(
        "[ZIGBEE] REGISTERED STATE INVALID"
      );


      Serial.println(
        "[ZIGBEE] CLEAR APP REGISTRATION FLAG"
      );


      preferences.putBool(
        "zb_registered",
        false
      );


      zigbeeRegistered =
        false;


      lcdWriteLine(
        0,
        "ZB STATE INVALID"
      );


      lcdWriteLine(
        1,
        "RECOVERING..."
      );


      delay(
        1500
      );


      ESP.restart();
    }


    // Device baru tetapi stack sendiri gagal start.
    lcdWriteLine(
      0,
      "ZIGBEE FAILED"
    );

    lcdWriteLine(
      1,
      "RESTARTING..."
    );


    delay(
      3000
    );


    ESP.restart();
  }


  Serial.println(
    "[ZIGBEE] STACK STARTED"
  );


  if (
    zigbeeRegistered
  ) {

    Serial.println(
      "[ZIGBEE] REGISTERED DEVICE - AUTO REJOIN"
    );


    lcdWriteLine(
      0,
      "ZB REJOINING..."
    );


    lcdWriteLine(
      1,
      "PROD ACTIVE"
    );

  } else {

    Serial.println(
      "[ZIGBEE] NEW DEVICE - FACTORY NEW NETWORK STATE"
    );


    Serial.println(
      "[ZIGBEE] NETWORK STEERING ACTIVE"
    );


    Serial.println(
      "[ZIGBEE] SCANNING CHANNEL 11-26"
    );


    Serial.println(
      "[ZIGBEE] ENABLE Z2M PERMIT JOIN"
    );


    lcdWriteLine(
      0,
      "ZB JOINING..."
    );


    lcdWriteLine(
      1,
      "PERMIT JOIN ON"
    );
  }


  Serial.println(
    "===================================="
  );


  Serial.println(
    "SIMON READY"
  );


  // ==========================================================
  // SESSION INFO
  // ==========================================================

  if (
    operatorLoggedIn
  ) {

    Serial.println(
      "SESSION RESTORED"
    );


    Serial.print(
      "RFID   : "
    );


    Serial.println(
      currentRFID
    );


    Serial.print(
      "NIK    : "
    );


    Serial.println(
      currentNIK
    );


    Serial.print(
      "NAME   : "
    );


    Serial.println(
      currentOperatorName
    );


    Serial.print(
      "PROCESS: "
    );

    Serial.println(
      currentProcessName
    );


    Serial.print(
      "BRANCH : "
    );

    Serial.println(
      currentBranch
    );


    Serial.println(
      "OPERATOR SESSION CONTINUES"
    );

  } else {

    Serial.println(
      "NO ACTIVE OPERATOR"
    );


    Serial.println(
      "PRODUCTION STILL ACTIVE"
    );


    Serial.println(
      "TAP RFID TO LOGIN"
    );
  }


  Serial.println(
    "===================================="
  );


  // ==========================================================
  // INITIAL PRODUCTION
  // ==========================================================

  resetProductionCycle(
    millis()
  );


  resetSensorTracking();
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  // ==========================================================
  // ZIGBEE AUTO COMMISSION / AUTO REJOIN
  // ==========================================================

  updateZigbeeConnectionState();


  // ==========================================================
  // DEVICE BOOT EVENT
  // ==========================================================

  if (
    Zigbee.connected() &&
    !deviceBootSent
  ) {

    sendDeviceBootInfo();
  }


  // ==========================================================
  // HEARTBEAT
  // ==========================================================

  sendHeartbeatIfDue();


  // ==========================================================
  // RFID
  // ==========================================================

  processRFID();


  // ==========================================================
  // AUTH RESPONSE
  // ==========================================================

  processAuthResponse();


  // ==========================================================
  // SENSOR
  // ==========================================================

  processSensor();


  // ==========================================================
  // MACHINE STATE
  // ==========================================================

  updateMachineState();


  // ==========================================================
  // BUZZER
  // ==========================================================

  updateBuzzer();


  // ==========================================================
  // LCD
  // ==========================================================

  updateLCD();


  delay(
    1
  );
}
