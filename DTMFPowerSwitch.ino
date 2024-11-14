/*
 * DTMF power switch
 * B. Ratoff - November 2024
 * 
 * Accepts a DTMF command string to control one or more power relays
 * 
 */

#include <avr/pgmspace.h>
#include <tinyDTMF.h>

#include "cwtable.h"


// Pin assignments
#define tonePin 0           // Define Audio out Pin
#define PTT 4               // Define PTT Pin 
#define dtmfPin A0          // Audio input pin for DTMF (A0 is PB5)

#define toneFreq 800        // Define Morse Tone Frequency in Hz
#define cspeed 2
#define mspeed 1

// Maximum time to complete a command
#define CMD_PERIOD 30000UL
// How often (ms) to check for DTMF input
#define DTMF_PERIOD 100UL
// Time until relay turns off after activated by command
#define RELAY_PERIOD 15000UL
// Time between IDs
#define ID_PERIOD 600000UL

const char toMsg[] PROGMEM = "CTO";
const char onMsg[] PROGMEM = "QRT";
const char offMsg[] PROGMEM = "QRO";
const char idMsg[] PROGMEM = "N4BRF/RC";

unsigned long nextID;             // Milliseconds to next ID
bool idNeeded;                    // True if PTT has been keyed during ID interval

unsigned long nextDTMF = 0;       // Milliseconds to next check for a key
unsigned long nextCmdTimeout = 0; // Milliseconds till current commend buffer times out
bool cmdActive = false;           // True if a command line is being processed

// Relay info
uint8_t relayPin[] = {2,1};           // Pins controlling relays
uint8_t onState[] = {LOW,HIGH};            // On state of each pin
uint8_t offState[] = {HIGH,LOW};           // Off state of each pin
unsigned long nextRelay[] = {0,0};  // Milliseconds till each relay turns off
uint8_t relayState[] = {1,0};     // Current state of each relay
bool stayOn[] = {false,false};    // True if relay should not time out
uint8_t numRelays = sizeof(relayPin)/sizeof(int); // How many relays are there?

tinyDTMF DTMF(dtmfPin);     // Declare tone decoder
char dtmfKey = 0;           // Last key received and not yet processed
bool waitForSpace = false;   // True if waiting for space between DTMF tones

char cmdBuffer[10];
char* cmdPtr;
uint8_t cmdRelay;
uint8_t cmdFunction;
enum CMD_STATE {
  CMD_IDLE,
  CMD_GET_PW,
  CMD_GET_RELAY,
  CMD_GET_FUNCTION,
  CMD_GET_END
} cmdState = CMD_IDLE;

void setup() {
  pinMode(tonePin,OUTPUT);
  pinMode(PTT,OUTPUT);
  digitalWrite(tonePin,LOW);
  digitalWrite(PTT,LOW);
  for(int i=0; i<numRelays; i++) {
    pinMode(relayPin[i],OUTPUT);
    digitalWrite(relayPin[i],offState[i]);
  }
  DTMF.begin();             // Initialize DTMF decoder
  cmdBuffer[0] = 0;         // Initialize command buffer
  cmdPtr = &cmdBuffer[0];   //   and its pointer
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
  delay(1000);               //Always wait 1 sec first so commands can unkey
  digitalWrite(PTT,HIGH);
  delay(500);                //Wait for Radio to TX
  idNeeded = true;           //Remember that we transmitted
}

// Unkey the radio
void pttUp(void) {
    delay(500);               //Wait for last element before unkeying
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
      delay(385/cspeed);
      noTone(tonePin);
      delay(90/cspeed);
}

//DIT Function - send a DIT
//void dit(int loops){
void dit(void){
      tone(tonePin,toneFreq);
      delay(125/cspeed);
      noTone(tonePin);
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
  digitalWrite(relayPin[r],offState[r]);
  SendMorse(offMsg,(r+'1'));           // Acknowledge command
  relayState[r] = offState[r];         // Remember relay state
}

void relayOn(int r=0) {
  digitalWrite(relayPin[r],onState[r]);
  SendMorse(onMsg,(r+'1'));            // Acknowledge command
  relayState[r] = onState[r];          // Remember relay state
}

// Process the latest DTMF key received
void processKey(void) {
  if(dtmfKey != 0) {
    switch(dtmfKey) {
      // Relay off
      case '0':
        relayOff(0);
        break;
      // Relay on for preconfigured time
      case '1':
        relayOn(0);
        stayOn[0] = false;                 // Relay should time out and turn off
        nextRelay[0] = millis()+RELAY_PERIOD;  // Set time to turn relay back off
        break;
      // Relay on until turned off by command
      case '2':
        relayOn(0);
        stayOn[0] = true;                  // Relay should stay on
        break;
      case '4':
        relayOff(1);
        break;
      // Relay on for preconfigured time
      case '5':
        relayOn(1);
        stayOn[1] = false;                 // Relay should time out and turn off
        nextRelay[1] = millis()+RELAY_PERIOD;  // Set time to turn relay back off
        break;
      // Relay on until turned off by command
      case '6':
        relayOn(1);
        stayOn[1] = true;                  // Relay should stay on
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
      cmdActive = true;                       // TEMPORARY code to check timeout logic
      nextCmdTimeout = timeNow+CMD_PERIOD;    //  DITTO
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
  for(int i=0; i<numRelays; i++) {
    if((relayState[i] == onState[i]) && !stayOn[i]) {   // If the relay is currently on in timed mode
      if((long)(nextRelay[i]-timeNow) < 0) {       // and the relay active period has expired
        relayOff(i);
      }
    }
  }
}
