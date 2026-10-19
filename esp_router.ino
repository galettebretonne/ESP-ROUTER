#include <ESP8266WiFi.h>

const char* ssid     = "3Com";
const char* password = "zf81jl3600";

const IPAddress tv_ip(192, 168, 1, 103);

#define MAX_SESSIONS 5
#define SESSION_TIMEOUT_MS 30000 // 30 secondes d'inactivité max

struct ProxySession {
  WiFiClient clientHA;
  WiFiClient clientTV;
  bool active = false;
  unsigned long lastActivity = 0;
};

class MultiTcpProxy {
private:
  uint16_t port;
  WiFiServer server;
  ProxySession sessions[MAX_SESSIONS];
  uint8_t buffer[2048];

public:
  MultiTcpProxy(uint16_t p) : port(p), server(p) {}

  void begin() {
    server.begin();
    server.setNoDelay(true);
  }

  void update() {
    // 1. Accepter les nouvelles connexions
    if (server.hasClient()) {
      WiFiClient newHA = server.available();
      newHA.setNoDelay(true);

      int slot = -1;
      for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].active) {
          slot = i;
          break;
        }
      }

      if (slot != -1) {
        if (sessions[slot].clientTV.connect(tv_ip, port)) {
          sessions[slot].clientTV.setNoDelay(true);
          sessions[slot].clientHA = newHA;
          sessions[slot].active = true;
          sessions[slot].lastActivity = millis();
          Serial.printf("[%d][Slot %d] Session établie\n", port, slot);
        } else {
          newHA.stop();
        }
      } else {
        Serial.printf("[%d] Trop de sessions ouvertes, refus\n", port);
        newHA.stop();
      }
    }

    // 2. Relayer les données et gérer la fin de vie des sockets
    for (int i = 0; i < MAX_SESSIONS; i++) {
      if (!sessions[i].active) continue;

      bool dataTransferred = false;

      // Transfert HA -> TV (tant qu'il y a des données, même si HA a commencé à se déconnecter)
      int availHA = sessions[i].clientHA.available();
      if (availHA > 0) {
        size_t len = sessions[i].clientHA.read(buffer, min((size_t)availHA, sizeof(buffer)));
        if (len > 0) {
          sessions[i].clientTV.write(buffer, len);
          dataTransferred = true;
        }
      }

      // Transfert TV -> HA
      int availTV = sessions[i].clientTV.available();
      if (availTV > 0) {
        size_t len = sessions[i].clientTV.read(buffer, min((size_t)availTV, sizeof(buffer)));
        if (len > 0) {
          sessions[i].clientHA.write(buffer, len);
          dataTransferred = true;
        }
      }

      if (dataTransferred) {
        sessions[i].lastActivity = millis();
      }

      // 3. Fermeture PROPRE si un côté n'a plus ni connexion ni données en attente
      bool haAlive = sessions[i].clientHA.connected() || (sessions[i].clientHA.available() > 0);
      bool tvAlive = sessions[i].clientTV.connected() || (sessions[i].clientTV.available() > 0);
      bool timedOut = (millis() - sessions[i].lastActivity) > SESSION_TIMEOUT_MS;

      if (!haAlive || !tvAlive || timedOut) {
        // Vider les buffers d'écriture pour éviter l'envoi de TCP RST
        if (sessions[i].clientHA) {
          sessions[i].clientHA.flush();
          sessions[i].clientHA.stop();
        }
        if (sessions[i].clientTV) {
          sessions[i].clientTV.flush();
          sessions[i].clientTV.stop();
        }
        sessions[i].active = false;
        Serial.printf("[%d][Slot %d] Session fermée proprement\n", port, i);
      }
    }
  }
};

MultiTcpProxy proxy8001(8001);
MultiTcpProxy proxy8002(8002);

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- RELAIS ESP8266 SOCAT-MATCHING SAMSUNG TV ---");

  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(500);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\n[OK] Wi-Fi connecté");
  Serial.print("IP ESP : ");
  Serial.println(WiFi.localIP());

  proxy8001.begin();
  proxy8002.begin();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(1000);
    return;
  }

  proxy8001.update();
  proxy8002.update();

  yield();
}