/*Include the libraries we need*/
#include "RTClib.h" //Needed for communication with Real Time Clock
#include <SPI.h>//Needed for working with SD card
#include <SD.h>//Needed for working with SD card
#include <ArduinoLowPower.h>//Needed for putting Feather M0 to sleep between samples
#include <IridiumSBD.h>//Needed for communication with IRIDIUM modem 
#include <CSV_Parser.h>//Needed for parsing CSV data
#include <SDI12.h>//Needed for SDI-12 communication
#include <QuickStats.h>//Needed for computing medians

/*Define global constants*/
const byte LED = 13; // Built in LED pin
const byte chipSelect = 4; // For SD card
const byte IridPwrPin = 6; // Pwr pin to Iridium modem
const byte HydSetPin = 5; //Pwr set pin to HYDROS21
const byte HydUnsetPin = 9; //Pwr unset pin to HYDROS21
const byte dataPin = 12; // The pin of the SDI-12 data bus
const byte wiper = A2; // Pin for wiper on Analite 195
const byte TurbSetPin = 10; // Pin to set pwr relay to Analite 195
const byte TurbUnsetPin = 11; // Pin to unset pwr relay to Analite 195
const byte TurbAlog = A1; // Pin for reading analog outout from voltage divder (R1=1000 Ohm, R2=5000 Ohm) conncted to Analite 195


/*Define global vars */
char **filename; //Name of log file
char **start_time; //Time at which to begin logging
String filestr; //Filename as string
int16_t *sample_intvl; //Sample interval in minutes
int16_t *irid_freq; //User provided transmission interval for Iridium modem in hours
float *turb_slope; //The linear slope parameter for converting 12-bit analog value to NTU, i.e., from calibration
float m;
float b;
float *turb_intercept; //The intercept parameter for converting 12-bit analog to NTU, i.e., form calibration
int wiper_cnt = 0;
uint32_t irid_freq_hrs;
uint32_t sleep_time;//Logger sleep time in milliseconds
DateTime transmit_time;//Datetime varible for keeping IRIDIUM transmit time
DateTime present_time;//Var for keeping the current time
int err; //IRIDIUM status var
String myCommand   = "";//SDI-12 command var
String sdiResponse = "";//SDI-12 responce var

/*Define Iridium seriel communication COM1*/
#define IridiumSerial Serial1

/*SDI-12 sensor address, assumed to be 0*/
#define SENSOR_ADDRESS 0

/*Create library instances*/
RTC_PCF8523 rtc; // Setup a PCF8523 Real Time Clock instance
File dataFile; // Setup a log file instance
IridiumSBD modem(IridiumSerial); // Declare the IridiumSBD object
SDI12 mySDI12(dataPin);// Define the SDI-12 bus
QuickStats stats;//Instance of QuickStats

/*Function reads data from HOURLY.CSV logfile and uses Iridium modem to send all observations
   since the previous transmission over satellite at midnight on the RTC.
*/
int send_hourly_data()
{

  int err;// For capturing Iridium errors

  digitalWrite(IridPwrPin, HIGH); // Provide power to Iridium Modem
  delay(200);  // Allow warm up

  IridiumSerial.begin(19200); // Start the serial port connected to the satellite modem

  if (err != ISBD_SUCCESS) // Indicate to user if there was a connection issue with modem 
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

  CSV_Parser cp("sdfdf", true, ',');  // Set paramters for parsing the log file (datetime,h2o_depths,h2o_temps,h2o_ecs)

  char **datetimes;  // Datetimes pointer
  int16_t *h2o_depths; // h2o_depths (stage) pointer 
  float *h2o_temps; // h2o_temps (water temp) pointer 
  int16_t *h2o_ecs; // h2o_ecs (electrical conductivity) pointer 
  float *h2o_turbs; // h2o_turbs (turbidity) pointer

  cp.readSDfile("/HOURLY.CSV"); // Read HOURLY.CSV file

  int num_rows = cp.getRowsCount(); // Get the number of rows in HOURLY.CSV

  /*Populate data arrays from logfile*/
  datetimes = (char**)cp["datetime"];
  h2o_depths = (int16_t*)cp["h2o_depth_mm"];
  h2o_temps = (float*)cp["h2o_temp_deg_c"];
  h2o_ecs = (int16_t*)cp["ec_dS_m"];
  h2o_turbs = (float*)cp["turb_ntu"];

  uint8_t dt_buffer[340];//Binary bufffer for iridium transmission (max allowed buffer size 340 bytes)

  int buff_idx = 0;//Buffer index counter var

  /*Formatted for CGI script >> sensor_letter_code:date_of_first_obs:hour_of_first_obs:data*/
  String datestamp = "ABCD:" + String(datetimes[0]).substring(0, 10) + ":" + String(datetimes[0]).substring(11, 13) + ":";

  /*Populate buffer with datestamp bytes*/
  for (int i = 0; i < datestamp.length(); i++)
  {
    dt_buffer[buff_idx] = datestamp.charAt(i);
    buff_idx++;
  }

  int start_year = String(datetimes[0]).substring(0, 4).toInt();//Get start year of first observation 
  int start_month = String(datetimes[0]).substring(5, 7).toInt();//Get start month of first observation 
  int start_day = String(datetimes[0]).substring(8, 10).toInt();//Get start day of first observation 
  int start_hour = String(datetimes[0]).substring(11, 13).toInt();//Get start hour of first observation 
  int end_year = String(datetimes[num_rows - 1]).substring(0, 4).toInt();//Get end year of first observation 
  int end_month = String(datetimes[num_rows - 1]).substring(5, 7).toInt();//Get end month of first observation 
  int end_day = String(datetimes[num_rows - 1]).substring(8, 10).toInt();//Get end day of first observation 
  int end_hour = String(datetimes[num_rows - 1]).substring(11, 13).toInt();//Get end hour of first observation 

  DateTime start_dt = DateTime(start_year, start_month, start_day, start_hour, 0, 0); //Set the start time to rounded first datetime hour in CSV
  DateTime end_dt = DateTime(end_year, end_month, end_day, end_hour + 1, 0, 0); //Set the end time to end of last datetime hour in CSV
  DateTime intvl_dt; //For keeping track of the datetime at the end of each hourly interval

  while (start_dt < end_dt) //While the start datetime is less than the end datetime
  {

    intvl_dt = start_dt + TimeSpan(0, 1, 0, 0);//Set interval datetime to the end of the current hour (start_dt)

    /*Declare average vars for each HYDROS21 output as missing*/
    float mean_depth = -9999.0;
    float mean_temp = -9999.0;
    float mean_ec = -9999.0;
    float mean_turb = -9999.0;
    
    boolean is_first_obs = false;//Boolean for keeping track of if it is the first obs
    int N = 0;//Sample size counter 

    for (int i = 0; i < num_rows; i++) {//For each observation in the HOURLY.CSV

      /*Read the datetime and parse*/
      String datetime = String(datetimes[i]);
      int dt_year = datetime.substring(0, 4).toInt();
      int dt_month = datetime.substring(5, 7).toInt();
      int dt_day = datetime.substring(8, 10).toInt();
      int dt_hour = datetime.substring(11, 13).toInt();
      int dt_min = datetime.substring(14, 16).toInt();
      int dt_sec = datetime.substring(17, 19).toInt();
      
      DateTime obs_dt = DateTime(dt_year, dt_month, dt_day, dt_hour, dt_min, dt_sec);//Create DateTime object from parsed datetime 

      if (obs_dt >= start_dt && obs_dt <= intvl_dt)//Check in the current observatioin falls withing interval window
      {

        /*Get data at row i*/
        float h2o_depth = (float) h2o_depths[i];
        float h2o_temp = h2o_temps[i];
        float h2o_ec = (float) h2o_ecs[i];
        float h2o_turb = (float) h2o_turbs[i];

        if (is_first_obs == false)//Check if this is the first observation for the hour
        {
          /*Update average vars to equal first obs value*/
          mean_depth = h2o_depth;
          mean_temp = h2o_temp;
          mean_ec = h2o_ec;
          mean_turb = h2o_turb;
          
          is_first_obs = true;//No longer first observation 
          N++;//Increment sample counter 
          
        } else {
          /*Update average vars cumlative value*/
          mean_depth = mean_depth + h2o_depth;
          mean_temp = mean_temp + h2o_temp;
          mean_ec = mean_ec + h2o_ec;
          mean_turb = mean_turb + h2o_turb;
          
          N++;//Increment sample counter 
        }
      }
    }

    if (N > 0)//Check if there were any observations for the interval hour
    {
      /*Compute averages*/
      mean_depth = (mean_depth / (float) N);
      mean_temp = (mean_temp / (float) N) * 10.0;
      mean_ec = (mean_ec / (float) N);
      mean_turb = (mean_turb / (float) N);

      String datastring = String(round(mean_depth)) + ',' + String(round(mean_temp)) + ',' + String(round(mean_ec)) + ',' + String(round(mean_turb)) + ':'; //Assemble the data string
      
      /*Populate the buffer with the datastring*/
      for (int i = 0; i < datastring.length(); i++)
      {
        dt_buffer[buff_idx] = datastring.charAt(i);
        buff_idx++;
      }

    }

    start_dt = intvl_dt; //Update start datetime to equal interval date time, i.e., + one hour 

  }
  
  modem.setPowerProfile(IridiumSBD::USB_POWER_PROFILE);// Prevent from trying to charge to quickly, low current setup

  err = modem.begin();//Begin satellite modem operation

  if (err == ISBD_IS_ASLEEP)//Call begin once more if modem is asleep for some reason (as found in previous launches)
  {
    modem.begin();
  }
  
  digitalWrite(LED, HIGH); //Indicate the modem is trying to send with LED

  err = modem.sendSBDBinary(dt_buffer, buff_idx);//Transmit binary buffer data via iridium

  if (err != ISBD_SUCCESS && err != 13)//If transmission faiLED and message is not too large try once more, increase time out
  {
    err = modem.begin();
    modem.adjustSendReceiveTimeout(500);
    err = modem.sendSBDBinary(dt_buffer, buff_idx);

  }

  digitalWrite(LED, LOW);//Indicate no longer transmitting 

  digitalWrite(IridPwrPin, LOW);//Kill power to Iridium Modem by writing the base pin low on PN2222 transistor
  delay(30);

  SD.remove("/HOURLY.CSV");//Remove HOURLY.CSV so that only new data will be sent next transmission 

  return err;//Return err code, not used but can be helpful for trouble shooting 

}

/*Function uses SDI-12 protocol to ping the HYDROS21 sensor for a sample, returns datestamp with the values 
 * measured by the probe appended, comma seperated 
 */
String sample_hydros21()
{
  /*Switch power to HYDR21 via latching relay*/
  digitalWrite(HydSetPin, HIGH);
  delay(30);
  digitalWrite(HydSetPin, LOW);

  delay(1000); //Give HYDROS21 sensor time to settle 

  myCommand = String(SENSOR_ADDRESS) + "M!";// first command to take a measurement

  mySDI12.sendCommand(myCommand);
  delay(30);  // wait a while for a response

  while (mySDI12.available()) {  // build response string
    char c = mySDI12.read();
    if ((c != '\n') && (c != '\r')) {
      sdiResponse += c;
      delay(10);  // 1 character ~ 7.5ms
    }
  }

  /*Clear buffer*/
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

  /*Switch power off to HYDROS21 via latching relay*/
  digitalWrite(HydUnsetPin, HIGH);
  delay(30);
  digitalWrite(HydUnsetPin, LOW);

  return hydrostring;
}

//This function samples Analite 195 analog turbidity probe 100 times and returns the scaled median NTU value from provided (PARAM.txt) linear calibration
int sample_analite_195()
{
  //Power up analite 195
  digitalWrite(TurbSetPin, HIGH);
  delay(30);
  digitalWrite(TurbSetPin, LOW);

  //Let probe settle
  delay(1000);

  float values[100];//Array for storing sampled distances

  //Probe will atomatically wipe after 30 power cycles, ititiate at 25 will prvent wiper covering sensor during reading, and prevent bio-foul
  if (wiper_cnt >= 5)
  {
    digitalWrite(wiper, HIGH);
    delay(100);
    digitalWrite(wiper, LOW);

    wiper_cnt = 0;

    //Let wiper complete rotation
    delay(30000);

  } else {

    wiper_cnt++;
    digitalWrite(wiper, LOW);

  }


  for(int i = 0; i<100; i++)
  {
    //Read analog value from probe
    values[i]= (float) analogRead(TurbAlog);
    delay(5);
  }

  float med_turb_alog = stats.median(values, 100);//Compute median 12-bit analog val

  //Convert analog value (0-4096) to NTU from provided linear calibration coefficients
  float ntu = (m * med_turb_alog) + b;

  int ntu_int = round(ntu);

  //Power down the probe
  digitalWrite(TurbUnsetPin, HIGH);
  delay(30);
  digitalWrite(TurbUnsetPin, LOW);

  return ntu_int;
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
  pinMode(wiper, OUTPUT);
  digitalWrite(wiper, LOW);
  pinMode(TurbSetPin, OUTPUT);
  pinMode(TurbUnsetPin, OUTPUT);
  pinMode(TurbAlog, INPUT);
  
  //Set analog resolution to 12-bit
  analogReadResolution(12);


  //Make sure a SD is available (1-sec flash LED means SD card did not initialize)
  while (!SD.begin(chipSelect)) {
    digitalWrite(LED, HIGH);
    delay(2000);
    digitalWrite(LED, LOW);
    delay(2000);
  }

  //Set paramters for parsing the log file
  CSV_Parser cp("sddffs", true, ',');


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
  irid_freq = (int16_t*)cp["irid_freq"];
  turb_slope = (float*)cp["turb_slope"];
  turb_intercept = (float*)cp["turb_intercept"];
  start_time = (char**)cp["start_time"];


  //Define global vars provided from parameter file
  sleep_time = sample_intvl[0] * 1000;
  filestr = String(filename[0]);
  irid_freq_hrs = irid_freq[0];
  m = turb_slope[0];
  b = turb_intercept[0];

  //Get logging start time from parameter file
  int start_hour = String(start_time[0]).substring(0, 3).toInt();
  int start_minute = String(start_time[0]).substring(3, 5).toInt();
  int start_second = String(start_time[0]).substring(6, 8).toInt();


  // Make sure RTC is available
  while (!rtc.begin())
  {
    digitalWrite(LED, HIGH);
    delay(500);
    digitalWrite(LED, LOW);
    delay(500);
  }

  //Set intial transmit time and start time, wait until start is reached
  present_time = rtc.now();
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
    //Send all data since last transmission over Iridium, averaged hourly
    int send_status = send_hourly_data();

    //Update next Iridium transmit time by 'irid_freq_hrs'
    transmit_time = (transmit_time + TimeSpan(0, irid_freq_hrs, 0, 0));
  }

  //Sample the HYDROS21 sensor for a reading
  String datastring = sample_hydros21();
  delay(100);
  datastring = datastring + ',' + String(sample_analite_195());


  //Write header if first time writing to the logfile
  if (!SD.exists(filestr.c_str()))
  {
    dataFile = SD.open(filestr.c_str(), FILE_WRITE);
    if (dataFile)
    {
      dataFile.println("datetime,h2o_depth_mm,h2o_temp_deg_c,ec_dS_m,turb_ntu");
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
      dataFile.println("datetime,h2o_depth_mm,h2o_temp_deg_c,ec_dS_m,turb_ntu");
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

  //Flash LED to idicate a sample was just taken
  digitalWrite(LED, HIGH);
  delay(250);
  digitalWrite(LED, LOW);
  delay(250);

  //Put logger in low power mode for lenght 'sleep_time'
  LowPower.sleep(sleep_time);


}
