/*
 * Sistema di Monitoraggio Multiparametrico basato su ESP32
 * 
 * Progetto: Dispositivo IoT per monitoraggio ambientale e biometrico
 * Piattaforma: ESP32 DevKit v1
 * 
 * Sensori integrati:
 * - DHT22: Temperatura e umidità ambientale
 * - DS3231: Real Time Clock per timestamp accurati
 * - KY-039: Sensore cardiaco a fotopletismografia
 * - RPI-1031: Sensore di orientamento a 4 posizioni
 * - Display OLED SSD1306 0.96" per interfaccia utente
 * 
 * Funzionalità:
 * - Acquisizione dati multiparametrica in tempo reale
 * - Interfaccia utente dinamica basata su orientamento
 * - Modalità sleep attivabile tramite gesture sequence
 * - Visualizzazione sincronizzata dei parametri vitali e ambientali
 * 
 * Protocolli: I2C (display, RTC), ADC (sensore cardiaco), GPIO (tilt sensor)
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <Fonts/FreeSans9pt7b.h>
#include <RTClib.h>

// ========== CONFIGURAZIONE HARDWARE ==========
#define DHT_PIN 4           // Pin digitale per DHT22
#define DHT_TYPE DHT22      // Specifica modello sensore
#define TILT_S1 18          // Pin sensore orientamento S1
#define TILT_S2 19          // Pin sensore orientamento S2
#define HEART_SENSOR_PIN 32 // Pin analogico sensore cardiaco

// Parametri display OLED I2C
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1      
#define SCREEN_ADDRESS 0x3C

// ========== INIZIALIZZAZIONE PERIFERICHE ==========
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
DHT dht(DHT_PIN, DHT_TYPE);
RTC_DS3231 rtc;

// ========== VARIABILI DI SISTEMA ==========
// Dati ambientali
float temperatura, umidita;
unsigned long lastSensorRead = 0;
const long SENSOR_INTERVAL = 1000;

// Gestione orientamento e display
int currentOrientation = -1;
int lastOrientation = -1;
int displayRotation = 0;
unsigned long lastDisplayUpdate = 0;
const long DISPLAY_REFRESH = 100;

// Buffer per ultimo BPM valido
String lastValidBPM = "0";

// Mappatura orientamenti alle modalità di visualizzazione
const char* displayModes[] = {"OROLOGIO", "QUALITA' ARIA", "BATTITI CARDIACI", "TEMP & UMIDITA'"};
const int rotationMapping[] = {0, 3, 1, 2};  // Rotazioni display per orientamenti

// ========== GESTIONE SLEEP MODE ==========
bool sleepModeActive = false;
int gestureSequence[10];                    // Buffer per rilevamento gesture
int sequenceIndex = 0;
const int wakeupPattern[] = {1, 0, 1, 0};   // Pattern: destra-sinistra-destra-sinistra
const int PATTERN_LENGTH = 4;
unsigned long lastGestureTime = 0;
const long GESTURE_TIMEOUT = 3000;         // Timeout reset gesture (ms)
unsigned long sleepStartTime = 0;

void setup() {
  Serial.begin(115200);
  
  // Configurazione pin sensore orientamento
  pinMode(TILT_S1, INPUT);
  pinMode(TILT_S2, INPUT);
  
  // Inizializzazione sensori
  dht.begin();
  
  // Configurazione display OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("Errore inizializzazione display OLED"));
    while(1);
  }
  
  // Inizializzazione RTC
  if (!rtc.begin()) {
    Serial.println(F("Errore inizializzazione RTC DS3231"));
    displayError("RTC NON TROVATO");
    delay(2000);
  }
  
  // Configurazione tempo iniziale (decommentare se necessario)
  // rtc.adjust(DateTime(2025, 1, 15, 12, 0, 0));

  initializeGestureBuffer();
  
  // Schermata di avvio
  showBootScreen();
  delay(3000);
  
  Serial.println(F("Sistema avviato - Rotare per cambiare modalità"));
}

void loop() {
  unsigned long currentTime = millis();
  
  // Lettura continua sensore orientamento per gesture detection
  readOrientationSensor(currentTime);
  
  // Modalità sleep: operazioni minime
  if (sleepModeActive) {
    if (currentTime - lastDisplayUpdate >= 1000) {
      lastDisplayUpdate = currentTime;
      displaySleepMode();
    }
    return;
  }
  
  // Modalità normale: acquisizione dati completa
  
  // Campionamento continuo sensore cardiaco
  String currentBPM = acquireHeartRate();
  if (currentBPM != "0") {
    lastValidBPM = currentBPM;
    Serial.print(F("BPM rilevato: "));
    Serial.println(lastValidBPM);
  }
  
  // Lettura sensori ambientali
  if (currentTime - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = currentTime;
    readEnvironmentalSensors();
  }
  
  // Aggiornamento display
  if (currentTime - lastDisplayUpdate >= DISPLAY_REFRESH) {
    lastDisplayUpdate = currentTime;
    updateCurrentDisplay();
  }
}

// ========== GESTIONE SENSORE ORIENTAMENTO ==========
void readOrientationSensor(unsigned long currentTime) {
  int s1 = digitalRead(TILT_S1);
  int s2 = digitalRead(TILT_S2);
  int newOrientation = (s1 << 1) | s2;  // Codifica binaria orientamento
  
  if (newOrientation != currentOrientation) {
    processOrientationChange(newOrientation, currentTime);
  }
}

void processOrientationChange(int newOrientation, unsigned long timestamp) {
  int previousOrientation = currentOrientation;
  currentOrientation = newOrientation;
  displayRotation = rotationMapping[currentOrientation];
  
  // Analisi direzione rotazione per gesture detection
  int rotationDirection = analyzeRotationDirection(previousOrientation, newOrientation);
  
  if (rotationDirection != -1) {
    addToGestureSequence(rotationDirection, timestamp);
    lastGestureTime = timestamp;
    
    Serial.print(F("Rotazione rilevata: "));
    Serial.print(displayModes[previousOrientation]);
    Serial.print(F(" -> "));
    Serial.print(displayModes[newOrientation]);
    Serial.print(F(" ("));
    Serial.print(rotationDirection == 1 ? "DESTRA" : "SINISTRA");
    Serial.println(F(")"));
  }
  
  // Aggiornamento immediato display se non in sleep
  if (!sleepModeActive) {
    updateCurrentDisplay();
    lastDisplayUpdate = timestamp;
  }
}

int analyzeRotationDirection(int from, int to) {
  if (from == -1) return -1;  // Prima lettura
  
  // Matrice transizioni valide - rotazione oraria
  if ((from == 0 && to == 1) || (from == 1 && to == 2) || 
      (from == 2 && to == 3) || (from == 3 && to == 0)) {
    return 1;  // Destra
  }
  
  // Matrice transizioni valide - rotazione antioraria
  if ((from == 0 && to == 3) || (from == 3 && to == 2) || 
      (from == 2 && to == 1) || (from == 1 && to == 0)) {
    return 0;  // Sinistra
  }
  
  return -1;  // Transizione non valida
}

// ========== GESTURE RECOGNITION ==========
void addToGestureSequence(int direction, unsigned long timestamp) {
  // Reset buffer su timeout
  if (timestamp - lastGestureTime > GESTURE_TIMEOUT && sequenceIndex > 0) {
    Serial.println(F("Timeout gesture - Reset"));
    initializeGestureBuffer();
  }
  
  // Aggiunta elemento al buffer circolare
  if (sequenceIndex < 10) {
    gestureSequence[sequenceIndex] = direction;
    sequenceIndex++;
  } else {
    // Shift buffer e aggiunta nuovo elemento
    for (int i = 0; i < 9; i++) {
      gestureSequence[i] = gestureSequence[i + 1];
    }
    gestureSequence[9] = direction;
  }
  
  validateWakeupGesture();
}

void validateWakeupGesture() {
  // Controllo pattern ripetuto due volte (8 elementi totali)
  if (sequenceIndex >= 8) {
    bool patternDetected = true;
    
    // Verifica doppia ripetizione del pattern
    for (int rep = 0; rep < 2; rep++) {
      for (int i = 0; i < PATTERN_LENGTH; i++) {
        int bufferPos = sequenceIndex - 8 + (rep * PATTERN_LENGTH) + i;
        if (gestureSequence[bufferPos] != wakeupPattern[i]) {
          patternDetected = false;
          break;
        }
      }
      if (!patternDetected) break;
    }
    
    if (patternDetected) {
      toggleSleepMode();
      initializeGestureBuffer();
    }
  }
}

void toggleSleepMode() {
  sleepModeActive = !sleepModeActive;
  
  if (sleepModeActive) {
    Serial.println(F("*** SLEEP MODE ATTIVATO ***"));
    sleepStartTime = millis();
    displaySleepMode();
  } else {
    Serial.println(F("*** MODALITÀ NORMALE RIPRISTINATA ***"));
    lastSensorRead = millis();
    lastDisplayUpdate = millis();
    updateCurrentDisplay();
  }
}

// ========== ACQUISIZIONE DATI SENSORI ==========
void readEnvironmentalSensors() {
  umidita = dht.readHumidity();
  temperatura = dht.readTemperature();
  
  // Validazione lettura DHT22
  if (isnan(umidita) || isnan(temperatura)) {
    Serial.println(F("Errore lettura DHT22"));
    displayError("ERRORE DHT22");
    return;
  }
  
  Serial.print(F("Ambiente - T: "));
  Serial.print(temperatura);
  Serial.print(F("°C, H: "));
  Serial.print(umidita);
  Serial.println(F("%"));
}

// Algoritmo di rilevamento battito cardiaco bassu su PPG
#define SAMPLE_SIZE 4
#define RISE_THRESHOLD 3

String acquireHeartRate() {
  static float sampleBuffer[SAMPLE_SIZE];
  static float bufferSum = 0;
  static int bufferIndex = 0;
  static float previousValue = 0;
  static bool risingEdge = false;
  static int riseCounter = 0;
  static float beatIntervals[3] = {0, 0, 0};
  static int validIntervals = 0;
  static unsigned long lastBeatTime = 0;
  static bool algorithmInitialized = false;
  
  // Inizializzazione buffer
  if (!algorithmInitialized) {
    for (int i = 0; i < SAMPLE_SIZE; i++) {
      sampleBuffer[i] = 0;
    }
    algorithmInitialized = true;
  }
  
  // Acquisizione campione mediato su 20ms
  int sampleCount = 0;
  float sampleSum = 0.0;
  unsigned long startTime = millis();
  
  do {
    sampleSum += analogRead(HEART_SENSOR_PIN);
    sampleCount++;
  } while (millis() < startTime + 20);
  
  float currentSample = sampleSum / sampleCount;
  
  // Aggiornamento buffer circolare con media mobile
  bufferSum -= sampleBuffer[bufferIndex];
  bufferSum += currentSample;
  sampleBuffer[bufferIndex] = currentSample;
  float smoothedSignal = bufferSum / SAMPLE_SIZE;
  
  // Rilevamento fronte di salita (peak detection)
  if (smoothedSignal > previousValue) {
    riseCounter++;
    if (!risingEdge && riseCounter > RISE_THRESHOLD) {
      risingEdge = true;
      unsigned long currentBeatTime = millis();
      
      if (lastBeatTime > 0) {
        float interval = currentBeatTime - lastBeatTime;
        
        // Filtro fisiologico: 30-200 BPM
        if (interval >= 300 && interval <= 2000) {
          // Aggiornamento buffer intervalli
          beatIntervals[2] = beatIntervals[1];
          beatIntervals[1] = beatIntervals[0];
          beatIntervals[0] = interval;
          
          if (validIntervals < 3) validIntervals++;
          
          // Calcolo BPM con media pesata
          if (validIntervals >= 2) {
            float avgInterval = (validIntervals == 2) ? 
              (beatIntervals[0] + beatIntervals[1]) / 2.0 :
              (0.5 * beatIntervals[0] + 0.3 * beatIntervals[1] + 0.2 * beatIntervals[2]);
            
            float bpm = 60000.0 / avgInterval;
            lastBeatTime = currentBeatTime;
            return String((int)(bpm + 0.5f));
          }
        }
      }
      lastBeatTime = currentBeatTime;
    }
  } else {
    risingEdge = false;
    riseCounter = 0;
  }
  
  previousValue = smoothedSignal;
  bufferIndex = (bufferIndex + 1) % SAMPLE_SIZE;
  return "0";
}

// ========== INTERFACCIA UTENTE ==========
void updateCurrentDisplay() {
  switch(currentOrientation) {
    case 0: displayClock(); break;
    case 1: displayAirQuality(); break;
    case 2: displayHeartRate(); break;
    case 3: displayEnvironmental(); break;
    default: displayError("ORIENTAMENTO INVALIDO"); break;
  }
}

void displayClock() {
  DateTime now = rtc.now();
  
  display.clearDisplay();
  display.setRotation(displayRotation);
  showOrientationIndicator();
  
  // Intestazione
  display.setCursor(5, 5);
  display.setTextSize(1);
  display.println(F("OROLOGIO"));
  display.drawLine(5, 15, display.width() - 20, 15, SSD1306_WHITE);
  
  // Orario principale
  display.setTextSize(2);
  display.setCursor(5, 25);
  display.printf("%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  
  // Data
  display.setTextSize(1);
  display.setCursor(15, 50);
  display.printf("%02d/%02d/%04d", now.day(), now.month(), now.year());
  
  display.display();
}

void displayAirQuality() {
  display.clearDisplay();
  display.setRotation(displayRotation);
  showOrientationIndicator();
  
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("QUALITA' ARIA"));
  display.drawLine(0, 10, display.width(), 10, SSD1306_WHITE);
  
  // Simulazione dati qualità aria (sostituire con sensore reale)
  display.setCursor(5, 20);
  display.println(F("PM2.5: 15 ug/m3"));
  display.setCursor(5, 30);
  display.println(F("PM10:  23 ug/m3"));
  display.setCursor(5, 40);
  display.println(F("CO2:   420 ppm"));
  display.setCursor(5, 50);
  display.println(F("Stato: BUONA"));
  
  display.display();
}

void displayHeartRate() {
  display.clearDisplay();
  display.setRotation(displayRotation);
  showOrientationIndicator();
  
  // Intestazione compatta
  display.setTextSize(1);
  display.setCursor(2, 0);
  display.println(F("BATTITO CARDIACO"));
  display.drawLine(0, 9, display.width() - 15, 9, SSD1306_WHITE);
  
  // Animazione cuore sincronizzata
  renderHeartAnimation();
  
  // Valore BPM centrato
  display.setTextSize(3);
  String bpmDisplay = (lastValidBPM == "0") ? "--" : lastValidBPM;
  int textWidth = bpmDisplay.length() * 18;
  int centerX = (display.width() - textWidth - 15) / 2;
  display.setCursor(centerX, 28);
  display.println(bpmDisplay);
  
  // Label
  display.setTextSize(1);
  display.setCursor(centerX + 8, 52);
  display.println(F("BPM"));
  
  // Traccia ECG stilizzata
  renderECGTrace();
  
  display.display();
}

void displayEnvironmental() {
  display.clearDisplay();
  display.setRotation(displayRotation);
  showOrientationIndicator();
  
  display.setTextSize(1);
  display.setCursor(2, 0);
  display.println(F("AMBIENTE"));
  display.drawLine(0, 9, display.width() - 15, 9, SSD1306_WHITE);
  
  // Temperatura - layout ottimizzato
  display.setCursor(2, 14);
  display.println(F("TEMPERATURA:"));
  
  display.setTextSize(2);
  String tempStr = String(temperatura, 1);
  int textWidth = tempStr.length() * 12;
  int centerX = (display.width() - textWidth - 15) / 2;
  display.setCursor(centerX, 24);
  display.print(temperatura, 1);
  display.setTextSize(1);
  display.print(F("°C"));
  
  // Separatore
  display.drawLine(0, 42, display.width() - 15, 42, SSD1306_WHITE);
  
  // Umidità
  display.setCursor(2, 46);
  display.print(F("UMIDITA': "));
  display.print(umidita, 0);
  display.print(F("%"));
  
  display.display();
}

void displaySleepMode() {
  display.clearDisplay();
  display.setRotation(0);
  
  // Icona sleep centrata
  int centerX = SCREEN_WIDTH / 2;
  int centerY = SCREEN_HEIGHT / 2;
  
  display.setTextSize(2);
  display.setCursor(centerX - 20, centerY - 15);
  display.println(F("ZZZ"));
  
  display.setTextSize(1);
  display.setCursor(centerX - 30, centerY + 10);
  display.println(F("SLEEP MODE"));
  
  // Timer sleep
  unsigned long sleepDuration = (millis() - sleepStartTime) / 1000;
  display.setCursor(centerX - 25, centerY + 20);
  display.printf("T: %lus", sleepDuration);
  
  display.display();
}

// ========== FUNZIONI DI SUPPORTO ==========
void renderHeartAnimation() {
  int heartX = display.width() - 25;
  int heartY = 18;
  bool heartBeat = (millis() % 1000 < 150);
  
  if (heartBeat) {
    // Cuore espanso durante battito
    display.fillCircle(heartX, heartY, 4, SSD1306_WHITE);
    display.fillCircle(heartX + 7, heartY, 4, SSD1306_WHITE);
    display.fillTriangle(heartX - 4, heartY + 3, heartX + 11, heartY + 3, 
                        heartX + 3, heartY + 10, SSD1306_WHITE);
  } else {
    // Cuore normale
    display.fillCircle(heartX, heartY, 3, SSD1306_WHITE);
    display.fillCircle(heartX + 6, heartY, 3, SSD1306_WHITE);
    display.fillTriangle(heartX - 3, heartY + 2, heartX + 9, heartY + 2, 
                        heartX + 3, heartY + 8, SSD1306_WHITE);
  }
}

void renderECGTrace() {
  int baseY = 58;
  bool heartBeat = (millis() % 1000 < 150);
  
  for (int x = 2; x < display.width() - 15; x += 4) {
    int offset = 0;
    if (heartBeat && (x % 16 == 0 || x % 16 == 4)) {
      offset = (x % 16 == 0) ? -3 : 2;
    }
    display.drawPixel(x, baseY + offset, SSD1306_WHITE);
    display.drawPixel(x + 1, baseY + offset, SSD1306_WHITE);
  }
}

void showOrientationIndicator() {
  // Indicatore orientamento corrente
  int x = display.width() - 12;
  int y = 2;
  
  display.fillRect(x, y, 10, 8, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(x + 2, y + 1);
  display.print(currentOrientation);
  display.setTextColor(SSD1306_WHITE);
}

void showBootScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("Sistema Multiparametrico"));
  display.println(F("ESP32 v1.0"));
  display.println(F(""));
  display.println(F("Sensori:"));
  display.println(F("- DHT22 (T/H)"));
  display.println(F("- DS3231 (RTC)"));
  display.println(F("- KY-039 (HR)"));
  display.println(F("- Tilt Sensor"));
  display.println(F(""));
  display.println(F("Gesture: DX-SX-DX-SX x2"));
  display.display();
}

void displayError(const char* message) {
  display.clearDisplay();
  display.setRotation(0);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("ERRORE SISTEMA"));
  display.println(message);
  display.display();
}

void initializeGestureBuffer() {
  for (int i = 0; i < 10; i++) {
    gestureSequence[i] = -1;
  }
  sequenceIndex = 0;
}