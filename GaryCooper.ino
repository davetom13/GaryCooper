////////////////////////////////////////////////////
// Gary Cooper chicken coop controller
////////////////////////////////////////////////////
#include <Arduino.h>
#include <EEPROM.h>

#include <GPSParser.h>
#include <SaveController.h>

#include "ICommInterface.h"
#include "Telemetry.h"
#include "TelemetryTags.h"
#include "SlidingBuf.h"
#include "Comm_Arduino.h"
#include "Command.h"
#include "MilliTimer.h"

#include "Pins.h"
#include "SunCalc.h"
#include "DoorController.h"
#include "LightController.h"
#include "BeepController.h"
#include "GaryCooper.h"

// GPS parser
CGPSParser g_GPSParser;
static bool s_gpsDataStreamActive = false;

// Door controller
CDoorController g_doorController;

// Light controller
CLightController g_lightController;

// Beep controller
CBeepController g_beepController(PIN_BEEPER);

// Settings controller
CSaveController g_saveController('C', 'o', 'o', 'p');
bool settingsLoaded = false;

// Sunrise / Sunset calculator
CSunCalc g_sunCalc;

// Telemetry module
CTelemetry g_telemetry;
static CCommand s_commandProcessor;
static CComm_Arduino g_telemetryComm;

// Various behavioral delays
#define TIME_CHECK_UPDATE_NO_GPS_LOCK	(5 * MILLIS_PER_SECOND)
#define TIME_CHECK_UPDATE_GPS_LOCK		(60 * MILLIS_PER_SECOND)
CMilliTimer g_timeCheckTimer;

// Frequency of telemetry transmission
#define TELEMETRY_UPDATE	(2 * MILLIS_PER_SECOND)
CMilliTimer g_telemetryUpdateTimer;

// Flashing the LED
bool g_heartbeat = false;

void saveSettings(bool _defaults)
{
#ifdef DEBUG_SETTINGS
	DEBUG_SERIAL.print(F("Save settings... "));
#endif // DEBUG_SETTINGS
	// Make sure the header is correct
	g_saveController.updateHeader(GARYCOOPER_DATA_VERSION);

	// Rewind so we write the settings in the correct place
	g_saveController.rewind();

	// Save everything
	g_doorController.saveSettings(g_saveController, _defaults);
	g_lightController.saveSettings(g_saveController, _defaults);

#ifdef DEBUG_SETTINGS
	DEBUG_SERIAL.println(F("complete."));
#endif
}

void loadSettings()
{
#ifdef DEBUG_SETTINGS
	DEBUG_SERIAL.print(F("Load settings checking header version: "));
#endif // DEBUG_SETTINGS

	// If the data version is incorrect then we need to update the EEPROM
	// to default settings
	int headerVersion = g_saveController.getDataVersion();

#ifdef DEBUG_SETTINGS
	DEBUG_SERIAL.print(headerVersion);
	DEBUG_SERIAL.print(F(" - "));
#endif
	if(headerVersion != GARYCOOPER_DATA_VERSION)
	{
#ifdef DEBUG_SETTINGS
		DEBUG_SERIAL.println(F("INCORRECT."));

		// Save defaults from object constructors
		DEBUG_SERIAL.println(F("Saving default settings."));
#endif
		saveSettings(true);
	}
	else
	{
#ifdef DEBUG_SETTINGS
		DEBUG_SERIAL.println(F("CORRECT."));
#endif
	}

#ifdef DEBUG_SETTINGS
	DEBUG_SERIAL.println(F("Loading settings... "));
#endif

	// Make sure we start at the beginning
	g_saveController.rewind();

	g_doorController.loadSettings(g_saveController);
	g_lightController.loadSettings(g_saveController);

#ifdef DEBUG_SETTINGS
	DEBUG_SERIAL.println(F("Load settings complete."));
#endif
}

void setup()
{
	// Setup heartbeat indicator
	pinMode(PIN_HEARTBEAT_LED, OUTPUT);

	// Prep debug port
	DEBUG_SERIAL.begin(DEBUG_BAUD_RATE);

	// Prep the GPS port
	GPS_SERIAL.begin(GPS_BAUD_RATE);

	// Prep the telemetry port
	g_telemetryComm.open(TELEMETRY_PORT, TELEMETRY_BAUD_RATE);
	g_telemetry.setInterfaces(&g_telemetryComm, &s_commandProcessor);

	// Setup the door controller
	g_doorController.setup();

	// And the light controller
	g_lightController.setup();

	// Beep to indicate starting the main loop
	g_beepController.setup();
	g_beepController.beep(BEEP_FREQ_INFO, 50, 50, 2);

	// Telemetry updates every two seconds
	g_telemetryUpdateTimer.start(TELEMETRY_UPDATE);

	// Prep the update timer. This delay
	// allows the GPS to get some data before we start
	// processing more slowly
	g_timeCheckTimer.start(TIME_CHECK_UPDATE_NO_GPS_LOCK);
}

void loop()
{
	// Load settings?
	if(!settingsLoaded)
	{
		loadSettings();
		settingsLoaded = true;
	}

	// Process all available GPS data
	while(GPS_SERIAL.available())
	{
		unsigned char GPSData[256];
		unsigned int GPSDataLen = 0;

		// Get the data from the serial port
		GPSDataLen = GPS_SERIAL.available();
		if(GPSDataLen > sizeof(GPSData) - 1)
			GPSDataLen = sizeof(GPSData) - 1;

		GPSDataLen = GPS_SERIAL.readBytes((unsigned char *)GPSData, GPSDataLen);
		if(GPSDataLen)
		{
#ifdef DEBUG_RAW_GPS
			GPSData[GPSDataLen] = '\0';
			String rawGPS((const char *)GPSData);
			DEBUG_SERIAL.print(rawGPS);
#endif
			// Parse the GPS data
			g_GPSParser.parse(GPSData, GPSDataLen);

			// Note that we have received some data
			// from the GPS serial port.
			s_gpsDataStreamActive = true;
		}
	}

	// Let the telemetry module process serial data
	g_telemetryComm.tick();
	g_telemetry.tick();

	// Let the beep controller run
	g_beepController.tick();

	// Let the door controller time its relay
	g_doorController.tick();

	// Send telemetry
	if(g_telemetryUpdateTimer.getState() == CMilliTimer::expired)
	{
		// Blink the LED
		g_heartbeat = !g_heartbeat;
		digitalWrite(PIN_HEARTBEAT_LED, g_heartbeat);

		// Reset the timer
		g_telemetryUpdateTimer.start(TELEMETRY_UPDATE);

		// Send telemetry version number
		g_telemetry.transmissionStart();
		g_telemetry.sendTerm(telemetry_tag_version);
		g_telemetry.sendTerm(TELEMETRY_VERSION_01);
		g_telemetry.transmissionEnd();

		// Send standing errors
		sendErrors();

		// And the rest of the telemetry
		g_sunCalc.sendTelemetry();
		g_doorController.sendTelemetry();
		g_lightController.sendTelemetry();
	}

	// If the update timer has completed then
	// read the GPS data and check the time to
	// see if anything needs to be done
	if(g_timeCheckTimer.getState() == CMilliTimer::expired)
	{
		// Prep for next update
		if(g_GPSParser.getGPSData().m_GPSLocked)
			g_timeCheckTimer.start(TIME_CHECK_UPDATE_GPS_LOCK);
		else
			g_timeCheckTimer.start(TIME_CHECK_UPDATE_NO_GPS_LOCK);


		// If the GPS is not sending any data then report an error
		if(!s_gpsDataStreamActive)
		{
			g_GPSParser.getGPSData().clear();
			reportError(telemetry_error_GPS_no_data, true);
		}
		else
		{
			reportError(telemetry_error_GPS_no_data, false);
		}

		s_gpsDataStreamActive = false;

		// If we have a valid time fix, control the door and light
		if(g_sunCalc.processGPSData(g_GPSParser.getGPSData()))
		{
			g_doorController.checkTime();
			g_lightController.checkTime();
		}
	}
}

// Utility functions
void debugPrintDoubleTime(double _t, bool _newline)
{
	int hour = (int)_t;
	int minute = 60. * (_t - hour);
	DEBUG_SERIAL.print(hour);
	DEBUG_SERIAL.print(F(":"));
	DEBUG_SERIAL.print(minute);
	if(_newline) DEBUG_SERIAL.println();
}

static uint16_t s_errorFlags = 0;
void reportError(telemetryErrorE _errorTag, bool _set)
{
	int beepCount = 0;

	// Log the error for transmission
	if(_set)
	{
		// Don't keep re-setting them
		if(s_errorFlags & _errorTag)
			return;

		// Or it in
		s_errorFlags |= _errorTag;
	}
	else
	{
		// Don't keep re-clearing them
		if(!(s_errorFlags & _errorTag))
			return;

		// Mask it out
		s_errorFlags &= ~_errorTag;
	}

	// Report error to console
	String errorString(_errorTag);
	switch(_errorTag)
	{
	case telemetry_error_GPS_no_data:
		errorString = F("telemetry_error_GPS_no_data");
		beepCount = 1;
		break;

	case telemetry_error_GPS_bad_data:
		errorString = F("telemetry_error_GPS_bad_data");
		beepCount = 2;
		break;

	case telemetry_error_GPS_not_locked:
		errorString = F("telemetry_error_GPS_not_locked");
		beepCount = 3;
		break;

	case telemetry_error_suncalc_invalid_time:
		errorString = F("telemetry_error_suncalc_invalid_time");
		beepCount = 4;
		break;

	case telemetry_error_no_door_motor:
		errorString = F("telemetry_error_no_door_motor");
		beepCount = 5;
		break;

	case telemetry_error_door_motor_unknown_state:
		errorString = F("telemetry_error_door_motor_unknown_state");
		beepCount = 6;
		break;

	case telemetry_error_door_motor_unknown_not_responding:
		errorString = F("telemetry_error_door_motor_unknown_not_responding");
		beepCount = 7;
		break;

	default:
		// This is handled in the initialization of the error string
		break;
	}

	DEBUG_SERIAL.println();
	if(_set)
	{
		DEBUG_SERIAL.print(F("*** SET ERROR: "));
#ifdef BEEP_ON_ERROR
		g_beepController.beep(BEEP_FREQ_ERROR, 100, 50, beepCount);
#else
		beepCount = beepCount;	// No warning if unused variable
#endif
	}
	else
	{
		DEBUG_SERIAL.print(F("*** CLEAR ERROR: "));
	}
	DEBUG_SERIAL.println(errorString);
	DEBUG_SERIAL.println();
}

void sendErrors()
{
	g_telemetry.transmissionStart();
	g_telemetry.sendTerm(telemetry_tag_error_flags);
	g_telemetry.sendTerm(s_errorFlags);
	g_telemetry.transmissionEnd();
}

