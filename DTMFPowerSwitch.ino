/*
 * DTMF power switch
 * B. Ratoff - November 2024
 * 
 * Accepts a DTMF command string to control one or more power relays
 * 
 */

#include <avr/pgmspace.h>
#include <avr/sleep.h>
#include <tinyDTMF.h>

/* MUST REMEMBER TO SET THESE FOR BOARD VERSION */
//#define DIGIWIRE_REV 2
#include <Digiwire_Plus_Pins.h>

#include "cwtable.h"


// Pin assignments
#define tonePin P0           // Define Audio out Pin
#define PTT P4               // Define PTT Pin 
#define dtmfPin P5           // Audio input pin for DTMF

// Relay info
const uint8_t relayPin[] = {P2,P3};           // Pins controlling relays
const uint8_t onState[] = {LOW,LOW};            // On state of each pin
// How many relays are there?
#define numRelays sizeof(relayPin)/sizeof(uint8_t)

#define toneFreq 800        // Define Morse Tone Frequency in Hz
#define cspeed 2
#define mspeed 1

// Maximum time to complete a command
#define CMD_PERIOD 60000UL
// How often (ms) to check for DTMF input
#define DTMF_PERIOD 100UL
// Time until relay turns off after activated by command
#define RELAY_PERIOD 300000UL
// Time between ID checks - should always be longest period
#define ID_PERIOD 30000UL

#define ADC_INTERNAL1V1 13

const char toMsg[] PROGMEM = "CTO";
const char onMsg[] PROGMEM = "DOWN";
const char offMsg[] PROGMEM = "UP";
const char perMsg[] PROGMEM = "OFF";
const char idMsg[] PROGMEM = "N4BRF/RC";
const char vccMsg[] PROGMEM = "VCC ";

// Relay housekeeping
unsigned long nextRelay[numRelays]; // Milliseconds till each relay turns off
uint8_t relayState[numRelays];      // Current state of each relay
bool stayOn[numRelays];             // True if relay should not time out

// DTMF housekeeping
tinyDTMF DTMF(dtmfPin);       // Declare tone decoder
char dtmfKey = 0;             // Last key received and not yet processed
bool waitForSpace = false;    // True if waiting for space between DTMF tones
unsigned long nextDTMF = 0;   // Milliseconds til next check for a key

// ID housekeeping
unsigned long nextID;             // Milliseconds to next ID
bool idNeeded;                    // True if PTT has been keyed during ID interval

// Command processor housekeeping
unsigned long nextCmdTimeout = 0; // Milliseconds till current commend buffer times out
bool cmdActive = false;           // True if a command line is being processed

char cmdBuffer[10];               // Holds part of command currently being input
char* cmdPtr;                     // Points to current character in buffer during command processing
uint8_t cmdRelay;                 // ID number of selected relay
uint8_t cmdFunction;              // Selected function

// States for input command processor
// Command syntax:  *password*relay*function#
enum CMD_STATE {
  CMD_IDLE,                       // Nothing happening
  CMD_GET_PW,                     // Collecting password digits
  CMD_GET_RELAY,                  // Collecting relay number
  CMD_GET_FUNCTION,               // Collecting function
  CMD_GET_END                     // Got all parts, ready to process command
} cmdState = CMD_IDLE;

// Initialization
void setup() {
  pinMode(tonePin,OUTPUT);                  // Enable tone output pin
  pinMode(PTT,OUTPUT);                      // Enable Push-to-talk pin
  pinMode(LED_BUILTIN,OUTPUT);              // Enable onboard LED
  digitalWrite(tonePin,LOW);                // Init tone and PTT pins to inactive
  digitalWrite(PTT,LOW);
  digitalWrite(LED_BUILTIN,LOW);            // Turn off LED for now
  for(uint16_t i=0; i<numRelays; i++) {          // For each relay
    pinMode(relayPin[i],OUTPUT);            // Enable output pin
    digitalWrite(relayPin[i],!onState[i]);  // Set relay to off state
    relayState[i] = !onState[i];            // Remember off state
    nextRelay[i] = 0L;                      // Init relay's timer count
    stayOn[i] = false;                      // Init "stay on" (timer disable) flag
  }
  DTMF.begin();             // Initialize DTMF decoder
  cmdBuffer[0] = 0;         // Initialize command buffer
  cmdPtr = &cmdBuffer[0];   //   and its pointer
  cmdState = CMD_IDLE;
  cmdRelay = 0;
  cmdFunction = 0;
  
  nextID = millis();        // make sure we ID at startup
  idNeeded = true;
}

// Returns true when a DTMF key has been pressed and released
bool checkDTMF(void) {
    char key = 0;
    bool retVal = false;
    
    DTMF.getSample();           // Fill sample buffer
    key = DTMF.procSample();    // Try to detect a key
    if(waitForSpace) {          // If key was previously being heard
      if(key == 0) {            // and is no longer being heard
        waitForSpace = false;   // then we're no longer waiting for release
        if(dtmfKey != 0) {
          retVal = true;        // So return true if there was a key pressed and released
        }
      }
    } else {                    // If last check did not detect a key
      if(key != 0) {            // and this time we hear one
        dtmfKey = key;          // save the key
        waitForSpace = true;    // and say we're waiting for it to be released
      }
    }
    return(retVal);             // return true if key just released, otherwise false
}

// Key the radio
void pttDown(void) {
  digitalWrite(PTT,HIGH);
  delay(400);                //Wait for Radio to TX
  idNeeded = true;           //Remember that we transmitted
  nextID = millis() + ID_PERIOD;  // set next time to ID
}

// Unkey the radio
void pttUp(void) {
    digitalWrite(PTT,LOW);
}

//Send a cw string from PROGMEM
void SendMorse(const char message[], const char prefix=0) {
  uint8_t i;
  char j;
  
  pttDown();        // Key the transmitter
  if(prefix != 0) {
    sendChar(prefix);
    sendChar(' ');
  }
  for(i=0; (j=pgm_read_byte(&message[i])); i++) {   // for each character in the string
    if(j > 'Z')     // Eliminate unhandled characters
      j -= ' ';     // Convert from ASCII to character offset
    sendChar(j);    // and send it
  }
  pttUp();          // Unkey the transmitter
}

//DAH Function - send a DAH
//void dah(int loops){
void dah(void) {
  tone(tonePin,toneFreq);
  digitalWrite(LED_BUILTIN,HIGH);
  delay(385/cspeed);
  noTone(tonePin);
  digitalWrite(LED_BUILTIN,LOW);
  delay(90/cspeed);
}

//DIT Function - send a DIT
//void dit(int loops){
void dit(void){
  tone(tonePin,toneFreq);
  digitalWrite(LED_BUILTIN,HIGH);
  delay(125/cspeed);
  noTone(tonePin);
  digitalWrite(LED_BUILTIN,LOW);
  delay(100/cspeed);
}

//WordBreak Function - delay between words
void wordBreak(void){
    noTone(tonePin);
    delay(850/cspeed);
}

//CharBreak Function - delay between cw characters
void CharBreak(void){
    noTone(tonePin);
    delay(165/cspeed);
}

// Send one byte of up to 4 elements of a character
// The elements were obtained from cwtable[]
// Each element is 2 bits, which can be a DIT, a DAH, a word break, or nothing (i.e. end fill)
void sendElements(unsigned char ele) {
  unsigned char bits = ele;
  char i,j;

  for(i=0; i<4; i++) {      // for each possible element
    j = (bits & 0xC0) >> 6; // isolate the 2 bits of that element
    bits <<= 2;             // shift off the element we're processing
    switch(j) {
      case DIT:
        dit();              // it's a DIT - send it
        break;
      case DAH:
        dah();              // it's a DAH - send it
        break;
      case WB:
        wordBreak();        // it's a word break - do it
        break;
      default:
        break;              // should never get here, but oh well
    }
  if(!bits)                 // if only end fill is left, quit early
    break;
  }
}

// Send one cw character
void sendChar(const char c) {
  unsigned char k,m;

  // Find offset of character in character table
  for(k=0; (m=pgm_read_byte(&chtable[k])); k++) {
    if(c == m)
      break;
  }
  // Dash is a special case - send character break
  if(m == '-') {
    CharBreak();
  } else if(m) {        // Otherwise, if we actually found the character in our table,
    k <<= 1;            // Double the offset because the element table is 2 bytes per character
    sendElements(pgm_read_byte(&cwtable[k]));     // Send the first 4 elements
    sendElements(pgm_read_byte(&cwtable[k+1]));   // Send the second 4 elements
  }
  // If it wasn't a space or dash, wait character break time
  if((c != ' ') && (c != '-'))
    CharBreak();
}

void relayOff(int r=0) {
  digitalWrite(relayPin[r],!onState[r]);
  SendMorse(offMsg,(r+'1'));           // Acknowledge command
  relayState[r] = !onState[r];         // Remember relay state
}

void relayOn(int r=0, bool p=false) {
  digitalWrite(relayPin[r],onState[r]);
  if(p) {
    SendMorse(perMsg, (r+'1'));
  } else {
    SendMorse(onMsg,(r+'1'));            // Acknowledge command
  }
  relayState[r] = onState[r];          // Remember relay state
}

uint16_t readVcc() {
  analogReference(DEFAULT);
  delay(100);
  uint16_t y = analogRead(ADC_INTERNAL1V1); // Dummy read for stability
  return(11830/analogRead(ADC_INTERNAL1V1));  // Return reading in 10ths of volts
}

void sendVcc() {
  uint16_t vcc = readVcc();
  SendMorse(vccMsg);
  pttDown();
  sendChar(vcc/10+'0');
  sendChar(vcc%10+'0');
  pttUp();
}

// Process the latest DTMF key received
void processKey(void) {
  if(dtmfKey != 0) {
    delay(500);      // Allow time for user to listen for response
    switch(dtmfKey) {
      // Relay off
      case '1':
        relayOff(0);
        break;
      // Relay on for preconfigured time
      case '2':
        relayOn(0,false);
        stayOn[0] = false;                 // Relay should time out and turn off
        nextRelay[0] = millis()+RELAY_PERIOD;  // Set time to turn relay back off
        break;
      // Relay on until turned off by command
      case '3':
        relayOn(0,true);
        stayOn[0] = true;                  // Relay should stay on
        break;
      case '4':
        relayOff(1);
        break;
      // Relay on for preconfigured time
      case '5':
        relayOn(1,false);
        stayOn[1] = false;                 // Relay should time out and turn off
        nextRelay[1] = millis()+RELAY_PERIOD;  // Set time to turn relay back off
        break;
      // Relay on until turned off by command
      case '6':
        relayOn(1,true);
        stayOn[1] = true;                  // Relay should stay on
        break;
      case 'B':
        sendVcc();
        break;
      default:
        pttDown();                      // Otherwise send key followed by '?'
        sendChar(dtmfKey);
        sendChar('?');
        pttUp();
    }
  }
  dtmfKey = 0;                          // Zap the key once it's processed
}

// Our main loop is checking several events with different intervals
// so for each one we have a target millis() value that must be reached
void loop() {
  unsigned long timeNow = millis();           // get the current millisecond counter
  if((long)(nextDTMF-timeNow) < 0) {          // is it time to look for a DTMF key?
    if(checkDTMF()) {                         // yes, see if there is one
      processKey();                           // got one, go process it
//      cmdActive = true;                       // TEMPORARY code to check timeout logic
//      nextCmdTimeout = timeNow+CMD_PERIOD;    //  DITTO
    }
    nextDTMF = timeNow+DTMF_PERIOD;           // set next target time to check for DTMF
  }
  if(cmdActive) {                             // If we're in the middle of entering a command
    if((long)(nextCmdTimeout-timeNow) < 0) {  // and the command timeout period has elapsed
      SendMorse(toMsg);                       // send timeout message and end command processing
      cmdActive = false;
    }
  }
  if((long)(nextID-timeNow) < 0) {            // has ID interval elapsed?
    if(idNeeded) {                            //   was there a PTT during this interval?
      SendMorse(idMsg);                       // send ID
      idNeeded = false;                       // clear PTT flag
    }
    nextID = timeNow + ID_PERIOD;             // set next time to check
  }
  for(uint16_t i=0; i<numRelays; i++) {
    if((relayState[i] == onState[i]) && !stayOn[i]) {   // If the relay is currently on in timed mode
      if((long)(nextRelay[i]-timeNow) < 0) {       // and the relay active period has expired
        relayOff(i);
      }
    }
  }
  set_sleep_mode(SLEEP_MODE_IDLE);            // Power down till the next timer interrupt
  sleep_mode();
}
