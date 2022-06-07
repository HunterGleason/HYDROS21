

// Include the libraries we need
#include "RTClib.h" //Needed for communication with Real Time Clock
#include <SPI.h>//Needed for working with SD card
#include <SD.h>//Needed for working with SD card
#include "ArduinoLowPower.h"//Needed for putting Feather M0 to sleep between samples
#include <IridiumSBD.h>//Needed for communication with IRIDIUM modem 
#include <CSV_Parser.h>//Needed for parsing CSV data
#include <SDI12.h>//Needed for SDI-12 communication



/*Define global constants*/
const byte LED = 13; // Built in LED pin
const byte chipSelect = 4; // For SD card
const byte IridPwrPin = 6; // Pwr pin to Iridium modem
const byte HydSetPin = 5; //Pwr set pin to HYDROS21
const byte HydUnsetPin = 9; //Pwr unset pin to HYDROS21
const byte dataPin = 12; // The pin of the SDI-12 data bus

// Define Iridium seriel communication COM1
#define IridiumSerial Serial1

//SDI-12 sensor address, assumed to be 0
#define SENSOR_ADDRESS 0

/*Create library instances*/
RTC_PCF8523 rtc; // Setup a PCF8523 Real Time Clock instance
File dataFile; // Setup a log file instance
IridiumSBD modem(IridiumSerial); // Declare the IridiumSBD object
SDI12 mySDI12(dataPin);// Define the SDI-12 bus

/*Define global vars */
char **filename; //Name of log file
String filestr; //Filename as string
int16_t *sample_intvl; //Sample interval in minutes
int sample_intvl_min; //sample interval as int
long IridTime;//Dattime varible for keeping IRIDIUM transmit time
int err; //IRIDIUM status var
String myCommand   = "";//SDI-12 command var
String sdiResponse = "";//SDI-12 responce var
DateTime current_time;//Var for keeping the current time

//Logger sleep time in milliseconds
uint32_t sleep_time;

/*Function pings RTC for datetime and returns formated datestamp YYYY-MM-DD HH:MM:SS*/
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
  String datestring = yr_str + "-" + mnth_str + "-" + day_str + " " + hr_str + ":" + min_str + ":" + sec_str + ",";

  return datestring;
}

/*Function reads data from a DAILY.CSV logfile, and uses Iridium modem to send all observations
   for the previous day over satellite at midnight on the RTC.
*/
int send_daily_data()
{

  //For capturing Iridium errors
  int err;

  //Provide power to Iridium Modem
  digitalWrite(IridPwrPin, HIGH);
  delay(200);


  // Start the serial port connected to the satellite modem
  IridiumSerial.begin(19200);

  // Begin satellite modem operation
  err = modem.begin();
  if (err != ISBD_SUCCESS)
  {
    digitalWrite(LED, HIGH);
    delay(1000);
    digitalWrite(LED, LOW);
    delay(1000);
    digitalWrite(LED, HIGH);
    delay(1000);
    digitalWrite(LED, LOW);
    delay(1000);
  }



  //Set paramters for parsing the log file
  CSV_Parser cp("sdfd", true, ',');

  //Varibles for holding data fields
  char **datetimes;
  int16_t *h2o_depths;
  float *h2o_temps;
  int16_t *h2o_ecs;

  //Read IRID.CSV
  cp.readSDfile("/DAILY.csv");


  //Populate data arrays from logfile
  datetimes = (char**)cp["datetime"];
  h2o_depths = (int16_t*)cp["h2o_depth_mm"];
  h2o_temps = (float*)cp["h2o_temp_deg_c"];
  h2o_ecs = (int16_t*)cp["ec_dS_m"];

  //Binary bufffer for iridium transmission (max allowed buffer size 340 bytes)
  uint8_t dt_buffer[340];
  int buff_idx = 0;

  //Get the start datetime stamp as string
  String datestamp = String(datetimes[0]).substring(0, 10);

  //Populate buffer with datestamp
  for (int i = 0; i < datestamp.length(); i++)
  {
    dt_buffer[buff_idx] = datestamp.charAt(buff_idx);
    buff_idx++;
  }

  dt_buffer[buff_idx] = ':';
  buff_idx++;

  //For each hour 0-23
  for (int day_hour = 0; day_hour < 24; day_hour++)
  {

    //Declare average vars for each HYDROS21 output
    float mean_depth = 999.0;
    float mean_temp = 999.0;
    float mean_ec = 999.0;
    boolean is_obs = false;
    int N = 0;

    //For each observation in the DAILY.CSV
    for (int i = 0; i < cp.getRowsCount(); i++) {

      //Read the datetime and hour
      String datetime = String(datetimes[i]);
      int dt_hour = datetime.substring(11, 13).toInt();

      //If the hour matches day hour
      if (dt_hour == day_hour)
      {

        //Get data
        float h2o_depth = (float) h2o_depths[i];
        float h2o_temp = h2o_temps[i];
        float h2o_ec = (float) h2o_ecs[i];

        //Check if this is the first observation for the hour
        if (is_obs == false)
        {
          //Update average vars
          mean_depth = h2o_depth;
          mean_temp = h2o_temp;
          mean_ec = h2o_ec;
          is_obs = true;
          N++;
        } else {
          //Update average vars
          mean_depth = mean_depth + h2o_depth;
          mean_temp = mean_temp + h2o_temp;
          mean_ec = mean_ec + h2o_ec;
          N++;
        }

      }
    }

    //Check if there were any observations for the hour
    if (N > 0)
    {
      //Compute averages
      mean_depth = mean_depth / N;
      mean_temp = (mean_temp / N) * 10.0;
      mean_ec = mean_ec / N;
    }

    //Assemble the data string, no EC for now
    //String datastring = String(round(mean_depth)) + ',' + String(round(mean_temp)) + ',' + String(round(mean_ec)) + ':';
    String datastring = String(round(mean_depth)) + ',' + String(round(mean_temp)) + ':';

    //Populate the buffer with the datastring
    for (int i = 0; i < datastring.length(); i++)
    {
      dt_buffer[buff_idx] = datastring.charAt(i);
      buff_idx++;
    }

  }

  //Indicate the modem is trying to send
  digitalWrite(LED, HIGH);
  //transmit binary buffer data via iridium
  err = modem.sendSBDBinary(dt_buffer, buff_idx);
  digitalWrite(LED, LOW);

  //Indicate the ISBD_SUCCESS
  if (err != ISBD_SUCCESS)
  {
    digitalWrite(LED, HIGH);
    delay(5000);
    digitalWrite(LED, LOW);
    delay(5000);
    digitalWrite(LED, HIGH);
    delay(5000);
    digitalWrite(LED, LOW);
    delay(5000);
  }


  //Kill power to Iridium Modem
  digitalWrite(IridPwrPin, LOW);
  delay(30);


  //Remove previous daily values CSV
  SD.remove("/DAILY.CSV");

  return err;


}


/*
   The setup function. We only start the sensors, RTC and SD here
*/
void setup(void)
{
  // Set pin modes
  pinMode(LED, OUTPUT);
  digitalWrite(LED, LOW);
  pinMode(HydSetPin, OUTPUT);
  digitalWrite(HydSetPin, LOW);
  pinMode(HydUnsetPin, OUTPUT);
  digitalWrite(HydUnsetPin, HIGH);
  delay(30);
  digitalWrite(HydUnsetPin, LOW);
  pinMode(IridPwrPin, OUTPUT);
  digitalWrite(IridPwrPin, LOW);


  //Make sure a SD is available (1-sec flash LED means SD card did not initialize)
  while (!SD.begin(chipSelect)) {
    digitalWrite(LED, HIGH);
    delay(2000);
    digitalWrite(LED, LOW);
    delay(2000);
  }

  //Set paramters for parsing the log file
  CSV_Parser cp("sd", true, ',');


  //Read IRID.CSV
  while (!cp.readSDfile("/PARAM.txt"))
  {
    digitalWrite(LED, HIGH);
    delay(1000);
    digitalWrite(LED, LOW);
    delay(1000);
  }


  //Populate data arrays from logfile
  filename = (char**)cp["filename"];
  sample_intvl = (int16_t*)cp["sample_intvl"];

  sample_intvl_min = String(sample_intvl[0]).toInt();

  sleep_time = sample_intvl[0] * 60000;
  filestr = String(filename[0]);


  // Make sure RTC is available
  while (!rtc.begin())
  {
    digitalWrite(LED, HIGH);
    delay(500);
    digitalWrite(LED, LOW);
    delay(500);
  }

  //Begin HYDROS21
  mySDI12.begin();


}

/*
   Main function, sample HYDROS21 and sample interval, log to SD, and transmit hourly averages over IRIDIUM at midnight on the RTC
*/
void loop(void)
{

  //Get the curent datetime
  current_time = rtc.now();

  //If the hour is 0, and minute is less than sample_intvl_min (i.e., just past midnight), send an Iridium message with daily values
  if ((current_time.hour() == 0) && (current_time.minute() <= sample_intvl_min+(sample_intvl_min/2)))
  {
    int send_status = send_daily_data();
  }
  
  //Switch power to HYDR21 via latching relay
  digitalWrite(HydSetPin, HIGH);
  delay(30);
  digitalWrite(HydSetPin, LOW);

  //Give HYDROS21 sensor time to power up
  delay(1000);

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

  //Clear buffer
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

  //subset responce
  sdiResponse = sdiResponse.substring(3);

  for (int i = 0; i < sdiResponse.length(); i++)
  {

    char c = sdiResponse.charAt(i);

    if (c == '+')
    {
      sdiResponse.setCharAt(i, ',');
    }

  }

  //clear buffer
  if (sdiResponse.length() > 1)
    mySDI12.clearBuffer();

  //Assemble datastring
  String datastring = gen_date_str(current_time);
  datastring = datastring + sdiResponse;

  //Switch power to HYDR21 via latching relay
  digitalWrite(HydUnsetPin, HIGH);
  delay(30);
  digitalWrite(HydUnsetPin, LOW);


  //Write header if first time writing to the file
  if (!SD.exists(filestr.c_str()))
  {
    dataFile = SD.open(filestr.c_str(), FILE_WRITE);
    if (dataFile)
    {
      dataFile.println("datetime,h2o_depth_mm,h2o_temp_deg_c,ec_dS_m");
      dataFile.close();
    }

  } else {
    //Write datastring and close logfile on SD card
    dataFile = SD.open(filestr.c_str(), FILE_WRITE);
    if (dataFile)
    {
      dataFile.println(datastring);
      dataFile.close();
    }

  }


  //Write header if first time writing to the file
  if (!SD.exists("DAILY.CSV"))
  {
    //Write datastring and close logfile on SD card
    dataFile = SD.open("DAILY.CSV", FILE_WRITE);
    if (dataFile)
    {
      dataFile.println("datetime,h2o_depth_mm,h2o_temp_deg_c,ec_dS_m");
      dataFile.close();
    }
  } else {
    //Write datastring and close logfile on SD card
    dataFile = SD.open("DAILY.CSV", FILE_WRITE);
    if (dataFile)
    {
      dataFile.println(datastring);
      dataFile.close();
    }
  }

  //Flash LED to idicate a sample was just taken
  digitalWrite(LED, HIGH);
  delay(250);
  digitalWrite(LED, LOW);
  delay(250);

  //Put logger in low power mode for lenght 'sleep_time'
  LowPower.sleep(sleep_time);


}
