#include <Arduino.h>
#include "EmotiBit.h"

#define SerialUSB SERIAL_PORT_USBVIRTUAL // Required to work in Visual Micro / Visual Studio IDE
const uint32_t SERIAL_BAUD = 2000000; //115200

EmotiBit emotibit;
const size_t dataSize = EmotiBit::MAX_DATA_BUFFER_SIZE;
float data[dataSize];

void onShortButtonPress()
{
#ifdef SWEA_BLE
	// SWEA BLE build: a short press opens the pairing window (120 s) for a new phone and prints the passkey on Serial.
	emotibit._emotiBitWiFi.openPairing(120);
	return;
#endif
	// toggle wifi on/off
	if (emotibit.getPowerMode() == EmotiBit::PowerMode::NORMAL_POWER)
	{
		emotibit.setPowerMode(EmotiBit::PowerMode::WIRELESS_OFF);
		Serial.println("PowerMode::WIRELESS_OFF");
	}
	else
	{
		emotibit.setPowerMode(EmotiBit::PowerMode::NORMAL_POWER);
		Serial.println("PowerMode::NORMAL_POWER");
	}
}

void onLongButtonPress()
{
	emotibit.sleep();
}

void setup() 
{
	Serial.begin(SERIAL_BAUD);
	Serial.println("Serial started");
	delay(2000);	// short delay to allow user to connect to serial, if desired

	// Capture the calling ino into firmware_variant information
	String inoFilename = __FILE__;  // returns absolute path of ino file
	inoFilename.replace("/","\\");  // conform to standard directory separator
	// extract filename only if directory separator found in absolute path
	// SWEA: take the file name after the last separator and cut at its last dot. The stock code cuts at the
	// first dot of the whole path, which breaks when a folder name contains a dot (e.g. SWEA.EmotiBit_FeatherWing).
	if(inoFilename.lastIndexOf("\\") != -1)
	{
		inoFilename = inoFilename.substring(inoFilename.lastIndexOf("\\") + 1);
	}
	if(inoFilename.lastIndexOf(".") > 0)
	{
		inoFilename = inoFilename.substring(0, inoFilename.lastIndexOf("."));
	}
	emotibit.setup(inoFilename);

	// Attach callback functions
	emotibit.attachShortButtonPress(&onShortButtonPress);
	emotibit.attachLongButtonPress(&onLongButtonPress);
}

void loop()
{
	//Serial.println("emotibit.update()");
	emotibit.update();

	size_t dataAvailable = emotibit.readData(EmotiBit::DataType::PPG_GREEN, &data[0], dataSize);
	if (dataAvailable > 0)
	{
		// Hey cool, I got some data! Maybe I can light up my shoes whenever I get excited!

		// print the data to view in the serial plotter
		bool printData = false;
		if (printData)
		{
			for (size_t i = 0; i < dataAvailable && i < dataSize; i++)
			{
				// Note that dataAvailable can be larger than dataSize
				Serial.println(data[i]);
			}
		}
	}
}