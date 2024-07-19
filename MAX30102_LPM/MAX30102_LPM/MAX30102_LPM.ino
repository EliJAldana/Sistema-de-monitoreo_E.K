#include <Wire.h>
#include "MAX30105.h"

#include "heartRate.h"

MAX30105 particleSensor;

const byte RATE_SIZE = 4;     //Aumentar para obtener más promedios
byte rates[RATE_SIZE];        //Conjunto de frecuencias cardíacas
byte rateSpot = 0;
long lastBeat = 0;            //Cuando ocurrió el último latido

float beatsPerMinute;
int beatAvg;

void setup()
{
  Serial.begin(115200);
  Serial.println("Initializing...");

  // Inicializar sensor
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) 
  {
    Serial.println("No se encontró MAX30102");
    while (1);
  }
  Serial.println("Coloque su dedo índice sobre el sensor con presión constante");

  particleSensor.setup();                       //Configurar el sensor de forma predeterminada
  particleSensor.setPulseAmplitudeRed(0x0A);    //LED rojo a nivel bajo para indicar que el sensor está funcionando
  particleSensor.setPulseAmplitudeGreen(0);     //Apagar el LED verde
}

void loop()
{
  long irValue = particleSensor.getIR();

  if (checkForBeat(irValue) == true)
  {
    //Sensado de un latido
    long delta = millis() - lastBeat;
    lastBeat = millis();

    beatsPerMinute = 60 / (delta / 1000.0);

    if (beatsPerMinute < 255 && beatsPerMinute > 20)
    {
      rates[rateSpot++] = (byte)beatsPerMinute;   //Almacenar esta lectura en la matriz
      rateSpot %= RATE_SIZE;                      //Variable de ajuste

      //Tomar promedio de lecturas
      beatAvg = 0;
      for (byte x = 0 ; x < RATE_SIZE ; x++)
        beatAvg += rates[x];
      beatAvg /= RATE_SIZE;
    }
  }

  //Serial.print("IR=");
  //Serial.print(irValue);
  Serial.print("LPM=");
  Serial.print(beatsPerMinute);
  //Serial.print(", Avg BPM=");
  //Serial.print(beatAvg);

  if (irValue < 50000)
    Serial.print(" No se identifica algun dedo ");

  Serial.println();
}


