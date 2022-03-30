/**
   @file hydos21_irid_hrly.ino
   @copyright ?
   @author Hunter Gleason, with significant code copied from Kevin M.Smith and Mikal Hart.
   @date March 2022

   @brief Checks SDI-12 address 0 for HYDROS-21 sensor, takes a reading and records to SD card.
   When midnight is reached, uses Iridium 9603 (RockBLOCk, Serial version NOT I2C) modem to
   send the current days data including water level, temp. and electric conductivity. Circuit is
   built around TPL5110, therefore the logging interval is manually set on the TPL5110. User must provide
   a CSV file (1-column) named HYDROS.CSV with the column heading "filename" with a return char and the
   desired file name for the logfile (!!Must be 8 char or less!!).

*/

#include <SDI12.h>/*Needed for SDI-12*/
#include <SPI.h>/*Needed for working with SD card*/
#include <SD.h>/*Needed for working with SD card*/
#include "RTClib.h"/*Needed for communication with Real Time Clock*/
#include <CSV_Parser.h>/*Needed for parsing CSV data*/
#include <IridiumSBD.h>

#define SERIAL_BAUD 115200 /*!< The baud rate for the output serial port */
#define IridiumSerial Serial1 /* Define Serial1 for communicating with Iridium Modem */
#define DATA_PIN 12         /*!< The pin of the SDI-12 data bus */
#define POWER_PIN -1       /*!< The sensor power pin (or -1 if not switching power) */
#define DONE_PIN 11 /* Done signal pin for TPL5110 */
#define SET_RELAY 5 /* Latching relay set pin for IRIDUM Modem */
#define UNSET_RELAY 6 /* Latching relay unset pin for IRIDIUM Modem */
#define CHIP_SELECT 4 /*Chip select pin for SD card reader */
#define LED 13 /*led pin built in */

/*Global constants*/
char **filename;/*Desired name for data file !!!must be less than equal to 8 char!!!*/


/* Define the SDI-12 bus */
SDI12 mySDI12(DATA_PIN);

/* Define the logfile */
File dataFile;

/*Define PCF8523 RTC*/
RTC_PCF8523 rtc;

/*Declare the IridiumSBD object*/
IridiumSBD modem(IridiumSerial);



// keeps track of active addresses
bool isActive[64] = {
  0,
};

uint8_t numSensors = 0;


/**
   @brief converts allowable address characters ('0'-'9', 'a'-'z', 'A'-'Z') to a
   decimal number between 0 and 61 (inclusive) to cover the 62 possible
   addresses.
*/
byte charToDec(char i) {
  if ((i >= '0') && (i <= '9')) return i - '0';
  if ((i >= 'a') && (i <= 'z')) return i - 'a' + 10;
  if ((i >= 'A') && (i <= 'Z'))
    return i - 'A' + 36;
  else
    return i;
}

/**
   @brief maps a decimal number between 0 and 61 (inclusive) to allowable
   address characters '0'-'9', 'a'-'z', 'A'-'Z',

   THIS METHOD IS UNUSED IN THIS EXAMPLE, BUT IT MAY BE HELPFUL.
*/
char decToChar(byte i) {
  if (i < 10) return i + '0';
  if ((i >= 10) && (i < 36)) return i + 'a' - 10;
  if ((i >= 36) && (i <= 62))
    return i + 'A' - 36;
  else
    return i;
}

/**
   @brief gets identification information from a sensor, and prints it to the serial
   port

   @param i a character between '0'-'9', 'a'-'z', or 'A'-'Z'.
*/
void printInfo(char i) {
  String command = "";
  command += (char)i;
  command += "I!";
  mySDI12.sendCommand(command);
  delay(100);

  String sdiResponse = mySDI12.readStringUntil('\n');
  sdiResponse.trim();
  // allccccccccmmmmmmvvvxxx...xx<CR><LF>
  Serial.print(sdiResponse.substring(0, 1));  // address
  Serial.print(", ");
  Serial.print(sdiResponse.substring(1, 3).toFloat() / 10);  // SDI-12 version number
  Serial.print(", ");
  Serial.print(sdiResponse.substring(3, 11));  // vendor id
  Serial.print(", ");
  Serial.print(sdiResponse.substring(11, 17));  // sensor model
  Serial.print(", ");
  Serial.print(sdiResponse.substring(17, 20));  // sensor version
  Serial.print(", ");
  Serial.print(sdiResponse.substring(20));  // sensor id
  Serial.print(", ");
}

bool getResults(char i, int resultsExpected) {
  uint8_t resultsReceived = 0;
  uint8_t cmd_number      = 0;
  while (resultsReceived < resultsExpected && cmd_number <= 9) {
    String command = "";
    // in this example we will only take the 'DO' measurement
    command = "";
    command += i;
    command += "D";
    command += cmd_number;
    command += "!";  // SDI-12 command to get data [address][D][dataOption][!]
    mySDI12.sendCommand(command);

    uint32_t start = millis();
    while (mySDI12.available() < 3 && (millis() - start) < 1500) {}
    mySDI12.read();           // ignore the repeated SDI12 address
    char c = mySDI12.peek();  // check if there's a '+' and toss if so
    if (c == '+') {
      mySDI12.read();
    }


    //Get the current time from RTC
    DateTime now = rtc.now();

    //String to log to SD card
    String month_str = "";
    String day_str = "";
    String hour_str = "";
    String minute_str = "";
    String second_str = "";

    if (now.month() < 10)
    {
      month_str = "0" + now.month();
    } else {
      month_str = now.month();
    }
    if (now.day() < 10)
    {
      day_str = "0" + now.day();
    } else {
      day_str = now.month();
    }
    if (now.hour() < 10)
    {
      hour_str = "0" + now.hour();
    } else {
      hour_str = now.hour();
    }
    if (now.minute() < 10)
    {
      minute_str = "0" + now.minute();
    } else {
      minute_str = now.minute();
    }
    if (now.second() < 10)
    {
      second_str = "0" + now.second();
    } else {
      second_str = now.second();
    }

    String datastring = String(now.year()) + "-" + month_str + "-" + day_str + " " + hour_str + ":" + minute_str + ":" + second_str + ", ";

    while (mySDI12.available()) {
      char c = mySDI12.peek();
      if (c == '-' || (c >= '0' && c <= '9') || c == '.') {
        float result = mySDI12.parseFloat(SKIP_NONE);
        Serial.print(String(result, 1));
        datastring = datastring + String(result, 1);
        if (result != -9999) {
          resultsReceived++;
        }
      } else if (c == '+') {
        mySDI12.read();
        Serial.print(", ");
        datastring = datastring + ", ";
      } else {
        mySDI12.read();
      }
      delay(10);  // 1 character ~ 7.5ms
    }


    //Set paramters for parsing the parameter file
    CSV_Parser cp(/*format*/ "s", /*has_header*/ true, /*delimiter*/ ',');

    //Read the parameter file off SD card (snowlog.csv), 1/4-sec flash means file is not available
    while (!cp.readSDfile("/HYDROS.csv"))
    {
      digitalWrite(LED, HIGH);
      delay(250);
      digitalWrite(LED, LOW);
      delay(250);
    }

    //Read values from SNOW_PARAM.TXT into global varibles
    filename = (char**)cp["filename"];

    //Write header if first time writing to the file
    if (!SD.exists(filename[0]))
    {
      dataFile = SD.open(filename[0], FILE_WRITE);
      if (dataFile)
      {
        dataFile.println("datetime,h2o_depth_mm,h2o_temp_deg_c,ec_dS_m");
        dataFile.close();
      }

    }

    //Write datastring and close logfile on SD card
    dataFile = SD.open(filename[0], FILE_WRITE);
    if (dataFile)
    {
      dataFile.println(datastring);
      dataFile.close();
    }

    if (resultsReceived < resultsExpected) {
      Serial.print(", ");
    }
    cmd_number++;
  }
  mySDI12.clearBuffer();

  return resultsReceived == resultsExpected;
}

bool takeMeasurement(char i, String meas_type = "") {
  mySDI12.clearBuffer();
  String command = "";
  command += i;
  command += "M";
  command += meas_type;
  command += "!";  // SDI-12 measurement command format  [address]['M'][!]
  mySDI12.sendCommand(command);
  delay(100);

  // wait for acknowlegement with format [address][ttt (3 char, seconds)][number of
  // measurments available, 0-9]
  String sdiResponse = mySDI12.readStringUntil('\n');
  sdiResponse.trim();

  String addr = sdiResponse.substring(0, 1);
  Serial.print(addr);
  Serial.print(", ");

  // find out how long we have to wait (in seconds).
  uint8_t wait = sdiResponse.substring(1, 4).toInt();
  Serial.print(wait);
  Serial.print(", ");

  // Set up the number of results to expect
  int numResults = sdiResponse.substring(4).toInt();
  Serial.print(numResults);
  Serial.print(", ");

  unsigned long timerStart = millis();
  while ((millis() - timerStart) < (1000 * (wait + 1))) {
    if (mySDI12.available())  // sensor can interrupt us to let us know it is done early
    {
      Serial.print(millis() - timerStart);
      Serial.print(", ");
      mySDI12.clearBuffer();
      break;
    }
  }
  // Wait for anything else and clear it out
  delay(30);
  mySDI12.clearBuffer();

  if (numResults > 0) {
    return getResults(i, numResults);
  }

  return true;
}

// this checks for activity at a particular address
// expects a char, '0'-'9', 'a'-'z', or 'A'-'Z'
boolean checkActive(char i) {
  String myCommand = "";
  myCommand        = "";
  myCommand += (char)i;  // sends basic 'acknowledge' command [address][!]
  myCommand += "!";

  for (int j = 0; j < 3; j++) {  // goes through three rapid contact attempts
    mySDI12.sendCommand(myCommand);
    delay(100);
    if (mySDI12.available()) {  // If we here anything, assume we have an active sensor
      mySDI12.clearBuffer();
      return true;
    }
  }
  mySDI12.clearBuffer();
  return false;
}


void setup() {

  //Set pin modes for digital IO
  pinMode(DONE_PIN, OUTPUT);
  pinMode(SET_RELAY, OUTPUT);
  pinMode(UNSET_RELAY, OUTPUT);
  pinMode(LED, OUTPUT);


  //In case user has USB connected
  Serial.begin(SERIAL_BAUD);

  //Make sure a SD is available (1-sec flash LED means SD card did not initialize)
  while (!SD.begin(CHIP_SELECT))
  {
    digitalWrite(LED, HIGH);
    delay(1000);
    digitalWrite(LED, LOW);
    delay(1000);
  }

  // Start RTC (10-sec flash LED means RTC did not initialize)
  while (!rtc.begin())
  {
    digitalWrite(LED, HIGH);
    delay(500);
    digitalWrite(LED, LOW);
    delay(500);
  }

  Serial.println("Opening SDI-12 bus...");
  mySDI12.begin();
  delay(500);  // allow things to settle

  Serial.println("Timeout value: ");
  Serial.println(mySDI12.TIMEOUT);

  // Power the sensors;
  if (POWER_PIN > 0) {
    Serial.println("Powering up sensors...");
    pinMode(POWER_PIN, OUTPUT);
    digitalWrite(POWER_PIN, HIGH);
    delay(200);
  }

  // Quickly Scan the Address Space
  Serial.println("Scanning all addresses, please wait...");
  Serial.println("Sensor Address, Protocol Version, Sensor Vendor, Sensor Model, "
                 "Sensor Version, Sensor ID");


  char addr = decToChar(0);
  if (checkActive(addr)) {
    numSensors++;
    isActive[0] = 1;
    printInfo(addr);
    Serial.println();
  }

  Serial.print("Total number of sensors found:  ");
  Serial.println(numSensors);

  if (numSensors == 0) {
    Serial.println(
      "No sensors found, please check connections and restart the Arduino.");
    while (true) {
      delay(10);  // do nothing forever
    }
  }

  Serial.println();
  Serial.println(
    "Time Elapsed (s), Sensor Address, Est Measurement Time (s), Number Measurements, "
    "Real Measurement Time (ms), Measurement 1, Measurement 2, ... etc.");
  Serial.println(
    "-------------------------------------------------------------------------------");


  if (isActive[0]) {
    Serial.print(millis());
    Serial.print(", ");
    takeMeasurement(addr, "");
    Serial.println();
  }

  DateTime now = rtc.now();

  //Check that the iridium modem is connected and the the clock has just reached midnight (i.e.,current time is within one logging interval of midnight)
  if (now.hour() == 0)
  {

    if (!SD.exists("IRID.CSV"))
    {
      dataFile = SD.open("IRID.CSV", FILE_WRITE);
      dataFile.println("day,day1");
      dataFile.println(String(now.day()) + "," + String(now.day()));
      dataFile.close();

    }


    CSV_Parser cp(/*format*/ "s-", /*has_header*/ true, /*delimiter*/ ',');


    while (!cp.readSDfile("/IRID.CSV"))
    {
      digitalWrite(LED, HIGH);
      delay(500);
      digitalWrite(LED, LOW);
      delay(500);
    }

    char **irid_day = (char**)cp["day"];

    if (String(irid_day[0]).toInt() == now.day())
    {
      //Update IRID.CSV with new day
      SD.remove("IRID.CSV");
      dataFile = SD.open("IRID.CSV", FILE_WRITE);
      dataFile.println("day,day1");
      DateTime next_day = (DateTime(now.year(), now.month(), now.day()) + TimeSpan(1, 0, 0, 0));
      dataFile.println(String(next_day.day()) + "," + String(next_day.day()));
      dataFile.close();

      int err;

      //Provide power to Iridium Modem
      digitalWrite(SET_RELAY, HIGH);
      delay(15);
      digitalWrite(SET_RELAY, LOW);

      //wait 5-sec while super capacitor charges
      delay(5000);


      // Start the serial port connected to the satellite modem
      IridiumSerial.begin(19200);

      // Begin satellite modem operation
      Serial.println("Starting modem...");
      err = modem.begin();
      if (err != ISBD_SUCCESS)
      {
        Serial.print("Begin failed: error ");
        Serial.println(err);
        if (err == ISBD_NO_MODEM_DETECTED)
          Serial.println("No modem detected: check wiring.");
        return;
      }

      //Get the datetime of the days start (i.e., 24 hours previous to current time)
      DateTime days_start (now - TimeSpan(1, 0, 0, 0));

      //Set paramters for parsing the log file
      CSV_Parser cp("ssss", true, ',');

      //Varibles for holding data fields
      char **datetimes;
      char **h2o_depths;
      char **h2o_temps;
      char **h2o_ecs;

      //Parse the logfile
      cp.readSDfile(filename[0]);


      //Populate data arrays from logfile
      datetimes = (char**)cp["datetime"];
      h2o_depths = (char**)cp["h2o_depth_mm"];
      h2o_temps = (char**)cp["h2o_temp_deg_c"];
      h2o_ecs = (char**)cp["ec_dS_m"];

      String iridium_string;

      //For each observation in the CSV
      for (int i = 0; i < cp.getRowsCount(); i++) {

        //Get the datetime stamp as string
        String datetime = String(datetimes[i]);

        //Get the observations year, month, day
        int dt_year = datetime.substring(0, 4).toInt();
        int dt_month = datetime.substring(5, 7).toInt();
        int dt_day = datetime.substring(8).toInt();

        //Check if the observations datetime occured during the day being summarised
        if (dt_year == days_start.year() && dt_month == days_start.month() && dt_day == days_start.day())
        {
          String h2o_depth = String(h2o_depths[i]);
          String h2o_temp = String(h2o_temps[i]);
          String h2o_ec = String(h2o_ecs[i]);

          String datastring = "{" + datetime + "," + h2o_depth + "," + h2o_temp + "," + h2o_ec + "}";
          iridium_string = iridium_string + datastring;
        }
      }

      //varible for keeping a byte count
      int byte_count = 0;

      //Length of the iridium string in bytes
      int str_len = iridium_string.length() + 1;

      //Convert iridium_string to character array
      char char_array[str_len];
      iridium_string.toCharArray(char_array, str_len);

      //Binary bufffer for iridium transmission (max allowed buffer size 340 bytes)
      uint8_t buffer[330];

      //For each charachter in the iridium string (i.e., string of the daily observations)
      for (int j = 0; j < str_len; j++)
      {
        //add character to the binary buffer, increment the byte count
        buffer[byte_count] = char_array[j];
        byte_count = byte_count + 1;

        //If maximum bytes have been reached
        if (byte_count == 330)
        {
          //reset byte count
          byte_count = 0;

          //transmit binary buffer data via iridium
          err = modem.sendSBDBinary(buffer, sizeof(buffer));

          if (err != ISBD_SUCCESS)
          {
            Serial.print("sendSBDText failed: error ");
            Serial.println(err);
            if (err == ISBD_SENDRECEIVE_TIMEOUT)
              Serial.println("Try again with a better view of the sky.");
          }

        }
      }

      //Kill power to Iridium Modem
      digitalWrite(UNSET_RELAY, HIGH);
      delay(15);
      digitalWrite(UNSET_RELAY, LOW);

    }

  }
}

  void loop() {

    // We're done!
    // It's important that the donePin is written LOW and THEN HIGH. This shift
    // from low to HIGH is how the TPL5110 Nano Power Timer knows to turn off the
    // microcontroller.
    digitalWrite(DONE_PIN, LOW);
    delay(10);
    digitalWrite(DONE_PIN, HIGH);
    delay(10);
  }
