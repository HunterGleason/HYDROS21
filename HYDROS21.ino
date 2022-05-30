/*
   Date: 2022-07-11
   Contact: Hunter Gleason (Hunter.Gleason@alumni.unbc.ca)
   Organization: Ministry of Forests (MOF)
   Description: This script is written for the purpose of gathering hydrometric data, including the real time transmission of the data via the Iridium satallite network.
   Using the HYDROS21 probe () water level, temperature and electrical conductivity are measured and stored to a SD card at a specified logging interval. Hourly averages
   of the data are transmitted over Iridium at a specified transmission frequency. The string output transmitted over Iridium is formatted to be compatible with digestion
   into a database run by MOF. The various parameters required by the script are specified by a TXT file on the SD card used for logging, for more details on usage of this
   script please visit the GitHub repository ().
*/


/*Include the libraries we need*/
#include "RTClib.h" //Needed for communication with Real Time Clock
#include <SPI.h> //Needed for working with SD card
#include <SD.h> //Needed for working with SD card
#include "ArduinoLowPower.h" //Needed for putting Feather M0 to sleep between samples
#include <IridiumSBD.h> //Needed for communication with IRIDIUM modem 
#include <CSV_Parser.h> //Needed for parsing CSV data
#include <SDI12.h> //Needed for SDI-12 communication

/*Define global constants*/
const byte led = 13; // Built in led pin
const byte chipSelect = 4; // Chip select pin for SD card
const byte irid_pwr_pin = 6; // Power base PN2222 transistor pin to Iridium modem
const byte HydSetPin = 5; //Power relay set pin to HYDROS21
const byte HydUnsetPin = 9; //Power relay unset pin to HYDROS21
const byte dataPin = 12; // The pin of the SDI-12 data bus


/*Define global vars */
char **filename; // Name of log file(Read from PARAM.txt)
char **start_time;// Time at which first Iridum transmission should occur (Read from PARAM.txt)
String filestr; // Filename as string
int16_t *sample_intvl; // Sample interval in minutes (Read from PARAM.txt)
int16_t *irid_freq; // Iridium transmit freqency in hours (Read from PARAM.txt)
uint32_t irid_freq_hrs; // Iridium transmit freqency in hours
uint32_t sleep_time;// Logger sleep time in milliseconds
DateTime transmit_time;// Datetime varible for keeping IRIDIUM transmit time
DateTime present_time;// Var for keeping the current time
int err; //IRIDIUM status var
String myCommand   = "";// SDI-12 command var
String sdiResponse = "";// SDI-12 responce var

/*Define Iridium seriel communication as Serial1 */
#define IridiumSerial Serial1

/*SDI-12 sensor address, assumed to be 0*/
#define SENSOR_ADDRESS 0

/*Create library instances*/
RTC_PCF8523 rtc; // Setup a PCF8523 Real Time Clock instance
File dataFile; // Setup a log file instance
IridiumSBD modem(IridiumSerial); // Declare the IridiumSBD object
SDI12 mySDI12(dataPin);// Define the SDI-12 bus


/*Function reads data from a .csv logfile, and uses Iridium modem to send all observations
   since the previous transmission over satellite at midnight on the RTC.
*/
int send_hourly_data()
{

  // For capturing Iridium errors
  int err;

  // Provide power to Iridium Modem
  digitalWrite(irid_pwr_pin, HIGH);
  // Allow warm up
  delay(200);


  // Start the serial port connected to the satellite modem
  IridiumSerial.begin(19200);

  if (err != ISBD_SUCCESS)
  {
    digitalWrite(led, HIGH);
    delay(1000);
    digitalWrite(led, LOW);
    delay(1000);
    digitalWrite(led, HIGH);
    delay(1000);
    digitalWrite(led, LOW);
    delay(1000);
  }



  // Set paramters for parsing the log file
  CSV_Parser cp("sdfd", true, ',');

  // Varibles for holding data fields
  char **datetimes;
  int16_t *h2o_depths;
  float *h2o_temps;
  int16_t *h2o_ecs;


  // Read HOURLY.CSV file
  cp.readSDfile("/HOURLY.CSV");

  int num_rows = cp.getRowsCount();

  //Populate data arrays from logfile
  datetimes = (char**)cp["datetime"];
  h2o_depths = (int16_t*)cp["h2o_depth_mm"];
  h2o_temps = (float*)cp["h2o_temp_deg_c"];
  h2o_ecs = (int16_t*)cp["ec_dS_m"];

  //Binary bufffer for iridium transmission (max allowed buffer size 340 bytes)
  uint8_t dt_buffer[340];

  //Buffer index counter var
  int buff_idx = 0;

  //Formatted for CGI script >> sensor_letter_code:date_of_first_obs:hour_of_first_obs:data
  String datestamp = "ABC:" + String(datetimes[0]).substring(0, 10) + ":" + String(datetimes[0]).substring(11, 13) + ":";

  //Populate buffer with datestamp
  for (int i = 0; i < datestamp.length(); i++)
  {
    dt_buffer[buff_idx] = datestamp.charAt(i);
    buff_idx++;
  }

  //Get start and end date information from HOURLY.CSV time series data
  int start_year = String(datetimes[0]).substring(0, 4).toInt();
  int start_month = String(datetimes[0]).substring(5, 7).toInt();
  int start_day = String(datetimes[0]).substring(8, 10).toInt();
  int start_hour = String(datetimes[0]).substring(11, 13).toInt();
  int end_year = String(datetimes[num_rows - 1]).substring(0, 4).toInt();
  int end_month = String(datetimes[num_rows - 1]).substring(5, 7).toInt();
  int end_day = String(datetimes[num_rows - 1]).substring(8, 10).toInt();
  int end_hour = String(datetimes[num_rows - 1]).substring(11, 13).toInt();

  //Set the start time to rounded first datetime hour in CSV
  DateTime start_dt = DateTime(start_year, start_month, start_day, start_hour, 0, 0);
  //Set the end time to end of last datetime hour in CSV
  DateTime end_dt = DateTime(end_year, end_month, end_day, end_hour + 1, 0, 0);
  //For keeping track of the datetime at the end of each hourly interval
  DateTime intvl_dt;

  while (start_dt < end_dt)
  {

    intvl_dt = start_dt + TimeSpan(0, 1, 0, 0);

    //Declare average vars for each HYDROS21 output
    float mean_depth = -9999.0;
    float mean_temp = -9999.0;
    float mean_ec = -9999.0;
    boolean is_first_obs = false;
    int N = 0;

    //For each observation in the HOURLY.CSV
    for (int i = 0; i < num_rows; i++) {

      //Read the datetime and hour
      String datetime = String(datetimes[i]);
      int dt_year = datetime.substring(0, 4).toInt();
      int dt_month = datetime.substring(5, 7).toInt();
      int dt_day = datetime.substring(8, 10).toInt();
      int dt_hour = datetime.substring(11, 13).toInt();
      int dt_min = datetime.substring(14, 16).toInt();
      int dt_sec = datetime.substring(17, 19).toInt();

      DateTime obs_dt = DateTime(dt_year, dt_month, dt_day, dt_hour, dt_min, dt_sec);

      //Check in the current observatioin falls withing time window
      if (obs_dt >= start_dt && obs_dt <= intvl_dt)
      {

        //Get data
        float h2o_depth = (float) h2o_depths[i];
        float h2o_temp = h2o_temps[i];
        float h2o_ec = (float) h2o_ecs[i];

        //Check if this is the first observation for the hour
        if (is_first_obs == false)
        {
          //Update average vars
          mean_depth = h2o_depth;
          mean_temp = h2o_temp;
          mean_ec = h2o_ec;
          is_first_obs = true;
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
      mean_depth = (mean_depth / (float) N);
      mean_temp = (mean_temp / (float) N) * 10.0;
      mean_ec = (mean_ec / (float) N);


      //Assemble the data string
      String datastring = String(round(mean_depth)) + "," + String(round(mean_temp)) + "," + String(round(mean_ec)) + ':';


      //Populate the buffer with the datastring
      for (int i = 0; i < datastring.length(); i++)
      {
        dt_buffer[buff_idx] = datastring.charAt(i);
        buff_idx++;
      }

    }

    start_dt = intvl_dt;

    digitalWrite(LED, HIGH);
    //transmit binary buffer data via iridium
    err = modem.sendSBDBinary(dt_buffer, buff_idx);
    digitalWrite(LED, LOW);


  }

  // Prevent from trying to charge to quickly, low current setup
  modem.setPowerProfile(IridiumSBD::USB_POWER_PROFILE);

  // Begin satellite modem operation, blink led (1-sec) if there was an issue
  err = modem.begin();

  if (err == ISBD_IS_ASLEEP)
  {
    modem.begin();
  }

  //Indicate the modem is trying to send with led
  digitalWrite(led, HIGH);

  //transmit binary buffer data via iridium
  err = modem.sendSBDBinary(dt_buffer, buff_idx);

  //If transmission failed and message is not too large try once more, increase time out
  if (err != ISBD_SUCCESS && err != 13)
  {
    err = modem.begin();
    modem.adjustSendReceiveTimeout(500);
    err = modem.sendSBDBinary(dt_buffer, buff_idx);

  }

  digitalWrite(led, LOW);


  //Kill power to Iridium Modem by writing the base pin low on PN2222 transistor
  digitalWrite(irid_pwr_pin, LOW);
  delay(30);


  //Remove previous daily values CSV as long as send was succesfull, or if message is more than 340 bites

  SD.remove("/HOURLY.CSV");

  return err;


}

String sample_hydros21()
{
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
  String hydrostring = present_time.timestamp() + ",";
  hydrostring = hydrostring + sdiResponse;

  //Switch power to HYDR21 via latching relay
  digitalWrite(HydUnsetPin, HIGH);
  delay(30);
  digitalWrite(HydUnsetPin, LOW);

  return hydrostring;
}


/*
   The setup function. We only start the sensors, RTC and SD here
*/
void setup(void)
{
  // Set pin modes
  pinMode(led, OUTPUT);
  digitalWrite(led, LOW);
  pinMode(HydSetPin, OUTPUT);
  digitalWrite(HydSetPin, LOW);
  pinMode(HydUnsetPin, OUTPUT);
  digitalWrite(HydUnsetPin, HIGH);
  delay(30);
  digitalWrite(HydUnsetPin, LOW);
  pinMode(irid_pwr_pin, OUTPUT);
  digitalWrite(irid_pwr_pin, LOW);


  //Make sure a SD is available (2-sec flash led means SD card did not initialize)
  while (!SD.begin(chipSelect)) {
    digitalWrite(led, HIGH);
    delay(2000);
    digitalWrite(led, LOW);
    delay(2000);
  }

  //Set paramters for parsing the parameter file PARAM.txt
  CSV_Parser cp("sdds", true, ',');


  //Read the parameter file 'PARAM.txt', blink (1-sec) if fail to read
  while (!cp.readSDfile("/PARAM.txt"))
  {
    digitalWrite(led, HIGH);
    delay(1000);
    digitalWrite(led, LOW);
    delay(1000);
  }


  //Populate data arrays from parameter file PARAM.txt
  filename = (char**)cp["filename"];
  sample_intvl = (int16_t*)cp["sample_intvl"];
  irid_freq = (int16_t*)cp["irid_freq"];
  start_time = (char**)cp["start_time"];

  //Sleep time between samples in miliseconds
  sleep_time = sample_intvl[0] * 1000;

  //Log file name
  filestr = String(filename[0]);

  //Iridium transmission frequency in hours
  irid_freq_hrs = irid_freq[0];

  //Get logging start time from parameter file
  int start_hour = String(start_time[0]).substring(0, 3).toInt();
  int start_minute = String(start_time[0]).substring(3, 5).toInt();
  int start_second = String(start_time[0]).substring(6, 8).toInt();

  // Make sure RTC is available
  while (!rtc.begin())
  {
    digitalWrite(led, HIGH);
    delay(500);
    digitalWrite(led, LOW);
    delay(500);
  }

  //Get the present time
  present_time = rtc.now();

  //Update the transmit time to the start time for present date
  transmit_time = DateTime(present_time.year(),
                           present_time.month(),
                           present_time.day(),
                           start_hour + irid_freq_hrs,
                           start_minute,
                           start_second);


  //Begin HYDROS21
  mySDI12.begin();


}

/*
   Main function, sample HYDROS21 and sample interval, log to SD, and transmit hourly averages over IRIDIUM at midnight on the RTC
*/
void loop(void)
{

  //Get the present datetime
  present_time = rtc.now();

  //If the presnet time has reached transmit_time send all data since last transmission averaged hourly
  if (present_time >= transmit_time)
  {
    // Send the hourly data over Iridium
    int send_status = send_hourly_data();

    //Update next Iridium transmit time by 'irid_freq_hrs'
    transmit_time = (transmit_time + TimeSpan(0, irid_freq_hrs, 0, 0));
  }


  //Sample the HYDROS21 sensor for a reading
  String datastring = sample_hydros21();

  //Write header if first time writing to the logfile
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


  /*The HOURLY.CSV file is the same as the log-file, but only contains observations since the last transmission and is used by the send_hourly_data() function */

  //Write header if first time writing to the DAILY file
  if (!SD.exists("HOURLY.CSV"))
  {
    //Write datastring and close logfile on SD card
    dataFile = SD.open("HOURLY.CSV", FILE_WRITE);
    if (dataFile)
    {
      dataFile.println("datetime,h2o_depth_mm,h2o_temp_deg_c,ec_dS_m");
      dataFile.close();
    }
  } else {

    //Write datastring and close logfile on SD card
    dataFile = SD.open("HOURLY.CSV", FILE_WRITE);
    if (dataFile)
    {
      dataFile.println(datastring);
      dataFile.close();
    }
  }

  //Flash led to idicate a sample was just taken
  digitalWrite(led, HIGH);
  delay(250);
  digitalWrite(led, LOW);
  delay(250);

  //Put logger in low power mode for lenght 'sleep_time'
  LowPower.sleep(sleep_time);


}
