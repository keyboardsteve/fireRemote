#include "Arduino.h"
//The setup function is called once at startup of the sketch

//#include <SoftwareSerial.h>

#define SAFETY 0
#define TEST   1
#define ARMED  2
#define NUMCHIPS 7
#define SWITCH_MODE_DELAY 500
#define TTL 10 //The number of interrupt cycles that the channel will be held active.  Interrupt @ 10Hz (10 = 1 seconds)
#define HB_PERIOD 5   //This is in 1/10th of a second.  5 => .5 seconds
#define TEST_PERIOD 5 //This is in 1/10th of a second.  5 => .5 seconds

char operatingMode = '0';
char testStatus = '0';
char buff;

String command = "";

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

volatile int testCounter = 0;
int testChannel = -1;
bool testRunning = false;
volatile bool testPassed = false;

volatile bool timeToFinishTesting = false;
volatile bool timeToSendHeartbeat = false;
volatile bool timeToShiftData = false;

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
	String response = "";

	if (Serial.available() > 0) {
		char c = Serial.read();
		command += c;
	}

	if (command.endsWith("\n")) {
		response = processCommand(command);
		if (response != "") {
			Serial.println(response);
		}
		command = "";
	}

	if (timeToSendHeartbeat) {
		timeToSendHeartbeat = false;
		Serial.print("R");
		Serial.println(operatingMode);
	}

	if (timeToShiftData) {
		timeToShiftData = false;
		shiftData();
	}
	if (testRunning) {
		if (timeToFinishTesting) {
			timeToFinishTesting = false;
			String response = stopTest();
			Serial.println(response);
		}
	}
}

ISR(TIMER1_COMPA_vect) {
	timeToShiftData = true;

	hbCounter += 1;
	if (hbCounter >= HB_PERIOD) {
		hbCounter = 0;
		timeToSendHeartbeat = true;
	}
	if (testRunning) {
		testCounter += 1;
		if (testCounter >= TEST_PERIOD) {
			testCounter = 0;
			timeToFinishTesting = true;
		}
	}
}

void shiftData() {
	digitalWrite(latchPin, HIGH);  //deactivate output enable here...
	for(unsigned int i=0;i<NUMCHIPS;i++) {                  //For each chip...
		shiftOut(dataPin, clockPin, MSBFIRST, channels[NUMCHIPS-1 - i]); //shift out the bits!
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
}

String processCommand(String cmd) {
	char buff = cmd[0];
	String ret = "";
	String tmp = "";

	switch (buff) {
		case 'H':  //HEARTBEAT

			break;
		case 'F':  //FIRE
			//Need to pull the channel number from the command, eg:  F1, F123, etc...
			for (unsigned int i=0; i < cmd.length(); i++) {
				if (isDigit(cmd[i])){
					tmp += cmd[i];
				}
			}
			if (operatingMode == '1') {  //TEST MODE
				clearChannels();
				startTest(tmp.toInt()-1);
			}
			else if (operatingMode == '2') {  //FIRE MODE
				ret = setChannelFire(tmp.toInt()-1);
			}
			else if (operatingMode == '0') { //SAFETY Mode

			}
			break;
		case 'M':  //CHANGE MODE
			clearChannels();
			switch (cmd[1]){
				case '0':
					ret = setModeSafety();
					break;
				case '1':
					ret = setModeTest();
					break;
				case '2':
					ret = setModeArmed();
					break;
			}
			operatingMode = cmd[1];
			break;
		default:
			ret = "E";
			ret += cmd;
	}
	return ret;
}

void startTest(unsigned int chn) {
	testRunning = true;
	testChannel = chn;
	testCounter = 0;
	setChannelTest(chn);
}

String stopTest() {
	String ret = "T";
	ret += String(testChannel + 1);
	ret += "S";
	if (testPassed) {
		ret += "1";
	}
	else {
		ret += "0";
	}
	testRunning = false;
	testPassed = false;
	testChannel = -1;
	return ret;
}

void testCircuit() {
	testPassed = true;
}

String setChannelFire(unsigned int chn) {
	String ret = "T";
	int chip = chn/8;
	int channel = chn+1;

	bitClear(channels[chip], chn%8);
	channelTimeouts[chn] = TTL;
	ret += String(channel);
	ret += "S";
	ret += "1";
	return ret;
}

void setChannelClear(unsigned int chn) {
	int chip = chn/8;
	bitSet(channels[chip], chn%8);
}

void setChannelTest(unsigned int chn) {
	int chip = chn/8;
	bitClear(channels[chip], chn%8);
	channelTimeouts[chn] = TTL;
}

void clearChannels() {  //This immediately clear all channels and timeouts
	for (unsigned int i=0; i < NUMCHIPS; i++){ //pre-init the channels array to all ones for safety (relays are neg-logic)
		channels[i] = B11111111;	         //each bit represents a channel fire state: ON||OFF
	}
	for (unsigned int i=0; i < NUMCHIPS*8; i++){  //pre-init the channelTimouts to all zeros
		channelTimeouts[i] = 0;
	}
}

String setModeSafety() {
	String ret = "O0";
	digitalWrite(highSupplyControlPin, LOW);
	digitalWrite(lowSupplyControlPin, LOW);
	digitalWrite(testControlPin, LOW);
	return ret;
}

String setModeTest() {
	String ret = "O1";
	digitalWrite(highSupplyControlPin, LOW);
	delay(SWITCH_MODE_DELAY);
	digitalWrite(lowSupplyControlPin, HIGH);
	digitalWrite(testControlPin, HIGH);
	return ret;
}

String setModeArmed() {
	String ret = "O2";
	digitalWrite(lowSupplyControlPin, LOW);
	digitalWrite(testControlPin, LOW);
	delay(SWITCH_MODE_DELAY);
	digitalWrite(highSupplyControlPin, HIGH);
	return ret;
}
