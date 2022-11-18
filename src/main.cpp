#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_adc_cal.h>
#include <esp_wps.h>
#include <ssl_client.h>

#include "Adafruit_SGP30.h"
#include "DHT12.h"
#include "FS.h"
#include "M5CoreInk.h"
#include "SPIFFS.h"
#include "time.h"

#define LGFX_M5STACK_COREINK // M5Stack CoreInk
#include <LovyanGFX.hpp>

Ink_Sprite InkPageSprite(&M5.M5Ink);

static LGFX lcd;
static LGFX_Sprite sprite;

DHT12 dht12;
Adafruit_SGP30 sgp;

RTC_TimeTypeDef RTCtime;
RTC_DateTypeDef RTCDate;

bool sleepMode = true;

void sendNotify(String message)
{
    WiFi.begin("", "");
    while (WiFi.status() != WL_CONNECTED)
        delay(500);

    const char *host = "notify-api.line.me";
    const char *token = "";
    WiFiClientSecure client;
    client.setInsecure();
    Serial.println("Try");
    // LineのAPIサーバに接続
    if (!client.connect(host, 443))
    {
        Serial.println("Connection failed");
        return;
    }
    Serial.println("Connected");
    // リクエストを送信
    String query = String("message=") + String(message);
    String request = String("") + "POST /api/notify HTTP/1.1\r\n" +
                     "Host: " + host + "\r\n" + "Authorization: Bearer " + token +
                     "\r\n" + "Content-Length: " + String(query.length()) +
                     "\r\n" +
                     "Content-Type: application/x-www-form-urlencoded\r\n\r\n" +
                     query + "\r\n";
    client.print(request);

    // 受信終了まで待つ
    while (client.connected())
    {
        String line = client.readStringUntil('\n');
        if (line == "\r")
        {
            break;
        }
    }

    String line = client.readStringUntil('\n');
    Serial.println(line);
}

bool spiffsExist(String name)
{
    // ファイルが存在してディレクトリじゃなければtrue
    File fp = SPIFFS.open(name, FILE_READ);
    bool temp = fp && !fp.isDirectory();
    fp.close();
    return temp;
}

void spiffsRemove(String name)
{
    // SPIFFSからファイルを削除
    if (SPIFFS.remove(name))
    {
        Serial.printf("%s deleted\n", name.c_str());
    }
    else
    {
        Serial.printf("%s delete failed\n", name.c_str());
    }
}

void spiffsWriteBaseline(uint16_t eCO2_new, uint16_t TVOC_new)
{
    // eCO2とTVOCのbaseline値を書き込む
    // uint16_tをuint8_tに分割する
    File fp = SPIFFS.open("/baseline", FILE_WRITE);
    uint8_t baseline[4] = {(eCO2_new & 0xFF00) >> 8, eCO2_new & 0xFF,
                           (TVOC_new & 0xFF00) >> 8, TVOC_new & 0xFF};
    fp.write(baseline, 4);
    fp.close();
    Serial.printf("SPIFFS Wrote %x %x %x %x/n", baseline[0], baseline[1],
                  baseline[2], baseline[3]);
}

float getBatVoltage()
{
    analogSetPinAttenuation(35, ADC_11db);
    esp_adc_cal_characteristics_t *adc_chars =
        (esp_adc_cal_characteristics_t *)calloc(
            1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 3600,
                             adc_chars);
    uint16_t ADCValue = analogRead(35);

    uint32_t BatVolmV = esp_adc_cal_raw_to_voltage(ADCValue, adc_chars);
    float BatVol = float(BatVolmV) * 25.1 / 5.1 / 1000;
    free(adc_chars);
    return BatVol;
}

const float minVoltage = 3.3;
int getBatCapacity()
{
    // 4.02 = 100%, 3.65 = 0%
    const float maxVoltage = 3.98;

    // int cap = (int) (100.0 * (getBatVoltage() - minVoltage)
    //                  / (maxVoltage - minVoltage));
    // cap = constrain(cap, 0, 100);

    int cap =
        map(getBatVoltage() * 100, minVoltage * 100, maxVoltage * 100, 0, 100);
    // if (cap > 100) {
    //   cap = 100;
    // }
    // if (cap < 0) {
    //   cap = 0;
    // }
    cap = constrain(cap, 0, 100);
    return cap;
}

void pushSprite(Ink_Sprite *coreinkSprite, LGFX_Sprite *lgfxSprite)
{
    coreinkSprite->clear();
    for (int y = 0; y < 200; y++)
    {
        for (int x = 0; x < 200; x++)
        {
            uint16_t c = lgfxSprite->readPixel(x, y);
            if (c == 0x0000)
            {
                coreinkSprite->drawPix(x, y, 0);
            }
        }
    }
    coreinkSprite->pushSprite();
}

uint32_t getAbsoluteHumidity(float temperature, float humidity)
{
    // approximation formula from Sensirion SGP30 Driver Integration chapter 3.15
    const float absoluteHumidity =
        216.7f * ((humidity / 100.0f) * 6.112f *
                  exp((17.62f * temperature) / (243.12f + temperature)) /
                  (273.15f + temperature)); // [g/m^3]
    const uint32_t absoluteHumidityScaled =
        static_cast<uint32_t>(1000.0f * absoluteHumidity); // [mg/m^3]
    return absoluteHumidityScaled;
}

void measureSprite()
{
    sprite.clear(TFT_WHITE);
    sprite.setFont(&fonts::lgfxJapanGothicP_20);
    sprite.setTextSize(3);
    sprite.setTextColor(TFT_BLACK, TFT_WHITE);
    sprite.setCursor(10, 10);
    sprite.println("CO2");
    sprite.setTextSize(2);
    sprite.println(" 計測中");

    sprite.setTextSize(1);
    sprite.setCursor(0, 170);
    sprite.printf("%5.2fV:%d", getBatVoltage(), getBatCapacity());
}
void calibrateSprite()
{
    sprite.clear(TFT_WHITE);
    sprite.setFont(&fonts::lgfxJapanGothicP_20);
    sprite.setTextSize(1);
    sprite.setTextColor(TFT_BLACK, TFT_WHITE);
    sprite.setCursor(0, 0);
    sprite.printf("キャリブレーション中\n\n");
    sprite.printf("なるべく空気が綺麗な場所に置き１分間放置してください\n");
    sprite.printf("画面が切り替わると終了です");
}

void makeSprite()
{
    sprite.clear(TFT_WHITE);

    sprite.setFont(&fonts::lgfxJapanGothicP_20);
    sprite.setTextSize(1);
    sprite.setTextColor(TFT_BLACK, TFT_WHITE);

    float tmp = dht12.readTemperature();
    float hum = dht12.readHumidity();

    sprite.setCursor(0, 65);
    sprite.printf("気温%2.0f℃ 湿度%2.0f％\n", tmp, hum);

    sgp.setHumidity(getAbsoluteHumidity(tmp, hum));

    if (sgp.IAQmeasure())
    {
        sprite.setCursor(0, 105);
        sprite.setTextSize(3);
        sprite.printf("%4d", sgp.eCO2);
        sprite.setTextSize(1);
        sprite.print("ppm");
        sprite.setCursor(0, 90);
        sprite.printf("二酸化炭素濃度\n");
        //    sprite.setCursor(0, 170);
        //    sprite.printf("%4.1f:%d", getBatVoltage(), getBatCapacity());

        if (1000 < sgp.eCO2)
        {
            sprite.setCursor(0, 170);
            sprite.setTextColor(TFT_WHITE, TFT_BLACK);
            sprite.setTextSize(1.2);
            sprite.print("換気してください\n");
            sprite.setTextColor(TFT_BLACK, TFT_WHITE);
            sendNotify(String(sgp.eCO2) + "ppm 換気してください " +
                       String(getBatCapacity()) + "％");
        }
        Serial.print("TVOC ");
        Serial.print(sgp.TVOC);
        Serial.print(" ppb\t");
        Serial.print("eCO2 ");
        Serial.print(sgp.eCO2);
        Serial.println(" ppm");
    }

    RTC_TimeTypeDef RTCtime;
    RTC_DateTypeDef RTCDate;
    char timeStrbuff[20];

    M5.rtc.GetTime(&RTCtime);
    M5.rtc.GetDate(&RTCDate);
    int hour = 0;
    hour = RTCtime.Hours + (RTCDate.Date - 1) * 24;
    // 100時間を超えていたら2桁に切り捨てる
    if (100 <= hour)
    {
        hour %= 100;
    }

    sprintf(timeStrbuff, "%02d:%02d", hour, RTCtime.Minutes);

    sprite.setCursor(0, 0);
    sprite.setTextColor(TFT_BLACK, TFT_WHITE);
    sprite.setFont(&fonts::Font7);
    sprite.setTextSize(1.3);
    sprite.print(timeStrbuff);
    sprite.setFont(&fonts::lgfxJapanGothicP_20);
    sprite.setTextSize(1);
    //  sprite.printf("%d", RTCtime.Seconds);
    sprite.fillRect(185, 0, 10, 5, 0);
    sprite.fillRect(180, 5, 20, 55, 0);
    sprite.fillRect(185, 10, 10, 45 * (100 - getBatCapacity()) / 100, 1);

    uint16_t TVOC_base, eCO2_base;
    if (sgp.getIAQBaseline(&eCO2_base, &TVOC_base))
    {
        //    sprite.setCursor(90, 170);
        //    sprite.printf("%x,%x", eCO2_base, TVOC_base);
        Serial.print(" eCO2: 0x");
        Serial.print(eCO2_base, HEX);
        Serial.print(" & TVOC: 0x");
        Serial.println(TVOC_base, HEX);
    }
    else
    {
        Serial.println("Failed to get baseline readings");
    }
    if (!spiffsExist("/baseline"))
    {
        Serial.println("/baseline not found");
        sprite.setCursor(0, 160);
        sprite.setTextColor(TFT_WHITE, TFT_BLACK);
        sprite.setTextSize(0.8);
        sprite.print("キャリブレーションしてください");
        sprite.setTextColor(TFT_BLACK, TFT_WHITE);
    }
}

void ButtonTest(char *str)
{
    InkPageSprite.clear();
    InkPageSprite.drawString(35, 59, str);
    InkPageSprite.pushSprite();
    delay(1000);
}

void setup()
{
    bool calibrationMode = false;
    // const int intPin = 19;
    // pinMode(intPin, INPUT);
    // int cause = esp_sleep_get_wakeup_cause();
    // int INT = digitalRead(intPin);

    M5.begin();
    // Serial.printf("cause: %d INT: %d\n", cause, INT);

    lcd.init();
    // Serial.println(lcd.getBoard());

    if (InkPageSprite.creatSprite(0, 0, 200, 200, true) != 0)
    {
        Serial.printf("Ink Sprite create faild");
    }
    Serial.println("setup\n");

    // スプライト作成
    sprite.setColorDepth(1);
    sprite.createPalette();
    sprite.createSprite(200, 200);

    M5.update();
    if (M5.BtnUP.isPressed())
    {
        Serial.println("BtnUP.isPressed()");
        // キャリブレーションフラグを立てる
        calibrationMode = true;
    }
    if (M5.BtnDOWN.isPressed())
    {
        Serial.println("BtnDOWN.isPressed()");
        // 再起動後下ボタンを押しっぱなしにするとインターバルモードから抜ける
        if (sleepMode)
        {
            sleepMode = false;
        }
    }

    Wire.begin(25, 26);

    if (sgp.begin(&Wire))
    {
        Serial.print("Found SGP30 serial #");
        Serial.print(sgp.serialnumber[0], HEX);
        Serial.print(sgp.serialnumber[1], HEX);
        Serial.println(sgp.serialnumber[2], HEX);

        uint16_t TVOC_base;
        uint16_t eCO2_base;
        uint8_t calibrationNum = 0;

        if (SPIFFS.begin(true))
        {
            // キャリブレーションフラグをチェック
            if (calibrationMode)
            {
                Serial.println("calibration mode");
                calibrationNum = 60;
                calibrateSprite();
                pushSprite(&InkPageSprite, &sprite);
                // 累積稼働時間のリセット
                RTC_TimeTypeDef TimeStruct;
                TimeStruct.Hours = 0;
                TimeStruct.Minutes = 0;
                TimeStruct.Seconds = 0;
                M5.rtc.SetTime(&TimeStruct);
                RTC_DateTypeDef RTCDate;
                RTCDate.Year = 2021;
                RTCDate.Month = 1;
                RTCDate.Date = 1;
                M5.rtc.SetDate(&RTCDate);
            }
            else
            {
                Serial.println("normal mode");
                calibrationNum = 30;
                calibrationMode = false;
                measureSprite();
                pushSprite(&InkPageSprite, &sprite);

                if (spiffsExist("/baseline"))
                {
                    File fp = SPIFFS.open("/baseline", FILE_READ);

                    uint8_t baseline2[4];
                    int i = 0;
                    while (fp.available())
                    {
                        baseline2[i] = fp.read();
                        i++;
                    }
                    eCO2_base = baseline2[0] & 0xff;
                    eCO2_base = eCO2_base << 8;
                    eCO2_base = eCO2_base | baseline2[1];
                    TVOC_base = baseline2[2] & 0xff;
                    TVOC_base = TVOC_base << 8;
                    TVOC_base = TVOC_base | baseline2[3];
                    sgp.setIAQBaseline(eCO2_base, TVOC_base);
                    fp.close();
                    Serial.print("SPIFFS eCO2: 0x");
                    Serial.print(eCO2_base, HEX);
                    Serial.print(" & TVOC: 0x");
                    Serial.println(TVOC_base, HEX);
                }
            }
        }
        else
        {
            Serial.println("SPIFFS Mount Failed");
        }

        int i = calibrationNum;
        long last_millis = 0;
        Serial.print("Sensor init\n");
        while (i > 0)
        {
            if (millis() - last_millis > 1000)
            {
                last_millis = millis();
                i--;
                if (sgp.IAQmeasure())
                {
                    Serial.printf("%d:", calibrationNum - i);
                    Serial.print("eCO2 ");
                    Serial.print(sgp.eCO2);
                    Serial.print(" ppm\t");
                    Serial.print("TVOC ");
                    Serial.print(sgp.TVOC);
                    Serial.println(" ppb");
                    if (sgp.getIAQBaseline(&eCO2_base, &TVOC_base))
                    {
                        // 現在のbaselineを表示
                        Serial.print("eCO2: 0x");
                        Serial.print(eCO2_base, HEX);
                        Serial.print(" TVOC: 0x");
                        Serial.println(TVOC_base, HEX);
                    }
                    if (!calibrationMode)
                    {
                        // キャリブレーションモードでは途中終了しない(60秒)
                        if (sgp.TVOC != 0 || sgp.eCO2 != 400)
                        {
                            break;
                        }
                    }
                }
            }
        }
        if (calibrationMode)
        {
            // キャリブレーションモードならばbalselineを設定(固定)してSPIFFSに書き込む
            sgp.setIAQBaseline(eCO2_base, TVOC_base);
            spiffsWriteBaseline(eCO2_base, TVOC_base);
        }
        Serial.println("done");
    }
    else
    {
        Serial.println("Sensor not found :(");
    }

    makeSprite();

    pushSprite(&InkPageSprite, &sprite);
}
void loop()
{
    M5.update();
    if (M5.BtnUP.wasPressed())
    {
        Serial.println("BtnUP.wasPressed()");
    }

    if (M5.BtnMID.wasPressed())
    {
        Serial.println("BtnMID.wasPressed()");
        if (SPIFFS.remove("/baseline"))
        {
            Serial.println("-base file deleted");
        }
        else
        {
            Serial.println("-base delete failed");
        }
        if (SPIFFS.remove("/calibration"))
        {
            Serial.println("-cali file deleted");
        }
        else
        {
            Serial.println("-cali delete failed");
        }
        ButtonTest("base and cali deleted");
    }
    if (M5.BtnDOWN.wasPressed())
    {
        Serial.println("BtnDOWN.wasPressed()");
        // 再起動後下ボタンを押しっぱなしにするとインターバルモードから抜ける
        if (sleepMode)
        {
            sleepMode = false;
        }
        else
        {
            Serial.printf("Measure\n");
            // 抜けた後下ボタンを押すと随時測定できる
            makeSprite();
            pushSprite(&InkPageSprite, &sprite);
        }
    }

    if (M5.BtnPWR.wasPressed())
    {
        ButtonTest("Btn PWR Pressed");
        M5.shutdown();
    }
    if (sleepMode)
    {
        if (getBatVoltage() < minVoltage)
        {
            // if (getBatVoltage() < 5.0) {
            sprite.setCursor(0, 170);
            sprite.setTextColor(TFT_WHITE, TFT_BLACK);
            sprite.setTextSize(1.2);
            sprite.print("充電してください\n");
            sprite.setTextColor(TFT_BLACK, TFT_WHITE);
            pushSprite(&InkPageSprite, &sprite);

            sendNotify("充電してください");
            M5.shutdown();
        }

        M5.shutdown(900);
        // esp_sleep_enable_timer_wakeup(600000000);
        // esp_sleep_enable_timer_wakeup(600000000);
        // esp_deep_sleep_start();

        // esp_restart();
    }
    delay(1);
}