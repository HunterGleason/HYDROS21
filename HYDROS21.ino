/**
   @file hydos21_irid_hrly.ino
   @copyright ?
   @author Hunter Gleason, with significant code copied from Kevin M.Smith and Mikal Hart.
   @date March 2022

   @brief Checks SDI-12 address 0 for HYDROS-21 sensor, takes a reading and records to SD card.
   When midnight is reached, uses Iridium 9603 (RockBLOCk, Serial version NOT I2C) modem to
   send the current days data including water level, temp. and electric conductivity. Circuit is
   built around TPL5110, therefore the logging interval is manually set on the TPL5110. User must provide
   a CSV file (1-column) named HYDROS.CSV with the column heading "filename" containing the
   desired file name for the logfile (!!Must be 8 char or less!!).

*/

#include <SDI12.h>/*Needed for SDI-12*/
#include <SPI.h>/*Needed for working with SD card*/
#include <SD.h>/*Needed for working with SD card*/
#include "RTClib.h"/*Needed for communication with Real Time Clock*/
#include <CSV_Parser.h>/*Needed for parsing CSV data*/
#include <IridiumSBD.h>

#define IridiumSerial Serial1 /* Define Serial1 for communicating with Iridium Modem */
#define DATA_PIN 12         /*!< The pin of the SDI-12 data bus */
#define DONE_PIN 11 /* Done signal pin for TPL5110 */
#define SET_RELAY 5 /* Latching relay set pin for IRIDUM Modem */
#define UNSET_RELAY 6 /* Latching relay unset pin for IRIDIUM Modem */
#define CHIP_SELECT 4 /*Chip select pin for SD card reader */
#define LED 13 /*led pin built in */
#define SENSOR_ADDRESS 0 /*SDI-12 sensor address, assumed to be 0/*

/*Global constants*/
char **filename;/*Desired name for data file !!!must be less than equal to 8 char!!!*/


String myCommand   = "";
String sdiResponse = "";

/* Define the SDI-12 bus */
SDI12 mySDI12(DATA_PIN);

/* Define the logfile */
File dataFile;

/*Define PCF8523 RTC*/
RTC_PCF8523 rtc;

/*Declare the IridiumSBD object*/
IridiumSBD modem(IridiumSerial);


/*Function pings RTC for datetime and returns formated datestamp*/
String gen_date_str(DateTime now) {

  //Format current date time values for writing to SD
  String yr_str = String(now.year());
  String mnth_str;
  if (now.month() >= 10)
  {
    mnth_str = String(now.month());
  } else {
    mnth_str = "0" + String(now.month());
  }

  String day_str;
  if (now.day() >= 10)
  {
    day_str = String(now.day());
  } else {
    day_str = "0" + String(now.day());
  }

  String hr_str;
  if (now.hour() >= 10)
  {
    hr_str = String(now.hour());
  } else {
    hr_str = "0" + String(now.hour());
  }

  String min_str;
  if (now.minute() >= 10)
  {
    min_str = String(now.minute());
  } else {
    min_str = "0" + String(now.minute());
  }


  String sec_str;
  if (now.second() >= 10)
  {
    sec_str = String(now.second());
  } else {
    sec_str = "0" + String(now.second());
  }

  //Assemble a data string for logging to SD, with date-time, snow depth (mm), temperature (deg C) and humidity (%)
  String datastring = yr_str + "-" + mnth_str + "-" + day_str + " " + hr_str + ":" + min_str + ":" + sec_str + ",";

  return datastring;
}

/*Function reads data on logfile, and uses Iridium modem to send all observations
   for the previous day over satellite.
*/
void send_daily_data(DateTime now)
{

  //Use IRID.CSV to keep track of day
  if (!SD.exists("IRID.CSV"))
  {
    dataFile = SD.open("IRID.CSV", FILE_WRITE);
    dataFile.println("day,day1");
    dataFile.println(String(now.day()) + "," + String(now.day()));
    dataFile.close();

  }

  //Set up parse params for IRID.CSV
  CSV_Parser cp(/*format*/ "s-", /*has_header*/ true, /*delimiter*/ ',');

  //Read IRID.CSV
  while (!cp.readSDfile("/IRID.CSV"))
  {
    digitalWrite(LED, HIGH);
    delay(1000);
    digitalWrite(LED, LOW);
    delay(1000);
  }

  //Get day from IRID CSV
  char **irid_day = (char**)cp["day"];

  //If IRID day matches RTC day
  if (String(irid_day[0]).toInt() == now.day())
  {
    //Update IRID.CSV with new day
    SD.remove("IRID.CSV");
    dataFile = SD.open("IRID.CSV", FILE_WRITE);
    dataFile.println("day,day1");
    DateTime next_day = (DateTime(now.year(), now.month(), now.day()) + TimeSpan(1, 0, 0, 0));
    dataFile.println(String(next_day.day()) + "," + String(next_day.day()));
    dataFile.close();

    //For capturing Iridium errors
    int err;

    //Provide power to Iridium Modem
    digitalWrite(SET_RELAY, HIGH);
    delay(20);
    digitalWrite(SET_RELAY, LOW);

    //wait 5-sec while super capacitor charges
    delay(5000);


    // Start the serial port connected to the satellite modem
    IridiumSerial.begin(19200);

    // Begin satellite modem operation
    err = modem.begin();
    if (err != ISBD_SUCCESS)
    {

      if (err == ISBD_NO_MODEM_DETECTED)
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

    //String for sending over modem
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

        //Add observation to iridium_string
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
          for (int i = 0; i < 20; i++)
          {
            digitalWrite(LED, HIGH);
            delay(50);
            digitalWrite(LED, LOW);
            delay(50);
          }
        }

      }
    }

    //Kill power to Iridium Modem
    digitalWrite(UNSET_RELAY, HIGH);
    delay(30);
    digitalWrite(UNSET_RELAY, LOW);
  }
}

void setup() {

  //Set pin modes for digital IO
  pinMode(DONE_PIN, OUTPUT);
  pinMode(SET_RELAY, OUTPUT);
  pinMode(UNSET_RELAY, OUTPUT);
  pinMode(LED, OUTPUT);


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

  mySDI12.begin();
  delay(500);

  DateTime now = rtc.now();

  // first command to take a measurement
  myCommand = String(SENSOR_ADDRESS) + "M!";

  mySDI12.sendCommand(myCommand);
  delay(30);  // wait a while for a response


  while (mySDI12.available()) {  // build response string
    char c = mySDI12.read();
    if ((c != '\n') && (c != '\r')) {
      sdiResponse += c;
      delay(10);  // 1 character ~ 7.5ms
    }
  }


  if (sdiResponse.length() > 1)
    mySDI12.clearBuffer();

  delay(1000);       // delay between taking reading and requesting data
  sdiResponse = "";  // clear the response string


  // next command to request data from last measurement
  myCommand = String(SENSOR_ADDRESS) + "D0!";

  mySDI12.sendCommand(myCommand);
  delay(30);  // wait a while for a response

  while (mySDI12.available()) {  // build string from response
    char c = mySDI12.read();
    if ((c != '\n') && (c != '\r')) {
      sdiResponse += c;
      delay(10);  // 1 character ~ 7.5ms
    }
  }

  sdiResponse = sdiResponse.substring(3);

  for (int i = 0; i < sdiResponse.length(); i++)
  {

    char c = sdiResponse.charAt(i);

    if (c == '+')
    {
      sdiResponse.setCharAt(i, ',');
    }

  }


  if (sdiResponse.length() > 1)
    mySDI12.clearBuffer();

  String datastring = gen_date_str(now);

  datastring = datastring + sdiResponse;

  //Write datastring and close logfile on SD card
  dataFile = SD.open(filename[0], FILE_WRITE);
  if (dataFile)
  {
    dataFile.println(datastring);
    dataFile.close();
  }



  //Check that the iridium modem is connected and the the clock has just reached midnight (i.e.,current time is within one logging interval of midnight)
  if (now.hour() == 0)
  {
    //Send daily data over Iridium
    send_daily_data(now);

  }
}

void loop() {

  //Kill power to Iridium Modem
  digitalWrite(UNSET_RELAY, HIGH);
  delay(30);
  digitalWrite(UNSET_RELAY, LOW);

  // We're done!
  // It's important that the donePin is written LOW and THEN HIGH. This shift
  // from low to HIGH is how the TPL5110 Nano Power Timer knows to turn off the
  // microcontroller.
  digitalWrite(DONE_PIN, LOW);
  delay(10);
  digitalWrite(DONE_PIN, HIGH);
  delay(10);
}
