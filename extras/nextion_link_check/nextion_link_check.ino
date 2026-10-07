// Nextion link check - finds out if wiring + baud rate work.
// Does NOT depend on your HMI: it blinks the screen brightness.
//
// Wiring: Nextion TX (blue)   -> ESP32 GPIO4  (RX2)
//         Nextion RX (yellow) <- ESP32 GPIO5  (TX2)
//         Nextion GND         -  ESP32 GND   (MUST be connected)
//
// Open Serial Monitor at 115200 and watch.

#include <Arduino.h>

const int NEX_RX = 4;   // ESP32 pin that RECEIVES  (goes to Nextion TX)
const int NEX_TX = 5;   // ESP32 pin that SENDS     (goes to Nextion RX)

void nexCmd(const char *c) {
  Serial2.print(c);
  Serial2.write(0xFF); Serial2.write(0xFF); Serial2.write(0xFF);
}

void printReply() {
  delay(150);
  if (!Serial2.available()) { Serial.println("   reply: (nothing)"); return; }
  Serial.print("   reply:");
  while (Serial2.available()) Serial.printf(" %02X", Serial2.read());
  Serial.println();
}

bool tryBaud(uint32_t baud) {
  Serial.printf("\n=== Trying %lu baud ===\n", (unsigned long)baud);
  Serial2.end();
  Serial2.begin(baud, SERIAL_8N1, NEX_RX, NEX_TX);
  delay(100);
  while (Serial2.available()) Serial2.read();

  Serial2.write(0xFF); Serial2.write(0xFF); Serial2.write(0xFF);
  nexCmd("bkcmd=3");                       // ask Nextion to answer every command
  printReply();

  Serial.println(" blinking brightness - watch the screen");
  for (int i = 0; i < 3; i++) {
    nexCmd("dim=10");  delay(300);
    nexCmd("dim=100"); delay(300);
  }
  printReply();

  Serial.println(" writing TEST into tRpm");
  nexCmd("tRpm.txt=\"TEST\"");
  printReply();   // 01 = OK, 1A = no component called tRpm, 1C/1B = bad command

  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\nNextion link check");
  Serial.println("Power-up bytes from Nextion (if any):");
  Serial2.begin(9600, SERIAL_8N1, NEX_RX, NEX_TX);
  delay(800);
  printReply();
}

void loop() {
  tryBaud(9600);
  tryBaud(115200);
  Serial.println("\n--- repeating in 3 s ---");
  delay(3000);
}
