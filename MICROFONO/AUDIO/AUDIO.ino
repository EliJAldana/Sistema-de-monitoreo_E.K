#define BLUETOOTH "ESP32BT"
#include "esp32dumbdisplay.h"
DumbDisplay dumbdisplay(new DDBluetoothSerialIO(BLUETOOTH));
// Controlador I2S
#include <driver/i2s.h>

// Asignación de pines I2S para INMP441
#define I2S_WS 2
#define I2S_SD 15
#define I2S_SCK 0
#define I2S_SAMPLE_BIT_COUNT 16
#define SOUND_SAMPLE_RATE 8000
#define SOUND_CHANNEL_COUNT 1
#define I2S_PORT I2S_NUM_0

PlotterDDLayer* plotterLayer;
LcdDDLayer* micTabLayer;
LcdDDLayer* recTabLayer;
LcdDDLayer* playTabLayer;
LcdDDLayer* startBtnLayer;
LcdDDLayer* stopBtnLayer;
LcdDDLayer* amplifyLblLayer;
LedGridDDLayer* amplifyMeterLayer;

// Nombre del archivo WAV grabado; dado que solo hay un nombre, uno nuevo siempre sobrescribirá al antiguo
const char* SoundName = "recorded_sound";

const int I2S_DMA_BUF_COUNT = 8;
const int I2S_DMA_BUF_LEN = 1024;

#if I2S_SAMPLE_BIT_COUNT == 32
const int StreamBufferNumBytes = 512;
const int StreamBufferLen = StreamBufferNumBytes / 4;
int32_t StreamBuffer[StreamBufferLen];
#else
#if SOUND_SAMPLE_RATE == 16000
// para 16 bits ... 16000 muestras por segundo (32000 bytes por segundo, ya que 16 bits por muestra) ==> 512 bytes = 16 ms por lectura
const int StreamBufferNumBytes = 512;
#else
// para 16 bits ... 8000 muestras por segundo (16000 bytes por segundo, ya que 16 bits por muestra) ==> 256 bytes = 16 ms por lectura
const int StreamBufferNumBytes = 256;
#endif
const int StreamBufferLen = StreamBufferNumBytes / 2;
int16_t StreamBuffer[StreamBufferLen];
#endif

// Amplificación de muestras de sonido (16 bits)
const int MaxAmplifyFactor = 20;
const int DefAmplifyFactor = 10;

esp_err_t i2s_install();
esp_err_t i2s_setpin();

DDConnectVersionTracker cvTracker;  // Rastrea la conexión de DD establecida (nueva)
int what = 1;                       // 1: micrófono; 2: grabar; 3: reproducir
bool started = false;
int amplifyFactor = DefAmplifyFactor;  // 10;
int soundChunkId = -1;                 // Cuando se comienza a enviar un sonido [chunk], el "id del chunk" asignado
long streamingMillis = 0;
int streamingTotalSampleCount = 0;

void setup() {

  Serial.begin(115200);

  Serial.println("CONFIGURANDO MICRÓFONO ...");

  // Configurar I2S
  if (i2s_install() != ESP_OK) {
    Serial.println("XXX falló al instalar I2S");
  }
  if (i2s_setpin() != ESP_OK) {
    Serial.println("XXX falló al configurar los pines I2S");
  }
  if (i2s_zero_dma_buffer(I2S_PORT) != ESP_OK) {
    Serial.println("XXX falló al poner a cero el buffer DMA de I2S");
  }
  if (i2s_start(I2S_PORT) != ESP_OK) {
    Serial.println("XXX falló al iniciar I2S");
  }

  Serial.println("... MICRÓFONO CONFIGURADO");

  dumbdisplay.recordLayerSetupCommands();  // Comenzar a grabar los comandos de configuración de la capa

  plotterLayer = dumbdisplay.createPlotterLayer(1024, 256, SOUND_SAMPLE_RATE / StreamBufferLen);

  // Crear capas lcd "MIC/REC/PLAY", como pestañas
  micTabLayer = dumbdisplay.createLcdLayer(8, 1);
  micTabLayer->writeCenteredLine("MIC");
  micTabLayer->border(1, "gray");
  micTabLayer->enableFeedback("f");
  recTabLayer = dumbdisplay.createLcdLayer(8, 1);
  recTabLayer->writeCenteredLine("REC");
  recTabLayer->border(1, "gray");
  recTabLayer->enableFeedback("f");
  playTabLayer = dumbdisplay.createLcdLayer(8, 1);
  playTabLayer->writeCenteredLine("PLAY");
  playTabLayer->border(1, "gray");
  playTabLayer->enableFeedback("f");

  // Crear capa lcd "START/STOP", actuando como botón
  startBtnLayer = dumbdisplay.createLcdLayer(12, 3);
  startBtnLayer->pixelColor("darkgreen");
  startBtnLayer->border(2, "darkgreen", "round");
  startBtnLayer->margin(1);
  startBtnLayer->enableFeedback("fl");
  stopBtnLayer = dumbdisplay.createLcdLayer(12, 3);
  stopBtnLayer->pixelColor("darkred");
  stopBtnLayer->border(2, "darkgreen", "round");
  stopBtnLayer->margin(1);
  stopBtnLayer->enableFeedback("fl");

  // Crear etiqueta "amplify" encima de la capa del medidor "amplify" (a ser creada a continuación)
  amplifyLblLayer = dumbdisplay.createLcdLayer(12, 1);
  amplifyLblLayer->pixelColor("darkred");
  amplifyLblLayer->noBackgroundColor();

  // Crear capa del medidor "amplify"
  amplifyMeterLayer = dumbdisplay.createLedGridLayer(MaxAmplifyFactor, 1, 1, 2);
  amplifyMeterLayer->onColor("darkblue");
  amplifyMeterLayer->offColor("lightgray");
  amplifyMeterLayer->border(0.2, "blue");
  amplifyMeterLayer->enableFeedback("fa:rpt50");  // rep50 significa repetición automática cada 50 milisegundos

  DDAutoPinConfigBuilder<1> builder('V');  // vertical
  builder
    .addLayer(plotterLayer)
    .beginGroup('H')  // horizontal
    .addLayer(micTabLayer)
    .addLayer(recTabLayer)
    .addLayer(playTabLayer)
    .endGroup()
    .beginGroup('H')  // horizontal
    .addLayer(startBtnLayer)
    .addLayer(stopBtnLayer)
    .endGroup()
    .beginGroup('S')  // apilado, uno sobre otro
    .addLayer(amplifyLblLayer)
    .addLayer(amplifyMeterLayer)
    .endGroup();
  dumbdisplay.configAutoPin(builder.build());

  dumbdisplay.playbackLayerSetupCommands("esp32ddmice");  // Reproducir los comandos de configuración de la capa grabados, así como persistir la disposición en el teléfono, para poder reconectar

  // Establecer el controlador de inactividad de DD ... aquí hay una expresión lambda
  dumbdisplay.setIdleCallback([](long idleForMillis, DDIdleConnectionState connectionState) {
    if (connectionState == DDIdleConnectionState::IDLE_RECONNECTING) {
      started = false;  // si está inactivo, por ejemplo, desconectado, detener lo que sea
    }
  });
}

void loop() {

  bool updateTab = false;
  bool updateStartStop = false;
  bool updateAmplifyFactor = false;
  if (cvTracker.checkChanged(dumbdisplay)) {
    // Si es la primera vez aquí, o la conexión DD cambió (por ejemplo, se reconectó), actualizar todos los componentes de la UI
    started = false;
    updateTab = true;
    updateStartStop = true;
    updateAmplifyFactor = true;
  } else {
    // Comprobar si es necesario actualizar algún componente de la UI
    int oriWhat = what;
    if (micTabLayer->getFeedback()) {
      what = 1;
    } else if (recTabLayer->getFeedback()) {
      what = 2;
    } else if (playTabLayer->getFeedback()) {
      what = 3;
    }
    if (what != oriWhat) {
      started = false;
      updateTab = true;
      updateStartStop = true;
    }
    if (startBtnLayer->getFeedback()) {
      started = true;
      updateStartStop = true;
    } else if (stopBtnLayer->getFeedback()) {
      started = false;
      updateStartStop = true;
    }
    const DDFeedback* feedback = amplifyMeterLayer->getFeedback();
    if (feedback != NULL) {
      amplifyFactor = feedback->x + 1;
      updateAmplifyFactor = true;
    }
  }

  if (updateTab) {
    const char* micColor = what == 1 ? "blue" : "gray";
    const char* micBoarderShape = what == 1 ? "flat" : "hair";
    const char* recColor = what == 2 ? "blue" : "gray";
    const char* recBoarderShape = what == 2 ? "flat" : "hair";
    const char* playColor = what == 3 ? "blue" : "gray";
    const char* playBoarderShape = what == 3 ? "flat" : "hair";
    micTabLayer->border(1, micColor, micBoarderShape);
    micTabLayer->pixelColor(micColor);
    recTabLayer->border(1, recColor, recBoarderShape);
    recTabLayer->pixelColor(recColor);
    playTabLayer->border(1, playColor, playBoarderShape);
    playTabLayer->pixelColor(playColor);
  }
  if (updateStartStop) {
    const char* whatTitle;
    if (what == 1) {
      whatTitle = "MIC";
    } else if (what == 2) {
      whatTitle = "REC";
    } else if (what == 3) {
      whatTitle = "PLAY";
    }
    startBtnLayer->writeCenteredLine(String("Start ") + whatTitle, 1);
    stopBtnLayer->writeCenteredLine(String("Stop ") + whatTitle, 1);
    if (what == 3) {
      startBtnLayer->disabled(false);
      stopBtnLayer->disabled(false);
      amplifyMeterLayer->disabled(true);
    } else {
      if (started) {
        startBtnLayer->disabled(true);
        stopBtnLayer->disabled(false);
      } else {
        startBtnLayer->disabled(false);
        stopBtnLayer->disabled(true);
      }
      micTabLayer->disabled(started);
      recTabLayer->disabled(started);
      playTabLayer->disabled(started);
      amplifyMeterLayer->disabled(false);
    }
  }
  if (updateAmplifyFactor) {
    amplifyMeterLayer->horizontalBar(amplifyFactor);
    amplifyLblLayer->writeLine(String(amplifyFactor), 0, "R");
  }

  // leer datos I2S y colocar en el buffer de datos
  size_t bytesRead = 0;
  esp_err_t result = i2s_read(I2S_PORT, &StreamBuffer, StreamBufferNumBytes, &bytesRead, portMAX_DELAY);

  int samplesRead = 0;
#if I2S_SAMPLE_BIT_COUNT == 32
  int16_t sampleStreamBuffer[StreamBufferLen];
#else
  int16_t* sampleStreamBuffer = StreamBuffer;
#endif
  if (result == ESP_OK) {
#if I2S_SAMPLE_BIT_COUNT == 32
    samplesRead = bytesRead / 4;  // 32 bits por muestra
#else
    samplesRead = bytesRead / 2;  // 16 bits por muestra
#endif
    if (samplesRead > 0) {
      // encontrar la media de las muestras ... y amplificar la muestra de sonido, simplemente multiplicándola por algún "factor de amplificación"
      float sumVal = 0;
      for (int i = 0; i < samplesRead; ++i) {
        int32_t val = StreamBuffer[i];
#if I2S_SAMPLE_BIT_COUNT == 32
        val = val / 0x0000ffff;
#endif
        if (amplifyFactor > 1) {
          val = amplifyFactor * val;
          if (val > 32700) {
            val = 32700;
          } else if (val < -32700) {
            val = -32700;
          }
          //StreamBuffer[i] = val;
        }
        sampleStreamBuffer[i] = val;
        sumVal += val;
      }
      float meanVal = sumVal / samplesRead;
      plotterLayer->set(meanVal);
    }
  }

  if (what == 3) {
    if (updateStartStop) {
      // es decir, hacer clic en inicio o parada
      if (started) {
        dumbdisplay.playSound(SoundName);
      } else {
        dumbdisplay.stopSound();
      }
    }
    return;
  }

  if (started) {
    if (soundChunkId == -1) {
      // mientras está iniciado ... si no hay un "id de chunk" asignado (es decir, aún no ha comenzado a enviar sonido)
      if (what == 1) {
        // comenzar a transmitir sonido y obtener el "id de chunk" asignado
        soundChunkId = dumbdisplay.streamSound16(SOUND_SAMPLE_RATE, SOUND_CHANNEL_COUNT);  // el sonido es de 16 bits por muestra
        dumbdisplay.writeComment(String("INICIADO streaming de micrófono con id de chunk [") + soundChunkId + "]");
      } else if (what == 2) {
        // comenzar a guardar sonido y obtener el "id de chunk" asignado
        soundChunkId = dumbdisplay.saveSoundChunked16(SoundName, SOUND_SAMPLE_RATE, SOUND_CHANNEL_COUNT);
        dumbdisplay.writeComment(String("INICIADO grabación de streaming con id de chunk [") + soundChunkId + "]");
      }
      streamingMillis = millis();
      streamingTotalSampleCount = 0;
    }
  }

  if (result == ESP_OK) {
    if (soundChunkId != -1) {
      // enviar muestras de sonido leídas
      bool isFinalChunk = !started;  // es el chunk final si acaba de detenerse
      dumbdisplay.sendSoundChunk16(soundChunkId, sampleStreamBuffer, samplesRead, isFinalChunk);
      streamingTotalSampleCount += samplesRead;
      if (isFinalChunk) {
        dumbdisplay.writeComment(String("FINALIZADO streaming con id de chunk [") + soundChunkId + "]");
        long forMillis = millis() - streamingMillis;
        int totalSampleCount = streamingTotalSampleCount;
        dumbdisplay.writeComment(String(". total de muestras transmitidas: ") + totalSampleCount + " en " + String(forMillis / 1000.0) + "s");
        dumbdisplay.writeComment(String(". tasa de muestras de transmisión: ") + String(1000.0 * ((float)totalSampleCount / forMillis)));
        soundChunkId = -1;
      }
    }
  }
}

esp_err_t i2s_install() {
  uint32_t mode = I2S_MODE_MASTER | I2S_MODE_RX;
#if I2S_SCK == I2S_PIN_NO_CHANGE
  mode |= I2S_MODE_PDM;
#endif
  const i2s_config_t i2s_config = {
    .mode = i2s_mode_t(mode /*I2S_MODE_MASTER | I2S_MODE_RX*/),
    .sample_rate = SOUND_SAMPLE_RATE,
    .bits_per_sample = i2s_bits_per_sample_t(I2S_SAMPLE_BIT_COUNT),
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
    .intr_alloc_flags = 0,
    .dma_buf_count = I2S_DMA_BUF_COUNT /*8*/,
    .dma_buf_len = I2S_DMA_BUF_LEN /*1024*/,
    .use_apll = false
  };
  return i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
}

esp_err_t i2s_setpin() {
  const i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE /*-1*/,
    .data_in_num = I2S_SD
  };
  return i2s_set_pin(I2S_PORT, &pin_config);
}