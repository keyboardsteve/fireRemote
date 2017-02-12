#include "Arduino.h"
//The setup function is called once at startup of the sketch

//#include <SoftwareSerial.h>

#define SAFETY 0
#define TEST   1
#define ARMED  2
#define NUMCHIPS 10
#define SWITCH_MODE_DELAY 500
#define TTL 10 //The number of interrupt cycles that the channel will be held active.  Interrupt @ 10Hz (10 = 1 seconds)

char operatingMode = '0';
char testStatus = '0';
char buff;

String command;

byte channels[NUMCHIPS]; //Each byte element represents an 8-bit shift register.
int channelTimeouts[NUMCHIPS*8];  //each integer in the array represents a Time-To-Live value


/* The two interrupt pins we have on this thing are 2 & 3 */
int latchPin = 8;
int clockPin = 12;
int dataPin = 11;
int highSupplyControlPin = 9;  //These should be on the "normal" side of the relay, so that it fails to test voltages & currents
int lowSupplyControlPin = 7;  //These should be on the "normal" side of the relay, so that it fails to test voltages & currents
int testControlPin = 10;   //These should be on the "normal" side of the relay (LOW inserts the test circuit)
int testSensePin = 3;  //Use Pin 2 for the Test input pin
volatile int hbCounter = 0;

volatile bool testDone = false;

void setup(){

	pinMode(LED_BUILTIN, OUTPUT);
	pinMode(latchPin, OUTPUT);
	pinMode(clockPin, OUTPUT);
	pinMode(dataPin, OUTPUT);
	pinMode(highSupplyControlPin, OUTPUT);
	pinMode(lowSupplyControlPin, OUTPUT);
	pinMode(testControlPin, OUTPUT);
	pinMode(testSensePin, INPUT);
	attachInterrupt(digitalPinToInterrupt(testSensePin), testCircuit, RISING);

	setModeSafety();

	digitalWrite(latchPin, HIGH);  // DO NOT expose the data in the pins. This disables the output.

	clearChannels();

	Serial.begin(19200);
	while (!Serial){
		//Serial.print('.');	//Initializing the PC coms
	}

	cli();  			//Stop all interrupts
						//set timer1 interrupt at 1Hz
	TCCR1A = 0;			// set entire TCCR1A register to 0
	TCCR1B = 0;			// same for TCCR1B
	TCNT1  = 0;			//initialize counter value to 0
						// set compare match register for 1hz increments
	OCR1A = 1562;		// = (16*10^6) / (1*1024) - 1 (must be <65536)  We are going for 10 Hz
	TCCR1B |= (1 << WGM12);					// turn on CTC mode
	TCCR1B |= (1 << CS12) | (1 << CS10);	// Set CS10 and CS12 bits for 1024 prescaler
	TIMSK1 |= (1 << OCIE1A);				// enable timer compare interrupt

	sei(); 				//Start interrupts
}

void loop()
{

	command = "";
	String tmp = "";
	String response = "";
	testDone = false;

	if (Serial.available()) {
		while (true) {	//Blocks to grab all available characters until the newline.
			while (Serial.available() < 1) { ; }
			char c = Serial.read();
			if (c == '\n') { break; }
			command += c;
		}
		buff  = char(command[0]);

		switch (buff) {
			case 'H':  //HEARTBEAT
				/*
				Serial.print("R");
				Serial.println(operatingMode);
				*/
				break;
			case 'F':  //FIRE
				//Need to pull the channel number from the command, eg:  F1, F123, etc...
				for (unsigned int i=0; i < command.length(); i++) {
					if (isDigit(command[i])){
						tmp += command[i];
					}
				}
				if (operatingMode == '1') {  //TEST MODE
					response = setChannelTest(tmp.toInt()-1);

					unsigned int j = 0;
					while ((j < 2) && (!testDone)) {
						delay(500);
						j++;
					}
					if (testDone) {
						 response += "1";
					}
					else {
						response += "0";
					}
					Serial.println(response);
				}
				else if (operatingMode == '2') {  //FIRE MODE
					setChannelFire(tmp.toInt()-1);
				}
				else if (operatingMode == '0') { //SAFETY Mode

				}
				break;
			case 'M':  //CHANGE MODE
				clearChannels();
				switch (command[1]){
					case '0':
						setModeSafety();
						break;
					case '1':
						setModeTest();
						break;
					case '2':
						setModeArmed();
						break;
				}
				operatingMode = command[1];
				break;
			default:
				Serial.print("E");
				Serial.println(command);
		}
	}
}

ISR(TIMER1_COMPA_vect) {
	digitalWrite(latchPin, HIGH);  //deactivate output enable here...
	for(unsigned int i=0;i<NUMCHIPS;i++) {                  //For each chip...
		shiftOut(dataPin, clockPin, LSBFIRST, channels[i]); //shift out the bits!
	}
	digitalWrite(latchPin, LOW);//Need to reactivate output enable here...

	for (unsigned int i=0; i < NUMCHIPS*8; i++){  //decrement the TTL values for each channel
		if (channelTimeouts[i] != 0){
			if (channelTimeouts[i] == 1){
				setChannelClear(i);
			}
			channelTimeouts[i] -= 1;
		}
	}
	hbCounter += 1;
	if (hbCounter >= 8) {
		hbCounter = 0;
		Serial.print("R");
		Serial.println(operatingMode);
	}
}

void testCircuit() {
	testDone = true;
	//Serial.println("DTestInterruptTripped");
}


void setChannelFire(unsigned int chn) {
	int chip;
	byte bit = B00000001;
	chip = (chn/8);
	channels[chip] |= (bit << (chn%8));
	channelTimeouts[chn] = TTL;
	Serial.print("T");  				  //substring denotes this is a fire/test response
	Serial.print(chn+1); //substring is the tube number (0 indexed)
	Serial.print("S");				  //substring denotes test result
	Serial.println("1");				  //substring denotes test pass
}

void setChannelClear(unsigned int chn) {
	int chip;
	byte bit = B00000001;
	chip = (chn/8);
	channels[chip] ^= (bit << (chn%8));
	//channelTimeouts[chn] = TTL;
}

String setChannelTest(unsigned int chn) {
	int chip;
	int channel = chn+1;
	String tmp = "";
	byte bit = B00000001;
	clearChannels();
	chip = (chn/8);
	channels[chip] |= (bit << (chn%8));
	channelTimeouts[chn] = TTL;
	tmp += "T";
	tmp += String(channel);
	tmp += "S";
	//Serial.print("T");  				  //substring denotes this is a fire/test response
	//Serial.print(chn+1); //substring is the tube number (0 indexed)
	//Serial.print("S");				  //substring denotes test result
	//  Below is where the information about PASS/FAIL need to be measured.
	//Serial.println("1");				  //substring denotes test pass
	return tmp;
}

void clearChannels() {  //This immediately clear all channels and timeouts

	for (unsigned int i=0; i < NUMCHIPS; i++){ //pre-init the channels array to all zeros for safety
		channels[i] = B00000000;	         //each bit represents a channel fire state: ON||OFF
	}
	for (unsigned int i=0; i < NUMCHIPS*8; i++){  //pre-init the channelTimouts to all zeros
		channelTimeouts[i] = 0;
	}
}

void setModeSafety() {
	digitalWrite(highSupplyControlPin, LOW);
	digitalWrite(lowSupplyControlPin, LOW);
	digitalWrite(testControlPin, LOW);
	Serial.println("O0");
}

void setModeTest() {
	digitalWrite(highSupplyControlPin, LOW);
	delay(SWITCH_MODE_DELAY);
	digitalWrite(lowSupplyControlPin, HIGH);
	digitalWrite(testControlPin, HIGH);
	Serial.println("O1");
}

void setModeArmed() {
	digitalWrite(lowSupplyControlPin, LOW);
	digitalWrite(testControlPin, LOW);
	delay(SWITCH_MODE_DELAY);
	digitalWrite(highSupplyControlPin, HIGH);
	Serial.println("O2");
}
